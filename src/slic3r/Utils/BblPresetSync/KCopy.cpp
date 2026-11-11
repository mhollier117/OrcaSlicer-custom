#include "KCopy.hpp"

#include <algorithm>
#include <map>
#include <set>
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

std::vector<KPrinter> k_copy_printers(const std::vector<KPrinterFacts>& printers)
{
    std::vector<KPrinter> out;
    std::set<std::string> seen;
    auto take = [&](const KPrinterFacts& f) {
        if (f.dev_id.empty() || !seen.insert(f.dev_id).second) return;
        if (f.lan && !f.selected) return;
        if (!f.connected || !f.info_ready) return;
        if (!f.pa_flag && f.tap_pa_supported != true && !f.series_x1) return;
        KPrinter p;
        p.dev_id  = f.dev_id;
        p.name    = f.name;
        p.limited = !f.series_x1;
        for (const float d : f.diameters)
            if (d > 0.0f && std::find(p.diameters.begin(), p.diameters.end(), d) == p.diameters.end()) p.diameters.push_back(d);
        if (p.diameters.empty() && f.tap_diameter > 0) p.diameters.push_back(static_cast<float>(f.tap_diameter));
        if (p.diameters.empty()) return;
        out.push_back(std::move(p));
    };
    for (const KPrinterFacts& f : printers)
        if (f.selected) take(f);
    for (const KPrinterFacts& f : printers)
        if (!f.selected) take(f);
    return out;
}

} // namespace Slic3r::BblPresetSync
