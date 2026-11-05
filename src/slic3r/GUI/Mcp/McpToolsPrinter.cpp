// Printer MCP tools (fork): printer_list, printer_status, printer_job, printer_hms, printer_account.
// Design: C:/claude-code/orca_mcp/docs/specs/2026-11-02-printer-mcp-design.md

#include "McpPrinterStatus.hpp"

#include "McpPrinterCommon.hpp"
#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "slic3r/GUI/DeviceCore/DevBed.h"
#include "slic3r/GUI/DeviceCore/DevChamber.h"
#include "slic3r/GUI/DeviceCore/DevConfig.h"
#include "slic3r/GUI/DeviceCore/DevConfigUtil.h"
#include "slic3r/GUI/DeviceCore/DevDefs.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevFan.h"
#include "slic3r/GUI/DeviceCore/DevFilaSwitch.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevFirmware.h"
#include "slic3r/GUI/DeviceCore/DevHMS.h"
#include "slic3r/GUI/DeviceCore/DevLamp.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceCore/DevNozzleRack.h"
#include "slic3r/GUI/DeviceCore/DevNozzleSystem.h"
#include "slic3r/GUI/DeviceCore/DevStorage.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/HMS.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <boost/algorithm/string/case_conv.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

std::string utf8(const wxString& s) { return std::string(s.ToUTF8().data()); }

double round4(double v) { return std::round(v * 10000.0) / 10000.0; }

json color_json(const std::string& c)
{
    if (c.empty())
        return nullptr;
    return "#" + boost::algorithm::to_upper_copy(c);
}

json opt_int(const std::string& s)
{
    if (s.empty())
        return nullptr;
    try {
        size_t pos = 0;
        const int v = std::stoi(s, &pos);
        return v;
    } catch (...) {
        return nullptr;
    }
}

const char* hms_level_name(HMSMessageLevel level)
{
    switch (level) {
    case HMS_FATAL: return "fatal";
    case HMS_SERIOUS: return "serious";
    case HMS_COMMON: return "common";
    case HMS_INFO: return "info";
    default: return "unknown";
    }
}

std::string ams_type_name(const DevAms& ams)
{
    if (ams.IsAmsLiteMixed())
        return "ams_lite_mixed";
    switch (ams.GetAmsType()) {
    case DevAms::AMS: return "ams";
    case DevAms::AMS_LITE: return "ams_lite";
    case DevAms::N3F: return "ams_2_pro";
    case DevAms::N3S: return "ams_ht";
    default: return "unknown";
    }
}

const char* nozzle_type_name(NozzleType t)
{
    switch (t) {
    case ntHardenedSteel: return "hardened_steel";
    case ntStainlessSteel: return "stainless_steel";
    case ntTungstenCarbide: return "tungsten_carbide";
    case ntBrass: return "brass";
    case ntE3D: return "e3d";
    default: return "unknown";
    }
}

const char* sdcard_state_name(DevStorage::SdcardState s)
{
    switch (s) {
    case DevStorage::NO_SDCARD: return "none";
    case DevStorage::HAS_SDCARD_NORMAL: return "normal";
    case DevStorage::HAS_SDCARD_ABNORMAL: return "abnormal";
    case DevStorage::HAS_SDCARD_READONLY: return "read_only";
    default: return "unknown";
    }
}

const char* speed_level_name(int level)
{
    switch (level) {
    case SPEED_LEVEL_SILENCE: return "silent";
    case SPEED_LEVEL_NORMAL: return "standard";
    case SPEED_LEVEL_RAPID: return "sport";
    case SPEED_LEVEL_RAMPAGE: return "ludicrous";
    default: return "unknown";
    }
}

std::string hex8(int v)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08X", static_cast<unsigned>(v));
    return buf;
}

// One tray / external spool. `lite_extra` fields are added by the caller.
json tray_fields(const DevAmsTray& t, bool exists)
{
    json colors = json::array();
    for (const std::string& c : t.cols)
        colors.push_back(color_json(c));
    json j;
    j["exists"]          = exists;
    j["ready"]           = t.is_tray_info_ready();
    j["empty"]           = !exists;
    j["placeholder"]     = t.is_slot_placeholder;
    j["type"]            = t.m_fila_type;
    j["sub_brand"]       = t.sub_brands;
    j["tray_info_idx"]   = t.setting_id;
    j["color"]           = color_json(t.color);
    j["colors"]          = std::move(colors);
    j["color_type"]      = t.ctype;
    j["remain"]          = t.remain >= 0 ? json(t.remain) : json(nullptr);
    j["k"]               = round4(t.k);
    j["n"]               = round4(t.n);
    j["cali_idx"]        = t.cali_idx;
    j["nozzle_temp_min"] = opt_int(t.nozzle_temp_min);
    j["nozzle_temp_max"] = opt_int(t.nozzle_temp_max);
    return j;
}

