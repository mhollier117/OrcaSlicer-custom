// Printer MCP tools (fork): printer_control, printer_ams, printer_options, printer_calibration.
// Design: C:/claude-code/orca_mcp/docs/specs/2026-11-02-printer-mcp-design.md
//
// The per-action logic lives in the dispatch_* functions (McpPrinterCtrl.hpp), which take a MachineObject and
// never touch wxGetApp(); the tool handlers resolve the printer, resolve filament presets (the only GUI
// state needed), and run a dispatcher through run_printer_command() for a real send or a dry run.
// Every command below issues the call the cited GUI code issues; the checks mirror the ones that code makes
// before it enables its control.

#include "McpPrinterCtrl.hpp"

#include "McpPrinterCommon.hpp"
#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/calib.hpp"
#include "slic3r/GUI/DeviceCore/DevConfig.h"
#include "slic3r/GUI/DeviceCore/DevCtrl.h"
#include "slic3r/GUI/DeviceCore/DevDefs.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevFan.h"
#include "slic3r/GUI/DeviceCore/DevFilaAmsSetting.h"
#include "slic3r/GUI/DeviceCore/DevFilaBlackList.h"
#include "slic3r/GUI/DeviceCore/DevFilaSwitch.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevLamp.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceCore/DevPrintOptions.h"
#include "slic3r/GUI/DeviceCore/DevStorage.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/Utils/PublishCapture.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

// ---- argument helpers --------------------------------------------------------------------------------

[[noreturn]] void fail(const std::string& message, json details = json()) { throw ToolFailure(message, std::move(details)); }

const json* member(const json& args, const char* key)
{
    if (!args.is_object())
        return nullptr;
    const auto it = args.find(key);
    return (it == args.end() || it->is_null()) ? nullptr : &*it;
}

std::string req_string(const json& args, const char* key)
{
    const json* v = member(args, key);
    if (v == nullptr || !v->is_string() || v->get<std::string>().empty())
        fail(std::string("\"") + key + "\" is required (a non-empty string)");
    return v->get<std::string>();
}

std::string opt_string(const json& args, const char* key, const std::string& fallback = {})
{
    const json* v = member(args, key);
    if (v == nullptr)
        return fallback;
    if (!v->is_string())
        fail(std::string("\"") + key + "\" must be a string");
    return v->get<std::string>();
}

bool is_integral(const json& v) { return v.is_number_integer() || (v.is_number_float() && v.get<double>() == std::floor(v.get<double>())); }

int checked_int(const json& v, const char* key, int lo, int hi)
{
    if (!v.is_number() || !is_integral(v))
        fail(std::string("\"") + key + "\" must be an integer");
    const double d = v.get<double>();
    if (d < lo || d > hi)
        fail(std::string("\"") + key + "\" must be between " + std::to_string(lo) + " and " + std::to_string(hi), {{key, d}});
    return int(d);
}

int req_int(const json& args, const char* key, int lo, int hi)
{
    const json* v = member(args, key);
    if (v == nullptr)
        fail(std::string("\"") + key + "\" is required");
    return checked_int(*v, key, lo, hi);
}

std::optional<int> opt_int(const json& args, const char* key, int lo, int hi)
{
    const json* v = member(args, key);
    if (v == nullptr)
        return std::nullopt;
    return checked_int(*v, key, lo, hi);
}

std::optional<double> opt_number(const json& args, const char* key, double lo, double hi)
{
    const json* v = member(args, key);
    if (v == nullptr)
        return std::nullopt;
    if (!v->is_number())
        fail(std::string("\"") + key + "\" must be a number");
    const double d = v->get<double>();
    if (!(d >= lo && d <= hi))
        fail(std::string("\"") + key + "\" must be between " + std::to_string(lo) + " and " + std::to_string(hi), {{key, d}});
    return d;
}

double req_number(const json& args, const char* key, double lo, double hi)
{
    auto v = opt_number(args, key, lo, hi);
    if (!v)
        fail(std::string("\"") + key + "\" is required");
    return *v;
}

std::optional<bool> opt_bool(const json& args, const char* key)
{
    const json* v = member(args, key);
    if (v == nullptr)
        return std::nullopt;
    if (!v->is_boolean())
        fail(std::string("\"") + key + "\" must be true or false");
    return v->get<bool>();
}

bool req_bool(const json& args, const char* key)
{
    auto v = opt_bool(args, key);
    if (!v)
        fail(std::string("\"") + key + "\" is required (true or false)");
    return *v;
}

std::string action_of(const json& args, const std::vector<std::string>& allowed)
{
    const std::string action = req_string(args, "action");
    if (std::find(allowed.begin(), allowed.end(), action) == allowed.end()) {
        std::string list;
        for (const std::string& a : allowed)
            list += (list.empty() ? "" : ", ") + a;
        fail("Unknown action \"" + action + "\"; expected one of: " + list, {{"action", action}});
    }
    return action;
}

void set_info(json* info, const char* key, json value)
{
    if (info != nullptr)
        (*info)[key] = std::move(value);
}

int first_error(int current, int next) { return current != 0 ? current : next; }

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// "#RRGGBB" / "#RRGGBBAA" (the # optional) to the AMS "RRGGBBAA" form, upper case, opaque by default
// (AMSMaterialsSetting.cpp:126 colour_to_ams_string writes RGBA).
std::string ams_color(const std::string& text)
{
    std::string s = text;
    if (!s.empty() && s[0] == '#')
        s.erase(0, 1);
    if ((s.size() != 6 && s.size() != 8) || !std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c) != 0; }))
        fail("Colour \"" + text + "\" must be #RRGGBB or #RRGGBBAA");
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::toupper(c)); });
    return s.size() == 6 ? s + "FF" : s;
}

const char* volume_type_name(NozzleVolumeType t)
{
    switch (t) {
    case nvtStandard: return "standard";
    case nvtHighFlow: return "high_flow";
    case nvtHybrid: return "hybrid";
    case nvtTPUHighFlow: return "tpu_high_flow";
    case nvtE3DHighFlow: return "e3d_high_flow";
    default: return "standard";
    }
}

NozzleVolumeType volume_type_from(const json& args)
{
    const std::string s = lower(opt_string(args, "nozzle_volume_type", "standard"));
    if (s == "standard") return nvtStandard;
    if (s == "high_flow") return nvtHighFlow;
    if (s == "hybrid") return nvtHybrid;
    if (s == "tpu_high_flow") return nvtTPUHighFlow;
    if (s == "e3d_high_flow") return nvtE3DHighFlow;
    fail("nozzle_volume_type must be standard, high_flow, hybrid, tpu_high_flow or e3d_high_flow");
}

// ---- printer_control -------------------------------------------------------------------------------------

constexpr int kExtrudeMinTemp = 170; // StatusPanel.cpp:128 TEMP_THRESHOLD_ALLOW_E_CTRL

int extruder_count(MachineObject& m) { return std::max(1, m.GetExtderSystem()->GetTotalExtderCount()); }

int control_set_temp(MachineObject& m, const json& args, json* info)
{
    const std::string target = req_string(args, "target");
    const int         value  = req_int(args, "value", 0, 1000);
    if (target == "bed") {
        // mirrors StatusPanel.cpp:4278 on_set_bed_temp (limit from the printer, clamped there)
        int limit = m.get_bed_temperature_limit();
        if (m.bed_temp_range.size() > 1)
            limit = m.bed_temp_range[1];
        if (value > limit)
            fail("Bed temperature " + std::to_string(value) + " exceeds the printer's limit of " + std::to_string(limit), {{"limit", limit}});
        return m.command_set_bed(value);
    }
    if (target == "nozzle") {
        // mirrors StatusPanel.cpp:4293 on_set_nozzle_temp: one nozzle uses the plain command, several the indexed one
        const int count    = extruder_count(m);
        const int extruder = opt_int(args, "extruder", 0, 15).value_or(MAIN_EXTRUDER_ID);
        if (extruder >= count)
            fail("This printer has " + std::to_string(count) + " extruder(s); extruder " + std::to_string(extruder) + " does not exist");
        const int lo = m.nozzle_temp_range.size() >= 2 ? m.nozzle_temp_range[0] : 0;
        const int hi = m.nozzle_temp_range.size() >= 2 ? m.nozzle_temp_range[1] : 300; // conservative when unreported
        if (value > hi || (value != 0 && value < lo))
            fail("Nozzle temperature must be 0 (off) or between " + std::to_string(lo) + " and " + std::to_string(hi), {{"min", lo}, {"max", hi}});
        return count == 1 ? m.command_set_nozzle(value) : m.command_set_nozzle_new(extruder, value);
    }
    if (target == "chamber") {
        // mirrors StatusPanel.cpp:4334 on_set_chamber_temp
        DevConfig* config = m.GetConfig();
        if (!config->SupportChamberEdit())
            fail("This printer's chamber temperature cannot be set");
        if (value != 0 && (value < config->GetChamberTempEditMin() || value > config->GetChamberTempEditMax()))
            fail("Chamber temperature must be 0 (off) or between " + std::to_string(config->GetChamberTempEditMin()) + " and " +
                     std::to_string(config->GetChamberTempEditMax()),
                 {{"min", config->GetChamberTempEditMin()}, {"max", config->GetChamberTempEditMax()}});
        if (m.is_in_printing() && m.GetFan()->GetSupportAirduct() && m.GetFan()->is_at_cooling_mode())
            fail("Chamber temperature cannot be changed in cooling mode while printing");
        if (!m.GetFan()->is_at_heating_mode() && value >= config->GetChamberTempSwitchHeat())
            set_info(info, "warning", "The printer switches to heating mode when the chamber target exceeds " + std::to_string(config->GetChamberTempSwitchHeat()) + " C");
        return m.command_set_chamber(value);
    }
    fail("target must be nozzle, bed or chamber");
}

