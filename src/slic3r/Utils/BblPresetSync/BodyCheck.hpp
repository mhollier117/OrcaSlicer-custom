#pragma once

#include "Types.hpp"

#include <set>
#include <string>

namespace Slic3r::BblPresetSync {

// Keys that carry identity or linkage, not preset content.
const std::set<std::string>& metadata_keys();

enum class BodyStatus { Valid, WrongIdentity, MetadataOnly, RootMissingType };

BodyStatus check_body(const ValueMap& body, PresetType row_type, const std::string& row_setting_id, bool row_is_root);

} // namespace Slic3r::BblPresetSync
