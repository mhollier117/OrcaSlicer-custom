#pragma once

#include <functional>
#include <string>
#include <vector>

namespace Slic3r::BblPresetSync {

// Text before the first " @"; the whole name when there is none (or the name starts with " @").
std::string filament_base_name(const std::string& preset_name);

// "P" + first 7 hex chars of md5(base_name + "@" + bambu_uid + salt).
std::string make_filament_id(const std::string& base_name, const std::string& bambu_uid, const std::string& salt = "");

struct ExistingRoot
{
    std::string filament_id;
    std::string base_name;
};

// Reuses the id of a root with the same base name; otherwise hashes, salting on a clash with another
// base name. Returns an empty string after 16 failed salts.
std::string choose_filament_id(const std::string&                  preset_name,
                               const std::string&                  bambu_uid,
                               const std::vector<ExistingRoot>&    roots,
                               const std::function<std::string()>& next_salt);

} // namespace Slic3r::BblPresetSync
