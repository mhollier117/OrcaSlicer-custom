#include "SpoolCloud.hpp"

#include "PluginLinkStructs.hpp"

#include "slic3r/Utils/BBLNetworkPlugin.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <set>
#include <utility>

namespace Slic3r::BblFilament {

namespace {

constexpr std::size_t k_max_message = 200;

std::string cut_message(std::string s)
{
    if (s.size() <= k_max_message) return s;
    std::size_t n = k_max_message;
    // do not end inside a UTF-8 sequence
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    s.resize(n);
    return s;
}

// The integer in `code` (a number, or a string of digits), or -1.
long long read_code(const json& j)
{
    auto it = j.find("code");
    if (it == j.end()) return -1;
    if (it->is_number_integer()) return it->get<long long>();
    if (it->is_string()) {
        const std::string s = it->get<std::string>();
        if (s.empty() || s.size() > 9) return -1;
        long long v = 0;
        for (char c : s) {
            if (c < '0' || c > '9') return -1;
            v = v * 10 + (c - '0');
        }
        return v;
    }
    return -1;
}

CloudReply fail(CloudError e, int rc = 0, std::string message = {})
{
    CloudReply r;
    r.error   = e;
    r.rc      = rc;
    r.message = std::move(message);
    return r;
}

std::string dump_body(const json& body) { return body.dump(-1, ' ', false, json::error_handler_t::replace); }

// Runs `call` with the plugin's agent; Unavailable when the plugin, the agent or the getter is missing.
template<class Getter, class Call> CloudReply with_plugin(Getter getter, Call call)
{
    try {
        BBLNetworkPlugin& plugin = BBLNetworkPlugin::instance();
        void*             agent  = plugin.get_agent();
        auto              fn     = (plugin.*getter)();
        if (agent == nullptr || fn == nullptr) return fail(CloudError::Unavailable);
        std::string http_body;
        const int   rc = call(fn, agent, &http_body);
        return classify_reply(rc, http_body);
    } catch (...) {
        return fail(CloudError::Unknown);
    }
}

// Runs `call` with a plugin function resolved by name now (never cached: the plugin can be reloaded).
template<class Fn, class Call> CloudReply with_symbol(const char* name, Call call)
{
    try {
        BBLNetworkPlugin& plugin = BBLNetworkPlugin::instance();
        void*             agent  = plugin.get_agent();
        Fn                fn     = reinterpret_cast<Fn>(plugin.get_function(name));
        if (agent == nullptr || fn == nullptr) return fail(CloudError::Unavailable);
        std::string http_body;
        const int   rc = call(fn, agent, &http_body);
        return classify_reply(rc, http_body);
    } catch (...) {
        return fail(CloudError::Unknown);
    }
}

int clamp_weight(long long w) { return static_cast<int>(std::clamp<long long>(w, 0, 999999999)); }

const json* find_array(const json& body, const char* key)
{
    if (!body.is_object()) return nullptr;
    auto it = body.find(key);
    return (it != body.end() && it->is_array()) ? &*it : nullptr;
}

std::string text_member(const json& j, const char* key)
{
    auto it = j.find(key);
    if (it == j.end()) return {};
    if (it->is_string()) return it->get<std::string>();
    if (it->is_number_integer()) return it->dump();
    return {};
}

// A spool id in a reply: a positive integer or a string of digits; anything else is "no id".
std::string id_text_member(const json& j, const char* key)
{
    const auto v = parse_spool_id(text_member(j, key));
    return v ? std::to_string(*v) : std::string();
}

} // namespace

std::optional<int> spool_id_as_int(const std::string& id)
{
    const auto v = parse_spool_id(id);
    if (!v || *v > std::numeric_limits<int>::max()) return std::nullopt;
    return static_cast<int>(*v);
}

std::vector<AmsSyncResult> parse_ams_sync_results(const json& body)
{
    std::vector<AmsSyncResult> out;
    const json*                arr = find_array(body, "results");
    if (!arr) return out;
    for (const json& e : *arr) {
        if (!e.is_object()) continue;
        AmsSyncResult r;
        r.ams_sn   = text_member(e, "amsSn");
        r.slot_id  = text_member(e, "slotId");
        r.spool_id = id_text_member(e, "spoolId");
        out.push_back(std::move(r));
    }
    return out;
}

std::vector<std::string> parse_created_rfids(const json& body)
{
    std::vector<std::string> out;
    const json*              arr = find_array(body, "createdRFIDs");
    if (!arr) return out;
    for (const json& e : *arr)
        if (e.is_string() && !e.get<std::string>().empty()) out.push_back(e.get<std::string>());
    return out;
}

std::vector<SlotResult> parse_slot_results(const json& body)
{
    std::vector<SlotResult> out;
    const json*             arr = find_array(body, "results");
    if (!arr) return out;
    for (const json& e : *arr) {
        if (!e.is_object()) continue;
        SlotResult r;
        r.ams_sn  = text_member(e, "amsSn");
        r.slot_id = text_member(e, "slotId");
        auto c    = e.find("code");
        if (c != e.end() && c->is_number_integer()) {
            const long long v = c->get<long long>();
            if (v >= std::numeric_limits<int>::min() && v <= std::numeric_limits<int>::max()) r.code = static_cast<int>(v);
        }
        out.push_back(std::move(r));
    }
    return out;
}

SoftMatchQueue parse_soft_match(const json& body)
{
    SoftMatchQueue q;
    if (const json* hits = find_array(body, "hits")) {
        for (const json& row : *hits) {
            auto s = spool_from_cloud(row);
            if (!s) continue;
            const bool dup = std::any_of(q.entries.begin(), q.entries.end(), [&](const SoftMatchEntry& e) { return e.hit.id == s->id; });
            if (dup) continue;
            SoftMatchEntry e;
            e.hit = std::move(*s);
            q.entries.push_back(std::move(e));
        }
    }
    if (const json* groups = find_array(body, "candidates")) {
        for (const json& g : *groups) {
            if (!g.is_object()) continue;
            const std::string pending = id_text_member(g, "pendingId");
            if (pending.empty()) continue;
            auto entry = std::find_if(q.entries.begin(), q.entries.end(), [&](const SoftMatchEntry& e) { return e.hit.id == pending; });
            if (entry == q.entries.end()) continue;
            const json* rows = find_array(g, "candidates");
            if (!rows) continue;
            for (const json& row : *rows)
                if (auto s = spool_from_cloud(row)) entry->candidates.push_back(std::move(*s));
        }
    }
    return q;
}

CloudReply classify_reply(int rc, const std::string& raw_body)
{
    json parsed = json::parse(raw_body, nullptr, false);
    CloudReply r;
    r.rc = rc;
    if (rc == 0) {
        // An empty or unparsable body leaves `body` null: a list or config reply like that is a failure for its reader.
        if (!parsed.is_discarded()) r.body = std::move(parsed);
        return r;
    }
    r.error = CloudError::Unknown;
    if (parsed.is_discarded() || !parsed.is_object()) return r;
    const long long code = read_code(parsed);
    if (code < 400 || code > 499) return r;
    r.error = code == 404 ? CloudError::NotFound : CloudError::Rejected;
    for (const char* key : {"error", "message"}) {
        auto it = parsed.find(key);
        if (it != parsed.end() && it->is_string()) {
            r.message = cut_message(it->get<std::string>());
            break;
        }
    }
    return r;
}

ListResult list_all(ISpoolCloud& cloud, const std::function<bool()>& cancelled)
{
    ListResult         out;
    std::set<std::string> seen;
    auto fail_with = [&out](CloudReply f) {
        out.ok      = false;
        out.failure = std::move(f);
        out.spools.clear();
        return out;
    };

    int offset = 0;
    for (int page = 1;; ++page) {
        if (cancelled && cancelled()) return fail_with(fail(CloudError::Unknown, 0, "cancelled"));
        CloudReply r = cloud.list_page(offset, k_spool_page_size);
        if (!r.ok()) return fail_with(std::move(r));
        if (!r.body.is_object()) return fail_with(fail(CloudError::Unknown, 0, "unexpected response"));

        const json* hits = nullptr;
        auto        it   = r.body.find("hits");
        if (it != r.body.end() && !it->is_null()) {
            if (!it->is_array()) return fail_with(fail(CloudError::Unknown, 0, "unexpected response"));
            hits = &*it;
        }
        const int n = hits ? static_cast<int>(hits->size()) : 0;
        int       added = 0;
        if (hits) {
            for (const json& row : *hits) {
                auto s = spool_from_cloud(row);
                if (!s) {
                    ++out.skipped_rows;
                    continue;
                }
                if (!seen.insert(s->id).second) continue;
                out.spools.push_back(std::move(*s));
                ++added;
            }
        }

        if (n == 0) break;
        // More rows than the limit asked for: the server does not page and sent everything.
        if (n > k_spool_page_size) break;
        auto total = r.body.find("total");
        if (total != r.body.end() && total->is_number() && static_cast<double>(out.spools.size()) >= total->get<double>()) break;
        if (added == 0) {
            out.paging_suspect = true;
            break;
        }
        if (n < k_spool_page_size) break;
        if (page >= k_spool_max_pages) {
            out.paging_suspect = true;
            break;
        }
        offset += n;
    }
    out.ok = true;
    return out;
}

CloudReply PluginSpoolCloud::list_page(int offset, int limit)
{
    return with_plugin(&BBLNetworkPlugin::get_get_filament_spools, [&](auto fn, void* agent, std::string* body) {
        FilamentQueryParams p;
        p.offset = offset;
        p.limit  = limit;
        return fn(agent, p, body);
    });
}

CloudReply PluginSpoolCloud::create(const json& body)
{
    return with_plugin(&BBLNetworkPlugin::get_create_filament_spool,
                       [&](auto fn, void* agent, std::string* out) { return fn(agent, dump_body(body), out); });
}

CloudReply PluginSpoolCloud::update(const std::string& id, const json& body)
{
    return with_plugin(&BBLNetworkPlugin::get_update_filament_spool,
                       [&](auto fn, void* agent, std::string* out) { return fn(agent, id, dump_body(body), out); });
}

CloudReply PluginSpoolCloud::remove(const std::vector<std::string>& ids)
{
    if (ids.empty()) return fail(CloudError::Rejected);
    return with_plugin(&BBLNetworkPlugin::get_delete_filament_spools, [&](auto fn, void* agent, std::string* out) {
        FilamentDeleteParams p;
        p.ids = ids;
        return fn(agent, p, out);
    });
}

CloudReply PluginSpoolCloud::config()
{
    return with_plugin(&BBLNetworkPlugin::get_get_filament_config, [&](auto fn, void* agent, std::string* out) { return fn(agent, out); });
}

CloudReply PluginSpoolCloud::sync_ams(const std::string& dev_id, const std::vector<AmsItem>& items)
{
    if (dev_id.empty() || items.empty()) return fail(CloudError::Rejected);
    return with_plugin(&BBLNetworkPlugin::get_sync_ams_filaments, [&](auto fn, void* agent, std::string* out) {
        AmsSyncParams p;
        p.devId = dev_id;
        for (const AmsItem& i : items) {
            AmsSyncItem t;
            t.RFID           = i.rfid;
            t.filamentVendor = i.vendor;
            t.filamentType   = i.type;
            t.filamentName   = i.name;
            t.filamentId     = i.filament_id;
            t.isSupport      = i.is_support;
            t.color          = i.color;
            t.colorType      = i.color_type;
            t.colors         = i.colors;
            t.netWeight      = clamp_weight(i.net_weight);
            t.totalNetWeight = clamp_weight(i.total_net_weight);
            t.trayIdName     = i.tray_id_name;
            t.note           = i.note;
            t.amsSn          = i.ams_sn;
            t.slotId         = i.slot_id;
            t.amsId          = i.ams_id;
            t.amsType        = i.ams_type;
            t.createNew      = i.create_new;
            p.items.push_back(std::move(t));
        }
        return fn(agent, std::move(p), out);
    });
}

CloudReply PluginSpoolCloud::sync_slots(const std::string& dev_id, const std::vector<SlotItem>& items)
{
    // All checks come before the plugin lookup. An id that does not fit an int must not become 0 (= unbind).
    plugin::SlotMappingsSyncParams p;
    p.devId = dev_id;
    for (const SlotItem& i : items) {
        if (i.ams_sn.empty() || i.slot_id.empty() || i.ams_id < 0 || i.ams_type < 0) return fail(CloudError::Rejected);
        plugin::SlotMappingItem m;
        m.amsSn   = i.ams_sn;
        m.slotId  = i.slot_id;
        m.amsId   = i.ams_id;
        m.amsType = i.ams_type;
        if (!i.spool_id.empty()) {
            const auto id = spool_id_as_int(i.spool_id);
            if (!id) return fail(CloudError::Rejected);
            m.spoolId = *id;
            m.rfid    = i.rfid;
        }
        p.mappings.push_back(std::move(m));
    }
    return with_symbol<plugin::func_sync_slot_mappings>("bambu_network_sync_slot_mappings",
                                                        [&](auto fn, void* agent, std::string* out) { return fn(agent, std::move(p), out); });
}

CloudReply PluginSpoolCloud::soft_match_pending(const std::string& dev_id, const std::string& ams_sn)
{
    return with_symbol<plugin::func_get_soft_match_pending>("bambu_network_get_soft_match_pending", [&](auto fn, void* agent, std::string* out) {
        plugin::SoftMatchPendingParams p;
        p.devId = dev_id;
        p.amsSn = ams_sn;
        return fn(agent, std::move(p), out);
    });
}

CloudReply PluginSpoolCloud::soft_match_action(const std::string& action, const std::string& spool_id, const std::string& target_spool_id)
{
    if (action != "accept" && action != "link_other" && action != "create_new") return fail(CloudError::Rejected);
    const auto id = spool_id_as_int(spool_id);
    if (!id) return fail(CloudError::Rejected);
    int target = 0;
    if (action == "link_other") {
        const auto t = spool_id_as_int(target_spool_id);
        if (!t) return fail(CloudError::Rejected);
        target = *t;
    }
    return with_symbol<plugin::func_post_soft_match_pending>("bambu_network_post_soft_match_pending", [&](auto fn, void* agent, std::string* out) {
        plugin::SoftMatchPendingActionParams p;
        p.action        = action;
        p.spoolId       = *id;
        p.targetSpoolId = target;
        return fn(agent, std::move(p), out);
    });
}

} // namespace Slic3r::BblFilament
