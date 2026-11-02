#pragma once

// printer_control / printer_ams / printer_options / printer_calibration action dispatchers (McpToolsPrinterCtrl.cpp).
// Testable pieces of the printer MCP tools (fork): each takes a MachineObject and the tool's arguments, validates
// them (ToolFailure for bad input, an unknown action, or a control the printer does not support or the GUI
// would not enable), then issues the same MachineObject / Dev* commands the GUI does and returns the first
// non-zero command result code (0 = sent). They never touch wxGetApp(), so they run without an MCP context.
// Optional `info` receives extra fields for the tool's reply (warnings, what was resolved).
//
// Threading: UI thread only (MachineObject access), like everything in the printer tools.

#include <nlohmann/json.hpp>

#include <string>

namespace Slic3r {
class MachineObject;
namespace GUI { namespace Mcp {

// printer_control: set_temp, set_fan, set_speed, light, home, jog, extrude, select_extruder.
int dispatch_control(MachineObject& printer, const nlohmann::json& args, nlohmann::json* info = nullptr);

// printer_ams: set_tray, reset_tray, set_k, refresh_rfid, settings, load, unload, dry_start, dry_stop, control,
// calibrate, reset. set_tray takes the filament already resolved (tray_info_idx, setting_id, tray_type,
// nozzle_temp_min/max); the tool resolves a filament preset name into those before calling.
int dispatch_ams(MachineObject& printer, const nlohmann::json& args, nlohmann::json* info = nullptr);

// printer_options: action "set" (get is options_state() and sends nothing: dispatch_options returns 0 for it).
int dispatch_options(MachineObject& printer, const nlohmann::json& args, nlohmann::json* info = nullptr);

// printer_calibration: pa_list / auto_pa_result / flow_result send their request (the tool then polls
// calibration_read_state()); pa_set, pa_select, pa_delete send their edit. Starting a calibration print is refused.
int dispatch_calibration(MachineObject& printer, const nlohmann::json& args, nlohmann::json* info = nullptr);

// printer_options get: [{option, supported, value, sensitivity?}] plus the camera state, read only.
nlohmann::json options_state(MachineObject& printer);

// State of a calibration read after its request was sent: {ready, failed, entries: [...]}. `action` is pa_list,
// auto_pa_result or flow_result. An entry of pa_list: {cali_idx, name, filament_id, setting_id, k_value, n_coef,
// nozzle_diameter, extruder_id, nozzle_volume_type, ...}.
nlohmann::json calibration_read_state(MachineObject& printer, const std::string& action);

}}} // namespace Slic3r::GUI::Mcp
