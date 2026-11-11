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

#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <map>
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

void on_printer_message(MachineObject& printer, const std::string& message)
{
    try {
        AppConfig* config = wxGetApp().app_config;
        if (config == nullptr || !config->get_bool("filament_manager_enabled") || !is_bambu(printer))
            return;
        Runtime::get().tap().on_message(printer.get_dev_id(), message, static_cast<long long>(std::time(nullptr)));
    } catch (...) {
    }
}

std::map<std::string, MachineObject*> account_printers(DeviceManager& devices) { return devices.get_my_cloud_machine_list(BBL_PRINTER_AGENT_ID); }

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

std::set<std::string> parse_only_slots(const std::string& only)
{
    std::set<std::string> out;
    std::size_t           begin = 0;
    while (begin <= only.size()) {
        std::size_t end = only.find(',', begin);
        if (end == std::string::npos)
            end = only.size();
        std::string item = only.substr(begin, end - begin);
        const auto  first = item.find_first_not_of(" \t");
        const auto  last  = item.find_last_not_of(" \t");
        if (first != std::string::npos)
            out.insert(item.substr(first, last - first + 1));
        begin = end + 1;
    }
    return out;
}

std::string join_only_slots(const std::set<std::string>& slots)
{
    std::string out;
    for (const std::string& s : slots) {
        if (!out.empty())
            out += ",";
        out += s;
    }
    return out;
}

bool is_link_mode_override(const std::string& mode_override)
{
    return mode_override == "off" || mode_override == "dry_run" || mode_override == "on";
}

fil::LinkConfig link_config(bool enabled, bool dry_run, const std::string& only, const std::string& mode_override)
{
    if (is_link_mode_override(mode_override)) {
        enabled = mode_override != "off";
        dry_run = mode_override == "dry_run";
    }
    fil::LinkConfig c;
    c.mode       = !enabled ? fil::LinkMode::Off : dry_run ? fil::LinkMode::DryRun : fil::LinkMode::On;
    c.only_slots = parse_only_slots(only);
    return c;
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
