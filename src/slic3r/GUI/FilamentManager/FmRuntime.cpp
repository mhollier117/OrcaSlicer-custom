#include "FmRuntime.hpp"

#include "FmPresets.hpp"
#include "FmPrinters.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/BBLPrinterAgent.hpp"
#include "slic3r/Utils/BblFilament/PluginGate.hpp"
#include "slic3r/Utils/BblFilament/SpoolCloud.hpp"
#include "slic3r/Utils/CloudProvider.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"

#include <wx/app.h>

#include <cstddef>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI::Fm {

namespace fil = BblFilament;

namespace {

constexpr std::size_t k_log_lines          = 30;
constexpr int         k_tick_ms            = 2000;
constexpr long long   k_tap_silence_s      = 90; // a printer the tap has heard nothing from for longer is forgotten
constexpr long long   k_pushall_spacing_s  = 60; // a printer without a full state is asked for one at most this often
constexpr long long   k_list_retry_s       = 60; // the linker without a list asks for one at most this often

long long unix_now() { return static_cast<long long>(std::time(nullptr)); }

class TickTimer : public wxTimer
{
public:
    explicit TickTimer(std::function<void()> fn) : m_fn(std::move(fn)) {}
    void Notify() override
    {
        if (m_fn)
            m_fn();
    }

private:
    std::function<void()> m_fn;
};

const char* fail_name(fil::FailKind kind)
{
    switch (kind) {
    case fil::FailKind::NotSignedIn: return "not_signed_in";
    case fil::FailKind::Unavailable: return "unavailable";
    case fil::FailKind::BadRequest: return "bad_request";
    case fil::FailKind::NotFound: return "not_found";
    case fil::FailKind::Rejected: return "rejected";
    default: return "unknown";
    }
}

} // namespace

Runtime& Runtime::get()
{
    static Runtime* runtime = new Runtime;
    return *runtime;
}

Runtime::Runtime()
{
    fil::ServiceDeps deps;
    deps.cloud      = std::make_shared<fil::PluginSpoolCloud>();
    deps.run_async  = [](std::function<void()> job) { return fil::shared_worker().post(std::move(job)); };
    deps.post_to_ui = [](std::function<void()> fn) {
        if (wxTheApp != nullptr)
            wxTheApp->CallAfter(std::move(fn));
    };
    deps.session = [this] { return session(); };
    // Each lookup reads the preset bundle when it is called: the bundle pointer can change.
    auto live = [this]() -> std::optional<fil::FilamentLookup> {
        PresetBundle* bundle = wxGetApp().preset_bundle;
        if (bundle == nullptr)
            return std::nullopt;
        return filament_lookup(&bundle->filaments, &BBLPrinterAgent::bambu_filament_id, &colors());
    };
    deps.lookup.is_support = [live](const std::string& id) -> std::optional<bool> {
        auto l = live();
        if (!l || !l->is_support)
            return std::nullopt;
        return l->is_support(id);
    };
    deps.lookup.filament_type = [live](const std::string& id) -> std::string {
        auto l = live();
        if (!l || !l->filament_type)
            return {};
        return l->filament_type(id);
    };
    deps.lookup.tray_id_name = [this](const std::string& id, const std::vector<std::string>& hex, int color_type) {
        return tray_id_name_for(colors(), id, hex, color_type);
    };
    deps.now  = [] { return unix_now(); };

    // The linker uses the service's filament lookup and clock.
    fil::LinkDriverDeps link_deps;
    link_deps.lookup = deps.lookup;
    link_deps.now    = deps.now;

    m_service         = std::make_unique<fil::SpoolService>(std::move(deps));
    link_deps.service = m_service.get();
    m_tap             = std::make_unique<fil::PrinterTap>();
    m_linker          = std::make_unique<fil::LinkDriver>(std::move(link_deps));
    m_linker->on_auto_push = [this](const fil::AutoPushSummary& summary) {
        log_event({{"event", "auto_push"}, {"pushed", summary.pushed}, {"skipped_cooldown", summary.skipped_cooldown}, {"busy", summary.busy}});
        if (m_vm != nullptr)
            m_vm->auto_push_summary(summary);
    };
    m_linker->on_registered = [this](const fil::SlotKey& slot, const std::string&) {
        log_event({{"event", "registered"}});
        m_new_spools.insert(slot);
    };

    // Runtime owns the service's events. Each one is logged, then handed to the open panel's view-model, if any.
    fil::ServiceEvents& ev = m_service->events;
    ev.spools_changed      = [this] {
        log_event({{"event", "spools_changed"}});
        note_list_state();
        if (auto h = m_vm_handlers.spools_changed)
            h();
    };
    ev.state_changed = [this] {
        log_event({{"event", "state_changed"}});
        note_list_state();
        if (auto h = m_vm_handlers.state_changed)
            h();
    };
    ev.pull_done = [this](int added, int kept) {
        log_event({{"event", "pull_done"}, {"added", added}, {"kept", kept}});
        if (auto h = m_vm_handlers.pull_done)
            h(added, kept);
    };
    ev.push_done = [this](const std::string& op, const std::string& spool_id) {
        log_event({{"event", "push_done"}, {"op", op}, {"spool_id", spool_id}});
        if (auto h = m_vm_handlers.push_done)
            h(op, spool_id);
    };
    ev.push_failed = [this](const std::string& op, const std::string& spool_id, fil::FailKind kind, const std::string& message) {
        log_event({{"event", "push_failed"}, {"op", op}, {"spool_id", spool_id}, {"kind", fail_name(kind)}, {"message", message}});
        if (auto h = m_vm_handlers.push_failed)
            h(op, spool_id, kind, message);
    };
    ev.config_fetched = [this](const json& config) {
        log_event({{"event", "config_fetched"}});
        if (auto h = m_vm_handlers.config_fetched)
            h(config);
    };
}

