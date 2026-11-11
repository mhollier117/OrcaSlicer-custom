#pragma once

#include <optional>
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

// What the app knows about one printer (the selected one, or one of the account's), for choosing where K profiles are copied.
struct KPrinterFacts
{
    std::string         dev_id, name;
    bool                selected{false};
    bool                lan{false};        // in LAN mode
    bool                connected{false};  // MachineObject::is_connected()
    bool                info_ready{false}; // MachineObject::is_info_ready()
    bool                pa_flag{false};    // MachineObject::is_support_pa_calibration
    bool                series_x1{false};
    std::vector<float>  diameters;          // the nozzle diameters of the printer's extruders (0 = unknown)
    double              tap_diameter{0};    // the printer tap's nozzle diameter, 0 when unknown
    std::optional<bool> tap_pa_supported;   // the printer tap's PA support, when reported
};

// A printer the K profiles are copied on.
struct KPrinter
{
    std::string        dev_id, name;
    std::vector<float> diameters; // distinct, above zero, never empty
    bool               limited{true}; // see plan_k_copy
};

// The printers that take part, the selected one first, each once: connected, its info ready, not a LAN printer unless it
// is the selected one, calibration supported (the flag, the tap, or the X1 series) and at least one nozzle diameter
// known (the extruders', else the tap's).
std::vector<KPrinter> k_copy_printers(const std::vector<KPrinterFacts>& printers);

} // namespace Slic3r::BblPresetSync
