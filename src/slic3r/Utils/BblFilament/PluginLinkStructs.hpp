#pragma once

#include <string>
#include <vector>

namespace Slic3r::BblFilament::plugin {

// Parameter structs of three network-plugin calls that BBLNetworkPlugin does not wrap. The plugin takes them BY VALUE
// across the DLL boundary, so member order, types and defaults must match the plugin's header exactly (OBN
// `bambu_networking.hpp:467-500`, equal to Bambu Studio's 02.08.03 header). Do not reorder or retype anything here.
// `AmsSyncItem` / `AmsSyncParams` are already in `slic3r/Utils/bambu_networking.hpp`.
struct SlotMappingItem
{
    std::string amsSn;
    std::string slotId;
    int         spoolId = 0;
    std::string rfid;
    int         amsId   = 0;
    int         amsType = 0;
};

struct SlotMappingsSyncParams
{
    std::string                  devId;
    std::vector<SlotMappingItem> mappings;
};

struct SoftMatchPendingParams
{
    std::string devId;
    std::string amsSn;
};

struct SoftMatchPendingActionParams
{
    std::string action;
    int         spoolId       = 0;
    int         targetSpoolId = 0;
};

// Symbol names, resolved with BBLNetworkPlugin::instance().get_function() at every call. All return 0 on success and
// fill *http_body with the raw reply.
using func_sync_slot_mappings      = int (*)(void* agent, SlotMappingsSyncParams params, std::string* http_body);
using func_get_soft_match_pending  = int (*)(void* agent, SoftMatchPendingParams params, std::string* http_body);
using func_post_soft_match_pending = int (*)(void* agent, SoftMatchPendingActionParams params, std::string* http_body);

} // namespace Slic3r::BblFilament::plugin
