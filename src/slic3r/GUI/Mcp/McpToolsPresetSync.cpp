// preset_sync MCP tool (fork): status and control of the Bambu cloud preset sync (BblPresetSyncController).

#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "slic3r/GUI/BblPresetSyncController.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/Utils/BblPresetSync/Types.hpp"

#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

namespace sync = Slic3r::BblPresetSync;

BblPresetSyncController& controller()
{
    BblPresetSyncController* c = wxGetApp().bbl_preset_sync();
    if (c == nullptr) throw ToolFailure("Preset sync is not running (sign in and turn on \"Auto sync user presets\")");
    return *c;
}

std::vector<std::string> string_list(const json& args, const char* key)
{
    std::vector<std::string> out;
    auto                     it = args.find(key);
    if (it != args.end() && it->is_array())
        for (const json& v : *it)
            if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

std::string result_name(sync::OpResult r)
{
    switch (r) {
    case sync::OpResult::Applied: return "applied";
    case sync::OpResult::Postponed: return "postponed";
    case sync::OpResult::Unresolvable: return "unresolvable";
    default: return "failed";
    }
}

json convert_json(const sync::ConvertResult& r)
{
    return {{"result", result_name(r.outcome.result)},
            {"reason", r.outcome.reason},
            {"from_filament_id", r.from_filament_id},
            {"to_filament_id", r.to_filament_id}};
}

json run_action(const json& args)
{
    const std::string action = arg_string(args, "action");
    BblPresetSyncController& c = controller();

    if (action == "status") return c.status_json();

    if (action == "sync_now") {
        c.request_sync();
        wxGetApp().request_user_preset_push();
        return {{"requested", true}};
    }

    if (action == "confirm_first_run") {
        if (!args.contains("accept")) throw ToolFailure("confirm_first_run needs accept");
        const bool accept = arg_bool(args, "accept", false);
        json       results = json::array();
        for (const std::string& name : string_list(args, "convert")) {
            json row = convert_json(c.convert(name));
            row["name"] = name;
            results.push_back({{"name", name},
                               {"result", row["result"]},
                               {"reason", row["reason"]},
                               {"to_filament_id", row["to_filament_id"]}});
        }
        for (const std::string& name : string_list(args, "decline")) {
            c.mirror().set_declined(sync::Key{sync::PresetType::Filament, name});
            results.push_back({{"name", name}, {"result", "declined"}, {"reason", ""}, {"to_filament_id", ""}});
        }
        c.mirror().confirm_first_run(accept);
        if (accept) c.request_sync();
        return {{"accepted", accept}, {"results", std::move(results)}};
    }

    if (action == "convert") {
        const std::string name = arg_string(args, "name");
        if (name.empty()) throw ToolFailure("convert needs name");
        return convert_json(c.convert(name));
    }

    if (action == "forget") {
        const auto type = sync::parse_type(arg_string(args, "type"));
        const std::string name = arg_string(args, "name");
        if (!type || name.empty()) throw ToolFailure("forget needs type (print|filament|printer) and name");
        c.mirror().forget(sync::Key{*type, name});
        return {{"forgotten", true}};
    }

    if (action == "resolve_halt") {
        const std::string choice = arg_string(args, "choice");
        sync::HaltChoice  halt;
        if (choice == "delete_in_orca") halt = sync::HaltChoice::ProceedLocalDeletes;
        else if (choice == "keep_in_orca") halt = sync::HaltChoice::KeepLocal;
        else if (choice == "delete_on_bambu") halt = sync::HaltChoice::ProceedRemoteDeletes;
        else if (choice == "restore_from_bambu") halt = sync::HaltChoice::RestoreFromBambu;
        else if (choice == "later") halt = sync::HaltChoice::Later;
        else throw ToolFailure("resolve_halt needs choice: delete_in_orca, keep_in_orca, delete_on_bambu, restore_from_bambu or later");
        c.mirror().resolve_halt(halt);
        if (halt != sync::HaltChoice::Later) c.request_sync();
        return {{"choice", choice}};
    }

    throw ToolFailure("unknown action: " + action);
}

} // namespace

void register_preset_sync_tools(McpService& service)
{
    service.registry().add(mutate_tool(
        "preset_sync", L("Preset sync"),
        "Sync of user presets with the Bambu cloud: what is linked, planned and waiting; confirm the first run, convert a filament to a "
        "standalone custom filament, resolve a delete stop.",
        object_schema({{"action", enum_prop({"status", "sync_now", "confirm_first_run", "convert", "forget", "resolve_halt"}, "What to do.")},
                       {"accept", prop("boolean", "confirm_first_run: true to start syncing, false to decline for now.")},
                       {"convert", array_prop({{"type", "string"}}, "confirm_first_run: filament names to convert to standalone custom filaments.")},
                       {"decline", array_prop({{"type", "string"}}, "confirm_first_run: filament names to leave out of the sync.")},
                       {"name", prop("string", "convert, forget: the preset name.")},
                       {"type", enum_prop({"print", "filament", "printer"}, "forget: the preset type.")},
                       {"choice",
                        enum_prop({"delete_in_orca", "keep_in_orca", "delete_on_bambu", "restore_from_bambu", "later"},
                                  "resolve_halt: how to settle the stopped deletes.")}},
                      {"action"}),
        [](const json& args, ToolContext& ctx) { return ToolResult::ok(ctx.run_ui([&] { return run_action(args); })); }));
}

}}} // namespace Slic3r::GUI::Mcp
