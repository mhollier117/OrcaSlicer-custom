#include "BblPresetSyncController.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"
#include "MsgDialog.hpp"
#include "NotificationManager.hpp"
#include "Plater.hpp"
#include "Tab.hpp"
#include "format.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/BBLPrinterAgent.hpp"
#include "slic3r/Utils/BblPresetSync/Ledger.hpp"
#include "slic3r/Utils/BblPresetSync/Snapshot.hpp"
#include "slic3r/Utils/CloudProvider.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>
#include <boost/thread/thread.hpp>

#include <wx/button.h>
#include <wx/checklst.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <chrono>
#include <ctime>
#include <future>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
namespace GUI {

namespace sync = BblPresetSync;

namespace {

bool has_bambu_id(const std::string& orca_filament_id)
{
    return BBLPrinterAgent::bambu_filament_id(orca_filament_id) != orca_filament_id;
}

std::string join_names(const std::vector<sync::Key>& keys)
{
    std::string out;
    for (const sync::Key& key : keys) {
        if (!out.empty()) out += ", ";
        out += key.name;
    }
    return out;
}

std::string join_names(const std::vector<std::string>& names)
{
    std::string out;
    for (const std::string& name : names) {
        if (!out.empty()) out += ", ";
        out += name;
    }
    return out;
}

sync::OpOutcome failed(const std::string& reason)
{
    sync::OpOutcome o;
    o.result = sync::OpResult::Failed;
    o.reason = reason;
    return o;
}

bool main_window_shown(const GUI_App& app)
{
    return app.mainframe != nullptr && app.mainframe->IsShown();
}

} // namespace

// ---------------------------------------------------------------------------
// First-run dialog
// ---------------------------------------------------------------------------

BblPresetSyncFirstRunDialog::BblPresetSyncFirstRunDialog(wxWindow* parent, const sync::FirstRunSummary& summary)
    : DPIDialog(parent, wxID_ANY, _L("Sync presets with the Bambu cloud"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    for (const sync::Key& key : summary.candidates) m_candidates.push_back(key.name);

    wxString text;
    auto     add_group = [&text](const wxString& line) {
        if (!text.empty()) text += "\n";
        text += line;
    };
    if (!summary.uploads.empty()) add_group(format_wxstr(_L("Upload to Bambu: %1%"), join_names(summary.uploads)));
    if (!summary.downloads.empty()) add_group(format_wxstr(_L("Download from Bambu: %1%"), join_names(summary.downloads)));
    if (!summary.links.empty()) add_group(format_wxstr(_L("Already the same on both sides: %1%"), join_names(summary.links)));
    if (!summary.conflicts_local_wins.empty())
        add_group(format_wxstr(_L("Different on both sides, OrcaSlicer's copy is newer and wins: %1%"), join_names(summary.conflicts_local_wins)));
    if (!summary.conflicts_remote_wins.empty())
        add_group(format_wxstr(_L("Different on both sides, Bambu's copy is newer and wins: %1%"), join_names(summary.conflicts_remote_wins)));
    if (text.empty() && m_candidates.empty()) text = _L("Nothing to sync yet.");

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    if (!text.empty()) {
        auto* box = new wxTextCtrl(this, wxID_ANY, text, wxDefaultPosition, wxSize(FromDIP(520), FromDIP(140)),
                                   wxTE_MULTILINE | wxTE_READONLY | wxTE_WORDWRAP);
        sizer->Add(box, 1, wxEXPAND | wxALL, FromDIP(10));
    }
    if (!m_candidates.empty()) {
        auto* note = new wxStaticText(this, wxID_ANY,
                                      _L("These filaments can only sync, and be chosen in the AMS, as standalone custom filaments. Converting one "
                                         "gives it its own filament ID; it then no longer follows its parent profile. A backup is kept."));
        note->Wrap(FromDIP(500));
        sizer->Add(note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));

        wxArrayString names;
        for (const std::string& name : m_candidates) names.Add(from_u8(name));
        m_list = new wxCheckListBox(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(500), FromDIP(120)), names);
        for (unsigned int i = 0; i < names.GetCount(); ++i) m_list->Check(i, true);
        sizer->Add(m_list, 1, wxEXPAND | wxALL, FromDIP(10));
    }