int control_set_fan(MachineObject& m, const json& args, json* info)
{
    const std::string fan     = req_string(args, "fan");
    const int         percent = req_int(args, "percent", 0, 100);
    int               id      = 0;
    if (fan == "part") id = FAN_COOLING_0_AIRDOOR;
    else if (fan == "aux") id = FAN_REMOTE_COOLING_0_IDX;
    else if (fan == "chamber") id = FAN_CHAMBER_0_IDX;
    else fail("fan must be part, aux or chamber");

    // mirrors FanControlPopupNew / FanControlNew::update_mode (FanControl.cpp:540): a fan the current air duct mode
    // runs automatically, or has off, cannot be set.
    DevFan*     fans = m.GetFan();
    AirDuctData data = fans->GetAirDuctData();
    bool        known = false;
    if (data.parts.empty()) { // old protocol: DevFan::converse_to_duct builds part, and aux + chamber with the aux fan
        known = id == FAN_COOLING_0_AIRDOOR || fans->GetSupportAuxFanData();
    } else {
        known = std::any_of(data.parts.begin(), data.parts.end(), [&](const AirParts& p) { return p.id == id; });
    }
    if (!known)
        fail("This printer has no " + fan + " fan to control");
    if (data.curren_mode >= 0) {
        auto it = data.modes.find(data.curren_mode);
        if (it != data.modes.end()) {
            const AirMode& mode = it->second;
            if (std::find(mode.off.begin(), mode.off.end(), id) != mode.off.end())
                fail("The " + fan + " fan is off in the printer's current air duct mode");
            if (std::find(mode.ctrl.begin(), mode.ctrl.end(), id) == mode.ctrl.end())
                fail("The " + fan + " fan is automatic in the printer's current air duct mode");
        }
    }
    // The GUI steps in tenths (FanControl.cpp:477 m_current_speed 0..10).
    const int level = int(std::lround(percent / 10.0));
    if (level * 10 != percent)
        set_info(info, "rounded_percent", level * 10);
    if (!m.is_enable_np)
        return fans->command_control_fan(id, int(std::floor(level * 25.5))); // FanControl.cpp:482 old protocol, 0..255
    return fans->command_control_fan_new(id, level * 10);                      // FanControl.cpp:485
}

int control_set_speed(MachineObject& m, const json& args)
{
    const json* v = member(args, "level");
    if (v == nullptr)
        fail("\"level\" is required: silent, standard, sport, ludicrous or 1-4");
    int level = 0;
    if (v->is_string()) {
        const std::string s = lower(v->get<std::string>());
        if (s == "silent") level = 1;
        else if (s == "standard") level = 2;
        else if (s == "sport") level = 3;
        else if (s == "ludicrous") level = 4;
        else fail("level must be silent, standard, sport, ludicrous or 1-4");
    } else {
        level = checked_int(*v, "level", 1, 4);
    }
    // StatusPanel.cpp:4966: "This only takes effect during printing"
    if (!m.is_in_printing())
        fail("The print speed level can only be changed while the printer is printing");
    return m.command_set_printing_speed(DevPrintingSpeedLevel(level)); // StatusPanel.cpp:4984
}

int control_light(MachineObject& m, const json& args, json* info)
{
    const bool on = req_bool(args, "on");
    if (!on && m.GetLamp()->HasLampCloseRecheck() && m.is_in_printing())
        set_info(info, "warning", "Turning the light off during a print makes AI monitoring (spaghetti detection) fail");
    m.GetLamp()->CtrlSetChamberLight(on ? DevLamp::LIGHT_EFFECT_ON : DevLamp::LIGHT_EFFECT_OFF); // StatusPanel.cpp:5060
    return 0; // CtrlSetChamberLight reports no code
}

int move_axis(MachineObject& m, const std::string& axis, double distance, int speed)
{
    if (axis == "E") {
        // mirrors StatusPanel.cpp:4231 on_axis_ctrl_e_up_10: the extruder must be hot
        const auto ext = m.GetExtderSystem()->GetCurrentExtder();
        if (!ext || ext->GetCurrentTemp() < kExtrudeMinTemp)
            fail("Heat the nozzle to at least " + std::to_string(kExtrudeMinTemp) + " C before extruding or retracting");
        if (m.is_enable_np)
            return m.command_extruder_control(ext->GetExtId(), distance); // StatusPanel.cpp:4238
        return m.command_axis_control("E", 1.0, distance, speed);
    }
    // StatusPanel.cpp:4140: an axis that is not at home must be homed first
    if (!m.is_axis_at_home(axis))
        fail("Axis " + axis + " is not homed; use printer_control {action: \"home\"} first");
    return m.command_axis_control(axis, 1.0, distance, speed);
}

int control_jog(MachineObject& m, const json& args)
{
    ui_require_not_printing(m, "Moving an axis");
    const std::string axis = req_string(args, "axis");
    if (axis != "X" && axis != "Y" && axis != "Z" && axis != "E")
        fail("axis must be X, Y, Z or E");
    const double distance = req_number(args, "distance", -100.0, 100.0);
    if (distance == 0.0)
        fail("distance must not be 0");
    const int default_speed = axis == "X" || axis == "Y" ? 3000 : 900; // StatusPanel.cpp:4154 / 4183 / 4238
    const int max_speed     = axis == "X" || axis == "Y" ? 6000 : 1200;
    const int speed         = opt_int(args, "speed", 60, max_speed).value_or(default_speed);
    // BBLPrinterAgent::command_axis_control: with MQTT axis control only a step of 1 or 10 mm exists.
    if (m.m_support_mqtt_axis_control && std::fabs(distance) != 1.0 && std::fabs(distance) != 10.0)
        fail("This printer moves its axes in steps of exactly 1 or 10 mm (positive or negative)");
    return move_axis(m, axis, distance, speed);
}

int control_extrude(MachineObject& m, const json& args)
{
    ui_require_not_printing(m, "Extruding or retracting");
    const double length = req_number(args, "length", -100.0, 100.0);
    if (length == 0.0)
        fail("length must not be 0");
    DevExtderSystem* system = m.GetExtderSystem();
    const int        current = system->GetCurrentExtderId();
    if (const auto wanted = opt_int(args, "extruder", 0, 15); wanted && *wanted != current)
        fail("Extruder " + std::to_string(*wanted) + " is not the active one (" + std::to_string(current) + "); select_extruder first");
    if (m.is_enable_np && std::floor(length) != length)
        fail("This printer extrudes in whole millimetres");
    if (m.m_support_mqtt_axis_control && !m.is_enable_np && std::fabs(length) != 1.0 && std::fabs(length) != 10.0)
        fail("This printer extrudes in steps of exactly 1 or 10 mm (positive or negative)");
    return move_axis(m, "E", length, opt_int(args, "speed", 60, 1200).value_or(900));
}

int control_select_extruder(MachineObject& m, const json& args)
{
    ui_require_not_printing(m, "Switching the extruder");
    const int count = extruder_count(m);
    if (count < 2)
        fail("This printer has a single extruder");
    const int id = req_int(args, "extruder", 0, count - 1);
    if (m.ams_status_main == AmsStatusMain::AMS_STATUS_MAIN_FILAMENT_CHANGE) // StatusPanel.cpp:5136
        fail("The printer is busy with a filament change");
    return m.GetCtrl()->command_select_extruder(id); // StatusPanel.cpp:5144
}

// ---- printer_ams -------------------------------------------------------------------------------------------

struct SlotRef
{
    int         ams_id  = 0;
    int         slot_id = 0;
    bool        virt    = false;
    DevAmsTray* tray    = nullptr;
};

SlotRef resolve_slot(MachineObject& m, const json& args)
{
    SlotRef ref;
    if (const auto idx = opt_int(args, "tray_index", 0, 15); idx && member(args, "ams_id") == nullptr) {
        ref.ams_id  = *idx / 4;
        ref.slot_id = *idx % 4;
    } else {
        ref.ams_id  = req_int(args, "ams_id", 0, 255);
        ref.slot_id = opt_int(args, "slot_id", 0, 3).value_or(0);
    }
    if (ref.ams_id == VIRTUAL_TRAY_MAIN_ID || ref.ams_id == VIRTUAL_TRAY_DEPUTY_ID) {
        ref.virt = true;
        if (ref.slot_id != 0)
            fail("An external spool has slot 0 only");
        const size_t idx = ref.ams_id == VIRTUAL_TRAY_MAIN_ID ? 0 : 1;
        if (idx >= m.vt_slot.size())
            fail("This printer has no external spool " + std::to_string(ref.ams_id));
        ref.tray = &m.vt_slot[idx];
        return ref;
    }
    if (m.GetFilaSystem()->GetAmsById(std::to_string(ref.ams_id)) == nullptr)
        fail("AMS " + std::to_string(ref.ams_id) + " is not present", {{"ams_id", ref.ams_id}});
    ref.tray = m.GetFilaSystem()->GetAmsTray(std::to_string(ref.ams_id), std::to_string(ref.slot_id));
    if (ref.tray == nullptr)
        fail("AMS " + std::to_string(ref.ams_id) + " has no slot " + std::to_string(ref.slot_id));
    return ref;
}

