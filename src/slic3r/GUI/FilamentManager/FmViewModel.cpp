#include "FmViewModel.hpp"

#include "FmDevices.hpp"
#include "slic3r/Utils/BblFilament/Spool.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <exception>
#include <utility>
#include <vector>

namespace Slic3r::GUI::Fm {

namespace fil = BblFilament;

namespace {

constexpr const char* k_module = "filament";

std::string utc_text(long long unix_seconds)
{
    if (unix_seconds <= 0)
        return {};
    long long days = unix_seconds / 86400;
    long long rest = unix_seconds % 86400;
    // civil_from_days (Howard Hinnant)
    days += 719468;
    const long long era = (days >= 0 ? days : days - 146096) / 146097;
    const long long doe = days - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long long       y   = yoe + era * 400;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp  = (5 * doy + 2) / 153;
    const long long d   = doy - (153 * mp + 2) / 5 + 1;
    const long long m   = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2)
        ++y;
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04lld-%02lld-%02lldT%02lld:%02lld:%02lldZ", y, m, d, rest / 3600, (rest % 3600) / 60, rest % 60);
    return buf;
}

const char* fallback_text(fil::FailKind kind)
{
    switch (kind) {
    case fil::FailKind::NotSignedIn: return "Not signed in.";
    case fil::FailKind::Unavailable: return "The network plug-in is not available.";
    case fil::FailKind::BadRequest: return "The request was not valid.";
    case fil::FailKind::NotFound: return "This spool no longer exists in the cloud.";
    case fil::FailKind::Rejected: return "The cloud refused the change.";
    default: return "No answer from the cloud. The change may or may not have been applied; the list was refreshed.";
    }
}

json default_ams() { return {{"selected_dev_id", ""}, {"ams_units", json::array()}, {"ext_slots", json::array()}, {"is_rack", false}}; }

std::string string_of(const json& obj, const char* key)
{
    auto it = obj.find(key);
    return (it != obj.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

} // namespace

json colors_response(const ColorCodes& colors, const std::string& fila_id, const std::string& lang)
{
    json out = {{"fila_id", fila_id}, {"fila_type", colors.fila_type(fila_id)}, {"candidates", json::array()}};
    if (const std::vector<ColorEntry>* list = colors.entries(fila_id))
        for (const ColorEntry& e : *list)
            out["candidates"].push_back({{"color_code", e.fila_color_code}, {"name", ColorCodes::name_for(e, lang)}, {"color_type", e.color_type}, {"colors", e.colors}});
    return out;
}

void decorate_color_name(json& page_spool, const ColorCodes& colors, const std::string& lang)
{
    if (!page_spool.is_object() || !string_of(page_spool, "color_name").empty())
        return;
    const std::string setting_id = string_of(page_spool, "setting_id");
    if (setting_id.empty())
        return;
    std::vector<std::string> list;
    auto                     cols = page_spool.find("colors");
    if (cols != page_spool.end() && cols->is_array())
        for (const json& c : *cols)
            if (c.is_string())
                list.push_back(c.get<std::string>());
    if (list.empty()) {
        const std::string code = string_of(page_spool, "color_code");
        if (!code.empty())
            list.push_back(code);
    }
    int  color_type = 2;
    auto ct         = page_spool.find("color_type");
    if (ct != page_spool.end() && ct->is_number_integer())
        color_type = ct->get<int>();
    if (const ColorEntry* e = colors.find(setting_id, list, color_type))
        page_spool["color_name"] = ColorCodes::name_for(*e, lang);
}

void decorate_tray_label(json& page_spool)
{
    if (!page_spool.is_object())
        return;
    auto in_printer = page_spool.find("in_printer");
    if (in_printer == page_spool.end() || !in_printer->is_boolean() || !in_printer->get<bool>())
        return;
    const int ams_type = page_spool.value("ams_type", -1);
    if (ams_type == 0) {
        page_spool["tray_label"] = "Ext";
        return;
    }
    const int         ams_id = page_spool.value("ams_id", -1);
    const std::string slot   = string_of(page_spool, "slot_id");
    if (ams_id >= 0 && !slot.empty() && slot.size() <= 6 && std::all_of(slot.begin(), slot.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
        page_spool["tray_label"] = tray_label(ams_id, std::stoi(slot));
}

ViewModel::ViewModel(fil::SpoolService& service, Host host, std::function<void(const json& body)> send_report)
    : m_service(service), m_host(std::move(host)), m_send(std::move(send_report)), m_list_seen(service.status().have_list)
{
}

fil::ServiceEvents ViewModel::event_handlers()
{
    fil::ServiceEvents ev;
    ev.spools_changed      = [this] {
        note_list_state();
        report(k_module, "spool", "list", spools_json());
    };
    ev.state_changed = [this] {
        note_list_state();
        report(k_module, "sync", "state", state_json());
    };
    ev.pull_done = [this](int added, int kept) {
        note_list_state();
        int updated = kept;
        if (!m_list_seen) { // the first complete list is not "added" spools
            updated = added + kept;
            added   = 0;
        }
        m_list_seen = true;
        report(k_module, "sync", "pull_done", {{"added", added}, {"updated", updated}, {"state", state_json()}, {"spools", spools_json()}});
    };
    ev.push_done = [this](const std::string& op, const std::string& id) {
        note_list_state();
        report(k_module, "sync", "push_done", {{"spool_id", id}, {"op", op}, {"state", state_json()}});
    };
    ev.push_failed = [this](const std::string& op, const std::string& id, fil::FailKind kind, const std::string& message) {
        note_list_state();
        report(k_module, "sync", "push_failed",
               {{"spool_id", id}, {"op", op}, {"code", fail_code(kind)}, {"message", fail_message(kind, message)}, {"state", state_json()}});
    };
    ev.config_fetched = [this](const json& config) {
        note_list_state();
        report(k_module, "config", "fetched", config);
    };
    return ev;
}

void ViewModel::note_list_state()
{
    if (!m_service.status().have_list)
        m_list_seen = false;
}

void ViewModel::report(const std::string& module, const std::string& submod, const std::string& action, json payload)
{
    if (!m_send)
        return;
    try {
        m_send(make_body(module, submod, action, 0, "", std::move(payload)));
    } catch (...) {
    }
}

int ViewModel::fail_code(fil::FailKind kind) const
{
    switch (kind) {
    case fil::FailKind::NotSignedIn: return -2;
    case fil::FailKind::Unavailable: return -3;
    case fil::FailKind::BadRequest: return 3;
    case fil::FailKind::NotFound: return 404;
    case fil::FailKind::Rejected: return 400;
    default: return -1;
    }
}

std::string ViewModel::fail_message(fil::FailKind kind, const std::string& server_message) const
{
    if (m_host.fail_text)
        return m_host.fail_text(kind, server_message);
    std::string text = fallback_text(kind);
    if (!server_message.empty())
        text += " (" + server_message + ")";
    return text;
}

json ViewModel::spools_json() const
{
    json out = json::array();
    for (const fil::Spool& s : m_service.view()) {
        json row = fil::spool_to_page(s);
        if (m_host.decorate_spool)
            m_host.decorate_spool(row);
        out.push_back(std::move(row));
    }
    return out;
}

json ViewModel::state_json() const
{
    const fil::SyncState st = m_service.state();
    json                 err = {{"code", 0}, {"message", ""}};
    if (st.has_error) {
        err["code"]    = fail_code(st.error_kind);
        err["message"] = fail_message(st.error_kind, st.error_message);
    }
    return {{"logged_in", st.signed_in}, {"is_syncing", st.is_syncing}, {"is_pulling", st.is_pulling}, {"last_synced_at", utc_text(st.last_synced_at)}, {"last_error", std::move(err)}};
}

json ViewModel::presets_or_empty() const
{
    return m_host.presets ? m_host.presets() : json{{"vendors", json::array()}};
}

json ViewModel::answer(const Request& r, int code, const std::string& message, json payload) const
{
    return make_body(k_module, r.submod, r.action, code, message, std::move(payload));
}

json ViewModel::refused(const Request& r, const fil::Accept& accept) const
{
    if (accept.kind == fil::FailKind::BadRequest)
        return answer(r, 3, "bad request", spools_json());
    if (accept.kind == fil::FailKind::NotSignedIn || accept.kind == fil::FailKind::Unavailable)
        return answer(r, -2, "cloud sync requires sign-in and network", spools_json());
    return answer(r, -1, "request refused", spools_json());
}

json ViewModel::handle(const Request& request)
{
    try {
        return dispatch(request);
    } catch (...) {
        return make_body(k_module, request.submod, request.action, 2, "internal error", json::object());
    }
}

json ViewModel::dispatch(const Request& r)
{
    if (r.module != k_module)
        return make_body(r.module, r.submod, r.action, 1, "unknown module", json::object());

    const json& p = r.payload;
    auto        write_result = [&](const fil::Accept& a) { return a.ok ? answer(r, 0, "", spools_json()) : refused(r, a); };
    auto        not_supported = [&] { return answer(r, -1, "not supported", spools_json()); };
    auto        bad_request   = [&] { return answer(r, 3, "bad request", spools_json()); };

    if (r.submod == "init" && r.action == "init") {
        bool dark = false;
        if (m_host.dark)
            dark = m_host.dark();
        return answer(r, 0, "",
                      {{"theme", dark ? "dark" : "light"},
                       {"spools", spools_json()},
                       {"presets", presets_or_empty()},
                       {"cloud_sync", state_json()},
                       {"debug_enabled", false}});
    }
    if (r.submod == "spool") {
        if (r.action == "list")
            return answer(r, 0, "", spools_json());
        if (r.action == "add")
            return write_result(m_service.add(p, 1));
        if (r.action == "batch_add") {
            auto spool = p.find("spool");
            if (spool == p.end() || !spool->is_object())
                return bad_request();
            int  quantity = 1;
            auto q        = p.find("quantity");
            if (q != p.end()) {
                if (!q->is_number_integer())
                    return bad_request();
                const long long n = q->get<long long>();
                if (n < 1 || n > 99)
                    return bad_request();
                quantity = static_cast<int>(n);
            }
            return write_result(m_service.add(*spool, quantity));
        }
        if (r.action == "batch_create") {
            json creates = p.value("creates", json::array());
            json updates = p.value("updates", json::array());
            return write_result(m_service.batch(creates, updates));
        }
        if (r.action == "update")
            return write_result(m_service.update(p));
        if (r.action == "remove") {
            auto id = p.find("spool_id");
            if (id == p.end() || !id->is_string())
                return bad_request();
            return write_result(m_service.remove({id->get<std::string>()}));
        }
        if (r.action == "batch_remove") {
            auto ids = p.find("spool_ids");
            if (ids == p.end() || !ids->is_array())
                return bad_request();
            std::vector<std::string> list;
            for (const json& id : *ids)
                if (id.is_string())
                    list.push_back(id.get<std::string>());
            return write_result(m_service.remove(list));
        }
        if (r.action == "mark_empty" || r.action == "toggle_favorite" || r.action == "archive")
            return not_supported();
    } else if (r.submod == "preset") {
        if (r.action == "list")
            return answer(r, 0, "", presets_or_empty());
        if (r.action == "create_custom") {
            if (!m_host.create_custom)
                return answer(r, -1, "not supported", json::object());
            m_host.create_custom(string_of(p, "vendor"), string_of(p, "client_request_id"));
            return answer(r, 0, "pending", json::object());
        }
    } else if (r.submod == "machine") {
        if (r.action == "list")
            return answer(r, 0, "", m_host.machines ? m_host.machines() : json{{"machines", json::array()}, {"selected_dev_id", ""}});
        if (r.action == "request_pushall") {
            std::string used;
            if (!m_host.request_pushall || !m_host.request_pushall(string_of(p, "dev_id"), used))
                return answer(r, -2, "machine not found", json::object());
            return answer(r, 0, "", {{"dev_id", used}});
        }
    } else if (r.submod == "ams") {
        if (r.action == "list") {
            const bool sw = p.contains("switch_selected") && p["switch_selected"] == true;
            return answer(r, 0, "", m_host.ams ? m_host.ams(string_of(p, "dev_id"), sw) : default_ams());
        }
    } else if (r.submod == "colors") {
        if (r.action == "query_for_id") {
            const std::string id = string_of(p, "fila_id");
            return answer(r, 0, "", m_host.colors ? m_host.colors(id) : json{{"fila_id", id}, {"fila_type", ""}, {"candidates", json::array()}});
        }
    } else if (r.submod == "sync") {
        if (r.action == "status")
            return answer(r, 0, "", state_json());
        if (r.action == "pull") {
            m_service.request_pull();
            return answer(r, 0, "", state_json());
        }
        if (r.action == "push_all_now")
            return answer(r, -1, "not supported", state_json());
    } else if (r.submod == "config") {
        if (r.action == "fetch") {
            const bool force = p.contains("force") && p["force"] == true;
            if (!force)
                if (const json* cached = m_service.cached_config())
                    return answer(r, 0, "cached", *cached);
            m_service.request_config(force);
            return answer(r, 0, "pending", json::object());
        }
    }
    return answer(r, -1, "unknown action", json::object());
}

void ViewModel::custom_created(const std::string& client_request_id, bool ok, const json& created)
{
    json payload = {{"ok", ok}, {"client_request_id", client_request_id}};
    if (ok && created.is_object() && !created.empty())
        payload["created"] = created;
    report(k_module, "preset", "create_custom_done", std::move(payload));
    if (ok)
        report(k_module, "preset", "list", presets_or_empty());
}

void ViewModel::machine_selected_changed()
{
    const json ams = m_host.ams ? m_host.ams("", false) : default_ams();
    report(k_module, "machine", "selected_changed",
           {{"machines", m_host.machines ? m_host.machines() : json{{"machines", json::array()}, {"selected_dev_id", ""}}},
            {"ams", ams},
            {"selected_dev_id", ams.is_object() ? string_of(ams, "selected_dev_id") : std::string()}});
}

} // namespace Slic3r::GUI::Fm