    auto* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto* sync_btn = new wxButton(this, wxID_OK, _L("Sync"));
    auto* later_btn = new wxButton(this, wxID_CANCEL, _L("Not now"));
    buttons->AddStretchSpacer(1);
    buttons->Add(sync_btn, 0, wxRIGHT, FromDIP(8));
    buttons->Add(later_btn, 0);
    sizer->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(10));
    sync_btn->SetDefault();

    SetSizerAndFit(sizer);
    CenterOnParent();
}

std::vector<std::string> BblPresetSyncFirstRunDialog::checked() const
{
    std::vector<std::string> out;
    if (m_list == nullptr) return out;
    for (size_t i = 0; i < m_candidates.size(); ++i)
        if (m_list->IsChecked(static_cast<unsigned int>(i))) out.push_back(m_candidates[i]);
    return out;
}

std::vector<std::string> BblPresetSyncFirstRunDialog::unchecked() const
{
    std::vector<std::string> out;
    if (m_list == nullptr) return out;
    for (size_t i = 0; i < m_candidates.size(); ++i)
        if (!m_list->IsChecked(static_cast<unsigned int>(i))) out.push_back(m_candidates[i]);
    return out;
}

void BblPresetSyncFirstRunDialog::on_dpi_changed(const wxRect&)
{
    Fit();
    Layout();
}

// ---------------------------------------------------------------------------
// Controller
// ---------------------------------------------------------------------------

// Forwards to an AgentCloud over the agent of the moment; the mirror keeps one reference for good.
class BblPresetSyncController::CloudProxy : public sync::ICloud
{
public:
    void set_agent(NetworkAgent* agent)
    {
        m_impl = agent != nullptr ? std::make_unique<sync::AgentCloud>(agent) : nullptr;
    }

    sync::ListResult list(const std::string& bundle_version, const std::function<bool(const sync::CloudRow&)>& want_body) override
    {
        if (!m_impl) return no_agent<sync::ListResult>();
        return m_impl->list(bundle_version, want_body);
    }
    sync::WriteResult create(const std::string& name, sync::ValueMap values) override
    {
        if (!m_impl) return no_agent<sync::WriteResult>();
        return m_impl->create(name, std::move(values));
    }
    sync::WriteResult update(const std::string& setting_id, const std::string& name, sync::ValueMap values) override
    {
        if (!m_impl) return no_agent<sync::WriteResult>();
        return m_impl->update(setting_id, name, std::move(values));
    }
    bool remove(const std::string& setting_id) override { return m_impl && m_impl->remove(setting_id); }

private:
    template<class T> static T no_agent()
    {
        T r;
        r.ok = false;
        if constexpr (std::is_same_v<T, sync::ListResult>) r.error = "no network agent";
        return r;
    }
    std::unique_ptr<sync::AgentCloud> m_impl;
};

BblPresetSyncController::BblPresetSyncController(GUI_App& app) : m_app(app), m_cloud(std::make_unique<CloudProxy>())
{
    m_mirror = std::make_unique<sync::Mirror>(*this, *m_cloud, *this);
}

BblPresetSyncController::~BblPresetSyncController() = default;

sync::Mirror& BblPresetSyncController::mirror()
{
    return *m_mirror;
}

void BblPresetSyncController::request_sync()
{
    m_manual_requested.store(true, std::memory_order_release);
    m_app.request_user_preset_push();
}

