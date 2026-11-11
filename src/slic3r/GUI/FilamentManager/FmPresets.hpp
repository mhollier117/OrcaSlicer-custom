#pragma once

#include "FmColorCodes.hpp"
#include "slic3r/Utils/BblFilament/Spool.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
class PresetCollection;
}

namespace Slic3r::GUI::Fm {

using json = nlohmann::json;

// Orca filament id -> Bambu filament id (an unknown id comes back unchanged).
using ToBambuId = std::function<std::string(const std::string& preset_filament_id)>;

// {"vendors":[{"name":V,"types":[{"name":T,"series":[names...],"items":[{"name":N,"series":N,"filament_id":B,("is_user":true)}]}]}]}
json preset_tree(PresetCollection& filaments, const ToBambuId& to_bambu);

// The series the page stores for a spool made from a preset with this display name (normalizePresetFilamentName of the
// page): the name without a trailing " @..." part and without the vendor in front ("Generic PETG" of Generic -> "PETG";
// "Bambu" stands for "Bambu Lab"); the type and the name as the page formats them when nothing is left.
std::string series_for_preset_name(const std::string& name, const std::string& vendor, const std::string& type);

// The page-shaped spool for a filament preset, or null when Bambu does not know the filament's id. The preset is the
// first one with that filament id that preset_tree() offers. Its series is series_for_preset_name() and its colours are
// "#RRGGBB" in upper case, as the page writes a tray's. `colors` counts only with more than one colour.
json page_spool_for_filament(PresetCollection& filaments, const std::string& preset_filament_id, const ToBambuId& to_bambu,
                             const std::string& color, const std::vector<std::string>& colors, int color_type);

// Lookups for the request bodies; `filaments` and `colors` may be null.
BblFilament::FilamentLookup filament_lookup(const PresetCollection* filaments, ToBambuId to_bambu, const ColorCodes* colors);

// "A00-K0" for an official colour of filament GFA00, else "".
std::string tray_id_name_for(const ColorCodes& colors, const std::string& bambu_id, const std::vector<std::string>& hex, int color_type);

} // namespace Slic3r::GUI::Fm
