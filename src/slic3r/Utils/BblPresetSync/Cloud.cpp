#include "Cloud.hpp"

#include "slic3r/Utils/CloudProvider.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <cstdlib>

namespace Slic3r::BblPresetSync {

namespace {

std::string value_of(const ValueMap& m, const char* key)
{
    auto it = m.find(key);
    return it == m.end() ? std::string() : it->second;
}

void fill_from_map(WriteResult& w, const ValueMap& values)
{
    w.server_code = value_of(values, "code");
    w.update_time = std::atoll(value_of(values, "updated_time").c_str());
}

} // namespace

ListResult AgentCloud::list(const std::string& bundle_version, const std::function<bool(const CloudRow&)>& want_body)
{
    ListResult r;
    if (m_agent == nullptr) {
        r.error = "list failed: no agent";
        return r;
    }

    auto check = [&r, &want_body](std::map<std::string, std::string> info) -> bool {
        const auto type = parse_type(value_of(info, "type"));
        if (!type) return false;
        CloudRow row;
        row.type        = *type;
        row.name        = value_of(info, "name");
        row.setting_id  = value_of(info, "setting_id");
        row.update_time = std::atoll(value_of(info, "updated_time").c_str());
        r.rows.push_back(row);
        return want_body(row);
    };
    int rc = m_agent->get_setting_list2(bundle_version, check, nullptr, nullptr, BBL_CLOUD_PROVIDER);
    if (rc != 0) {
        r.error = "list failed: " + std::to_string(rc);
        return r;
    }
    rc = m_agent->get_user_presets(&r.bodies_by_name, BBL_CLOUD_PROVIDER);
    if (rc != 0) {
        r.error = "list failed: user presets " + std::to_string(rc);
        return r;
    }
    r.ok = true;
    return r;
}

WriteResult AgentCloud::create(const std::string& name, ValueMap values)
{
    WriteResult w;
    if (m_agent == nullptr) return w;
    unsigned          http = 0;
    const std::string id   = m_agent->request_setting_id(name, &values, &http, BBL_CLOUD_PROVIDER);
    w.http_code            = http;
    w.setting_id           = id;
    w.ok                   = !id.empty();
    fill_from_map(w, values);
    return w;
}

WriteResult AgentCloud::update(const std::string& setting_id, const std::string& name, ValueMap values)
{
    WriteResult w;
    if (m_agent == nullptr) return w;
    unsigned http = 0;
    const int rc  = m_agent->put_setting(setting_id, name, &values, &http, BBL_CLOUD_PROVIDER, false);
    w.http_code   = http;
    w.ok          = rc == 0 && http < 400;
    fill_from_map(w, values);
    return w;
}

bool AgentCloud::remove(const std::string& setting_id)
{
    return m_agent != nullptr && m_agent->delete_setting(setting_id, BBL_CLOUD_PROVIDER) == 0;
}

} // namespace Slic3r::BblPresetSync