json section_print(MachineObject& m)
{
    json j;
    j["print_status"]    = m.print_status;
    j["stage"]           = {{"number", m.mc_print_stage}, {"current", m.stage_curr}, {"text", utf8(m.get_curr_stage())}};
    j["progress"]        = m.mc_print_percent;
    j["remaining_min"]   = m.mc_left_time / 60; // mc_left_time is seconds (DeviceManager.cpp: mc_remaining_time * 60)
    j["layer"]           = m.curr_layer;
    j["total_layers"]    = m.total_layers;
    j["subtask_name"]    = m.subtask_name;
    j["job_id"]          = m.job_id_;
    j["task_id"]         = m.task_id_;
    j["print_error"]     = {{"code", m.print_error}, {"hex", hex8(m.print_error)}, {"text", m.get_print_error_str()}};
    const int speed      = static_cast<int>(m.GetPrintingSpeedLevel());
    j["speed_level"]     = {{"level", speed}, {"name", speed_level_name(speed)}, {"percent", m.printing_speed_mag}};
    j["can_pause"]       = m.can_pause();
    j["can_resume"]      = m.can_resume();
    j["can_abort"]       = m.can_abort();
    j["is_in_printing"]  = m.is_in_printing();
    j["paused"]          = m.is_in_printing_pause();
    j["skipped_object_ids"] = m.m_partskip_ids;
    j["supports_skip_objects"] = m.is_support_partskip;
    return j;
}

json section_temps(MachineObject& m)
{
    json j;
    json ext = json::array();
    if (DevExtderSystem* sys = m.GetExtderSystem()) {
        for (const DevExtder& e : sys->GetExtruders())
            ext.push_back({{"id", e.GetExtId()},
                           {"side", printer_side_name(m, e.GetExtId())},
                           {"current", e.GetCurrentTemp()},
                           {"target", e.GetTargetTemp()}});
        j["active_extruder"] = printer_side_name(m, sys->GetCurrentExtderId());
    }
    j["extruders"] = std::move(ext);
    if (DevBed* bed = m.GetBed())
        j["bed"] = {{"current", round4(bed->GetBedTemp())}, {"target", round4(bed->GetBedTempTarget())}};
    else
        j["bed"] = nullptr;
    if (std::shared_ptr<DevChamber> ch = m.GetChamber()) {
        const bool supported = ch->HasChamber();
        j["chamber"] = {{"supported", supported},
                        {"current", supported ? json(round4(ch->GetChamberTemp())) : json(nullptr)},
                        {"target", supported ? json(round4(ch->GetChamberTempTarget())) : json(nullptr)}};
    } else {
        j["chamber"] = nullptr;
    }
    return j;
}

int fan_percent(int raw) { return static_cast<int>(std::round(raw / 25.5)) * 10; } // FanControl.cpp:748 raw 0..255 -> gear 0..10

json section_fans(MachineObject& m)
{
    DevFan* fan = m.GetFan();
    if (fan == nullptr)
        return nullptr;
    auto entry = [](int raw) { return json{{"raw", raw}, {"percent", fan_percent(raw)}}; };
    json j;
    j["part_cooling"] = entry(fan->GetCoolingFanSpeed());
    j["aux"]          = entry(fan->GetBigFan1Speed());
    j["chamber"]      = entry(fan->GetBigFan2Speed());
    j["heatbreak"]    = fan->GetHeatBreakFanSpeed();
    const AirDuctData duct = fan->GetAirDuctData();
    if (fan->GetSupportAirduct()) {
        json parts = json::array();
        for (const AirParts& p : duct.parts)
            parts.push_back({{"id", p.id}, {"func", p.func}, {"state_percent", p.state}});
        j["air_duct"] = {{"mode", duct.curren_mode}, {"sub_mode", duct.m_sub_mode}, {"parts", std::move(parts)}};
    }
    return j;
}

json section_lights(MachineObject& m)
{
    DevLamp* lamp = m.GetLamp();
    if (lamp == nullptr)
        return nullptr;
    // toolhead_logo is not reported by the printer: it is what Orca last sent
    // during the current job (a new print's start G-code turns it back on).
    return {{"chamber_light", lamp->IsChamberLightOn() ? "on" : "off"},
            {"toolhead_logo", lamp->IsToolheadLogoOn() ? "on" : "off"}};
}

