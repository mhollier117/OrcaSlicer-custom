#include "FmPrinters.hpp"

#include "FmRuntime.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/DeviceCore/DevDefs.h"
#include "slic3r/GUI/DeviceCore/DevFirmware.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/BBLPrinterAgent.hpp"
#include "slic3r/Utils/CloudProvider.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"

#include <cstdlib>
#include <ctime>
#include <exception>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace Slic3r::GUI::Fm {

namespace fil = BblFilament;

namespace {

constexpr long long k_fresh_seconds = 60; // a printer is fresh when its last message is at most this old

bool is_bambu(const MachineObject& m) { return m.printer_agent_id == BBL_PRINTER_AGENT_ID; }

// What Bambu Studio counts as busy for the weight cooldown (compute_device_state), as far as this code base can tell
// without side effects: printing (calibrations run as print jobs), or the AMS doing anything but idling.
bool is_busy(MachineObject& m)
{
    if (m.is_in_printing())
        return true;
    return m.ams_status_main != AMS_STATUS_MAIN_IDLE && m.ams_status_main != AMS_STATUS_MAIN_UNKNOWN;
}

// DevFilaSystemParser::ParseV1_0 (DevFilaSystem.cpp:499) takes the units of an "f1" printer without the new protocol
// for AMS Lite; the tap cannot know the model.
bool has_ams_lite(const MachineObject& m) { return !m.is_enable_ams_np && m.get_printer_ams_type() == "f1"; }

fil::PrinterView view_of(DeviceManager& devices, MachineObject& m, MachineObject* selected, const fil::PrinterTap& tap, long long now)
{
    fil::PrinterView v;
    v.dev_id   = m.get_dev_id();
    v.name     = m.get_dev_name();
    v.bound    = devices.get_user_machine(v.dev_id, BBL_CLOUD_PROVIDER) != nullptr;
    v.lan_only = m.is_lan_mode_printer();
    v.selected = selected == &m;
    v.busy     = is_busy(m);
    if (const fil::PrinterFacts* f = tap.facts(v.dev_id)) {
        v.fresh        = m.is_online() && f->has_full && now - f->last_message_at <= k_fresh_seconds;
        v.observations = f->observations;
        v.trays        = f->trays;
    }
    for (const auto& [ams_id, info] : m.get_ams_version())
        if (!info.sn.empty())
            v.ams_sn[ams_id] = info.sn;
    const bool lite = has_ams_lite(m);
    for (fil::TrayFacts& t : v.trays) {
        if (!t.filament_id.empty())
            t.filament_id = BBLPrinterAgent::bambu_filament_id(t.filament_id);
        if (lite && !t.external)
            t.ams_type = 2;
    }
    return v;
}

} // namespace

bool tap_enabled()
{
    const AppConfig* config = wxGetApp().app_config;
    return config != nullptr && (config->get_bool("filament_manager_enabled") || config->get_bool("sync_bbl_user_preset"));
}

void on_printer_message(MachineObject& printer, const std::string& message)
{
    try {
        if (!tap_enabled() || !is_bambu(printer))
            return;
        Runtime::get().tap().on_message(printer.get_dev_id(), message, static_cast<long long>(std::time(nullptr)));
    } catch (...) {
    }
}

std::map<std::string, MachineObject*> account_printers(DeviceManager& devices) { return devices.get_my_cloud_machine_list(BBL_PRINTER_AGENT_ID); }

MachineObject* find_printer(DeviceManager& devices, const std::string& dev_id)
{
    if (dev_id.empty())
        return nullptr;
    if (MachineObject* selected = devices.get_selected_machine(); selected != nullptr && selected->get_dev_id() == dev_id)
        return selected;
    if (MachineObject* m = devices.get_my_machine(dev_id))
        return m;
    if (MachineObject* m = devices.get_user_machine(dev_id, BBL_CLOUD_PROVIDER))
        return m;
    return devices.get_local_machine(dev_id);
}

std::vector<fil::PrinterView> printer_views(DeviceManager* devices, const fil::PrinterTap& tap, long long now)
{
    std::vector<fil::PrinterView> out;
    if (devices == nullptr)
        return out;
    try {
        std::map<std::string, MachineObject*> machines = account_printers(*devices);
        MachineObject*                        selected = devices->get_selected_machine();
        if (selected != nullptr && is_bambu(*selected) && selected->is_lan_mode_printer())
            machines.emplace(selected->get_dev_id(), selected);
        for (const auto& entry : machines)
            if (entry.second != nullptr)
                out.push_back(view_of(*devices, *entry.second, selected, tap, now));
    } catch (const std::exception&) {
        out.clear();
    }
    return out;
}

std::optional<fil::LinkMode> parse_link_mode(const std::string& text)
{
    if (text == "off")
        return fil::LinkMode::Off;
    if (text == "dry_run")
        return fil::LinkMode::DryRun;
    if (text == "on")
        return fil::LinkMode::On;
    return std::nullopt;
}

const char* link_mode_text(fil::LinkMode mode)
{
    switch (mode) {
    case fil::LinkMode::Off: return "off";
    case fil::LinkMode::DryRun: return "dry_run";
    default: return "on";
    }
}

fil::LinkConfig link_config(bool enabled, const std::string& env, const std::optional<fil::LinkMode>& session_mode, const std::set<std::string>& session_only)
{
    fil::LinkConfig c;
    if (session_mode)
        c.mode = *session_mode;
    else if (const std::optional<fil::LinkMode> from_env = parse_link_mode(env))
        c.mode = *from_env;
    else
        c.mode = enabled ? fil::LinkMode::On : fil::LinkMode::Off;
    c.only_slots = session_only;
    return c;
}

const char* link_mode_source(const std::string& env, const std::optional<fil::LinkMode>& session_mode)
{
    if (session_mode)
        return "session";
    return parse_link_mode(env) ? "env" : "config";
}

const std::string& link_mode_env()
{
    static const std::string value = [] {
        const char* v = std::getenv("ORCA_FM_LINK_MODE");
        return std::string(v != nullptr ? v : "");
    }();
    return value;
}

} // namespace Slic3r::GUI::Fm
