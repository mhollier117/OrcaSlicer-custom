#include "KCopy.hpp"

#include <algorithm>
#include <map>
#include <utility>

namespace Slic3r::BblPresetSync {

KCopyPlan plan_k_copy(const std::vector<KEntry>& table, const std::string& from_id, const std::string& to_id, bool limited)
{
    KCopyPlan plan;
    if (from_id.empty() || from_id == to_id) return plan;

    using Slot = std::pair<int, int>; // extruder_id, nozzle_volume_type
    std::map<Slot, int> used;
    for (const KEntry& e : table) ++used[{e.extruder_id, e.nozzle_volume_type}];

    auto same = [](const KEntry& a, const KEntry& b) {
        return a.name == b.name && a.extruder_id == b.extruder_id && a.nozzle_volume_type == b.nozzle_volume_type;
    };

    for (const KEntry& e : table) {
        if (e.filament_id != from_id) continue;

        KEntry copy       = e;
        copy.filament_id  = to_id;
        copy.cali_idx     = -1;
        const auto exists = [&](const KEntry& other) { return other.filament_id == to_id && same(other, e); };
        if (std::any_of(table.begin(), table.end(), exists) || std::any_of(plan.to_write.begin(), plan.to_write.end(), exists)) {
            ++plan.skipped_existing;
            continue;
        }
        int& count = used[{e.extruder_id, e.nozzle_volume_type}];
        if (limited && count >= K_TABLE_LIMIT) {
            ++plan.skipped_full;
            continue;
        }
        ++count;
        plan.to_write.push_back(std::move(copy));
    }
    return plan;
}

} // namespace Slic3r::BblPresetSync