json section_ams(MachineObject& m)
{
    std::shared_ptr<DevFilaSystem> fila = m.GetFilaSystem();
    if (!fila)
        return nullptr;
    json units = json::array();
    for (auto& [id, ams_ptr] : fila->GetAmsList()) {
        const DevAms* ams = ams_ptr;
        if (ams == nullptr)
            continue;
        const int  ams_id   = std::atoi(ams->GetAmsId().c_str());
        const bool mixed    = ams->IsAmsLiteMixed();
        const bool single   = ams->GetAmsType() == DevAms::N3S;
        json       bound    = json::array();
        for (int e : ams->GetBindedExtruderSet())
            bound.push_back(printer_side_name(m, e));
        json unit;
        unit["ams_id"]           = ams_id;
        unit["type"]             = ams_type_name(*ams);
        unit["display_name"]     = utf8(ams->GetDisplayName());
        unit["extruder"]         = ams->GetBindedExtruderSet().size() > 1 ? std::string("switch") : printer_side_name(m, ams->GetExtruderId());
        unit["bound_extruders"]  = std::move(bound);
        unit["exists"]           = ams->IsExist();
        unit["humidity_level"]   = ams->SupportHumidity() ? json(ams->GetHumidityLevel()) : json(nullptr);
        unit["humidity_percent"] = (ams->SupportHumidity() && ams->GetHumidityPercent() >= 0) ? json(ams->GetHumidityPercent()) : json(nullptr);
        unit["temperature"]      = ams->GetCurrentTemperature() == INVALID_AMS_TEMPERATURE ? json(nullptr) : json(round4(ams->GetCurrentTemperature()));
        if (ams->SupportDrying())
            unit["drying"] = {{"drying", const_cast<DevAms*>(ams)->AmsIsDrying()}, {"left_min", ams->GetLeftDryTime()}};
        json trays = json::array();
        for (auto& [tid, tray] : ams->GetTrays()) {
            if (tray == nullptr)
                continue;
            const int slot = std::atoi(tid.c_str());
            json      t    = tray_fields(*tray, tray->is_exists);
            t["slot_id"]    = slot;
            t["tray_index"] = printer_tray_index(mixed, single, ams_id, slot);
            trays.push_back(std::move(t));
        }
        unit["trays"] = std::move(trays);
        units.push_back(std::move(unit));
    }
    json ext = json::array();
    for (const DevAmsTray& t : m.vt_slot) {
        const int id = std::atoi(t.id.c_str());
        json      e  = tray_fields(t, t.is_tray_info_ready()); // external spools never get is_exists
        e["id"]         = id;
        e["tray_index"] = id;
        e["side"]       = printer_side_name(m, id == VIRTUAL_TRAY_MAIN_ID ? MAIN_EXTRUDER_ID : DEPUTY_EXTRUDER_ID); // DevMapping.cpp:196-197
        ext.push_back(std::move(e));
    }
    json current = json::array();
    if (DevExtderSystem* sys = m.GetExtderSystem())
        for (const DevExtder& e : sys->GetExtruders())
            current.push_back({{"side", printer_side_name(m, e.GetExtId())},
                               {"ams_id", e.GetSlotNow().ams_id},
                               {"slot_id", e.GetSlotNow().slot_id},
                               {"target_ams_id", e.GetSlotTarget().ams_id},
                               {"target_slot_id", e.GetSlotTarget().slot_id},
                               {"has_filament_in_extruder", e.HasFilamentInExt()}});
    return {{"units", std::move(units)}, {"external_spools", std::move(ext)}, {"current_tray", std::move(current)}};
}

json nozzle_json(const DevNozzle& n)
{
    return {{"id", n.GetNozzleId()},
            {"diameter", n.GetNozzleDiameter() > 0.f ? json(round4(n.GetNozzleDiameter())) : json(nullptr)},
            {"flow_type", DevNozzle::GetNozzleFlowTypeString(n.GetNozzleFlowType())},
            {"type", nozzle_type_name(n.GetNozzleType())},
            {"wear", round4(n.GetNozzleWear())}};
}

json section_nozzles(MachineObject& m)
{
    json j;
    json ext = json::array();
    if (DevExtderSystem* sys = m.GetExtderSystem())
        for (const DevExtder& e : sys->GetExtruders()) {
            json n = {{"side", printer_side_name(m, e.GetExtId())},
                      {"diameter", e.GetNozzleDiameter() > 0.f ? json(round4(e.GetNozzleDiameter())) : json(nullptr)},
                      {"flow_type", DevNozzle::GetNozzleFlowTypeString(e.GetNozzleFlowType())},
                      {"type", nozzle_type_name(e.GetNozzleType())},
                      {"hardened", e.GetNozzleType() == ntHardenedSteel || e.GetNozzleType() == ntTungstenCarbide}};
            ext.push_back(std::move(n));
        }
    j["extruders"] = std::move(ext);
    json rack = {{"supported", false}, {"nozzles", json::array()}};
    if (DevNozzleSystem* sys = m.GetNozzleSystem()) {
        if (std::shared_ptr<DevNozzleRack> r = sys->GetNozzleRack(); r && r->IsSupported()) {
            rack["supported"] = true;
            for (auto& [idx, n] : r->GetRackNozzles())
                rack["nozzles"].push_back(nozzle_json(n));
        }
    }
    j["rack"] = std::move(rack);
    if (DevFilaSwitch* sw = m.GetFilaSwitch())
        j["filament_switch"] = {{"installed", sw->IsInstalled()}, {"ready", sw->IsInstalled() && sw->IsReady()}};
    else
        j["filament_switch"] = nullptr;
    return j;
}

json section_hms(MachineObject& m, const HmsTextFn& hms_text)
{
    DevHMS* hms = m.GetHMS();
    if (hms == nullptr)
        return nullptr;
    json items = json::array();
    for (const DevHMSItem& it : hms->GetHMSItems()) {
        const std::string code = it.get_long_error_code();
        std::string       disp;
        for (size_t i = 0; i < code.size(); ++i) {
            if (i > 0 && i % 4 == 0)
                disp += '_';
            disp += code[i];
        }
        items.push_back({{"code", code},
                         {"display_code", disp},
                         {"level", hms_level_name(it.get_level())},
                         {"text", hms_text ? hms_text(code) : std::string()},
                         {"read", it.has_read()}});
    }
    return items;
}

