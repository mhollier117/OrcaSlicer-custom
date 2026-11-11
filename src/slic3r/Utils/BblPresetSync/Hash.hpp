#pragma once

#include "Types.hpp"

#include <set>
#include <string>

namespace Slic3r::BblPresetSync {

// Lowercase hex MD5 of data.
std::string md5_hex(const std::string& data);

// Keys that never take part in the upload hash.
const std::set<std::string>& hash_ignored_keys();

// md5 over "key=value\n" lines in map order, minus ignored keys and an empty base_id.
std::string upload_hash(const ValueMap& values);

} // namespace Slic3r::BblPresetSync