void Runtime::log_event(json line)
{
    line["t"] = static_cast<long long>(std::time(nullptr));
    m_log.push_back(std::move(line));
    while (m_log.size() > k_log_lines)
        m_log.pop_front();
}

fil::SpoolService& Runtime::service() { return *m_service; }
fil::PrinterTap&   Runtime::tap() { return *m_tap; }
fil::LinkDriver&   Runtime::linker() { return *m_linker; }

void Runtime::note_list_state()
{
    // The list is gone (signed out, another account, the plugin reloaded): the linker forgets everything.
    const bool have = m_service->status().have_list;
    if (m_had_list && !have)
        m_linker->reset();
    m_had_list = have;
}

void Runtime::start()
{
    if (!m_timer)
        m_timer = std::make_unique<TickTimer>([this] { tick(); });
    if (!m_timer->IsRunning())
        m_timer->Start(k_tick_ms);
}

std::vector<fil::PrinterView> Runtime::views() { return printer_views(wxGetApp().getDeviceManager(), *m_tap, unix_now()); }

void Runtime::tick()
{
    try {
        const fil::Session s = session();
        if (s.closing) {
            m_timer->Stop();
            return;
        }
        if (!s.signed_in)
            return;
        AppConfig* config = wxGetApp().app_config;
        if (config == nullptr)
            return;
        m_linker->config = link_config(config->get_bool("filament_manager_link_enabled"), config->get_bool("filament_manager_link_dry_run"),
                                       config->get("filament_manager_link_only"));
        const long long now = unix_now();

        // The linker needs the account's list also while the Filament Manager tab is closed.
        if (m_linker->config.mode != fil::LinkMode::Off) {
            const bool changed = m_service->refresh_session();
            if (changed || (!m_service->status().have_list && !m_service->state().is_syncing && now - m_pull_asked_at >= k_list_retry_s)) {
                m_pull_asked_at = now;
                m_service->request_pull();
            }
        }

        for (const std::string& dev : m_tap->printers()) {
            const fil::PrinterFacts* f = m_tap->facts(dev);
            if (f != nullptr && now - f->last_message_at > k_tap_silence_s) {
                m_tap->forget(dev);
                m_pushall_asked_at.erase(dev);
            }
        }

        DeviceManager* devices = wxGetApp().getDeviceManager();
        if (devices != nullptr) {
            for (const auto& [dev, machine] : devices->get_my_cloud_machine_list(BBL_PRINTER_AGENT_ID)) {
                if (machine == nullptr || !machine->is_online())
                    continue;
                const fil::PrinterFacts* f = m_tap->facts(dev);
                if (f == nullptr || f->has_full)
                    continue;
                long long& asked = m_pushall_asked_at[dev];
                if (asked != 0 && now - asked < k_pushall_spacing_s)
                    continue;
                asked = now;
                machine->command_request_push_all(); // its own 3-second throttle applies
            }
        }

        m_linker->tick(printer_views(devices, *m_tap, now));
    } catch (...) {
    }
}

