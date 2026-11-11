#pragma once

#include "Eligibility.hpp"
#include "Mirror.hpp"

#include <boost/filesystem/path.hpp>

#include <functional>
#include <string>

namespace Slic3r {
class Preset;
class PresetBundle;
} // namespace Slic3r

namespace Slic3r::BblPresetSync {

struct LocalOpsContext
{
    PresetBundle*              bundle{nullptr}; // not null
    HasBambuId                 has_bambu_id;
    boost::filesystem::path    backup_dir;      // Ledger::backup_dir(data_dir)
    std::string                bambu_uid;       // for filament ids
    std::function<long long()> now;             // unix seconds
    // Queue the Orca-cloud delete of a preset about to be deleted (production: GUI_App::delete_preset_from_cloud).
    std::function<void(const Preset&)> queue_orca_cloud_delete;
};

// Each returns what the mirror needs; none touches wx. Call on the thread that owns the presets.
OpOutcome apply_pull(const LocalOpsContext& ctx, const PullOp& op);
OpOutcome delete_local(const LocalOpsContext& ctx, const DeleteOp& op);

struct ConvertResult
{
    OpOutcome   outcome;
    std::string from_filament_id, to_filament_id;
};
ConvertResult convert_filament(const LocalOpsContext& ctx, const std::string& preset_name);

} // namespace Slic3r::BblPresetSync
