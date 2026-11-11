// filament_manager MCP tool (fork): the spool list of the Bambu account, through the Filament Manager's spool service.

#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "slic3r/GUI/FilamentManager/FmRuntime.hpp"
#include "slic3r/GUI/FilamentManager/FmViewModel.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/Utils/BblFilament/Spool.hpp"
#include "slic3r/Utils/BblFilament/SpoolService.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

namespace fil = Slic3r::BblFilament;

const char* kind_name(fil::FailKind kind)
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

json accept_json(const fil::Accept& accept) { return {{"accepted", accept.ok}, {"kind", accept.ok ? "" : kind_name(accept.kind)}}; }

json object_arg(const json& args, const char* key)
{
    auto it = args.find(key);
    if (it == args.end() || !it->is_object()) throw ToolFailure(std::string(key) + " must be an object");
    return *it;
}

json spool_list()
{
    Fm::Runtime& runtime = Fm::Runtime::get();
    json         spools  = json::array();
    for (const fil::Spool& s : runtime.service().view()) {
        json row = fil::spool_to_page(s);
        Fm::decorate_color_name(row, runtime.colors(), runtime.lang());
        Fm::decorate_tray_label(row);
        spools.push_back(std::move(row));
    }
    const std::size_t count = spools.size();
    return {{"spools", std::move(spools)}, {"count", count}};
}

json run_action(const json& args)
{
    const std::string action  = arg_string(args, "action");
    Fm::Runtime&      runtime = Fm::Runtime::get();

    if (action == "status") return runtime.status_json();
    if (action == "list") return spool_list();

    if (action == "pull") {
        runtime.service().request_pull();
        return {{"requested", true}};
    }

    if (action == "add") return accept_json(runtime.service().add(object_arg(args, "spool"), arg_int(args, "quantity", 1)));

    if (action == "update") return accept_json(runtime.service().update(object_arg(args, "patch")));

    if (action == "remove") {
        std::vector<std::string> ids;
        auto                     it = args.find("ids");
        if (it != args.end() && it->is_array())
            for (const json& v : *it)
                if (v.is_string()) ids.push_back(v.get<std::string>());
        if (ids.empty()) throw ToolFailure("remove needs ids");
        return accept_json(runtime.service().remove(ids));
    }

    throw ToolFailure("unknown action: " + action);
}

} // namespace

void register_filament_manager_tools(McpService& service)
{
    service.registry().add(mutate_tool(
        "filament_manager", L("Filament Manager"),
        "The filament spools kept in the user's Bambu account (the Filament Manager tab). Writes (add, update, remove) go to the user's Bambu "
        "account. Results arrive asynchronously: poll status until is_syncing is false and read recent for what happened.",
        object_schema({{"action", enum_prop({"status", "list", "pull", "add", "update", "remove"}, "What to do.")},
                       {"spool", prop("object", "add: the new spool in the page's shape (brand, material_type, series, setting_id, color_code, colors, color_type, initial_weight, net_weight, note, ...).")},
                       {"quantity", prop("integer", "add: how many copies of the spool to create (default 1).")},
                       {"patch", prop("object", "update: spool_id plus the fields to change, in the page's spool shape.")},
                       {"ids", array_prop({{"type", "string"}}, "remove: the spool_id values to delete.")}},
                      {"action"}),
        [](const json& args, ToolContext& ctx) { return ToolResult::ok(ctx.run_ui([&] { return run_action(args); })); }));
}

}}} // namespace Slic3r::GUI::Mcp