void BblPresetSyncController::tick(const std::weak_ptr<int>& sync_token)
{
    bool new_token;
    {
        std::lock_guard<std::mutex> lock(m_token_mutex);
        new_token = m_token.owner_before(sync_token) || sync_token.owner_before(m_token);
        m_token   = sync_token;
    }
    NetworkAgent* agent = m_app.getAgent();
    if (agent != m_cloud_agent) {
        m_cloud->set_agent(agent);
        m_cloud_agent = agent;
    }
    const bool manual = m_manual_requested.exchange(false, std::memory_order_acq_rel);
    m_mirror->run_pass(new_token || manual ? sync::Trigger::Manual : sync::Trigger::Tick);
}

// --- IHost -----------------------------------------------------------------

std::optional<sync::Session> BblPresetSyncController::session()
{
    AppConfig* config = m_app.app_config;
    if (config == nullptr || config->get_stealth_mode() || config->get("sync_user_preset") != "true" ||
        !config->get_bool("sync_bbl_user_preset"))
        return std::nullopt;
    NetworkAgent* agent = m_app.getAgent();
    if (agent == nullptr || !agent->is_user_login(BBL_CLOUD_PROVIDER) || !agent->is_user_login()) return std::nullopt;
    const std::string orca_id  = agent->get_user_id();
    const std::string bambu_id = agent->get_user_id(BBL_CLOUD_PROVIDER);
    if (orca_id.empty() || bambu_id.empty() || cancelled()) return std::nullopt;
    return sync::Session{m_app.user_preset_session(), orca_id, bambu_id};
}

bool BblPresetSyncController::cancelled()
{
    if (m_app.is_closing()) return true;
    std::lock_guard<std::mutex> lock(m_token_mutex);
    return m_token.expired();
}

long long BblPresetSyncController::now()
{
    return static_cast<long long>(std::time(nullptr));
}

void BblPresetSyncController::sleep_ms(int ms)
{
    boost::this_thread::sleep_for(boost::chrono::milliseconds(ms));
}

std::string BblPresetSyncController::bundle_version()
{
    return m_app.preset_bundle->get_vendor_profile_version("BBL").to_string();
}

boost::filesystem::path BblPresetSyncController::data_dir()
{
    return boost::filesystem::path(Slic3r::data_dir());
}

void BblPresetSyncController::on_first_run(const sync::FirstRunSummary& summary)
{
    const uint64_t generation = m_app.user_preset_session();
    m_app.CallAfter([this, summary, generation] {
        if (m_app.is_closing() || !main_window_shown(m_app) || m_app.user_preset_session() != generation) {
            m_mirror->reset_first_run_prompt();
            return;
        }
        if (!run_conversion_dialog(summary)) {
            m_mirror->confirm_first_run(false);
            return;
        }
        m_mirror->confirm_first_run(true);
        request_sync();
    });
}

bool BblPresetSyncController::run_conversion_dialog(const sync::FirstRunSummary& summary)
{
    BblPresetSyncFirstRunDialog dlg(m_app.mainframe, summary);
    if (dlg.ShowModal() != wxID_OK) return false;

    std::vector<std::string> not_converted;
    for (const std::string& name : dlg.checked()) {
        const sync::ConvertResult r = convert(name);
        if (r.outcome.result != sync::OpResult::Applied) not_converted.push_back(name + " (" + r.outcome.reason + ")");
    }
    for (const std::string& name : dlg.unchecked()) m_mirror->set_declined(sync::Key{sync::PresetType::Filament, name});
    if (!not_converted.empty()) {
        MessageDialog msg(m_app.mainframe,
                          format_wxstr(_L("These filaments were not converted and will be offered again: %1%"), join_names(not_converted)),
                          _L("Sync presets with the Bambu cloud"), wxOK);
        msg.ShowModal();
    }
    return true;
}

