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

// Lookups for the request bodies; `filaments` and `colors` may be null.
BblFilament::FilamentLookup filament_lookup(const PresetCollection* filaments, ToBambuId to_bambu, const ColorCodes* colors);

// "A00-K0" for an official colour of filament GFA00, else "".
std::string tray_id_name_for(const ColorCodes& colors, const std::string& bambu_id, const std::vector<std::string>& hex, int color_type);

} // namespace Slic3r::GUI::Fm