json section_storage(MachineObject& m)
{
    DevStorage* st = m.GetStorage();
    if (st == nullptr)
        return nullptr;
    return {{"sd_card", sdcard_state_name(st->get_sdcard_state())}, {"emmc_print", m.is_support_print_with_emmc}};
}

json section_capabilities(MachineObject& m)
{
    json j;
    j["is_multi_extruders"]           = m.is_multi_extruders();
    j["extruder_count"]               = m.GetExtderSystem() ? m.GetExtderSystem()->GetTotalExtderSize() : 1;
    j["max_filament_color_count"]     = m.get_max_filament_color_count();
    j["pa_mode"]                      = m.is_support_pa_mode;
    j["bed_leveling"]                 = m.is_support_bed_leveling;
    j["internal_timelapse"]           = m.is_support_internal_timelapse;
    j["ext_change_assist"]            = m.is_support_ext_change_assist;
    j["timelapse"]                    = m.is_support_timelapse;
    j["flow_calibration"]             = m.is_support_flow_calibration;
    j["pa_calibration"]               = m.is_support_pa_calibration;
    j["filament_backup"]              = m.is_support_filament_backup;
    j["ams_humidity"]                 = m.is_support_ams_humidity;
    // print.flag3 bit 3: the printer accepts AMS slot edits during a print (AMSMaterialsSetting gates on it)
    j["filament_setting_in_print"]    = m.is_support_filament_setting_inprinting;
    j["filament_setting_in_print_allowed"] = m.can_edit_filament_while_printing(); // advertised, or allowed by the app config
    j["send_to_sdcard"]               = m.is_support_send_to_sdcard;
    j["skip_objects"]                 = m.is_support_partskip;
    j["remote_drying"]                = m.is_support_remote_dry;
    try {
        j["ams_ext_mix_print"] = DevPrinterConfigUtil::support_ams_ext_mix_print(m.printer_type);
    } catch (...) {
        j["ams_ext_mix_print"] = nullptr;
    }
    if (DevConfig* cfg = m.GetConfig()) {
        j["print_all_plates"]    = cfg->SupportPrintAllPlates();
        j["print_without_sd"]    = cfg->SupportPrintWithoutSD();
        j["first_layer_inspect"] = cfg->SupportFirstLayerInspect();
        j["ai_monitor"]          = cfg->SupportAIMonitor();
    }
    bool rack = false;
    if (DevNozzleSystem* sys = m.GetNozzleSystem())
        if (std::shared_ptr<DevNozzleRack> r = sys->GetNozzleRack())
            rack = r->IsSupported();
    j["nozzle_rack"] = rack;
    j["filament_switch"] = m.GetFilaSwitch() ? m.GetFilaSwitch()->IsInstalled() : false;
    return j;
}

json section_firmware(MachineObject& m)
{
    json modules = json::array();
    for (const auto& [name, v] : m.module_vers) // serial numbers are left out
        modules.push_back({{"name", name}, {"sw_ver", v.sw_ver}, {"hw_ver", v.hw_ver}});
    json updates = json::array();
    for (const auto& [name, v] : m.new_ver_list)
        updates.push_back({{"name", name}, {"sw_new_ver", v.sw_new_ver}});
    return {{"modules", std::move(modules)},
            {"upgrade_available", m.is_upgrading_avalable()},
            {"new_versions", std::move(updates)},
            {"ota_new_version", m.ota_new_version_number}};
}

} // namespace

const std::vector<std::string>& printer_status_section_names()
{
    static const std::vector<std::string> names = {"print", "temps", "fans", "lights", "ams", "nozzles", "hms", "storage", "capabilities", "firmware"};
    return names;
}

std::string printer_side_name(const MachineObject& m, int extruder_id)
{
    if (!m.is_multi_extruders())
        return "main";
    if (extruder_id == MAIN_EXTRUDER_ID)
        return "right";
    if (extruder_id == DEPUTY_EXTRUDER_ID)
        return "left";
    return "switch";
}

int printer_tray_index(bool lite_mixed, bool single_slot_unit, int ams_id, int slot_id)
{
    // mirrors DevMapping.cpp:95-153 ams_filament_mapping / _parse_tray_info
    if (lite_mixed)
        return AMS_LITE_MIXED_TRAY_INDEX_OFFSET + slot_id;
    if (single_slot_unit)
        return ams_id + slot_id;
    return ams_id * 4 + slot_id;
}