void BblPresetSyncController::on_candidates(const std::vector<sync::Key>& candidates)
{
    m_app.CallAfter([this, candidates] {
        if (m_app.is_closing() || m_app.plater() == nullptr) return;
        auto* notifications = m_app.plater()->get_notification_manager();
        if (notifications == nullptr) return;
        const std::string text = format(
            _u8L("These filaments can only sync with the Bambu cloud, and be chosen in the AMS, as standalone custom filaments: %1%"),
            join_names(candidates));
        auto callback = [this, candidates](wxEvtHandler*) {
            m_app.CallAfter([this, candidates] {
                if (m_app.is_closing() || !main_window_shown(m_app)) return;
                sync::FirstRunSummary summary;
                summary.candidates = candidates;
                if (run_conversion_dialog(summary)) request_sync();
            });
            return true;
        };
        notifications->push_notification(NotificationType::CustomNotification, NotificationManager::NotificationLevel::RegularNotificationLevel,
                                         text, _u8L("Convert"), callback);
    });
}

void BblPresetSyncController::on_halt(const std::string& kind, const std::vector<sync::Key>& local_deletes,
                                      const std::vector<sync::Key>& remote_deletes)
{
    m_app.CallAfter([this, kind, local_deletes, remote_deletes] {
        if (m_app.is_closing() || !main_window_shown(m_app)) return;
        const wxString caption = _L("Sync presets with the Bambu cloud");
        const wxString prefix  = kind == "empty_list" ? _L("The Bambu cloud returned no presets at all.") + "\n" : wxString();
        bool           decided = false;

        if (!local_deletes.empty()) {
            MessageDialog dlg(m_app.mainframe,
                              prefix + format_wxstr(_L("Bambu reports that these synced presets were deleted: %1%\n\nYes: delete them in OrcaSlicer too (a backup is kept).\nNo: keep them in OrcaSlicer and stop syncing them.\nCancel: decide later."),
                                                    join_names(local_deletes)),
                              caption, wxYES_NO | wxCANCEL);
            const int answer = dlg.ShowModal();
            if (answer == wxID_YES) {
                m_mirror->resolve_halt(sync::HaltChoice::ProceedLocalDeletes);
                decided = true;
            } else if (answer == wxID_NO) {
                m_mirror->resolve_halt(sync::HaltChoice::KeepLocal);
                decided = true;
            } else {
                m_mirror->resolve_halt(sync::HaltChoice::Later);
            }
        }
        if (!remote_deletes.empty()) {
            MessageDialog dlg(m_app.mainframe,
                              prefix + format_wxstr(_L("These synced presets are gone from OrcaSlicer: %1%\n\nYes: delete them from the Bambu cloud too.\nNo: restore them from the Bambu cloud.\nCancel: decide later."),
                                                    join_names(remote_deletes)),
                              caption, wxYES_NO | wxCANCEL);
            const int answer = dlg.ShowModal();
            if (answer == wxID_YES) {
                m_mirror->resolve_halt(sync::HaltChoice::ProceedRemoteDeletes);
                decided = true;
            } else if (answer == wxID_NO) {
                m_mirror->resolve_halt(sync::HaltChoice::RestoreFromBambu);
                decided = true;
            } else {
                m_mirror->resolve_halt(sync::HaltChoice::Later);
            }
        }
        if (decided) request_sync();
    });
}

void BblPresetSyncController::notify(const std::string& code, const std::string& detail)
{
    std::string text;
    if (code == "preset_limit")
        text = _u8L("The Bambu cloud holds the maximum number of user presets; new presets are not uploaded to it.");
    else if (code == "auth")
        text = _u8L("The Bambu cloud rejected the sign-in; preset sync with it is paused until you sign in to Bambu again.");
    else if (code == "unresolvable")
        text = format(_u8L("A preset from the Bambu cloud could not be loaded: %1%"), detail);
    else {
        BOOST_LOG_TRIVIAL(info) << "bbl preset sync: " << code << ": " << detail;
        return;
    }
    m_app.CallAfter([this, text] {
        if (m_app.is_closing() || m_app.plater() == nullptr) return;
        auto* notifications = m_app.plater()->get_notification_manager();
        if (notifications == nullptr) return;
        notifications->push_notification(NotificationType::CustomNotification, NotificationManager::NotificationLevel::RegularNotificationLevel, text);
    });
}

