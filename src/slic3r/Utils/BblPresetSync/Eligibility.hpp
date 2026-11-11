#pragma once

#include "libslic3r/Preset.hpp"

#include <functional>
#include <string>

namespace Slic3r::BblPresetSync {

enum class Shape { Child, Root };
enum class Eligibility { Eligible, Candidate, Ineligible };

struct Classified
{
    Eligibility eligibility{Eligibility::Ineligible};
    Shape       shape{Shape::Child};
    std::string reason; // set for Ineligible and Candidate
};

// True when this Orca filament id has a Bambu id (production: BBLPrinterAgent::bambu_filament_id(id) != id).
using HasBambuId = std::function<bool(const std::string& orca_filament_id)>;

struct EligibilityContext
{
    const PresetCollection* prints{nullptr};
    const PresetCollection* filaments{nullptr};
    const PresetCollection* printers{nullptr};
    HasBambuId              has_bambu_id;
    bool                    id_map_usable{false};
};

// The id map counts as loaded when at least one BBL-vendor system filament preset has a Bambu id.
bool compute_id_map_usable(const PresetCollection& filaments, const HasBambuId& has_bambu_id);

Classified classify(const Preset& preset, const PresetCollection& collection, const EligibilityContext& ctx);

} // namespace Slic3r::BblPresetSync
