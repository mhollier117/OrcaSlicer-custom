#include "FmRuntime.hpp"

#include "FmPresets.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/BBLPrinterAgent.hpp"
#include "slic3r/Utils/BblFilament/PluginGate.hpp"
#include "slic3r/Utils/BblFilament/SpoolCloud.hpp"
#include "slic3r/Utils/CloudProvider.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <wx/app.h>

#include <cstddef>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI::Fm {

namespace fil = BblFilament;

namespace {

constexpr std::size_t k_log_lines = 30;

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
    deps.now  = [] { return static_cast<long long>(std::time(nullptr)); };
    m_service = std::make_unique<fil::SpoolService>(std::move(deps));

    // Runtime owns the service's events. Each one is logged, then handed to the open panel's view-model, if any.
    fil::ServiceEvents& ev = m_service->events;
    ev.spools_changed      = [this] {
        log_event({{"event", "spools_changed"}});
        if (auto h = m_vm_handlers.spools_changed)
            h();
    };
    ev.state_changed = [this] {
        log_event({{"event", "state_changed"}});
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
