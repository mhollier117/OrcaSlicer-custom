#pragma once

#include "Spool.hpp"

#include <functional>
#include <string>
#include <vector>

namespace Slic3r::BblFilament {

enum class CloudError {
    None,
    Unavailable, // no plugin, no agent, or the plugin lacks the call: nothing was sent
    NotFound,    // the server answered: no such record
    Rejected,    // the server answered with another client error: not applied
    Unknown      // no usable answer (timeout, transport error, server error): it may or may not have been applied
};

struct CloudReply
{
    CloudError  error{CloudError::None};
    int         rc{0};   // the plugin's return code
    std::string message; // the server's short error text, at most 200 bytes; never a whole body
    json        body;    // the parsed response body on success; null otherwise
    bool        ok() const { return error == CloudError::None; }
};

// Pure: turns a plugin return code and raw body into a reply.
CloudReply classify_reply(int rc, const std::string& raw_body);

class ISpoolCloud
{
public:
    virtual ~ISpoolCloud()                                              = default;
    virtual CloudReply list_page(int offset, int limit)                 = 0;
    virtual CloudReply create(const json& body)                         = 0;
    virtual CloudReply update(const std::string& id, const json& body)  = 0;
    virtual CloudReply remove(const std::vector<std::string>& ids)      = 0;
    virtual CloudReply config()                                         = 0;
};

struct ListResult
{
    bool               ok{false};
    CloudReply         failure;               // set when !ok
    std::vector<Spool> spools;                // complete list, cloud order, no duplicate ids
    bool               paging_suspect{false}; // the server did not honour the offset, or the page cap was hit
    int                skipped_rows{0};       // rows without a usable id
};

constexpr int k_spool_page_size = 20; // the value Bambu Studio sends
constexpr int k_spool_max_pages = 500;

// Pages through the whole list. A failed page fails the whole list. `cancelled` may be empty.
ListResult list_all(ISpoolCloud& cloud, const std::function<bool()>& cancelled);

// The real thing, over BBLNetworkPlugin. Every method must be called on the spool worker thread only.
class PluginSpoolCloud : public ISpoolCloud
{
public:
    CloudReply list_page(int offset, int limit) override;
    CloudReply create(const json& body) override;
    CloudReply update(const std::string& id, const json& body) override;
    CloudReply remove(const std::vector<std::string>& ids) override;
    CloudReply config() override;
};

} // namespace Slic3r::BblFilament
