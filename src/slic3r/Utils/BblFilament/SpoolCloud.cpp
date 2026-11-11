#include "SpoolCloud.hpp"

#include "slic3r/Utils/BBLNetworkPlugin.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

#include <algorithm>
#include <cstddef>
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

} // namespace

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

} // namespace Slic3r::BblFilament
