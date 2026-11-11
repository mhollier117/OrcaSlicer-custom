#pragma once

#include "Spool.hpp"

#include <functional>
#include <optional>
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

struct AmsItem // one item of an ams sync, as sent
{
    std::string              rfid, vendor, type, name, filament_id;
    bool                     is_support{false};
    std::string              color;
    int                      color_type{2};
    std::vector<std::string> colors;
    long long                net_weight{0}, total_net_weight{0};
    std::string              tray_id_name, note;
    std::string              ams_sn, slot_id; // the mount; all four empty / 0 = not in a printer
    int                      ams_id{0}, ams_type{0};
    bool                     create_new{false};
};

struct SlotItem // one slot mapping
{
    std::string ams_sn, slot_id;
    int         ams_id{0}, ams_type{0};
    std::string spool_id; // empty = unbind the slot
    std::string rfid;     // only for an RFID spool that is bound by hand
};

struct AmsSyncResult
{
    std::string ams_sn, slot_id, spool_id; // spool_id empty = unresolved
};

struct SlotResult
{
    std::string ams_sn, slot_id;
    int         code{-1}; // 0 applied, anything else refused
};

struct SoftMatchEntry // a new official spool waiting for a decision
{
    Spool              hit;
    std::vector<Spool> candidates;
};

struct SoftMatchQueue
{
    std::vector<SoftMatchEntry> entries;
};

std::optional<int>         spool_id_as_int(const std::string& id); // parse_spool_id and <= INT_MAX
std::vector<AmsSyncResult> parse_ams_sync_results(const json& body);
std::vector<std::string>   parse_created_rfids(const json& body);
std::vector<SlotResult>    parse_slot_results(const json& body);
SoftMatchQueue             parse_soft_match(const json& body);

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
    virtual CloudReply sync_ams(const std::string& dev_id, const std::vector<AmsItem>& items)                                          = 0;
    virtual CloudReply sync_slots(const std::string& dev_id, const std::vector<SlotItem>& items)                                       = 0;
    virtual CloudReply soft_match_pending(const std::string& dev_id, const std::string& ams_sn)                                        = 0;
    virtual CloudReply soft_match_action(const std::string& action, const std::string& spool_id, const std::string& target_spool_id) = 0;
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
    CloudReply sync_ams(const std::string& dev_id, const std::vector<AmsItem>& items) override;
    CloudReply sync_slots(const std::string& dev_id, const std::vector<SlotItem>& items) override;
    CloudReply soft_match_pending(const std::string& dev_id, const std::string& ams_sn) override;
    CloudReply soft_match_action(const std::string& action, const std::string& spool_id, const std::string& target_spool_id) override;
};

} // namespace Slic3r::BblFilament
