#pragma once

// printer_list / printer_status / printer_job / printer_hms building blocks (McpToolsPrinter.cpp).
// These are the testable pieces of the printer MCP tools (fork): they take a MachineObject and
// return JSON or send commands, with no wx widgets and no ToolContext. UI thread only (the callers
// run them inside ToolContext::run_ui).
// Design: C:/claude-code/orca_mcp/docs/specs/2026-11-02-printer-mcp-design.md

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
class MachineObject;
namespace GUI { namespace Mcp {

// Resolves the text of an HMS long error code (HMSQuery), "" when unknown. Supplied by the tool,
// since the lookup needs the app; builders called without one leave the text empty.
using HmsTextFn = std::function<std::string(const std::string& long_error_code)>;

// The section names printer_status accepts, in the order they are emitted.
const std::vector<std::string>& printer_status_section_names();

// One printer in printer_list: {dev_id, name, model, printer_type, connection ("lan"|"cloud"),
// online, connected, selected, info_ready, has_access_code (never the code), ip, printing,
// print_status, progress, printer_agent_id}.
nlohmann::json printer_summary_json(MachineObject& m, bool selected);

// printer_status for one printer. `sections` empty = all of printer_status_section_names(); an
// unknown name throws ToolFailure listing the valid ones. A missing sub-object makes its section
// null. The result is passed through redact().
nlohmann::json printer_status_json(MachineObject& m, const std::vector<std::string>& sections, const HmsTextFn& hms_text = {});

// "left" / "right" for the two extruders of a dual-nozzle printer (device extruder 0 = MAIN = right,
// 1 = DEPUTY = left), "main" on a single-nozzle printer, "switch" for an AMS bound through a
// Filament Track Switch (feeds both).
std::string printer_side_name(const MachineObject& m, int extruder_id);

// The global tray index of an AMS slot, as the mapping code (DevMapping.cpp ams_filament_mapping)
// and the AMS/tray bit masks use it: AMS / AMS-Lite / AMS 2 Pro ams*4+slot, AMS-HT (128+) ams+slot,
// AMS-Lite-mixed 24+slot.
int printer_tray_index(bool lite_mixed, bool single_slot_unit, int ams_id, int slot_id);

// ---- printer_job ----------------------------------------------------------------------------

// ToolFailure unless `action` (pause | resume | stop | skip_objects) is possible in the printer's
// current state; the message names print_status. For skip_objects `object_ids` must be non-empty.
void check_printer_job_allowed(MachineObject& m, const std::string& action, const std::vector<int>& object_ids);

// Sends the command for a job action as StatusPanel / PartSkipDialog do and returns its result code.
// Unknown action: ToolFailure. Call check_printer_job_allowed first.
int send_printer_job_command(MachineObject& m, const std::string& action, const std::vector<int>& object_ids);

// ---- printer_hms ----------------------------------------------------------------------------

// ToolFailure unless HMS `action` (resume | ignore | stop | clear | stop_buzzer) is possible: all but
// stop_buzzer need an active print error (MachineObject::print_error != 0).
void check_printer_hms_allowed(MachineObject& m, const std::string& action);

// Sends the command DeviceErrorDialog sends for the printer's current print error and returns its
// result code (the first non-zero one when several are sent). `no_reminder` makes ignore the idle
// "don't remind me next time" variant (command_hms_idle_ignore).
int send_printer_hms_command(MachineObject& m, const std::string& action, bool no_reminder);

}}} // namespace Slic3r::GUI::Mcp
