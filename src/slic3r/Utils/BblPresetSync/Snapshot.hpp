#pragma once

#include "Eligibility.hpp"
#include "Planner.hpp"
#include "Types.hpp"

#include <string>
#include <vector>

namespace Slic3r {
class PresetBundle;
}

namespace Slic3r::BblPresetSync {

struct LocalItem
{
    Key         key;                 // from Preset::type
    Classified  cls;
    ValueMap    upload;              // filled only when Eligible
    std::string hash;                // upload_hash(upload), only when Eligible
    long long   mtime{0};            // last write time of preset.file, unix seconds; 0 when the file is missing
    std::string orca_setting_id;
    std::string filament_id;
    std::string file;                // preset.file
    bool        selected{false};     // it is the collection's selected preset
};

struct LocalSnapshot
{
    std::vector<LocalItem> items;    // every user preset of the three collections, whatever its eligibility
    bool prints_dirty{false}, filaments_dirty{false}, printers_dirty{false}; // get_edited_preset().is_dirty
    bool id_map_usable{false};
};

// Must be called on the thread that owns the presets (the UI thread in the app).
LocalSnapshot build_snapshot(PresetBundle& bundle, const HasBambuId& has_bambu_id);

std::vector<LocalPreset> to_local_presets(const LocalSnapshot& snapshot);

} // namespace Slic3r::BblPresetSync
