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
#include "slic3r/GUI/DeviceCore/DevDefs.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/Utils/BblPresetSync/Ledger.hpp"
#include "slic3r/Utils/BblPresetSync/KCopy.hpp"
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

#include <algorithm>
#include <chrono>
#include <ctime>
#include <future>
#include <set>
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
    k_copy_step();
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
        m_mirror->reset_candidates_prompt(); // they are offered again
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

template<class T> std::optional<T> BblPresetSyncController::run_on_ui(std::function<T()> fn)
{
    auto promise   = std::make_shared<std::promise<T>>();
    auto future    = promise->get_future();
    // Set when the caller stops waiting. The queued closure then does nothing. It is checked only when the closure
    // starts, so fn must still not rely on objects of the caller's stack: capture by value.
    auto abandoned = std::make_shared<std::atomic<bool>>(false);
    m_app.CallAfter([promise, abandoned, fn = std::move(fn)] {
        if (abandoned->load(std::memory_order_acquire)) return;
        try {
            promise->set_value(fn());
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });
    while (future.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready)
        if (cancelled()) {
            abandoned->store(true, std::memory_order_release);
            return std::nullopt;
        }
    try {
        return future.get();
    } catch (const std::future_error&) {
        return std::nullopt; // the queued call was dropped at shutdown
    }
}

std::optional<sync::LocalSnapshot> BblPresetSyncController::snapshot(const sync::Session& session)
{
    using Result = std::optional<sync::LocalSnapshot>;
    const auto result = run_on_ui<Result>([this, session]() -> Result {
        if (!m_app.is_closing() && m_app.user_preset_session() == session.generation && m_app.preset_bundle != nullptr &&
            !m_app.user_preset_pull_pending())
            return sync::build_snapshot(*m_app.preset_bundle, has_bambu_id);
        return std::nullopt;
    });
    return result ? *result : std::nullopt;
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

// --- K profile copy ---------------------------------------------------------

namespace {

struct KPrinter
{
    bool               ok{false};
    std::string        dev_id, dev_name;
    std::vector<float> diameters;
    bool               limited{true};
};

struct KJob
{
    sync::Key   key;
    std::string from, to;
    bool        manual{false};
    bool        has_conversion{false};
};

// The selected printer when it still is the one with this dev id.
MachineObject* selected_machine(const std::string& dev_id)
{
    DeviceManager* dev = wxGetApp().getDeviceManager();
    MachineObject* m   = dev != nullptr ? dev->get_selected_machine() : nullptr;
    return (m != nullptr && m->get_dev_id() == dev_id) ? m : nullptr;
}

} // namespace

void BblPresetSyncController::request_k_copy(const std::string& filament_name, const std::string& from_filament_id_override)
{
    {
        std::lock_guard<std::mutex> lock(m_k_mutex);
        m_k_requests.push_back({filament_name, from_filament_id_override});
    }
    request_sync();
}

void BblPresetSyncController::set_k_last(const std::string& key, const std::string& text)
{
    std::lock_guard<std::mutex> lock(m_k_mutex);
    m_k_last[key] = text;
}

void BblPresetSyncController::k_copy_step()
{
    if (!session()) return;
    const std::shared_ptr<sync::Ledger> ledger = m_mirror->ledger();
    if (!ledger) return;

    std::vector<KRequest> requests;
    {
        std::lock_guard<std::mutex> lock(m_k_mutex);
        requests.swap(m_k_requests);
    }
    const auto conversions = ledger->conversions();
    bool       any         = !requests.empty();
    for (const auto& [key, c] : conversions)
        if (!c.to_filament_id.empty()) any = true;
    if (!any) return;

    // The printer the K profiles can be copied on: the selected one, when it is ready.
    const auto printer = run_on_ui<KPrinter>([]() {
        KPrinter       p;
        DeviceManager* dev = wxGetApp().getDeviceManager();
        MachineObject* m   = dev != nullptr ? dev->get_selected_machine() : nullptr;
        if (m == nullptr || !m->is_connected() || !m->is_info_ready() || !m->is_support_pa_calibration) return p;
        p.ok       = true;
        p.dev_id   = m->get_dev_id();
        p.dev_name = m->get_dev_name();
        p.limited  = m->get_printer_series() != PrinterSeries::SERIES_X1;
        if (DevExtderSystem* es = m->GetExtderSystem()) {
            for (int i = 0; i < es->GetTotalExtderCount(); ++i) {
                const float d = es->GetNozzleDiameter(i);
                if (d > 0.0f && std::find(p.diameters.begin(), p.diameters.end(), d) == p.diameters.end()) p.diameters.push_back(d);
            }
        }
        return p;
    });
    if (!printer) return;
    if (!printer->ok || printer->diameters.empty()) {
        for (const KRequest& r : requests) set_k_last(sync::Key{sync::PresetType::Filament, r.name}.str(), "no printer");
        return;
    }

    // What to copy: manual requests first, then every conversion not yet copied to this printer.
    std::vector<KJob> jobs;
    std::set<std::string> manual_names;
    for (const KRequest& r : requests) {
        const sync::Key key{sync::PresetType::Filament, r.name};
        const auto      conv = ledger->get_conversion(key);
        const std::string to = run_on_ui<std::string>([this, name = r.name]() {
                                   if (m_app.preset_bundle == nullptr) return std::string();
                                   const Preset* p = m_app.preset_bundle->filaments.find_preset(name, false, true);
                                   return (p != nullptr && p->name == name && p->is_user() && p->inherits().empty()) ? p->filament_id : std::string();
                               }).value_or(std::string());
        const std::string from = !r.from.empty() ? r.from : (conv ? conv->from_filament_id : std::string());
        if (to.empty()) {
            set_k_last(key.str(), "not a standalone custom filament");
            continue;
        }
        if (from.empty()) {
            set_k_last(key.str(), "no old filament id given");
            continue;
        }
        manual_names.insert(r.name);
        jobs.push_back({key, from, to, true, conv.has_value()});
    }
    for (const auto& [key, c] : conversions) {
        if (c.to_filament_id.empty() || manual_names.count(key.name)) continue;
        if (std::find(c.k_copied_printers.begin(), c.k_copied_printers.end(), printer->dev_id) != c.k_copied_printers.end()) continue;
        {
            std::lock_guard<std::mutex> lock(m_k_mutex);
            const auto                  it = m_k_retry_after.find(printer->dev_id + "|" + key.str());
            if (it != m_k_retry_after.end() && it->second > now()) continue;
        }
        jobs.push_back({key, c.from_filament_id, c.to_filament_id, false, true});
    }

    for (const KJob& job : jobs) {
        if (cancelled()) return;
        const std::string retry_key = printer->dev_id + "|" + job.key.str();
        const auto        fail      = [&](const char* what) {
            std::lock_guard<std::mutex> lock(m_k_mutex);
            m_k_retry_after[retry_key] = now() + 600;
            m_k_last[job.key.str()]    = printer->dev_name + ": could not " + what + " the K profile table";
        };

        int copied = 0, existing = 0, full = 0;
        bool ok    = true;
        for (const float diameter : printer->diameters) {
            // a. Ask for the whole table of this nozzle.
            const auto asked = run_on_ui<int>([dev_id = printer->dev_id, diameter]() {
                MachineObject* m = selected_machine(dev_id);
                if (m == nullptr) return -2;
                PACalibExtruderInfo cali;
                cali.extruder_id            = 0;
                cali.use_extruder_id        = false;
                cali.use_nozzle_volume_type = false;
                cali.nozzle_volume_type     = nvtStandard;
                cali.nozzle_diameter        = diameter;
                cali.filament_id            = "";
                return m->command_get_pa_calibration_tab(cali);
            });
            if (!asked || *asked == -2) return; // printer gone or shutting down: end the step
            if (*asked != 0) {
                fail("read");
                ok = false;
                break;
            }

            // b. Wait for the reply.
            struct Table
            {
                bool                      gone{false}, ready{false}, success{false};
                std::vector<sync::KEntry> entries;
            };
            Table table;
            for (int waited = 0; waited <= 10000; waited += 300) {
                const auto polled = run_on_ui<Table>([dev_id = printer->dev_id]() {
                    Table t;
                    MachineObject* m = selected_machine(dev_id);
                    if (m == nullptr) {
                        t.gone = true;
                        return t;
                    }
                    t.ready   = !m->request_tab_from_bbs;
                    t.success = m->has_get_pa_calib_tab;
                    if (t.ready && t.success)
                        for (const PACalibResult& r : m->pa_calib_tab) {
                            sync::KEntry e;
                            e.filament_id        = r.filament_id;
                            e.name               = r.name;
                            e.k_value            = r.k_value;
                            e.n_coef             = r.n_coef;
                            e.nozzle_diameter    = r.nozzle_diameter;
                            e.extruder_id        = r.extruder_id;
                            e.nozzle_volume_type = static_cast<int>(r.nozzle_volume_type);
                            e.cali_idx           = r.cali_idx;
                            t.entries.push_back(std::move(e));
                        }
                    return t;
                });
                if (!polled || polled->gone) return;
                table = *polled;
                if (table.ready) break;
                sleep_ms(300);
            }
            if (!table.ready || !table.success) {
                fail("read");
                ok = false;
                break;
            }

            // c. Plan.
            const sync::KCopyPlan plan = sync::plan_k_copy(table.entries, job.from, job.to, printer->limited);
            existing += plan.skipped_existing;
            full += plan.skipped_full;

            // d. Write.
            if (plan.to_write.empty()) continue;
            std::string setting_id;
            if (const auto entry = ledger->get(job.key); entry && entry->state == "linked") setting_id = entry->setting_id;
            const auto written = run_on_ui<int>([dev_id = printer->dev_id, diameter, entries = plan.to_write, setting_id]() {
                MachineObject* m = selected_machine(dev_id);
                if (m == nullptr) return -2;
                std::vector<PACalibResult> values;
                for (const sync::KEntry& e : entries) {
                    PACalibResult r;
                    r.tray_id            = -1;
                    r.cali_idx           = -1;
                    r.name               = e.name;
                    r.k_value            = e.k_value;
                    r.n_coef             = e.n_coef;
                    r.extruder_id        = e.extruder_id;
                    r.nozzle_volume_type = static_cast<NozzleVolumeType>(e.nozzle_volume_type);
                    r.nozzle_diameter    = diameter;
                    r.filament_id        = e.filament_id;
                    r.setting_id         = setting_id;
                    values.push_back(std::move(r));
                }
                return m->command_set_pa_calibration(values, true);
            });
            if (!written || *written == -2) return;
            if (*written != 0) {
                fail("write");
                ok = false;
                break;
            }
            copied += static_cast<int>(plan.to_write.size());
        }
        if (!ok) continue;

        {
            std::lock_guard<std::mutex> lock(m_k_mutex);
            m_k_retry_after.erase(retry_key);
        }
        if (job.has_conversion) {
            if (auto conv = ledger->get_conversion(job.key)) {
                if (std::find(conv->k_copied_printers.begin(), conv->k_copied_printers.end(), printer->dev_id) == conv->k_copied_printers.end())
                    conv->k_copied_printers.push_back(printer->dev_id);
                ledger->put_conversion(job.key, *conv);
                std::string error;
                if (!ledger->save(&error)) BOOST_LOG_TRIVIAL(warning) << "bbl preset sync: ledger save failed: " << error;
            }
        }
        set_k_last(job.key.str(), printer->dev_name + ": copied " + std::to_string(copied) + ", already there " + std::to_string(existing) +
                                      ", no room " + std::to_string(full));
        if (copied > 0 || full > 0) {
            const std::string text = format(_u8L("K profiles copied to %1% on %2%: %3% (already there: %4%, no room: %5%)."), job.key.name,
                                            printer->dev_name, copied, existing, full);
            m_app.CallAfter([this, text] {
                if (m_app.is_closing() || m_app.plater() == nullptr) return;
                auto* notifications = m_app.plater()->get_notification_manager();
                if (notifications == nullptr) return;
                notifications->push_notification(NotificationType::CustomNotification,
                                                 NotificationManager::NotificationLevel::RegularNotificationLevel, text);
            });
        }
    }
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
    {
        json k_copy = json::object();
        std::lock_guard<std::mutex> lock(m_k_mutex);
        for (const auto& [key, text] : m_k_last) k_copy[key] = text;
        j["k_copy"] = std::move(k_copy);
    }

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
