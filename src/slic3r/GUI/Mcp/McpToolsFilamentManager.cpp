// filament_manager MCP tool (fork): the spool list of the Bambu account, through the Filament Manager's spool service,
// and the linker that keeps the spools' printer slots up to date.

#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/FilamentManager/FmPrinters.hpp"
#include "slic3r/GUI/FilamentManager/FmRuntime.hpp"
#include "slic3r/GUI/FilamentManager/FmViewModel.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/Utils/BblFilament/LinkDriver.hpp"
#include "slic3r/Utils/BblFilament/Linker.hpp"
#include "slic3r/Utils/BblFilament/PrinterTap.hpp"
#include "slic3r/Utils/BblFilament/Spool.hpp"
#include "slic3r/Utils/BblFilament/SpoolService.hpp"

#include <cstddef>
#include <ctime>
#include <optional>
#include <set>
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

int quantity_arg(const json& args)
{
    auto it = args.find("quantity");
    if (it == args.end()) return 1;
    if (!it->is_number_integer()) throw ToolFailure("quantity must be a whole number from 1 to 99");
    const long long q = it->get<long long>();
    if (it->is_number_unsigned() && it->get<unsigned long long>() > 99ULL) throw ToolFailure("quantity must be a whole number from 1 to 99");
    if (q < 1 || q > 99) throw ToolFailure("quantity must be a whole number from 1 to 99");
    return static_cast<int>(q);
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

// ---------------------------------------------------------------------------------------------------------------------
// linking. Printers are named, never identified by their serial (the dev id).

const char* k_enabled = "filament_manager_link_enabled";
const char* k_dry_run = "filament_manager_link_dry_run";
const char* k_only    = "filament_manager_link_only";

AppConfig& app_config()
{
    AppConfig* config = wxGetApp().app_config;
    if (config == nullptr) throw ToolFailure("the app configuration is not available");
    return *config;
}

const fil::PrinterView& printer_named(const std::string& name, const std::vector<fil::PrinterView>& views)
{
    const fil::PrinterView* found = nullptr;
    int                     count = 0;
    for (const fil::PrinterView& v : views)
        if (v.bound && v.name == name) {
            found = &v;
            ++count;
        }
    if (count == 0) throw ToolFailure("no printer of the signed-in Bambu account is named \"" + name + "\"");
    if (count > 1) throw ToolFailure("more than one printer of the account is named \"" + name + "\"");
    return *found;
}

// A bound Bambu printer by its exact name; the selected one when `key` is absent.
const fil::PrinterView& printer_arg(const json& args, const char* key, const std::vector<fil::PrinterView>& views)
{
    auto it = args.find(key);
    if (it == args.end() || it->is_null()) {
        for (const fil::PrinterView& v : views)
            if (v.selected) {
                if (!v.bound) throw ToolFailure("the selected printer is not a printer of the signed-in Bambu account");
                return v;
            }
        throw ToolFailure("no Bambu printer is selected: name one with printer");
    }
    if (!it->is_string()) throw ToolFailure(std::string(key) + " must be a printer name");
    return printer_named(it->get<std::string>(), views);
}

int ams_id_arg(const json& args)
{
    auto it = args.find("ams_id");
    if (it == args.end() || !it->is_number_integer()) throw ToolFailure("ams_id must be a whole number");
    const long long v = it->get<long long>();
    if (v < 0 || v > 255) throw ToolFailure("ams_id must be from 0 to 255");
    return static_cast<int>(v);
}

bool is_digits(const std::string& s) { return !s.empty() && s.size() <= 3 && s.find_first_not_of("0123456789") == std::string::npos; }

std::string slot_id_arg(const json& args)
{
    auto it = args.find("slot_id");
    if (it != args.end() && it->is_number_integer() && it->get<long long>() >= 0 && it->get<long long>() <= 255)
        return std::to_string(it->get<long long>());
    if (it != args.end() && it->is_string() && is_digits(it->get<std::string>())) return std::to_string(std::stoi(it->get<std::string>()));
    throw ToolFailure("slot_id must be a slot number");
}

fil::SlotKey slot_arg(const json& args, const std::vector<fil::PrinterView>& views)
{
    const fil::PrinterView& p = printer_arg(args, "printer", views);
    return fil::SlotKey{p.dev_id, ams_id_arg(args), slot_id_arg(args)};
}

std::string printer_name(const std::string& dev_id, const std::vector<fil::PrinterView>& views)
{
    for (const fil::PrinterView& v : views)
        if (v.dev_id == dev_id) return v.name;
    return "?";
}

// "<dev id>:<ams id>:<slot id>" -> "<printer name>:<ams id>:<slot id>".
std::string slot_by_name(const std::string& text, const std::vector<fil::PrinterView>& views)
{
    const auto b = text.rfind(':');
    const auto a = b == std::string::npos || b == 0 ? std::string::npos : text.rfind(':', b - 1);
    if (a == std::string::npos) return "?";
    return printer_name(text.substr(0, a), views) + text.substr(a);
}

json settings_json(const std::vector<fil::PrinterView>& views)
{
    const AppConfig& config = app_config();
    json             only   = json::array();
    for (const std::string& slot : Fm::parse_only_slots(config.get(k_only))) only.push_back(slot_by_name(slot, views));
    return {{k_enabled, config.get_bool(k_enabled)},
            {k_dry_run, config.get_bool(k_dry_run)},
            {k_only, std::move(only)},
            {"mode_source", Fm::is_link_mode_override(Fm::link_mode_env()) ? "env" : "config"}};
}

json tap_json(const std::vector<fil::PrinterView>& views)
{
    const fil::PrinterTap& tap = Fm::Runtime::get().tap();
    const long long        now = static_cast<long long>(std::time(nullptr));
    json                   out = json::array();
    for (const fil::PrinterView& v : views) {
        const fil::PrinterFacts* f = tap.facts(v.dev_id);
        json trays = json::array();
        for (const fil::TrayFacts& t : v.trays)
            trays.push_back({{"ams_id", t.ams_id},
                             {"slot_id", t.slot_id},
                             {"ams_type", t.ams_type},
                             {"occupied", t.occupied},
                             {"reading", t.reading},
                             {"has_rfid", fil::valid_rfid(t.uuid)},
                             {"filament_id", t.filament_id},
                             {"color", t.color},
                             {"remain", t.remain},
                             {"remain_g", t.remain_g},
                             {"total_g", t.total_g}});
        out.push_back({{"name", v.name},
                       {"has_full", f != nullptr && f->has_full},
                       {"last_message_age_s", f != nullptr ? json(now - f->last_message_at) : json(nullptr)},
                       {"observations", f != nullptr ? f->observations : 0ULL},
                       {"trays", std::move(trays)}});
    }
    return out;
}

json link_status()
{
    Fm::Runtime&                        runtime = Fm::Runtime::get();
    const std::vector<fil::PrinterView> views   = runtime.views();
    json                                j       = runtime.linker().status_json(views);
    j["tap"]        = tap_json(views);
    j["settings"]   = settings_json(views);
    j["soft_match"] = runtime.soft_match_json();
    return j;
}

json link_mode(const json& args)
{
    const std::vector<fil::PrinterView> views = Fm::Runtime::get().views();
    const std::string                   mode  = arg_string(args, "mode");
    if (args.contains("mode") && mode != "on" && mode != "off" && mode != "dry_run") throw ToolFailure("mode must be on, off or dry_run");

    // Everything is checked before anything is written.
    std::optional<std::set<std::string>> only;
    if (auto it = args.find("only"); it != args.end()) {
        if (!it->is_array()) throw ToolFailure("only must be an array of \"<printer name>:<ams id>:<slot id>\"");
        only.emplace();
        for (const json& item : *it) {
            const std::string text = item.is_string() ? item.get<std::string>() : std::string();
            const auto        b    = text.rfind(':');
            const auto        a    = b == std::string::npos || b == 0 ? std::string::npos : text.rfind(':', b - 1);
            if (a == std::string::npos || a == 0) throw ToolFailure("only: \"" + text + "\" is not \"<printer name>:<ams id>:<slot id>\"");
            const std::string ams  = text.substr(a + 1, b - a - 1);
            const std::string slot = text.substr(b + 1);
            if (!is_digits(ams) || std::stoi(ams) > 255 || !is_digits(slot)) throw ToolFailure("only: \"" + text + "\" has no valid AMS id and slot id");
            const fil::PrinterView& p = printer_named(text.substr(0, a), views);
            only->insert(fil::SlotKey{p.dev_id, std::stoi(ams), std::to_string(std::stoi(slot))}.text());
        }
    }

    AppConfig& config = app_config();
    if (args.contains("mode")) {
        config.set_bool(k_enabled, mode != "off");
        config.set_bool(k_dry_run, mode == "dry_run");
    }
    if (only) config.set(k_only, Fm::join_only_slots(*only));
    config.save();
    Fm::Runtime::get().linker().config = Fm::link_config(config.get_bool(k_enabled), config.get_bool(k_dry_run), config.get(k_only), Fm::link_mode_env());
    json reply = settings_json(views);
    if (Fm::is_link_mode_override(Fm::link_mode_env()))
        reply["note"] = "the environment variable ORCA_FM_LINK_MODE=" + Fm::link_mode_env() +
                        " decides the mode while this process runs; the stored mode applies when the app starts without it";
    return reply;
}

json assign(const json& args)
{
    Fm::Runtime&                        runtime = Fm::Runtime::get();
    const std::vector<fil::PrinterView> views   = runtime.views();
    std::string                         spool_id;
    if (auto it = args.find("spool_id"); it != args.end() && it->is_string()) spool_id = it->get<std::string>();
    else if (it != args.end() && it->is_number_integer()) spool_id = std::to_string(it->get<long long>());
    if (spool_id.empty()) throw ToolFailure("assign needs spool_id");

    const fil::Spool* row = nullptr;
    for (const fil::Spool& r : runtime.service().confirmed())
        if (r.id == spool_id) row = &r;
    if (row == nullptr) throw ToolFailure("no spool " + spool_id + " in the last list of the account");
    if (!row->rfid.empty()) throw ToolFailure("spool " + spool_id + " has an RFID tag: the printer reports where it is");
    if (row->filament_id.empty()) throw ToolFailure("spool " + spool_id + " has no filament id");

    fil::Assignment a;
    a.slot        = slot_arg(args, views);
    a.spool_id    = row->id;
    a.filament_id = row->filament_id;
    a.colors      = row->colors.empty() ? std::vector<std::string>{row->color} : row->colors;
    if (a.colors.front().empty()) throw ToolFailure("spool " + spool_id + " has no colour");
    runtime.linker().assign(a);
    return {{"assigned", true}};
}

json soft_match(const json& args)
{
    Fm::Runtime&                        runtime = Fm::Runtime::get();
    const std::vector<fil::PrinterView> views   = runtime.views();
    const fil::PrinterView&             p       = printer_arg(args, "printer", views);
    const int                           ams_id  = ams_id_arg(args);
    const fil::Accept                   a       = runtime.request_soft_match(p, ams_id);
    if (!a.ok)
        throw ToolFailure(a.kind == fil::FailKind::BadRequest ? "no AMS serial is known for that AMS" : std::string("refused: ") + kind_name(a.kind));
    return {{"requested", true}};
}

json run_action(const json& args)
{
    const std::string action  = arg_string(args, "action");
    Fm::Runtime&      runtime = Fm::Runtime::get();

    if (action == "status") return runtime.status_json();
    if (action == "link_status") return link_status();
    if (action == "link_mode") return link_mode(args);
    if (action == "assign") return assign(args);
    if (action == "unassign") {
        const std::vector<fil::PrinterView> views = runtime.views();
        return accept_json(runtime.linker().unassign(slot_arg(args, views), views));
    }
    if (action == "sync_now") {
        runtime.linker().sync_now();
        return {{"requested", true}};
    }
    if (action == "soft_match") return soft_match(args);
    if (action == "list") return spool_list();

    if (action == "pull") {
        runtime.service().request_pull();
        return {{"requested", true}};
    }

    if (action == "add") return accept_json(runtime.service().add(object_arg(args, "spool"), quantity_arg(args)));

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
        "account. Results arrive asynchronously: poll status until is_syncing is false and read recent for what happened. "
        "Linking keeps the account's record of which spool is in which AMS slot, and how much is left on it, in step with every Bambu printer "
        "of the account: link_status shows per printer what the printers report, what each tray matched, what would be sent and why not, and "
        "the last round; link_mode sets on, off or dry_run (dry_run plans without sending anything) and optionally limits linking to some "
        "slots; assign puts a spool without RFID into a slot, but binds it only when the printer reports that spool's filament and colour in "
        "that slot (within two minutes); unassign clears the slot of a spool without RFID now; sync_now runs the next round at once; "
        "soft_match reads the cloud's queue of new official spools of one AMS (shown in link_status). Printers are named by their names.",
        object_schema({{"action", enum_prop({"status", "list", "pull", "add", "update", "remove", "link_status", "link_mode", "assign", "unassign",
                                             "sync_now", "soft_match"},
                                            "What to do.")},
                       {"spool", prop("object", "add: the new spool in the page's shape (brand, material_type, series, setting_id, color_code, colors, color_type, initial_weight, net_weight, note, ...).")},
                       {"quantity", [] {
                            json q = prop("integer", "add: how many copies of the spool to create, 1 to 99 (default 1).");
                            q["minimum"] = 1;
                            q["maximum"] = 99;
                            return q;
                        }()},
                       {"patch", prop("object", "update: spool_id plus the fields to change, in the page's spool shape.")},
                       {"ids", array_prop({{"type", "string"}}, "remove: the spool_id values to delete.")},
                       {"mode", enum_prop({"on", "off", "dry_run"}, "link_mode: on, off, or dry_run (plans without sending). Omitted: unchanged.")},
                       {"only", array_prop({{"type", "string"}}, "link_mode: the only slots linking may touch, each \"<printer name>:<ams id>:<slot id>\"; an empty array lifts the limit. Omitted: unchanged.")},
                       {"spool_id", prop("string", "assign: the spool (without RFID) to put into the slot.")},
                       {"printer", prop("string", "assign, unassign, soft_match: the printer's name (default: the selected printer).")},
                       {"ams_id", prop("integer", "assign, unassign, soft_match: the AMS id (0 to 3 for AMS units, 128 and up for AMS HT).")},
                       {"slot_id", prop("integer", "assign, unassign: the slot of that AMS, from 0.")}},
                      {"action"}),
        [](const json& args, ToolContext& ctx) { return ToolResult::ok(ctx.run_ui([&] { return run_action(args); })); }));
}

}}} // namespace Slic3r::GUI::Mcp