json printer_summary_json(MachineObject& m, bool selected)
{
    std::string model = DevPrinterConfigUtil::get_printer_display_name(m.printer_type);
    if (model.empty())
        model = m.printer_type;
    json j;
    j["dev_id"]           = m.get_dev_id();
    j["name"]             = m.get_dev_name();
    j["model"]            = model;
    j["printer_type"]     = m.printer_type;
    j["connection"]       = m.is_lan_mode_printer() ? "lan" : "cloud";
    j["online"]           = m.is_online();
    j["connected"]        = m.is_connected();
    j["selected"]         = selected;
    j["info_ready"]       = m.is_info_ready();
    j["has_access_code"]  = !m.get_access_code().empty();
    j["ip"]               = m.get_dev_ip();
    j["printing"]         = m.is_in_printing();
    j["print_status"]     = m.print_status;
    j["progress"]         = m.mc_print_percent;
    j["printer_agent_id"] = m.printer_agent_id;
    return redact(std::move(j));
}

json printer_status_json(MachineObject& m, const std::vector<std::string>& sections, const HmsTextFn& hms_text)
{
    const std::vector<std::string>& valid = printer_status_section_names();
    for (const std::string& s : sections)
        if (std::find(valid.begin(), valid.end(), s) == valid.end()) {
            std::string list;
            for (const std::string& v : valid)
                list += (list.empty() ? "" : ", ") + v;
            throw ToolFailure("Unknown section \"" + s + "\"; valid sections: " + list, {{"valid", valid}});
        }
    const std::vector<std::string>& wanted = sections.empty() ? valid : sections;
    auto has = [&](const char* name) { return std::find(wanted.begin(), wanted.end(), name) != wanted.end(); };
    auto build = [&](const char* name, const std::function<json()>& fn) {
        try {
            return fn();
        } catch (const std::exception& e) {
            return json{{"error", std::string("could not read ") + name + ": " + e.what()}};
        }
    };

    json out = json::object();
    if (has("print")) out["print"] = build("print", [&] { return section_print(m); });
    if (has("temps")) out["temps"] = build("temps", [&] { return section_temps(m); });
    if (has("fans")) out["fans"] = build("fans", [&] { return section_fans(m); });
    if (has("lights")) out["lights"] = build("lights", [&] { return section_lights(m); });
    if (has("ams")) out["ams"] = build("ams", [&] { return section_ams(m); });
    if (has("nozzles")) out["nozzles"] = build("nozzles", [&] { return section_nozzles(m); });
    if (has("hms")) out["hms"] = build("hms", [&] { return section_hms(m, hms_text); });
    if (has("storage")) out["storage"] = build("storage", [&] { return section_storage(m); });
    if (has("capabilities")) out["capabilities"] = build("capabilities", [&] { return section_capabilities(m); });
    if (has("firmware")) out["firmware"] = build("firmware", [&] { return section_firmware(m); });
    return redact(std::move(out));
}

// ---- printer_job / printer_hms -------------------------------------------------------------------

void check_printer_job_allowed(MachineObject& m, const std::string& action, const std::vector<int>& object_ids)
{
    const json details = {{"print_status", m.print_status}};
    if (action == "pause") {
        if (!m.can_pause())
            throw ToolFailure("Cannot pause: print_status is \"" + m.print_status + "\" (a print must be RUNNING)", details);
    } else if (action == "resume") {
        if (!m.can_resume())
            throw ToolFailure("Cannot resume: print_status is \"" + m.print_status + "\" (a print must be PAUSE)", details);
        if (m.jobState_ > 1) // DeviceManager.cpp:1727 check_resume_condition would open a dialog
            throw ToolFailure("This job (e.g. a laser job) can only be resumed on the printer itself", details);
    } else if (action == "stop") {
        if (!m.can_abort())
            throw ToolFailure("Cannot stop: print_status is \"" + m.print_status + "\" (no print is active)", details);
    } else if (action == "skip_objects") {
        if (!m.is_in_printing())
            throw ToolFailure("Cannot skip objects: print_status is \"" + m.print_status + "\" (no print is active)", details);
        if (!m.is_support_partskip)
            throw ToolFailure("This printer does not support skipping objects", details);
        if (object_ids.empty())
            throw ToolFailure("skip_objects needs object_ids");
    } else {
        throw ToolFailure("Unknown action \"" + action + "\"; use pause, resume, stop or skip_objects");
    }
}

int send_printer_job_command(MachineObject& m, const std::string& action, const std::vector<int>& object_ids)
{
    // mirrors StatusPanel.cpp:2805-2825 (pause/resume/abort) and PartSkipDialog.cpp:939 (skip)
    if (action == "pause") return m.command_task_pause();
    if (action == "resume") return m.command_task_resume();
    if (action == "stop") return m.command_task_abort();
    if (action == "skip_objects") return m.command_task_partskip(object_ids);
    throw ToolFailure("Unknown action \"" + action + "\"");
}

void check_printer_hms_allowed(MachineObject& m, const std::string& action)
{
    if (action == "stop_buzzer")
        return;
    if (action != "resume" && action != "ignore" && action != "stop" && action != "clear")
        throw ToolFailure("Unknown action \"" + action + "\"; use list, resume, ignore, stop, clear or stop_buzzer");
    if (m.print_error == 0)
        throw ToolFailure("The printer reports no active print error to " + action, {{"print_status", m.print_status}});
    if ((action == "resume" || action == "ignore") && m.jobState_ > 1) // check_resume_condition would open a dialog
        throw ToolFailure("This job can only be resumed on the printer itself");
}

