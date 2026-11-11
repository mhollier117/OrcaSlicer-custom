#pragma once

#include <string>
#include <vector>

namespace Slic3r::BblPresetSync {

// The fields of PACalibResult that matter for copying a K (flow dynamics) profile.
struct KEntry
{
    std::string filament_id, name;
    float       k_value{0}, n_coef{0}, nozzle_diameter{0};
    int         extruder_id{0};
    int         nozzle_volume_type{0};
    int         cali_idx{-1};
};

struct KCopyPlan
{
    std::vector<KEntry> to_write;      // filament_id already replaced by the new id, cali_idx = -1
    int                 skipped_existing{0}; // an entry with the same name, extruder and volume type already exists under the new id
    int                 skipped_full{0};     // no room left for that nozzle
};

constexpr int K_TABLE_LIMIT = 16;

// `table`: every entry the printer returned for one nozzle diameter (all filaments).
// `limited`: the printer holds at most K_TABLE_LIMIT entries per (extruder_id, nozzle_volume_type).
KCopyPlan plan_k_copy(const std::vector<KEntry>& table, const std::string& from_id, const std::string& to_id, bool limited);

} // namespace Slic3r::BblPresetSync