// --- ILocalStore -----------------------------------------------------------

std::optional<sync::LocalSnapshot> BblPresetSyncController::snapshot(const sync::Session& session)
{
    using Result = std::optional<sync::LocalSnapshot>;
    auto promise = std::make_shared<std::promise<Result>>();
    auto future  = promise->get_future();
    m_app.CallAfter([this, promise, session] {
        Result result;
        if (!m_app.is_closing() && m_app.user_preset_session() == session.generation && m_app.preset_bundle != nullptr &&
            !m_app.user_preset_pull_pending())
            result = sync::build_snapshot(*m_app.preset_bundle, has_bambu_id);
        promise->set_value(std::move(result));
    });
    while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
        if (cancelled()) return std::nullopt;
    try {
        return future.get();
    } catch (const std::future_error&) {
        return std::nullopt; // the queued call was dropped at shutdown
    }
}

sync::LocalOpsContext BblPresetSyncController::ops_context(const std::string& bambu_uid) const
{
    sync::LocalOpsContext ctx;
    ctx.bundle                  = m_app.preset_bundle;
    ctx.has_bambu_id            = has_bambu_id;
    ctx.backup_dir              = sync::Ledger::backup_dir(boost::filesystem::path(Slic3r::data_dir()));
    ctx.bambu_uid               = bambu_uid;
    ctx.now                     = [] { return static_cast<long long>(std::time(nullptr)); };
    ctx.queue_orca_cloud_delete = [app = &m_app](const Preset& p) { app->delete_preset_from_cloud(p); };
    return ctx;
}

void BblPresetSyncController::refresh_ui()
{
    if (m_app.mainframe != nullptr) m_app.mainframe->update_side_preset_ui();
    for (Tab* tab : m_app.tabs_list) {
        tab->reload_config();
        tab->update_changed_ui();
    }
    if (m_app.plater_ != nullptr) m_app.plater_->sidebar().update_all_preset_comboboxes();
}

void BblPresetSyncController::apply_pull(sync::PullOp op, std::function<void(sync::OpOutcome)> done)
{
    m_app.CallAfter([this, op = std::move(op), done = std::move(done)] {
        if (m_app.is_closing() || m_app.user_preset_session() != op.session.generation || m_app.preset_bundle == nullptr) {
            done(failed("session changed"));
            return;
        }
        const sync::OpOutcome outcome = sync::apply_pull(ops_context(op.session.bambu_uid), op);
        if (outcome.result == sync::OpResult::Applied) refresh_ui();
        done(outcome);
    });
}

void BblPresetSyncController::delete_local(sync::DeleteOp op, std::function<void(sync::OpOutcome)> done)
{
    m_app.CallAfter([this, op = std::move(op), done = std::move(done)] {
        if (m_app.is_closing() || m_app.user_preset_session() != op.session.generation || m_app.preset_bundle == nullptr) {
            done(failed("session changed"));
            return;
        }
        const sync::OpOutcome outcome = sync::delete_local(ops_context(op.session.bambu_uid), op);
        if (outcome.result == sync::OpResult::Applied) refresh_ui();
        done(outcome);
    });
}

// --- UI thread API ---------------------------------------------------------

std::string BblPresetSyncController::bbl_setting_id_for(const Preset& filament) const
{
    if (const auto ledger = m_mirror->ledger()) {
        const auto entry = ledger->get(sync::Key{sync::PresetType::Filament, filament.name});
        if (entry && entry->state == "linked" && !entry->setting_id.empty()) return entry->setting_id;
    }
    return filament.setting_id;
}