int tray_temp(const DevAmsTray* tray) // StatusPanel.cpp:4496: the mean of the tray's min and max, -1 when unknown
{
    if (tray == nullptr || tray->nozzle_temp_max.empty() || tray->nozzle_temp_min.empty())
        return -1;
    return (std::atoi(tray->nozzle_temp_min.c_str()) + std::atoi(tray->nozzle_temp_max.c_str())) / 2;
}

int ams_set_tray(MachineObject& m, const json& args, json* info)
{
    const SlotRef ref = resolve_slot(m, args);
    // AMSMaterialsSetting: view only in 2D mode, read only for a Bambu RFID tray, no edits during a print unless supported
    if (!m.is_fdm_type())
        fail("Filament slots cannot be edited in this printer mode");
    if (m.is_in_printing() || m.can_resume()) {
        // override_print_lock (fork, experimental): send although the printer does not advertise in-print edits
        // (print.flag3 bit 3), to find out whether the firmware itself refuses them.
        if (!m.can_edit_filament_while_printing() && !opt_bool(args, "override_print_lock").value_or(false))
            fail(std::string(ref.virt ? "Setting external spool information" : "Setting AMS slot information") +
                 " while printing is not supported by this printer (override_print_lock sends it anyway)");
    }
    if (DevFilaSystem::IsBBL_Filament(ref.tray->tag_uid))
        fail("This slot holds a Bambu Lab RFID spool; its information is read only");

    const std::string tray_info_idx = req_string(args, "tray_info_idx");
    const std::string setting_id    = opt_string(args, "setting_id");
    const std::string tray_type     = req_string(args, "tray_type");
    const int         tmin          = req_int(args, "nozzle_temp_min", 0, 500);
    const int         tmax          = req_int(args, "nozzle_temp_max", 0, 500);
    if (tmin > tmax)
        fail("nozzle_temp_min must not exceed nozzle_temp_max");
    const std::string color = ams_color(req_string(args, "color"));

    std::vector<std::string> colors;
    int                      ctype = 2;
    if (m.is_support_filament_manual_multi_color) { // AMSMaterialsSetting.cpp:863: only firmware advertising it takes cols
        if (const json* list = member(args, "colors")) {
            if (!list->is_array() || list->empty() || list->size() > 4)
                fail("colors must be a list of 1 to 4 colours");
            for (const json& c : *list) {
                if (!c.is_string())
                    fail("colors must be strings");
                colors.push_back(ams_color(c.get<std::string>()));
            }
        }
        if (colors.empty())
            colors.push_back(color);
        ctype = colors.size() > 1 ? opt_int(args, "color_type", 0, 1).value_or(0) : 2;
    } else if (member(args, "colors") != nullptr) {
        fail("This printer's firmware does not take multi-colour spools");
    }

    // AMSMaterialsSetting.cpp:896: ams_id/slot_id, filament id, setting id, colour, type and the temperature range
    int rc = m.command_ams_filament_settings(ref.ams_id, ref.slot_id, tray_info_idx, setting_id, color, tray_type, tmin, tmax, colors, ctype);
    if (rc == 0 && PublishCapture::current() == nullptr) { // AMSMaterialsSetting.cpp:900: show the edit until the push confirms it
        ref.tray->color = color;
        ref.tray->cols  = colors;
        ref.tray->ctype = ctype;
        ref.tray->set_hold_count();
    }
    set_info(info, "note", "The slot keeps its previous K profile; use printer_ams {action: \"set_k\"} to select one for the new filament");
    return rc;
}

// Selects a K profile (AMSMaterialsSetting.cpp:962 / 988) or, for the old protocol, sets K directly (:1014).
int select_k(MachineObject& m, const SlotRef& ref, const json& args, json* info)
{
    int tray_id = ref.ams_id * 4 + ref.slot_id;
    if (ref.virt) {
        tray_id = ref.ams_id;
        if (!m.is_enable_np)
            tray_id = VIRTUAL_TRAY_DEPUTY_ID;
    }
    if (m.cali_version >= 0) {
        const json* idx = member(args, "cali_idx");
        if (idx == nullptr)
            fail("This printer selects a K profile: pass cali_idx (from printer_calibration pa_list; -1 is the default profile)");
        PACalibIndexInfo sel;
        sel.tray_id         = tray_id;
        sel.ams_id          = ref.ams_id;
        sel.slot_id         = ref.virt ? 0 : ref.slot_id;
        sel.nozzle_diameter = float(opt_number(args, "nozzle_diameter", 0.1, 2.0).value_or(m.GetExtderSystem()->GetNozzleDiameter(0)));
        sel.cali_idx        = checked_int(*idx, "cali_idx", -1, 100000);
        sel.filament_id     = opt_string(args, "filament_id", ref.tray->setting_id);
        sel.nozzle_volume_type = nvtStandard;
        set_info(info, "tray_id", tray_id);
        return m.commnad_select_pa_calibration(sel);
    }
    // old protocol: K is written directly (n is fixed by command_extrusion_cali_set)
    if (member(args, "cali_idx") != nullptr)
        fail("This printer has no K profiles; pass k");
    const double k = req_number(args, "k", 0.0, 2.0);
    const double n = opt_number(args, "n", 0.0, 10.0).value_or(0.0);
    set_info(info, "tray_id", tray_id);
    return m.command_extrusion_cali_set(tray_id, "", "", float(k), float(n));
}

int ams_reset_tray(MachineObject& m, const json& args, json* info)
{
    const SlotRef ref = resolve_slot(m, args);
    if (!m.is_fdm_type())
        fail("Filament slots cannot be edited in this printer mode");
    if (m.is_in_printing() || m.can_resume()) {
        if (!m.can_edit_filament_while_printing() && !opt_bool(args, "override_print_lock").value_or(false))
            fail("Clearing a slot while printing is not supported by this printer (override_print_lock sends it anyway)");
    }
    int rc = 0;
    // AMSMaterialsSetting.cpp:682 on_select_reset: a third-party tray is cleared (empty ids, white, 0 temperatures)...
    if (!DevFilaSystem::IsBBL_Filament(ref.tray->tag_uid))
        rc = m.command_ams_filament_settings(ref.ams_id, ref.slot_id, "", "", "FFFFFF00", "", 0, 0);
    // ...and its K goes back to the default profile (:688)
    json k_args = {{"ams_id", ref.ams_id}, {"slot_id", ref.slot_id}, {"cali_idx", -1}, {"filament_id", ref.tray->setting_id}};
    if (m.cali_version < 0)
        k_args = {{"ams_id", ref.ams_id}, {"slot_id", ref.slot_id}, {"k", 0.0}, {"n", 0.0}};
    return first_error(rc, select_k(m, ref, k_args, info));
}

int ams_refresh_rfid(MachineObject& m, const json& args)
{
    ui_require_not_printing(m, "Reading a spool tag");
    const SlotRef ref = resolve_slot(m, args);
    if (ref.virt)
        fail("An external spool has no RFID to refresh"); // StatusPanel.cpp:4780
    // StatusPanel.cpp:4814: not with filament loaded to the tool head
    DevAms*    ams = m.GetFilaSystem()->GetAmsById(std::to_string(ref.ams_id));
    bool       at_extruder = false;
    const bool np          = m.is_enable_np || m.is_enable_ams_np;
    if (np) {
        if (ams->GetExtruderId() < m.GetExtderSystem()->GetTotalExtderSize())
            at_extruder = m.GetExtderSystem()->HasFilamentInExt(ams->GetExtruderId());
    } else {
        at_extruder = m.is_filament_at_extruder();
    }
    if (at_extruder)
        fail("Cannot read filament info: the filament is loaded to the tool head. Unload it and try again.");
    return np ? m.command_ams_refresh_rfid(ref.ams_id, ref.slot_id) : m.command_ams_refresh_rfid(-1, ref.ams_id * 4 + ref.slot_id);
}

int ams_settings(MachineObject& m, const json& args)
{
    const auto insert = opt_bool(args, "insert_read");
    const auto startup = opt_bool(args, "startup_read");
    const auto remain = opt_bool(args, "remain_detect");
    const auto refill = opt_bool(args, "auto_refill");
    const auto air    = opt_bool(args, "air_print_detect");
    if (!insert && !startup && !remain && !refill && !air)
        fail("Pass at least one of insert_read, startup_read, remain_detect, auto_refill, air_print_detect");
    int rc = 0;
    if (insert || startup || remain) { // AMSSetting.cpp:533 on_insert_material_read / on_starting_read / on_remain
        DevAmsSystemSetting& cur = m.GetFilaSystem()->GetAmsSystemSetting();
        if (remain && !m.is_support_update_remain)
            fail("This printer does not support remaining-filament detection");
        if (!insert && !cur.IsDetectOnInsertEnabled().has_value())
            fail("The printer has not reported the insert-read setting yet; pass insert_read as well");
        rc = first_error(rc, m.command_ams_user_settings(startup.value_or(cur.IsDetectOnPowerupEnabled()),
                                                          insert.value_or(cur.IsDetectOnInsertEnabled().value_or(false)),
                                                          remain.value_or(cur.IsDetectRemainEnabled())));
    }
    if (refill) { // AMSSetting.cpp:599
        if (!m.is_support_filament_backup)
            fail("This printer does not support automatic filament refill");
        rc = first_error(rc, m.command_ams_switch_filament(*refill));
    }
    if (air) { // AMSSetting.cpp:606
        if (!m.is_support_air_print_detection)
            fail("This printer does not support air-printing detection");
        rc = first_error(rc, m.command_ams_air_print_detect(*air));
    }
    return rc;
}