int send_printer_hms_command(MachineObject& m, const std::string& action, bool no_reminder)
{
    // mirrors DeviceErrorDialog.cpp:543-614 on_button_click; the error argument is the print_error as decimal text
    const std::string err = std::to_string(m.print_error);
    if (action == "resume") return m.command_hms_resume(err, m.job_id_);
    if (action == "ignore") return no_reminder ? m.command_hms_idle_ignore(err, 0) : m.command_hms_ignore(err, m.job_id_);
    if (action == "stop") return m.command_hms_stop(err, m.job_id_);
    if (action == "stop_buzzer") return m.command_stop_buzzer();
    if (action == "clear") { // DBL_CHECK_OK, DeviceErrorDialog.cpp:688-689
        const int rc1 = m.command_clean_print_error(m.subtask_id_, m.print_error);
        const int rc2 = m.command_clean_print_error_uiop(m.print_error);
        return rc1 != 0 ? rc1 : rc2;
    }
    throw ToolFailure("Unknown action \"" + action + "\"");
}

// ---- tools ----------------------------------------------------------------------------------------

namespace {

std::vector<std::string> string_list(const json& args, const char* key)
{
    std::vector<std::string> out;
    if (args.contains(key) && args[key].is_array())
        for (const json& v : args[key])
            if (v.is_string())
                out.push_back(v.get<std::string>());
    return out;
}

HmsTextFn make_hms_text(MachineObject& m)
{
    MachineObject* obj = &m;
    return [obj](const std::string& code) -> std::string {
        HMSQuery* q = wxGetApp().get_hms_query();
        return q ? utf8(q->query_hms_msg(obj, code)) : std::string();
    };
}

json list_printers()
{
    DeviceManager&       dev      = ui_device_manager();
    MachineObject*       selected = dev.get_selected_machine();
    const std::string    agent    = dev.get_current_printer_agent_id();
    json                 printers = json::array();
    std::set<std::string> seen;
    auto add = [&](MachineObject* obj, const char* source) {
        if (obj == nullptr || !seen.insert(obj->get_dev_id()).second)
            return;
        json s    = printer_summary_json(*obj, selected == obj);
        s["source"] = source;
        printers.push_back(std::move(s));
    };
    for (auto& [id, obj] : dev.get_my_machine_list(agent))
        add(obj, "bound");
    for (auto& [id, obj] : dev.get_local_machinelist())
        if (obj != nullptr && (agent.empty() || obj->printer_agent_id == agent))
            add(obj, "lan");
    return {{"printers", std::move(printers)},
            {"selected", selected ? json(selected->get_dev_id()) : json(nullptr)},
            {"agent", agent}};
}

ToolDef printer_list_tool()
{
    return mutate_tool(
        "printer_list", L("Printers"),
        "List, select, connect and refresh printers. action list (default): the user's printers plus LAN printers "
        "found on the network, each with name, model, connection (lan/cloud), online, selected, print state and whether an "
        "access code is stored (never the code). select: makes dev_id the selected printer (full AMS and nozzle state exist "
        "only for the selected printer) and waits until it reports its state. connect: gives a LAN printer its access code "
        "(dev_id and access_code) as the Input access code dialog does, then selects it; the code is never echoed or "
        "journaled. Adding a printer by IP address is not supported. refresh: asks the printer for a full status push; "
        "Orca rate-limits this to once per 3 seconds (a refused request is reported as rate_limited).",
        object_schema({{"action", enum_prop({"list", "select", "connect", "refresh"}, "What to do. Default list.")},
                       {"dev_id", dev_id_prop()},
                       {"ip", prop("string", "Not supported: adding a printer by IP address.")},
                       {"access_code", prop("string", "LAN access code, for connect.")},
                       {"dry_run", dry_run_prop()}}),
        [](const json& args, ToolContext& ctx) {
            const std::string action  = arg_string(args, "action", "list");
            const bool        dry_run = arg_bool(args, "dry_run", false);
            if (args.contains("access_code")) {
                json recorded = args;
                recorded.erase("access_code");
                ctx.set_journal_args(std::move(recorded));
            }
            if (action == "list")
                return ToolResult::ok(ctx.run_ui([] { return list_printers(); }));

            if (action == "refresh") {
                const PrinterTarget target = resolve_printer(ctx, args, false, false);
                json r = run_printer_command(ctx, target.dev_id, dry_run, [](MachineObject& m) { return m.command_request_push_all(); });
                r["dev_id"] = target.dev_id;
                if (r.value("code", 0) == -1 && !dry_run)
                    r["rate_limited"] = true; // Orca allows one push request per 3 s (REQUEST_PUSH_MIN_TIME)
                r["note"] = "Orca rate-limits push requests to once per 3 seconds; re-read printer_status shortly after.";
                return ToolResult::ok(std::move(r));
            }

            if (action == "select" || action == "connect") {
                if (!args.contains("dev_id") || !args["dev_id"].is_string() || args["dev_id"].get<std::string>().empty())
                    throw ToolFailure(action + " needs dev_id");
                if (action == "connect") {
                    if (args.contains("ip") && !args["ip"].is_null() && !args["ip"].get<std::string>().empty())
                        throw ToolFailure("Adding a printer by IP address is not supported; connect takes the dev_id of a printer "
                                          "printer_list already shows (discovered on the LAN) and its access_code");
                    const std::string code = arg_string(args, "access_code");
                    if (code.empty())
                        throw ToolFailure("connect needs access_code");
                    for (char c : code)
                        if (!std::isalnum(static_cast<unsigned char>(c)))
                            throw ToolFailure("Invalid access code (letters and digits only)"); // ConnectPrinterDialog::on_button_confirm
                    const json check = ctx.run_ui([&]() -> json {
                        MachineObject& m = ui_printer(arg_string(args, "dev_id"));
                        if (!m.is_lan_mode_printer())
                            throw ToolFailure("connect applies to LAN printers; this printer connects through the cloud");
                        if (!dry_run)
                            m.set_access_code(code); // mirrors ConnectPrinter.cpp:on_button_confirm
                        return {{"name", m.get_dev_name()}};
                    });
                    if (dry_run)
                        return ToolResult::ok({{"dry_run", true}, {"would", "set the access code of " + check["name"].get<std::string>() + " and select it"}});
                } else if (dry_run) {
                    return ToolResult::ok({{"dry_run", true}, {"would", "select printer " + arg_string(args, "dev_id")}});
                }
                const PrinterTarget target = resolve_printer(ctx, args, true, true, 20);
                json                out    = ctx.run_ui([&]() -> json {
                    DeviceManager& dev = ui_device_manager();
                    MachineObject& m   = ui_printer(target.dev_id);
                    return {{"printer", printer_summary_json(m, dev.get_selected_machine() == &m)}};
                });
                out["selected_now"] = target.selected_now;
                return ToolResult::ok(std::move(out));
            }
            throw ToolFailure("Unknown action \"" + action + "\"");
        });
}

ToolDef printer_status_tool()
{
    return inspect_tool(
        "printer_status",
        L("Printer status"),
        "Read-only live state of a printer (it never sends anything and never selects a printer; call printer_list refresh "
        "first for fresh data). Sections: print (state, stage, progress, remaining_min, layers, error, can_pause/resume/abort), "
        "temps (per extruder with side, bed, chamber), fans, lights, ams (units with type, extruder side, bound extruders, humidity, "
        "trays with tray_index as the print mapping uses it, colour, type, remain, k/n; external spools 254 left / 255 right; "
        "current tray), nozzles (diameter, flow type, material; H2C rack; filament switch), hms (codes, level, text), storage, "
        "capabilities, firmware. Sides are right (extruder 0) and left (extruder 1) on dual-nozzle printers, main otherwise. "
        "Full AMS and nozzle state exists only for the selected printer; for any other the reply says partial.",
        object_schema({{"dev_id", dev_id_prop()},
                       {"sections", array_prop({{"type", "string"}}, "Sections to return; default all.")}}),
        [](const json& args, ToolContext& ctx) {
            const std::vector<std::string> sections = string_list(args, "sections");
            const PrinterTarget            target   = resolve_printer(ctx, args, false, false);
            json out = ctx.run_ui([&]() -> json {
                MachineObject& m        = ui_printer(target.dev_id);
                MachineObject* selected = ui_selected_printer();
                const bool     is_sel   = selected != nullptr && selected->get_dev_id() == target.dev_id;
                json           r        = printer_status_json(m, sections, make_hms_text(m));
                json           res;
                res["printer"] = printer_summary_json(m, is_sel);
                res["status"]  = std::move(r);
                if (!is_sel) {
                    res["partial"] = true;
                    res["hint"]    = "select it with printer_list {action: select} for full AMS and nozzle state";
                }
                return res;
            });
            return ToolResult::ok(std::move(out));
        });
}

ToolDef printer_job_tool()
{
    return mutate_tool(
        "printer_job", L("Control the print job"),
        "Pause, resume or stop the running print, or skip objects in it. pause needs a RUNNING print, resume a PAUSE, "
        "stop any active print; the error names the printer's print_status when the action is not possible. Only stop and "
        "skip_objects are destructive (they cannot be undone). skip_objects takes object_ids as the printer lists them in its "
        "Skip objects dialog (the ids printer_status shows as skipped_object_ids are already skipped). Resume uses the same "
        "command as the Device tab; a laser job that can only be resumed on the printer is refused. The reply is sent: true "
        "with no acknowledgement; re-read printer_status. dry_run returns the payload instead of sending.",
        object_schema({{"action", enum_prop({"pause", "resume", "stop", "skip_objects"}, "What to do.")},
                       {"object_ids", array_prop({{"type", "integer"}}, "skip_objects: ids of the objects to skip, as the printer knows them.")},
                       {"dev_id", dev_id_prop()},
                       {"dry_run", dry_run_prop()}},
                      {"action"}),
        [](const json& args, ToolContext& ctx) {
            const std::string action = arg_string(args, "action");
            std::vector<int>  ids;
            if (args.contains("object_ids") && args["object_ids"].is_array())
                for (const json& v : args["object_ids"])
                    if (v.is_number_integer())
                        ids.push_back(v.get<int>());
            const bool          dry_run = arg_bool(args, "dry_run", false);
            const PrinterTarget target  = resolve_printer(ctx, args, false, false);
            json r = run_printer_command(ctx, target.dev_id, dry_run, [&](MachineObject& m) {
                check_printer_job_allowed(m, action, ids);
                return send_printer_job_command(m, action, ids);
            });
            if (!dry_run && !r.value("sent", false))
                throw ToolFailure("The printer connection refused the " + action + " command", {{"code", r.value("code", -1)}});
            r["dev_id"] = target.dev_id;
            r["action"] = action;
            return ToolResult::ok(std::move(r));
        },
        /*destructive=*/true);
}

ToolDef printer_hms_tool()
{
    return mutate_tool(
        "printer_hms", L("Printer errors (HMS)"),
        "Printer errors and warnings. list: the active HMS items (code, level, text) and the current print error. "
        "The other actions answer the error dialog the printer raised, for the printer's current print error, with the "
        "same commands the dialog buttons send: resume (continue the print), ignore (ignore the error and continue; "
        "no_reminder: the idle \"don't remind me next time\" variant), stop (stop the print), clear (dismiss the error), "
        "stop_buzzer (silence the fire alarm buzzer). They fail when no print error is active. Sends no acknowledgement; "
        "re-read printer_status. dry_run returns the payload(s) instead of sending. Only stop is destructive.",
        object_schema({{"action", enum_prop({"list", "resume", "ignore", "stop", "clear", "stop_buzzer"}, "What to do. Default list.")},
                       {"no_reminder", prop("boolean", "ignore: use the idle \"don't remind me next time\" command.")},
                       {"dev_id", dev_id_prop()},
                       {"dry_run", dry_run_prop()}}),
        [](const json& args, ToolContext& ctx) {
            const std::string   action  = arg_string(args, "action", "list");
            const bool          dry_run = arg_bool(args, "dry_run", false);
            const PrinterTarget target  = resolve_printer(ctx, args, false, false);
            if (action == "list") {
                json out = ctx.run_ui([&]() -> json {
                    MachineObject& m = ui_printer(target.dev_id);
                    json           r = {{"dev_id", target.dev_id},
                                        {"hms", printer_status_json(m, {"hms"}, make_hms_text(m))["hms"]},
                                        {"print_error", {{"code", m.print_error}, {"hex", hex8(m.print_error)}, {"text", m.get_print_error_str()}}}};
                    if (m.print_error != 0)
                        if (HMSQuery* q = wxGetApp().get_hms_query())
                            r["print_error"]["message"] = utf8(q->query_print_error_msg(&m, m.print_error));
                    return r;
                });
                return ToolResult::ok(std::move(out));
            }
            const bool no_reminder = arg_bool(args, "no_reminder", false);
            json       r = run_printer_command(ctx, target.dev_id, dry_run, [&](MachineObject& m) {
                check_printer_hms_allowed(m, action);
                return send_printer_hms_command(m, action, no_reminder);
            });
            if (!dry_run && !r.value("sent", false))
                throw ToolFailure("The printer connection refused the " + action + " command", {{"code", r.value("code", -1)}});
            r["dev_id"] = target.dev_id;
            r["action"] = action;
            return ToolResult::ok(std::move(r));
        },
        /*destructive=*/true);
}

ToolDef printer_account_tool()
{
    return inspect_tool(
        "printer_account", L("Printer account"),
        "Read-only: whether the user is logged in to the printer cloud, the user name, the active printer agent, how many "
        "bound and LAN printers are known. Whether LAN discovery is running is not exposed by Orca.",
        object_schema({}),
        [](const json&, ToolContext& ctx) {
            return ToolResult::ok(ctx.run_ui([]() -> json {
                json j;
                NetworkAgent* agent = wxGetApp().getAgent();
                const bool    login = wxGetApp().is_user_login();
                j["logged_in"] = login;
                if (login && agent != nullptr)
                    j["user_name"] = agent->get_user_name();
                DeviceManager& dev = ui_device_manager();
                const std::string agent_id = dev.get_current_printer_agent_id();
                j["agent"]                 = agent_id;
                j["printer_cloud_provider"] = wxGetApp().get_printer_cloud_provider();
                j["bound_printers"]        = static_cast<int>(dev.get_my_machine_list(agent_id).size());
                j["lan_printers"]          = static_cast<int>(dev.get_local_machinelist().size());
                return redact(std::move(j));
            }));
        });
}

} // namespace

void register_printer_tools(McpService& service)
{
    service.registry().add(printer_list_tool());
    service.registry().add(printer_status_tool());
    service.registry().add(printer_job_tool());
    service.registry().add(printer_hms_tool());
    service.registry().add(printer_account_tool());
}

}}} // namespace Slic3r::GUI::Mcp
