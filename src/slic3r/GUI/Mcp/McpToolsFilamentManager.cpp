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
#include "slic3r/Utils/BblFilament/NewSpools.hpp"
#include "slic3r/Utils/BblFilament/PrinterTap.hpp"
#include "slic3r/Utils/BblFilament/Spool.hpp"
#include "slic3r/Utils/BblFilament/SpoolCloud.hpp"
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

// An explicit write refused by the link mode or the allow-list is a tool failure that says why.
void check_write(const char* what, const fil::SlotKey* slot, bool dry_run_answers = false)
{
    const std::string why = fil::write_refusal(Fm::Runtime::get().link_settings(), slot, dry_run_answers);
    if (!why.empty()) throw ToolFailure(std::string(what) + " refused: " + why);
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
    const Fm::Runtime&    runtime  = Fm::Runtime::get();
    const fil::LinkConfig settings = runtime.link_settings();
    const AppConfig*      config   = wxGetApp().app_config;
    json                  only     = json::array();
    for (const std::string& slot : settings.only_slots) only.push_back(slot_by_name(slot, views));
    return {{"mode", Fm::link_mode_text(settings.mode)},
            {"mode_source", Fm::link_mode_source(Fm::link_mode_env(), runtime.session_link_mode())},
            {"only", std::move(only)},
            {k_enabled, config != nullptr && config->get_bool(k_enabled)}};
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

// Session settings only: nothing is written to the app config.
json link_mode(const json& args)
{
    Fm::Runtime&                        runtime = Fm::Runtime::get();
    const std::vector<fil::PrinterView> views   = runtime.views();
    const std::string                   mode    = arg_string(args, "mode");
    if (args.contains("mode") && mode != "default" && !Fm::parse_link_mode(mode)) throw ToolFailure("mode must be on, off, dry_run or default");

    // Everything is checked before anything is set.
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

    if (args.contains("mode")) runtime.set_session_link_mode(Fm::parse_link_mode(mode)); // "default": none
    if (only) runtime.set_session_link_only(std::move(*only));
    runtime.linker().config = runtime.link_settings();
    return settings_json(views);
}

json assign(const json& args)
{
    Fm::Runtime&                        runtime = Fm::Runtime::get();
    const std::vector<fil::PrinterView> views   = runtime.views();
    std::string                         spool_id;
    if (auto it = args.find("spool_id"); it != args.end() && it->is_string()) spool_id = it->get<std::string>();
    else if (it != args.end() && it->is_number_integer()) spool_id = std::to_string(it->get<long long>());
    if (spool_id.empty()) throw ToolFailure("assign needs spool_id");

    const fil::SlotKey slot = slot_arg(args, views);
    check_write("assign", &slot);
    const fil::Accept a = runtime.assign(spool_id, slot);
    if (a.ok) return {{"assigned", true}};
    if (a.kind == fil::FailKind::Rejected) throw ToolFailure("assign refused: linking is not on or the slot is not allowed");
    if (a.kind == fil::FailKind::NotFound) throw ToolFailure("no spool " + spool_id + " in the last list of the account");
    for (const fil::Spool& r : runtime.service().confirmed()) {
        if (r.id != spool_id) continue;
        if (!r.rfid.empty()) throw ToolFailure("spool " + spool_id + " has an RFID tag: the printer reports where it is");
        if (r.in_printer) throw ToolFailure("spool " + spool_id + " is in a printer already: unassign it from its slot first");
    }
    throw ToolFailure("spool " + spool_id + " has no filament id or no colour");
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

// The slots that show the new-spool marker; never an RFID.
json new_spools()
{
    Fm::Runtime&                        runtime = Fm::Runtime::get();
    const std::vector<fil::PrinterView> views   = runtime.views();
    json                                slots   = json::array();
    for (const fil::PrinterView& v : views)
        for (const fil::SlotKey& slot : runtime.new_spools().of_printer(v.dev_id))
            slots.push_back({{"printer", v.name}, {"ams_id", slot.ams_id}, {"slot_id", slot.slot_id}});
    return {{"slots", std::move(slots)}};
}

std::string id_arg(const json& args, const char* key)
{
    auto        it = args.find(key);
    std::string id;
    if (it != args.end() && it->is_number_integer()) id = std::to_string(it->get<long long>());
    else if (it != args.end() && it->is_string()) id = it->get<std::string>();
    if (!fil::spool_id_as_int(id)) throw ToolFailure(std::string(key) + " must be a spool id");
    return id;
}

json soft_match_act(const json& args)
{
    const std::string action = arg_string(args, "match_action");
    if (action != "accept" && action != "link_other" && action != "create_new")
        throw ToolFailure("match_action must be accept, link_other or create_new");
    const std::string spool_id = id_arg(args, "spool_id");
    const std::string target   = action == "link_other" ? id_arg(args, "target_spool_id") : std::string();
    check_write("soft_match_act", nullptr);
    return accept_json(Fm::Runtime::get().soft_match_act(action, spool_id, target));
}

json unassign(const json& args)
{
    Fm::Runtime&                        runtime = Fm::Runtime::get();
    const std::vector<fil::PrinterView> views   = runtime.views();
    const fil::SlotKey                  slot    = slot_arg(args, views);
    check_write("unassign", &slot, true);
    std::string       would_unbind, why;
    const fil::Accept a = runtime.unassign(slot, &would_unbind, &why);
    if (!a.ok) throw ToolFailure(why.empty() ? "unassign refused: " + std::string(kind_name(a.kind)) : why);
    json reply = accept_json(a);
    if (a.ok && runtime.link_settings().mode == fil::LinkMode::DryRun) {
        reply["dry_run"]    = true;
        reply["would_send"] = {{"unbind", {{"printer", printer_name(slot.dev_id, views)}, {"ams_id", slot.ams_id}, {"slot_id", slot.slot_id}, {"spool_id", would_unbind}}}};
    }
    return reply;
}

json run_action(const json& args)
{
    const std::string action  = arg_string(args, "action");
    Fm::Runtime&      runtime = Fm::Runtime::get();

    if (action == "status") return runtime.status_json();
    if (action == "link_status") return link_status();
    if (action == "link_mode") return link_mode(args);
    if (action == "assign") return assign(args);
    if (action == "unassign") return unassign(args);
    if (action == "sync_now") {
        runtime.linker().sync_now();
        return {{"requested", true}};
    }
    if (action == "soft_match") return soft_match(args);
    if (action == "new_spools") return new_spools();
    if (action == "soft_match_act") return soft_match_act(args);
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
        "the last round; link_mode sets on, off or dry_run for this session only (dry_run plans without sending anything; default returns to "
        "the ORCA_FM_LINK_MODE environment variable, else the Preferences setting; nothing is stored) and optionally limits linking to some "
        "slots for this session; link_status settings shows the mode, where it comes from and the slots. assign puts a spool without RFID into "
        "a slot, but binds it only when the printer reports that spool's filament and colour in that slot (within two minutes); unassign "
        "clears the slot of a spool without RFID now (in dry_run it answers what it would send); sync_now runs the next round at once; "
        "soft_match reads the cloud's queue of new official spools of one AMS (shown in link_status: per entry the spool id, brand, name, "
        "colour and grams left, and its candidates with the same fields); new_spools lists the slots that show the new-spool marker; soft_match_act answers an entry of that queue (accept the match, "
        "link_other to another spool, or create_new; the outcome shows in status recent). assign, unassign and soft_match_act fail unless "
        "linking is on (unassign also in dry_run), and assign and unassign fail for a slot outside the session's slots. Printers are named "
        "by their names.",
        object_schema({{"action", enum_prop({"status", "list", "pull", "add", "update", "remove", "link_status", "link_mode", "assign", "unassign",
                                             "sync_now", "soft_match", "new_spools", "soft_match_act"},
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
                       {"mode", enum_prop({"on", "off", "dry_run", "default"}, "link_mode, for this session: on, off, dry_run (plans without sending), or default (the environment variable, else the Preferences setting). Omitted: unchanged.")},
                       {"only", array_prop({{"type", "string"}}, "link_mode, for this session: the only slots linking may touch, each \"<printer name>:<ams id>:<slot id>\"; an empty array lifts the limit. Omitted: unchanged.")},
                       {"spool_id", prop("string", "assign: the spool (without RFID) to put into the slot. soft_match_act: the new spool's id (the queue entry).")},
                       {"match_action", enum_prop({"accept", "link_other", "create_new"}, "soft_match_act: accept the match, link the new spool to target_spool_id, or keep it as a new spool.")},
                       {"target_spool_id", prop("string", "soft_match_act with link_other: the existing spool the new one is.")},
                       {"printer", prop("string", "assign, unassign, soft_match: the printer's name (default: the selected printer).")},
                       {"ams_id", prop("integer", "assign, unassign, soft_match: the AMS id (0 to 3 for AMS units, 128 and up for AMS HT).")},
                       {"slot_id", prop("integer", "assign, unassign: the slot of that AMS, from 0.")}},
                      {"action"}),
        [](const json& args, ToolContext& ctx) { return ToolResult::ok(ctx.run_ui([&] { return run_action(args); })); }));
}

}}} // namespace Slic3r::GUI::Mcp