int ams_load(MachineObject& m, const json& args)
{
    ui_require_not_printing(m, "Loading filament");
    const SlotRef ref = resolve_slot(m, args);
    if (!ref.virt && !ref.tray->is_exists)
        fail("AMS " + std::to_string(ref.ams_id) + " slot " + std::to_string(ref.slot_id) + " holds no filament");
    // StatusPanel.cpp:4396 on_ams_load_curr: with a Filament Track Switch the load goes to a chosen extruder
    std::optional<int> extruder_id;
    if (m.GetFilaSwitch() && m.GetFilaSwitch()->IsInstalled()) {
        if (!m.GetFilaSwitch()->IsReady())
            fail("The Filament Track Switch has not been set up. Set it up on the printer.");
        extruder_id = req_int(args, "extruder", 0, 1);
    } else if (member(args, "extruder") != nullptr) {
        fail("extruder applies only to a printer with a Filament Track Switch; the AMS binding picks the extruder");
    }
    int old_temp = -1;
    int new_temp = -1;
    if (ref.virt) { // :4430 both temperatures come from the external spool itself
        old_temp = new_temp = tray_temp(ref.tray);
    } else {
        old_temp = tray_temp(m.get_curr_tray()); // :4491
        new_temp = tray_temp(ref.tray);
        if (m.get_curr_tray() == nullptr)
            old_temp = new_temp = -1;
    }
    // :4460 / :4505 (the np and old branches issue the same call, except that the old protocol addresses the external spool as 254)
    const std::string ams = std::to_string(ref.ams_id);
    if (ref.virt)
        return m.command_ams_change_filament(true, (m.is_enable_np || m.is_enable_ams_np) ? ams : std::to_string(VIRTUAL_TRAY_DEPUTY_ID), "0", old_temp, new_temp, extruder_id);
    return m.command_ams_change_filament(true, ams, std::to_string(ref.slot_id), old_temp, new_temp, extruder_id);
}

int ams_unload(MachineObject& m, const json& args)
{
    ui_require_not_printing(m, "Unloading filament");
    const int extruder = opt_int(args, "extruder", 0, 15).value_or(m.GetExtderSystem()->GetCurrentExtderId());
    const auto ext = m.GetExtderSystem()->GetExtderById(extruder);
    if (!ext)
        fail("Extruder " + std::to_string(extruder) + " does not exist");
    const std::string ams_id = ext->GetSlotNow().ams_id;
    if (ams_id.empty() || !ext->HasFilamentInExt())
        fail("No filament is loaded in extruder " + std::to_string(extruder));
    return m.command_ams_change_filament(false, ams_id, "255"); // StatusPanel.cpp:4570
}

int ams_dry_start(MachineObject& m, const json& args, json* info)
{
    const int ams_id = req_int(args, "ams_id", 0, 255);
    DevAms*   ams    = m.GetFilaSystem()->GetAmsById(std::to_string(ams_id));
    if (ams == nullptr)
        fail("AMS " + std::to_string(ams_id) + " is not present");
    if (!ams->IsSupportRemoteDry(&m))
        fail("This AMS cannot be dried remotely");
    if (ams->AmsIsDrying())
        fail("This AMS is already drying");
    if (const auto reasons = ams->GetCannotDryReason(); reasons && !reasons->empty()) {
        json codes = json::array();
        for (auto r : *reasons)
            codes.push_back(int(r));
        fail("The printer reports this AMS cannot dry now", {{"reasons", codes}});
    }
    // AMSDryControl.cpp:1223 temperature limits per AMS type and the 1..24 h range
    const int temp  = req_int(args, "temperature", 0, 200);
    const int hours = req_int(args, "hours", 1, 24);
    const int lo    = 45;
    const int hi    = ams->GetAmsType() == DevAmsType::N3S ? 85 : 65;
    if (temp < lo || temp > hi)
        fail("Drying temperature for this AMS must be between " + std::to_string(lo) + " and " + std::to_string(hi) + " C", {{"min", lo}, {"max", hi}});
    std::string type = opt_string(args, "filament_type");
    if (type.empty()) { // the GUI dries one chosen tray's filament: default to the first loaded one
        for (const auto& [id, tray] : ams->GetTrays())
            if (tray && tray->is_exists && !tray->m_fila_type.empty()) {
                type = tray->m_fila_type;
                break;
            }
    }
    if (type.empty())
        fail("filament_type is required (the AMS holds no filament to take it from)");
    const int  cooling = opt_int(args, "cooling_temp", 0, 200).value_or(50); // AMSDryControl.cpp:849 default 50
    const bool rotate  = opt_bool(args, "rotate_tray").value_or(false);
    set_info(info, "filament_type", type);
    return m.GetFilaSystem()->CtrlAmsStartDryingHour(ams_id, type, temp, hours, rotate, cooling, false); // AMSDryControl.cpp:856
}

int ams_dry_stop(MachineObject& m, const json& args)
{
    const int ams_id = req_int(args, "ams_id", 0, 255);
    if (m.GetFilaSystem()->GetAmsById(std::to_string(ams_id)) == nullptr)
        fail("AMS " + std::to_string(ams_id) + " is not present");
    return m.GetFilaSystem()->CtrlAmsStopDrying(ams_id); // AMSDryControl.cpp:564
}

int ams_control(MachineObject& m, const json& args)
{
    const std::string cmd = req_string(args, "cmd");
    if (cmd != "resume" && cmd != "reset" && cmd != "pause" && cmd != "done" && cmd != "abort")
        fail("cmd must be resume, reset, pause, done or abort");
    return m.command_ams_control(cmd);
}

int ams_calibrate(MachineObject& m, const json& args)
{
    ui_require_not_printing(m, "Calibrating the AMS");
    const int ams_id = req_int(args, "ams_id", 0, 255);
    if (m.GetFilaSystem()->GetAmsById(std::to_string(ams_id)) == nullptr)
        fail("AMS " + std::to_string(ams_id) + " is not present");
    return m.command_ams_calibrate(ams_id);
}

int ams_reset(MachineObject& m, const json& args)
{
    ui_require_not_printing(m, "Resetting the AMS ids");
    // AMSSetting.cpp:795: offered only when the AMS firmware can be switched, after a confirmation
    auto sw = m.GetFilaSystem()->GetAmsFirmwareSwitch().lock();
    if (!sw || !sw->SupportSwitchFirmware())
        fail("This printer does not offer AMS id reset");
    if (!opt_bool(args, "confirm").value_or(false))
        fail("This resets the ids of ALL AMS units (disconnect them first to choose an order). Pass confirm: true to proceed.");
    return m.GetFilaSystem()->CtrlAmsReset();
}

// ---- printer_options -------------------------------------------------------------------------------------

struct OptionSpec
{
    const char* name;
    bool        sensitivity; // takes low/medium/high
};

const OptionSpec kOptions[] = {
    {"ai_monitoring", true},          {"spaghetti_detection", true},   {"purgechutepileup_detection", true},
    {"nozzleclumping_detection", true}, {"airprinting_detection", true}, {"first_layer_inspection", false},
    {"buildplate_marker_detection", false}, {"auto_recovery_step_loss", false}, {"prompt_sound", false},
    {"filament_tangle_detection", false}, {"save_remote_print_file_to_storage", false}, {"buildplate_align_detection", false},
    {"fod_check", false}, {"displacement_detection", false}, {"purify_air_at_print_end", false},
    {"smart_nozzle_blob_detection", false}, {"snapshot", false}, {"camera_recording", false}, {"timelapse", false},
    {"camera_resolution", false},
};


bool detection_supported(MachineObject& m, PrintOptionEnum e)
{
    PrintOptionData* d = m.GetPrintOptions() ? m.GetPrintOptions()->GetDetectionOption(e) : nullptr;
    return d != nullptr && d->is_support_detect;
}

