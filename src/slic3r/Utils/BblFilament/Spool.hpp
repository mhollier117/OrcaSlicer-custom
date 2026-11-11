#pragma once

#include <nlohmann/json.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::BblFilament {

using json = nlohmann::json;

struct Spool
{
    std::string              id;          // cloud id as a decimal string; never empty for a stored spool
    std::string              filament_id; // "filamentId": Bambu's filament id (GFA00, P1234567...)
    std::string              rfid;        // "RFID" when valid, else empty
    std::string              tray_id_name;
    std::string              vendor, type, name; // filamentVendor / filamentType / filamentName
    std::string              color;              // as the cloud sent it (#RRGGBBAA or #RRGGBB)
    std::vector<std::string> colors;
    int                      color_type{2};          // 0 gradient, 1 multicolour, 2 single
    double                   total_net_weight{0};    // grams, full spool
    double                   net_weight{0};          // grams, remaining
    int                      status{0};              // 0 active, 1 information needed
    std::string              create_type;            // "manual" | "ams"
    std::string              created_at, updated_at; // the cloud's values as text
    std::string              note;
    bool                     in_printer{false};
    std::string              dev_id, ams_sn, slot_id, device_name;
    int                      ams_id{-1}, ams_type{-1};
};

// Lookups against OrcaSlicer's presets and colour table. Each may be empty (not available).
// The id is the Bambu filament id a spool carries.
struct FilamentLookup
{
    std::function<std::optional<bool>(const std::string& filament_id)> is_support;
    std::function<std::string(const std::string& filament_id)>         filament_type;
    std::function<std::string(const std::string& filament_id, const std::vector<std::string>& colors, int color_type)> tray_id_name;
};

bool                     valid_rfid(const std::string& rfid);                           // non-empty and not all '0'
std::optional<long long> parse_spool_id(const std::string& id);                         // 1..18 decimal digits, value > 0
int                      normalize_color_type(int color_type, std::size_t color_count); // 0,1,2 kept; else 1 if count > 1, else 2

std::optional<Spool> spool_from_cloud(const json& row);                // nullopt when the row is no object or has no usable id
json                 spool_to_page(const Spool& s);                    // the page's Spool object
void                 apply_page_patch(Spool& s, const json& patch);    // an edit from the page, applied to our copy

json create_body(const json& page_spool, const FilamentLookup& lookup);
// The PUT body for an edit; an empty object when the patch changes nothing the cloud stores.
// nullopt when current.id is not a valid id.
std::optional<json> update_body(const Spool& current, const json& page_patch, const FilamentLookup& lookup);

} // namespace Slic3r::BblFilament
