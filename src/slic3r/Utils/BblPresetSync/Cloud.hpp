#pragma once

#include "Types.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Slic3r {
class NetworkAgent;
}

namespace Slic3r::BblPresetSync {

struct CloudRow
{
    PresetType  type{PresetType::Print};
    std::string name;
    std::string setting_id;
    long long   update_time{0};
};

struct ListResult
{
    bool                            ok{false};
    std::string                     error;
    std::vector<CloudRow>           rows;
    std::map<std::string, ValueMap> bodies_by_name; // exactly what the drain returned
};

struct WriteResult
{
    bool        ok{false};
    unsigned    http_code{0};
    std::string server_code;    // values_map["code"] after the call ("14" = preset limit)
    std::string setting_id;     // create: the new id
    long long   update_time{0}; // values_map["updated_time"] after the call, 0 if absent
};

class ICloud
{
public:
    virtual ~ICloud() = default;
    virtual ListResult  list(const std::string& bundle_version, const std::function<bool(const CloudRow&)>& want_body) = 0;
    virtual WriteResult create(const std::string& name, ValueMap values)                                              = 0;
    virtual WriteResult update(const std::string& setting_id, const std::string& name, ValueMap values)              = 0;
    virtual bool        remove(const std::string& setting_id)                                                         = 0;
};

// Production implementation over NetworkAgent with BBL_CLOUD_PROVIDER.
class AgentCloud : public ICloud
{
public:
    explicit AgentCloud(NetworkAgent* agent) : m_agent(agent) {}

    ListResult  list(const std::string& bundle_version, const std::function<bool(const CloudRow&)>& want_body) override;
    WriteResult create(const std::string& name, ValueMap values) override;
    WriteResult update(const std::string& setting_id, const std::string& name, ValueMap values) override;
    bool        remove(const std::string& setting_id) override;

private:
    NetworkAgent* m_agent;
};

} // namespace Slic3r::BblPresetSync