bool option_supported(MachineObject& m, const std::string& o)
{
    // PrintOptionsDialog.cpp:368 update_options and CameraPopup.cpp:300 check_func_supported
    if (o == "ai_monitoring") return m.GetConfig()->SupportAIMonitor() && !m.xcam_disable_ai_detection_display;
    if (o == "spaghetti_detection") return m.is_support_spaghetti_detection;
    if (o == "purgechutepileup_detection") return m.is_support_purgechutepileup_detection;
    if (o == "nozzleclumping_detection") return m.is_support_nozzleclumping_detection;
    if (o == "airprinting_detection") return m.is_support_airprinting_detection;
    if (o == "first_layer_inspection") return m.GetConfig()->SupportFirstLayerInspect();
    if (o == "buildplate_marker_detection") return m.is_support_build_plate_marker_detect;
    if (o == "auto_recovery_step_loss") return m.is_support_auto_recovery_step_loss;
    if (o == "prompt_sound") return m.is_support_prompt_sound;
    if (o == "filament_tangle_detection") return m.is_support_filament_tangle_detect;
    if (o == "save_remote_print_file_to_storage") return m.GetConfig()->SupportSaveRemotePrintFileToStorage();
    if (o == "buildplate_align_detection") return detection_supported(m, PrintOptionEnum::Buildplate_Align_Detection);
    if (o == "fod_check") return detection_supported(m, PrintOptionEnum::FOD_Check_Detection);
    if (o == "displacement_detection") return detection_supported(m, PrintOptionEnum::Displacement_Detection);
    if (o == "purify_air_at_print_end") return detection_supported(m, PrintOptionEnum::Purify_Air_At_Print_End);
    if (o == "smart_nozzle_blob_detection") return detection_supported(m, PrintOptionEnum::Smart_Nozzle_Blob_Detection);
    if (o == "snapshot") return detection_supported(m, PrintOptionEnum::Snapshot_Detection);
    if (o == "camera_recording") return m.has_ipcam;
    if (o == "timelapse") return m.is_support_timelapse;
    if (o == "camera_resolution") return m.has_ipcam && m.camera_resolution_supported.size() > 1 && m.is_support_1080dpi;
    return false;
}

json option_value(MachineObject& m, const std::string& o)
{
    auto detect = [&](PrintOptionEnum e) -> json {
        PrintOptionData* d = m.GetPrintOptions() ? m.GetPrintOptions()->GetDetectionOption(e) : nullptr;
        return d ? json(d->current_detect_value) : json();
    };
    if (o == "ai_monitoring") return m.xcam_ai_monitoring;
    if (o == "spaghetti_detection") return m.xcam_spaghetti_detection;
    if (o == "purgechutepileup_detection") return m.xcam_purgechutepileup_detection;
    if (o == "nozzleclumping_detection") return m.xcam_nozzleclumping_detection;
    if (o == "airprinting_detection") return m.xcam_airprinting_detection;
    if (o == "first_layer_inspection") return m.xcam_first_layer_inspector;
    if (o == "buildplate_marker_detection") return m.xcam_buildplate_marker_detector;
    if (o == "auto_recovery_step_loss") return m.xcam_auto_recovery_step_loss;
    if (o == "prompt_sound") return m.xcam_allow_prompt_sound;
    if (o == "filament_tangle_detection") return m.xcam_filament_tangle_detect;
    if (o == "save_remote_print_file_to_storage") return m.get_save_remote_print_file_to_storage();
    if (o == "buildplate_align_detection") return detect(PrintOptionEnum::Buildplate_Align_Detection);
    if (o == "fod_check") return detect(PrintOptionEnum::FOD_Check_Detection);
    if (o == "displacement_detection") return detect(PrintOptionEnum::Displacement_Detection);
    if (o == "purify_air_at_print_end") return detect(PrintOptionEnum::Purify_Air_At_Print_End); // 0 off, 1 inside, 2 outside
    if (o == "smart_nozzle_blob_detection") return detect(PrintOptionEnum::Smart_Nozzle_Blob_Detection); // 0 off, 1 on, 2 auto
    if (o == "snapshot") return detect(PrintOptionEnum::Snapshot_Detection);
    if (o == "camera_recording") return m.camera_recording_when_printing;
    if (o == "timelapse") return m.is_timelapse();
    if (o == "camera_resolution") return m.camera_resolution;
    return json();
}

std::string sensitivity_of(MachineObject& m, const std::string& o)
{
    if (o == "ai_monitoring") return m.xcam_ai_monitoring_sensitivity;
    if (o == "spaghetti_detection") return m.xcam_spaghetti_detection_sensitivity;
    if (o == "purgechutepileup_detection") return m.xcam_purgechutepileup_detection_sensitivity;
    if (o == "nozzleclumping_detection") return m.xcam_nozzleclumping_detection_sensitivity;
    if (o == "airprinting_detection") return m.xcam_airprinting_detection_sensitivity;
    return {};
}

bool sd_card_normal(MachineObject& m) { return m.GetStorage() && m.GetStorage()->get_sdcard_state() == DevStorage::SdcardState::HAS_SDCARD_NORMAL; }

int options_set(MachineObject& m, const json& args, json* info)
{
    const std::string o = req_string(args, "option");
    const OptionSpec* spec = nullptr;
    for (const OptionSpec& s : kOptions)
        if (o == s.name)
            spec = &s;
    if (spec == nullptr)
        fail("Unknown option \"" + o + "\"", {{"option", o}});
    if (!option_supported(m, o))
        fail("This printer does not support the option \"" + o + "\"", {{"option", o}});
    const json* value = member(args, "value");
    if (value == nullptr)
        fail("\"value\" is required");

    // PrintOptionsDialog.cpp:83..: on/off plus a sensitivity for the AI detections
    if (spec->sensitivity) {
        if (!value->is_boolean())
            fail("value must be true or false for " + o);
        std::string lvl = lower(opt_string(args, "sensitivity", sensitivity_of(m, o)));
        if (lvl.empty())
            lvl = "medium";
        if (lvl != "low" && lvl != "medium" && lvl != "high")
            fail("sensitivity must be low, medium or high");
        const bool on = value->get<bool>();
        if (o == "ai_monitoring") return m.command_xcam_control_ai_monitoring(on, lvl);
        if (o == "spaghetti_detection") return m.command_xcam_control_spaghetti_detection(on, lvl);
        if (o == "purgechutepileup_detection") return m.command_xcam_control_purgechutepileup_detection(on, lvl);
        if (o == "nozzleclumping_detection") return m.command_xcam_control_nozzleclumping_detection(on, lvl);
        return m.command_xcam_control_airprinting_detection(on, lvl);
    }
    if (o == "camera_resolution") { // CameraPopup.cpp:192
        if (!value->is_string())
            fail("value must be a resolution string such as \"1080p\"");
        const std::string r = value->get<std::string>();
        if (std::find(m.camera_resolution_supported.begin(), m.camera_resolution_supported.end(), r) == m.camera_resolution_supported.end()) {
            json supported = m.camera_resolution_supported;
            fail("Resolution \"" + r + "\" is not supported by this printer", {{"supported", supported}});
        }
        return m.command_ipcam_resolution_set(r);
    }
    if (o == "smart_nozzle_blob_detection") { // PrintOptionsDialog.cpp:262: protocol 0 off, 1 on, 2 auto
        const std::string v = value->is_string() ? lower(value->get<std::string>()) : "";
        int mode = v == "off" ? 0 : v == "on" ? 1 : v == "auto" ? 2 : -1;
        if (mode < 0)
            fail("value must be off, on or auto");
        PrintOptionData* d = m.GetPrintOptions()->GetDetectionOption(PrintOptionEnum::Smart_Nozzle_Blob_Detection);
        if (mode == 1 && d->current_detect_value == 2 && m.is_in_printing() && m.any_loaded_filament_is_stringing_prone() &&
            !opt_bool(args, "confirm").value_or(false))
            fail("There is stringing-prone filament in the current print; enabling nozzle clumping detection now may degrade print quality. Pass confirm: true to proceed.");
        return m.GetPrintOptions()->command_smart_nozzle_blob_detect_mode(mode);
    }
    if (o == "purify_air_at_print_end") { // PrintOptionsDialog.cpp:232 / 238
        const std::string v = value->is_string() ? lower(value->get<std::string>()) : value->is_boolean() ? (value->get<bool>() ? "inside" : "off") : "";
        int state = v == "off" ? 0 : v == "inside" ? 1 : v == "outside" ? 2 : -1;
        if (state < 0)
            fail("value must be off, inside or outside");
        return m.GetPrintOptions()->command_xcam_control_purify_air_at_print_end(state);
    }
    if (!value->is_boolean())
        fail("value must be true or false for " + o);
    const bool on = value->get<bool>();
    if (o == "first_layer_inspection") return m.command_xcam_control_first_layer_inspector(on, false);
    if (o == "buildplate_marker_detection") return m.command_xcam_control_buildplate_marker_detector(on);
    if (o == "auto_recovery_step_loss") return m.command_xcam_control_auto_recovery_step_loss(on);
    if (o == "prompt_sound") return m.command_xcam_control_allow_prompt_sound(on);
    if (o == "filament_tangle_detection") return m.command_xcam_control_filament_tangle_detect(on);
    if (o == "save_remote_print_file_to_storage") {
        m.command_set_save_remote_print_file_to_storage(on);
        return 0;
    }
    if (o == "buildplate_align_detection") return m.GetPrintOptions()->command_xcam_control_build_plate_align_detector(on);
    if (o == "fod_check") return m.GetPrintOptions()->command_xcam_control_fod_check(on);
    if (o == "displacement_detection") return m.GetPrintOptions()->command_xcam_control_displacement_detection(on);
    if (o == "snapshot") { // PrintOptionsDialog.cpp:285: enabling asks first (photos of parts go to the cloud)
        if (on && !opt_bool(args, "confirm").value_or(false))
            fail("Enabling snapshots makes the printer photograph printed parts and upload them to the cloud. Pass confirm: true to proceed.");
        return m.GetPrintOptions()->command_snapshot_control(on);
    }
    if (o == "camera_recording") { // CameraPopup.cpp:176
        if (!sd_card_normal(m))
            fail("Recording needs a storage card in the printer");
        return m.command_ipcam_record(on);
    }
    if (o == "timelapse") {
        if (!m.is_support_internal_timelapse && !sd_card_normal(m))
            fail("Timelapse needs a storage card in this printer");
        return m.command_ipcam_timelapse(on);
    }
    (void) info;
    fail("Unknown option \"" + o + "\"");
}

