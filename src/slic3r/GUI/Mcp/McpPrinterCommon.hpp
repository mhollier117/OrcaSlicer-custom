#pragma once

// Shared helpers for the printer MCP tools (fork): finding the printer a call acts on, running
// printer commands for real or as a dry run, guards, redaction and common schema properties.
// Design: C:/claude-code/orca_mcp/docs/specs/2026-11-02-printer-mcp-design.md
//
// Threading: every MachineObject / DeviceManager access happens on the UI thread (inside
// ToolContext::run_ui). Functions named ui_* must only be called there. resolve_printer() is the
// exception: it is called on the handler thread and polls the UI thread itself.

#include <nlohmann/json.hpp>

#include <functional>
#include <string>

namespace Slic3r {
class DeviceManager;
class MachineObject;
namespace GUI { namespace Mcp {

class ToolContext;

// The app's DeviceManager; ToolFailure while the network layer is not up. UI thread.
DeviceManager& ui_device_manager();

// The printer with this dev_id among the user's and the LAN printers; ToolFailure when unknown. UI thread.
MachineObject& ui_printer(const std::string& dev_id);

// The selected printer, or nullptr. UI thread.
MachineObject* ui_selected_printer();

struct PrinterTarget
{
    std::string dev_id;
    bool        selected_now = false; // this call selected it (a side effect the reply reports)
};

// The printer a call acts on: args["dev_id"], else the selected printer.
//  - `select`: a dev_id that is not the selected printer is selected first, as the Device tab does
//    (full AMS / nozzle state exists only for the selected printer).
//  - `wait_ready`: then waits until MachineObject::is_info_ready() (polling the UI thread every
//    250 ms, honouring ctx.cancelled()), at most `timeout_s`.
// Throws ToolFailure: no dev_id and nothing selected, unknown printer, not ready in time.
// Handler thread (NOT inside run_ui).
PrinterTarget resolve_printer(ToolContext& ctx, const nlohmann::json& args, bool select, bool wait_ready, int timeout_s = 20);

// Whether dry runs are possible for this printer: its messages go through BBLPrinterAgent (the
// Bambu plugin or a drop-in such as open_bambu_networking), whose send paths PublishCapture
// intercepts. UI thread.
bool ui_supports_dry_run(MachineObject& printer);

// Runs `send` (which issues MachineObject command_* calls and returns their result code) on the UI
// thread for the printer `dev_id`.
//  - dry_run: inside a ScopedPublishCapture, so nothing is sent. Refused (ToolFailure) when
//    !ui_supports_dry_run. Returns {"dry_run": true, "code": rc, "would_send": [{"lan": bool,
//    "payload": <parsed json, redacted>}...]}.
//  - otherwise: sends and returns {"sent": rc == 0, "code": rc}. A non-zero rc is NOT thrown; the
//    caller decides (most tools throw ToolFailure with the code).
// There are no acks: agents re-read printer_status afterwards.
nlohmann::json run_printer_command(ToolContext& ctx, const std::string& dev_id, bool dry_run,
                                   const std::function<int(MachineObject&)>& send);

// ToolFailure("<what> is not allowed while the printer is printing") when the printer is printing
// or paused. UI thread.
void ui_require_not_printing(MachineObject& printer, const std::string& what);

// Copy of `j` with secrets removed: any key named (case-insensitively) password, passwd,
// access_code, accesscode, dev_access_code, token, ticket, url, camera_url, or containing
// "passwd=" / "access" in a string value of a url-like key, is replaced by "<redacted>".
// Applied recursively to objects and arrays.
nlohmann::json redact(nlohmann::json j);

// Schema properties shared by the printer tools.
nlohmann::json dev_id_prop();  // {"type":"string", description: printer id; default the selected printer}
nlohmann::json dry_run_prop(); // {"type":"boolean", description: build and return what would be sent, send nothing}

}} // namespace GUI::Mcp
} // namespace Slic3r