fil::Accept Runtime::request_soft_match(const fil::PrinterView& printer, int ams_id)
{
    auto sn = printer.ams_sn.find(ams_id);
    if (sn == printer.ams_sn.end() || sn->second.empty())
        return {false, fil::FailKind::BadRequest};
    fil::SpoolService::LinkRequest req;
    req.kind              = fil::SpoolService::LinkRequest::Kind::SoftMatchRead;
    req.dev_id            = printer.dev_id;
    req.ams_sn            = sn->second;
    const std::string name = printer.name;
    req.done = [this, name, ams_id](const fil::SpoolService::LinkOutcome& out) {
        // Brand, name, colour and the number of candidates; never an RFID.
        json j = {{"printer", name}, {"ams_id", ams_id}, {"at", unix_now()}, {"ok", out.ok}, {"kind", out.ok ? "" : fail_name(out.kind)}};
        if (out.ok) {
            const fil::SoftMatchQueue queue = fil::parse_soft_match(out.body);
            json                      items = json::array();
            for (const fil::SoftMatchEntry& e : queue.entries)
                items.push_back({{"brand", e.hit.vendor}, {"name", e.hit.name}, {"color", e.hit.color}, {"candidates", e.candidates.size()}});
            j["entries"] = queue.entries.size();
            j["items"]   = std::move(items);
        }
        m_soft_match = std::move(j);
    };
    return m_service->link(std::move(req));
}

const ColorCodes& Runtime::colors()
{
    if (!m_colors)
        m_colors = std::make_unique<ColorCodes>(ColorCodes::load_file(ColorCodes::default_path()));
    return *m_colors;
}

fil::Session Runtime::session() const
{
    fil::Session s;
    AppConfig*   config = wxGetApp().app_config;
    NetworkAgent* agent = wxGetApp().getAgent();
    s.signed_in = config != nullptr && !config->get_stealth_mode() && agent != nullptr && agent->is_user_login(BBL_CLOUD_PROVIDER);
    if (s.signed_in)
        s.uid = agent->get_user_id(BBL_CLOUD_PROVIDER);
    s.generation = fil::plugin_generation();
    s.closing    = wxGetApp().is_closing();
    return s;
}

std::string Runtime::lang() const { return std::string(wxGetApp().current_language_code_safe().ToUTF8().data()); }

void Runtime::attach(ViewModel* vm)
{
    m_vm          = vm;
    m_vm_handlers = vm != nullptr ? vm->event_handlers() : fil::ServiceEvents{};
}

void Runtime::detach(ViewModel* vm)
{
    if (m_vm != vm)
        return;
    m_vm = nullptr;
    m_vm_handlers = {};
}

json Runtime::status_json()
{
    const fil::ServiceStatus st = m_service->status();
    json                     recent = json::array();
    for (const json& line : m_log)
        recent.push_back(line);
    return {{"signed_in", st.state.signed_in},
            {"have_list", st.have_list},
            {"spool_count", st.spool_count},
            {"pending_writes", st.pending_writes},
            {"pull_wanted", st.pull_wanted},
            {"is_syncing", st.state.is_syncing},
            {"is_pulling", st.state.is_pulling},
            {"last_synced_at", st.state.last_synced_at},
            {"error", {{"kind", st.state.has_error ? fail_name(st.state.error_kind) : ""}, {"message", st.state.has_error ? st.state.error_message : std::string()}}},
            {"paging_suspect", st.paging_suspect},
            {"skipped_rows", st.skipped_rows},
            {"plugin_generation", fil::plugin_generation()},
            {"worker_accepting", fil::shared_worker().accepting()},
            {"recent", std::move(recent)}};
}

} // namespace Slic3r::GUI::Fm