// ---- printer_calibration -----------------------------------------------------------------------------------

json pa_entry_json(const PACalibResult& r)
{
    return {{"cali_idx", r.cali_idx},
            {"name", r.name},
            {"filament_id", r.filament_id},
            {"setting_id", r.setting_id},
            {"k_value", r.k_value},
            {"n_coef", r.n_coef},
            {"nozzle_diameter", r.nozzle_diameter},
            {"extruder_id", r.extruder_id},
            {"nozzle_volume_type", volume_type_name(r.nozzle_volume_type)},
            {"ams_id", r.ams_id},
            {"slot_id", r.slot_id},
            {"tray_id", r.tray_id},
            {"confidence", r.confidence}};
}

float nozzle_diameter_arg(MachineObject& m, const json& args, int extruder)
{
    float d = float(opt_number(args, "nozzle_diameter", 0.1, 2.0).value_or(m.GetExtderSystem()->GetNozzleDiameter(extruder)));
    if (d <= 0.0f)
        fail("The printer has not reported its nozzle diameter; pass nozzle_diameter");
    return d;
}

int calibration_pa_set(MachineObject& m, const json& args)
{
    const json* entries = member(args, "entries");
    if (entries == nullptr || !entries->is_array() || entries->empty())
        fail("\"entries\" is required: a list of {name, filament_id, setting_id?, k, n?, nozzle_diameter?, extruder?, nozzle_volume_type?, cali_idx?}");
    std::vector<PACalibResult> values;
    for (const json& e : *entries) {
        PACalibResult r;
        r.name        = req_string(e, "name");
        r.filament_id = req_string(e, "filament_id");
        r.setting_id  = opt_string(e, "setting_id");
        r.k_value     = float(req_number(e, "k", 0.0, 2.0));
        r.n_coef      = float(opt_number(e, "n", 0.0, 10.0).value_or(0.0));
        r.extruder_id = opt_int(e, "extruder", 0, 15).value_or(0);
        r.nozzle_volume_type = volume_type_from(e);
        r.nozzle_diameter    = nozzle_diameter_arg(m, e, r.extruder_id);
        r.cali_idx           = opt_int(e, "cali_idx", -1, 100000).value_or(-1); // -1 adds a new profile
        r.tray_id            = -1;                                                // CaliHistoryDialog.cpp:489
        // CaliHistoryDialog.cpp:1010: a name is unique per filament/extruder/nozzle; replacing one needs a decision
        if (r.cali_idx < 0 && m.has_get_pa_calib_tab && !opt_bool(args, "overwrite").value_or(false)) {
            for (const PACalibResult& old : m.pa_calib_tab)
                if (old.name == r.name && old.filament_id == r.filament_id && old.extruder_id == r.extruder_id &&
                    old.nozzle_volume_type == r.nozzle_volume_type)
                    fail("A profile named \"" + r.name + "\" already exists for this filament and nozzle; pass overwrite: true to replace it (cali_idx " +
                         std::to_string(old.cali_idx) + ")");
        }
        values.push_back(std::move(r));
    }
    return m.command_set_pa_calibration(values, true); // CaliHistoryDialog.cpp:1030
}

int calibration_pa_delete(MachineObject& m, const json& args)
{
    PACalibIndexInfo info;
    info.cali_idx           = req_int(args, "cali_idx", 0, 100000);
    info.filament_id        = req_string(args, "filament_id");
    info.extruder_id        = opt_int(args, "extruder", 0, 15).value_or(0);
    info.nozzle_volume_type = volume_type_from(args);
    info.nozzle_diameter    = nozzle_diameter_arg(m, args, info.extruder_id);
    info.tray_id            = -1;
    return m.command_delete_pa_calibration(info); // CaliHistoryDialog.cpp:472
}

} // namespace

// ---- dispatchers ----------------------------------------------------------------------------------------------

int dispatch_control(MachineObject& m, const json& args, json* info)
{
    const std::string action = action_of(args, {"set_temp", "set_fan", "set_speed", "light", "home", "jog", "extrude", "select_extruder"});
    if (action == "set_temp") return control_set_temp(m, args, info);
    if (action == "set_fan") return control_set_fan(m, args, info);
    if (action == "set_speed") return control_set_speed(m, args);
    if (action == "light") return control_light(m, args, info);
    if (action == "home") {
        ui_require_not_printing(m, "Homing"); // StatusPanel.cpp:4163 (the Device tab's home button)
        return m.command_go_home();
    }
    if (action == "jog") return control_jog(m, args);
    if (action == "extrude") return control_extrude(m, args);
    return control_select_extruder(m, args);
}

int dispatch_ams(MachineObject& m, const json& args, json* info)
{
    const std::string action = action_of(args, {"set_tray", "reset_tray", "set_k", "refresh_rfid", "settings", "load", "unload", "dry_start",
                                                "dry_stop", "control", "calibrate", "reset"});
    if (action == "set_tray") return ams_set_tray(m, args, info);
    if (action == "reset_tray") return ams_reset_tray(m, args, info);
    if (action == "set_k") return select_k(m, resolve_slot(m, args), args, info);
    if (action == "refresh_rfid") return ams_refresh_rfid(m, args);
    if (action == "settings") return ams_settings(m, args);
    if (action == "load") return ams_load(m, args);
    if (action == "unload") return ams_unload(m, args);
    if (action == "dry_start") return ams_dry_start(m, args, info);
    if (action == "dry_stop") return ams_dry_stop(m, args);
    if (action == "control") return ams_control(m, args);
    if (action == "calibrate") return ams_calibrate(m, args);
    return ams_reset(m, args);
}

int dispatch_options(MachineObject& m, const json& args, json* info)
{
    const std::string action = action_of(args, {"get", "set"});
    if (action == "get")
        return 0;
    return options_set(m, args, info);
}

json options_state(MachineObject& m)
{
    json out = json::array();
    for (const OptionSpec& s : kOptions) {
        json e = {{"option", s.name}, {"supported", option_supported(m, s.name)}, {"value", option_value(m, s.name)}};
        if (s.sensitivity)
            e["sensitivity"] = sensitivity_of(m, s.name);
        if (std::string(s.name) == "camera_resolution")
            e["supported_values"] = m.camera_resolution_supported;
        out.push_back(std::move(e));
    }
    return out;
}

int dispatch_calibration(MachineObject& m, const json& args, json* info)
{
    const std::string action = req_string(args, "action");
    if (action == "auto_pa_start" || action == "flow_start" || action == "start")
        fail("Starting a calibration prints a test object; it is not available through this tool. Use the Calibration page in OrcaSlicer.");
    const std::vector<std::string> allowed = {"pa_list", "pa_set", "pa_select", "pa_delete", "auto_pa_result", "flow_result"};
    action_of(args, allowed);
    if (action == "pa_list") { // StatusPanel.cpp:3454 / CaliHistoryDialog.cpp:346
        PACalibExtruderInfo cali;
        const auto          extruder = opt_int(args, "extruder", 0, 15);
        cali.extruder_id             = extruder.value_or(0);
        cali.use_extruder_id         = extruder.has_value();
        cali.use_nozzle_volume_type  = false;
        cali.nozzle_volume_type      = nvtStandard;
        cali.nozzle_diameter         = nozzle_diameter_arg(m, args, cali.extruder_id);
        cali.filament_id             = opt_string(args, "filament_id");
        set_info(info, "nozzle_diameter", cali.nozzle_diameter);
        return m.command_get_pa_calibration_tab(cali);
    }
    if (action == "pa_set") return calibration_pa_set(m, args);
    if (action == "pa_select") return select_k(m, resolve_slot(m, args), [&] { json a = args; if (!a.contains("cali_idx")) fail("\"cali_idx\" is required"); return a; }(), info);
    if (action == "pa_delete") return calibration_pa_delete(m, args);
    if (action == "auto_pa_result") return m.command_get_pa_calibration_result(nozzle_diameter_arg(m, args, 0));
    return m.command_get_flow_ratio_calibration_result(nozzle_diameter_arg(m, args, 0));
}

json calibration_read_state(MachineObject& m, const std::string& action)
{
    json entries = json::array();
    if (action == "pa_list") {
        for (const PACalibResult& r : m.pa_calib_tab)
            entries.push_back(pa_entry_json(r));
        // the reply parser clears request_tab_from_bbs when it has handled the answer; it sets has_get_pa_calib_tab on success
        return {{"ready", !m.request_tab_from_bbs}, {"failed", !m.request_tab_from_bbs && !m.has_get_pa_calib_tab}, {"entries", entries}};
    }
    if (action == "auto_pa_result") {
        for (const PACalibResult& r : m.pa_calib_results)
            entries.push_back(pa_entry_json(r));
        return {{"ready", m.get_pa_calib_result}, {"failed", false}, {"entries", entries}};
    }
    for (const FlowRatioCalibResult& r : m.flow_ratio_results)
        entries.push_back({{"tray_id", r.tray_id},
                           {"filament_id", r.filament_id},
                           {"setting_id", r.setting_id},
                           {"nozzle_diameter", r.nozzle_diameter},
                           {"flow_ratio", r.flow_ratio},
                           {"confidence", r.confidence}});
    return {{"ready", m.get_flow_calib_result}, {"failed", false}, {"entries", entries}};
}