sync::ConvertResult BblPresetSyncController::convert(const std::string& filament_name)
{
    sync::ConvertResult result;
    NetworkAgent*       agent     = m_app.getAgent();
    const std::string   bambu_uid = agent != nullptr ? agent->get_user_id(BBL_CLOUD_PROVIDER) : std::string();
    if (m_app.preset_bundle == nullptr || bambu_uid.empty()) {
        result.outcome = failed("not signed in to Bambu");
        return result;
    }
    result = sync::convert_filament(ops_context(bambu_uid), filament_name);
    if (result.outcome.result != sync::OpResult::Applied) return result;

    const sync::Key key{sync::PresetType::Filament, filament_name};
    if (const auto ledger = m_mirror->ledger()) {
        ledger->put_conversion(key, sync::Conversion{result.from_filament_id, result.to_filament_id, {}});
        std::string error;
        if (!ledger->save(&error)) BOOST_LOG_TRIVIAL(warning) << "bbl preset sync: ledger save failed: " << error;
    }
    m_mirror->forget(key);
    refresh_ui();
    return result;
}

nlohmann::json BblPresetSyncController::status_json() const
{
    using nlohmann::json;
    auto* self = const_cast<BblPresetSyncController*>(this); // session() reads the app only

    const sync::Status status = m_mirror->status();
    json               j;
    j["last_pass"]           = status.last_pass;
    j["last_result"]         = status.last_result;
    j["notes"]               = status.notes;
    j["planned"]             = status.planned;
    j["waiting"]             = status.waiting;
    j["halted"]              = status.halted;
    j["first_run_confirmed"] = status.first_run_confirmed;
    j["linked"]              = status.linked;

    NetworkAgent* agent       = m_app.getAgent();
    j["enabled"]              = self->session().has_value();
    j["orca_signed_in"]       = agent != nullptr && agent->is_user_login() && !agent->get_user_id().empty();
    j["bambu_signed_in"]      = agent != nullptr && agent->is_user_login(BBL_CLOUD_PROVIDER) && !agent->get_user_id(BBL_CLOUD_PROVIDER).empty();

    json entries = json::array(), conversions = json::array();
    if (const auto ledger = m_mirror->ledger()) {
        for (const auto& [key, e] : ledger->entries())
            entries.push_back({{"type", sync::type_string(key.type)},
                               {"name", key.name},
                               {"state", e.state},
                               {"reason", e.reason},
                               {"has_setting_id", !e.setting_id.empty()},
                               {"update_time", e.update_time},
                               {"absent_passes", e.absent_passes},
                               {"last_error", e.last_error}});
        for (const auto& [key, c] : ledger->conversions())
            conversions.push_back({{"type", sync::type_string(key.type)},
                                   {"name", key.name},
                                   {"from_filament_id", c.from_filament_id},
                                   {"to_filament_id", c.to_filament_id},
                                   {"k_copied_printers", c.k_copied_printers}});
    }
    j["entries"]     = std::move(entries);
    j["conversions"] = std::move(conversions);

    if (wxThread::IsMain() && m_app.preset_bundle != nullptr) {
        json candidates = json::array(), ineligible = json::array(), eligible = json::array();
        const sync::LocalSnapshot snap = sync::build_snapshot(*m_app.preset_bundle, has_bambu_id);
        for (const sync::LocalItem& item : snap.items) {
            const std::string shape = item.cls.shape == sync::Shape::Root ? "root" : "child";
            json              row   = {{"type", sync::type_string(item.key.type)}, {"name", item.key.name}, {"reason", item.cls.reason}};
            if (item.cls.eligibility == sync::Eligibility::Candidate) {
                row["shape"] = shape;
                candidates.push_back(std::move(row));
            } else if (item.cls.eligibility == sync::Eligibility::Ineligible) {
                ineligible.push_back(std::move(row));
            } else {
                eligible.push_back({{"type", sync::type_string(item.key.type)}, {"name", item.key.name}, {"shape", shape}});
            }
        }
        j["eligible"]   = std::move(eligible);
        j["candidates"] = std::move(candidates);
        j["ineligible"] = std::move(ineligible);
    }
    return j;
}

} // namespace GUI
} // namespace Slic3r
