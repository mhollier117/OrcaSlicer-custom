#include "McpPrinterCommon.hpp"

#include "McpTypes.hpp"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"
#include "slic3r/Utils/PublishCapture.hpp"

#include <boost/algorithm/string/case_conv.hpp>

#include <chrono>
#include <string>
#include <thread>
#include <utility>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

DeviceManager& ui_device_manager()
{
    DeviceManager* dev = wxGetApp().getDeviceManager();
    if (dev == nullptr)
        throw ToolFailure("The printer connection layer is not available (no network plugin loaded)");
    return *dev;
}

MachineObject* ui_selected_printer() { return ui_device_manager().get_selected_machine(); }

MachineObject& ui_printer(const std::string& dev_id)
{
    DeviceManager& dev = ui_device_manager();
    if (MachineObject* selected = dev.get_selected_machine(); selected != nullptr && selected->get_dev_id() == dev_id)
        return *selected;
    if (MachineObject* mine = dev.get_my_machine(dev_id))
        return *mine;
    if (MachineObject* local = dev.get_local_machine(dev_id))
        return *local;
    for (const auto& [id, obj] : dev.get_user_machinelist())
        if (id == dev_id && obj != nullptr)
            return *obj;
    throw ToolFailure("No printer with dev_id \"" + dev_id + "\"; printer_list lists them", {{"dev_id", dev_id}});
}

PrinterTarget resolve_printer(ToolContext& ctx, const json& args, bool select, bool wait_ready, int timeout_s)
{
    const std::string wanted = args.contains("dev_id") && args["dev_id"].is_string() ? args["dev_id"].get<std::string>() : std::string();
    PrinterTarget     target;
    const json        picked = ctx.run_ui([&]() -> json {
        DeviceManager& dev      = ui_device_manager();
        MachineObject* selected = dev.get_selected_machine();
        if (wanted.empty()) {
            if (selected == nullptr)
                throw ToolFailure("No printer is selected; pass dev_id or select one with printer_list {action: \"select\"}");
            return {{"dev_id", selected->get_dev_id()}, {"selected_now", false}};
        }
        MachineObject& printer = ui_printer(wanted); // throws for an unknown id
        if (select && (selected == nullptr || selected->get_dev_id() != wanted)) {
            if (!dev.set_selected_machine(wanted))
                throw ToolFailure("Could not select printer \"" + printer.get_dev_name() + "\"", {{"dev_id", wanted}});
            return {{"dev_id", wanted}, {"selected_now", true}};
        }
        return {{"dev_id", wanted}, {"selected_now", false}};
    });
    target.dev_id       = picked["dev_id"].get<std::string>();
    target.selected_now = picked["selected_now"].get<bool>();
    if (!wait_ready)
        return target;

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    for (;;) {
        const bool ready = ctx.observe_ui([&]() -> json { return ui_printer(target.dev_id).is_info_ready(); }).get<bool>();
        if (ready)
            return target;
        if (ctx.cancelled())
            throw ToolFailure("Cancelled while waiting for the printer");
        if (std::chrono::steady_clock::now() >= deadline)
            throw ToolFailure("The printer did not report its state within " + std::to_string(timeout_s) +
                                  " s (offline, or not connected in LAN mode without an access code?)",
                              {{"dev_id", target.dev_id}});
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

bool ui_supports_dry_run(MachineObject& printer)
{
    if (!printer.printer_agent_id.empty())
        return printer.printer_agent_id == BBL_PRINTER_AGENT_ID;
    return ui_device_manager().get_current_printer_agent_id() == BBL_PRINTER_AGENT_ID;
}

json run_printer_command(ToolContext& ctx, const std::string& dev_id, bool dry_run, const std::function<int(MachineObject&)>& send)
{
    return ctx.run_ui([&]() -> json {
        MachineObject& printer = ui_printer(dev_id);
        if (!dry_run) {
            const int rc = send(printer);
            return {{"sent", rc == 0}, {"code", rc}};
        }
        if (!ui_supports_dry_run(printer))
            throw ToolFailure("dry_run is supported only for Bambu Lab printers (the bbl printer agent)");
        ScopedPublishCapture capture;
        const int            rc = send(printer);
        json                 would_send = json::array();
        for (const PublishCapture::Message& m : capture.messages()) {
            json payload = json::parse(m.json, nullptr, false);
            if (payload.is_discarded())
                payload = m.json;
            would_send.push_back({{"lan", m.lan}, {"payload", redact(std::move(payload))}});
        }
        return {{"dry_run", true}, {"code", rc}, {"would_send", std::move(would_send)}};
    });
}

void ui_require_not_printing(MachineObject& printer, const std::string& what)
{
    if (printer.is_in_printing() || printer.is_in_printing_pause())
        throw ToolFailure(what + " is not allowed while the printer is printing");
}

namespace {

bool is_secret_key(const std::string& key)
{
    const std::string k = boost::algorithm::to_lower_copy(key);
    return k == "password" || k == "passwd" || k == "access_code" || k == "accesscode" || k == "dev_access_code" || k == "token" ||
           k == "ticket" || k == "url" || k == "camera_url" || k == "ftp_password";
}

bool looks_secret_value(const std::string& value)
{
    return value.find("passwd=") != std::string::npos || value.find("access_code=") != std::string::npos;
}

} // namespace

json redact(json j)
{
    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (is_secret_key(it.key()))
                it.value() = "<redacted>";
            else
                it.value() = redact(std::move(it.value()));
        }
    } else if (j.is_array()) {
        for (json& item : j)
            item = redact(std::move(item));
    } else if (j.is_string() && looks_secret_value(j.get<std::string>())) {
        j = "<redacted>";
    }
    return j;
}

json dev_id_prop()
{
    return {{"type", "string"},
            {"description", "Printer id (dev_id from printer_list). Default: the selected printer. A printer that is not selected is "
                            "selected first, as the Device tab does."}};
}

json dry_run_prop()
{
    return {{"type", "boolean"},
            {"description", "Build and return exactly what would be sent to the printer, and send nothing. Bambu Lab printers only."}};
}

}}} // namespace Slic3r::GUI::Mcp