// ---- tools --------------------------------------------------------------------------------------------------------

namespace {

// Fills tray_info_idx / setting_id / tray_type / temperatures of a set_tray call from the filament preset the
// caller named (filament_preset) or identified (tray_info_idx), as AMSMaterialsSetting::on_select_filament and
// sCheckFilamentInfo (AMSMaterialsSetting.cpp:742, 1575) do. Explicit values win. UI thread.
json resolve_filament(MachineObject& m, const json& args, json* info)
{
    json out = args;
    PresetBundle* bundle = wxGetApp().preset_bundle;
    const std::string preset_name = opt_string(args, "filament_preset");
    const std::string idx_arg     = opt_string(args, "tray_info_idx");
    if (preset_name.empty() && idx_arg.empty())
        fail("Pass filament_preset (a filament preset name) or tray_info_idx");
    const Preset* preset = nullptr;
    if (bundle != nullptr) {
        if (!preset_name.empty()) {
            preset = bundle->filaments.find_preset(preset_name, false);
            if (preset == nullptr)
                fail("No filament preset named \"" + preset_name + "\"");
        } else {
            for (auto it = bundle->filaments.begin(); it != bundle->filaments.end(); ++it)
                if (it->filament_id == idx_arg) {
                    preset = &*it;
                    break;
                }
        }
    }
    if (preset != nullptr) {
        if (preset->filament_id.empty())
            fail("Filament preset \"" + preset->name + "\" has no filament id, so the AMS cannot be given it; pick a system preset");
        if (!out.contains("tray_info_idx")) out["tray_info_idx"] = preset->filament_id;
        if (!out.contains("setting_id")) out["setting_id"] = preset->setting_id;
        if (!out.contains("tray_type")) {
            std::string display;
            out["tray_type"] = const_cast<Preset*>(preset)->config.get_filament_type(display);
        }
        // index 0 of the preset's per-variant ranges (the dialog picks the printer's nozzle variant)
        if (!out.contains("nozzle_temp_min"))
            if (auto* o = dynamic_cast<const ConfigOptionInts*>(preset->config.option("nozzle_temperature_range_low")); o && !o->values.empty())
                out["nozzle_temp_min"] = o->get_at(0);
        if (!out.contains("nozzle_temp_max"))
            if (auto* o = dynamic_cast<const ConfigOptionInts*>(preset->config.option("nozzle_temperature_range_high")); o && !o->values.empty())
                out["nozzle_temp_max"] = o->get_at(0);
        set_info(info, "filament_preset", preset->name);

        // the dialog's blacklist check (AMSMaterialsSetting.cpp:782), without its per-nozzle dimensions
        if (wxGetApp().app_config->get("skip_ams_blacklist_check") != "true") {
            DevFilaBlacklist::CheckFilamentInfo check;
            check.dev_id    = m.get_dev_id();
            check.model_id  = m.printer_type;
            check.fila_id   = preset->filament_id;
            check.fila_type = out.value("tray_type", std::string());
            check.fila_name = preset->name;
            check.has_filament_switch = m.GetFilaSwitch()->IsInstalled();
            check.ams_id  = out.value("ams_id", 0);
            check.slot_id = out.value("slot_id", 0);
            if (auto* v = dynamic_cast<const ConfigOptionStrings*>(preset->config.option("filament_vendor")); v && !v->values.empty())
                check.fila_vendor = v->values[0];
            const auto result = DevFilaBlacklist::check_filaments_in_blacklist(check);
            if (const auto items = result.get_items_by_action("prohibition"); !items.empty()) {
                json msgs = json::array();
                for (const auto& i : items)
                    msgs.push_back(i.info_msg.ToStdString());
                fail("This filament is prohibited on this slot", {{"reasons", msgs}});
            }
            if (const auto items = result.get_items_by_action("warning"); !items.empty()) {
                json msgs = json::array();
                for (const auto& i : items)
                    msgs.push_back(i.info_msg.ToStdString());
                set_info(info, "warnings", msgs);
            }
        }
    }
    return out;
}

using Dispatcher = int (*)(MachineObject&, const json&, json*);

ToolResult run_action(const json& args, ToolContext& ctx, const char* tool, Dispatcher dispatch, bool resolve_tray_filament = false)
{
    const bool          dry_run = arg_bool(args, "dry_run", false);
    const PrinterTarget target  = resolve_printer(ctx, args, true, true);
    json                info    = json::object();
    json                result  = run_printer_command(ctx, target.dev_id, dry_run, [&](MachineObject& m) {
        json eff = args;
        if (resolve_tray_filament && args.value("action", std::string()) == "set_tray")
            eff = resolve_filament(m, args, &info);
        return dispatch(m, eff, &info);
    });
    const int code = result.value("code", 0);
    if (!dry_run && code != 0)
        throw ToolFailure("printer refused/failed: code " + std::to_string(code), {{"code", code}, {"action", args.value("action", std::string())}});
    result["action"] = args.value("action", std::string());
    result["dev_id"] = target.dev_id;
    if (target.selected_now)
        result["selected_printer"] = true;
    for (auto it = info.begin(); it != info.end(); ++it)
        result[it.key()] = it.value();
    (void) tool;
    settle(ctx);
    return ToolResult::ok(std::move(result));
}

json action_schema_props(const std::vector<std::string>& actions)
{
    return enum_prop(actions, "What to do.");
}

} // namespace

void register_printer_ctrl_tools(McpService& service)
{
    service.registry().add(mutate_tool(
        "printer_control", L("Control the printer"),
        "Heat, fans, speed, light and motion on the selected (or dev_id) Bambu printer, as the Device tab does. Actions: set_temp {target: nozzle|bed|chamber, "
        "value C (0 = off), extruder? (0 right/main, 1 left)}; set_fan {fan: part|aux|chamber, percent 0-100, in steps of 10}; set_speed {level: silent|standard|sport|ludicrous or 1-4} "
        "(printing only); light {on}; home; jog {axis: X|Y|Z|E, distance mm +-100, speed? mm/min} (positive X is right, Y back, Z moves the bed down, E extrudes; "
        "some printers move in exact steps of 1 or 10 mm only); extrude {length mm, negative retracts; nozzle at least 170 C}; select_extruder {extruder 0|1} "
        "(dual-nozzle printers). Temperatures, fans, speed and the light work during a print; home, jog, extrude and select_extruder are refused while printing. "
        "Nothing acknowledges: re-read printer_status. dry_run returns the exact messages without sending. A printer error reply may open a dialog.",
        object_schema({{"action", action_schema_props({"set_temp", "set_fan", "set_speed", "light", "home", "jog", "extrude", "select_extruder"})},
                       {"dev_id", dev_id_prop()},
                       {"dry_run", dry_run_prop()},
                       {"target", enum_prop({"nozzle", "bed", "chamber"}, "set_temp: what to heat.")},
                       {"value", prop("integer", "set_temp: target in C.")},
                       {"extruder", prop("integer", "set_temp / extrude / select_extruder: extruder index (0 right/main, 1 left).")},
                       {"fan", enum_prop({"part", "aux", "chamber"}, "set_fan: which fan.")},
                       {"percent", prop("integer", "set_fan: 0-100.")},
                       {"level", {{"type", {"string", "integer"}}, {"description", "set_speed: silent|standard|sport|ludicrous or 1-4."}}},
                       {"on", prop("boolean", "light: on or off.")},
                       {"axis", enum_prop({"X", "Y", "Z", "E"}, "jog: axis.")},
                       {"distance", prop("number", "jog: mm, signed, at most 100.")},
                       {"speed", prop("integer", "jog / extrude: mm/min (defaults 3000 for X/Y, 900 otherwise).")},
                       {"length", prop("number", "extrude: mm, negative retracts.")}},
                      {"action"}),
        [](const json& args, ToolContext& ctx) { return run_action(args, ctx, "printer_control", &dispatch_control); }, /*destructive*/ true));

    service.registry().add(mutate_tool(
        "printer_ams", L("Manage the AMS"),
        "AMS and external spool management on the selected (or dev_id) Bambu printer. ams_id is the AMS unit (0-3; AMS-HT 128+) or 254/255 for the left/right external "
        "spool (slot_id 0). Actions: set_tray {ams_id, slot_id, filament_preset (name) or tray_info_idx, color #RRGGBB[AA], nozzle_temp_min/max (default from the preset), "
        "colors?[]} (not for Bambu RFID spools); reset_tray {ams_id, slot_id}; set_k {ams_id, slot_id, cali_idx (printers with K profiles; -1 = default) or k}; "
        "refresh_rfid {ams_id, slot_id}; settings {insert_read?, startup_read?, remain_detect?, auto_refill?, air_print_detect?}; load {ams_id, slot_id (or tray_index), "
        "extruder? (Filament Track Switch only)}; unload {extruder?}; dry_start {ams_id, hours 1-24, temperature C, filament_type?, rotate_tray?}; dry_stop {ams_id}; "
        "control {cmd: resume|reset|pause|done|abort}; calibrate {ams_id}; reset {confirm: true} (resets every AMS id). load, unload, calibrate, reset and refresh_rfid are "
        "refused while printing. Read the state with printer_status. dry_run returns the exact messages without sending.",
        object_schema({{"action", action_schema_props({"set_tray", "reset_tray", "set_k", "refresh_rfid", "settings", "load", "unload", "dry_start", "dry_stop", "control", "calibrate", "reset"})},
                       {"dev_id", dev_id_prop()},
                       {"dry_run", dry_run_prop()},
                       {"ams_id", prop("integer", "AMS unit id, or 254/255 for an external spool.")},
                       {"slot_id", prop("integer", "Slot 0-3.")},
                       {"tray_index", prop("integer", "load: tray 0-15 as ams_id * 4 + slot_id (instead of ams_id and slot_id).")},
                       {"filament_preset", prop("string", "set_tray: filament preset name.")},
                       {"tray_info_idx", prop("string", "set_tray: Bambu filament id (instead of filament_preset).")},
                       {"setting_id", prop("string", "set_tray: filament setting id (default from the preset).")},
                       {"tray_type", prop("string", "set_tray: filament type such as PLA (default from the preset).")},
                       {"color", prop("string", "set_tray: #RRGGBB or #RRGGBBAA.")},
                       {"colors", array_prop(prop("string", "colour"), "set_tray: up to 4 colours for a multi-colour spool (supporting firmware).")},
                       {"color_type", prop("integer", "set_tray: 0 or 1 for several colours.")},
                       {"nozzle_temp_min", prop("integer", "set_tray: C.")},
                       {"nozzle_temp_max", prop("integer", "set_tray: C.")},
                       {"cali_idx", prop("integer", "set_k: K profile index.")},
                       {"filament_id", prop("string", "set_k: profile's filament id (default the slot's).")},
                       {"k", prop("number", "set_k: K value for printers without profiles.")},
                       {"n", prop("number", "set_k: N value (old protocol).")},
                       {"insert_read", prop("boolean", "settings: read spool on insert.")},
                       {"startup_read", prop("boolean", "settings: read spools at power up.")},
                       {"remain_detect", prop("boolean", "settings: detect remaining filament.")},
                       {"auto_refill", prop("boolean", "settings: switch to a backup spool.")},
                       {"air_print_detect", prop("boolean", "settings: air-printing detection.")},
                       {"extruder", prop("integer", "load / unload: extruder 0 right/main, 1 left.")},
                       {"hours", prop("integer", "dry_start: 1-24.")},
                       {"temperature", prop("integer", "dry_start: C (AMS 2: 45-65, AMS-S: 45-85).")},
                       {"filament_type", prop("string", "dry_start: filament type (default the first loaded spool's).")},
                       {"cooling_temp", prop("integer", "dry_start: C, default 50.")},
                       {"rotate_tray", prop("boolean", "dry_start: rotate the spools.")},
                       {"cmd", enum_prop({"resume", "reset", "pause", "done", "abort"}, "control: AMS command.")},
                       {"confirm", prop("boolean", "reset: confirm.")},
                       {"override_print_lock", prop("boolean", "set_tray / reset_tray: send during a print even though the printer does not advertise in-print slot edits (experimental).")}},
                      {"action"}),
        [](const json& args, ToolContext& ctx) { return run_action(args, ctx, "printer_ams", &dispatch_ams, true); }, /*destructive*/ true));

    service.registry().add(mutate_tool(
        "printer_options", L("Printer options"),
        "Printer print options. action \"get\" lists every option with whether this printer supports it and its value (AI detections with sensitivity, first layer "
        "inspection, build plate detection, auto recovery, prompt sound, filament tangle, save print files to storage, snapshots, purify air, nozzle clumping, camera "
        "recording, timelapse, camera resolution). action \"set\" {option, value (boolean; off|on|auto for smart_nozzle_blob_detection; off|inside|outside for "
        "purify_air_at_print_end; \"720p\"/\"1080p\" for camera_resolution), sensitivity? low|medium|high for the AI detections, confirm? for snapshot and blob detection "
        "prompts} refuses options the printer does not support. dry_run returns the exact messages without sending.",
        object_schema({{"action", action_schema_props({"get", "set"})},
                       {"dev_id", dev_id_prop()},
                       {"dry_run", dry_run_prop()},
                       {"option", prop("string", "set: option name from get.")},
                       {"value", {{"type", {"boolean", "string"}}, {"description", "set: the new value."}}},
                       {"sensitivity", enum_prop({"low", "medium", "high"}, "set: AI detection sensitivity.")},
                       {"confirm", prop("boolean", "set: confirm snapshot / blob detection prompts.")}},
                      {"action"}),
        [](const json& args, ToolContext& ctx) {
            if (req_string(args, "action") == "get") {
                const PrinterTarget target = resolve_printer(ctx, args, true, true);
                json options = ctx.run_ui([&]() -> json { return options_state(ui_printer(target.dev_id)); });
                return ToolResult::ok({{"action", "get"}, {"dev_id", target.dev_id}, {"options", std::move(options)}});
            }
            return run_action(args, ctx, "printer_options", &dispatch_options);
        }));

    service.registry().add(mutate_tool(
        "printer_calibration", L("Printer calibration data"),
        "Pressure advance (K) profiles and calibration results stored on the printer. Actions: pa_list {nozzle_diameter?, filament_id?, extruder?} lists the stored "
        "profiles [{cali_idx, name, filament_id, k_value, ...}] (waits up to 10 s for the reply); pa_set {entries: [{name, filament_id, setting_id?, k, nozzle_diameter?, "
        "extruder?, nozzle_volume_type?, cali_idx? (-1 adds)}], overwrite?} adds or edits profiles; pa_select {ams_id, slot_id, cali_idx} assigns a profile to a slot; "
        "pa_delete {cali_idx, filament_id, nozzle_diameter?, extruder?} deletes one (destructive); auto_pa_result and flow_result {nozzle_diameter?} fetch the last "
        "automatic calibration's results. Starting a calibration (it prints a test) is not available. dry_run returns the exact messages without sending.",
        object_schema({{"action", action_schema_props({"pa_list", "pa_set", "pa_select", "pa_delete", "auto_pa_result", "flow_result"})},
                       {"dev_id", dev_id_prop()},
                       {"dry_run", dry_run_prop()},
                       {"nozzle_diameter", prop("number", "mm; default the printer's nozzle.")},
                       {"filament_id", prop("string", "Filament id.")},
                       {"extruder", prop("integer", "Extruder index.")},
                       {"nozzle_volume_type", enum_prop({"standard", "high_flow", "hybrid", "tpu_high_flow", "e3d_high_flow"}, "Nozzle flow type, default standard.")},
                       {"entries", array_prop({{"type", "object"}}, "pa_set: profiles to write.")},
                       {"overwrite", prop("boolean", "pa_set: replace a same-named profile.")},
                       {"ams_id", prop("integer", "pa_select: AMS unit id.")},
                       {"slot_id", prop("integer", "pa_select: slot.")},
                       {"cali_idx", prop("integer", "pa_select / pa_delete: profile index.")}},
                      {"action"}),
        [](const json& args, ToolContext& ctx) {
            const std::string action  = req_string(args, "action");
            const bool        is_read = action == "pa_list" || action == "auto_pa_result" || action == "flow_result";
            if (!is_read)
                return run_action(args, ctx, "printer_calibration", &dispatch_calibration);
            const bool          dry_run = arg_bool(args, "dry_run", false);
            const PrinterTarget target  = resolve_printer(ctx, args, true, true);
            json                info    = json::object();
            json                result  = run_printer_command(ctx, target.dev_id, dry_run, [&](MachineObject& m) {
                if (!dry_run) { // forget the previous answer so the poll sees the new one (CalibUtils / DeviceManager.cpp:2164)
                    if (action == "auto_pa_result") m.reset_pa_cali_result();
                    else if (action == "flow_result") m.reset_flow_rate_cali_result();
                }
                return dispatch_calibration(m, args, &info);
            });
            result["action"] = action;
            result["dev_id"] = target.dev_id;
            for (auto it = info.begin(); it != info.end(); ++it)
                result[it.key()] = it.value();
            if (dry_run)
                return ToolResult::ok(std::move(result));
            const int code = result.value("code", 0);
            if (code != 0)
                throw ToolFailure("printer refused/failed: code " + std::to_string(code), {{"code", code}});
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            for (;;) {
                const json state = ctx.observe_ui([&]() -> json { return calibration_read_state(ui_printer(target.dev_id), action); });
                if (state["ready"].get<bool>()) {
                    if (state["failed"].get<bool>())
                        throw ToolFailure("The printer answered that it has no calibration data for this request");
                    result["entries"] = state["entries"];
                    return ToolResult::ok(std::move(result));
                }
                if (ctx.cancelled())
                    throw ToolFailure("Cancelled while waiting for the printer's reply");
                if (std::chrono::steady_clock::now() >= deadline)
                    throw ToolFailure("The printer did not answer within 10 s", {{"sent", true}});
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
        },
        /*destructive*/ true));
}

}}} // namespace Slic3r::GUI::Mcp
