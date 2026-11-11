#pragma once

#include "Types.hpp"

#include <boost/filesystem/path.hpp>

#include <set>
#include <string>
#include <utility>

namespace Slic3r::BblPresetSync {

// Keys OrcaSlicer can hold for a preset type: the print, filament or printer options, plus the linkage keys
// ("inherits", "base_id", "filament_id", the three "*_settings_id") and the compatibility keys.
bool orca_knows(PresetType type, const std::string& key);

// The setting keys Bambu Studio understands (resources/printers/bambu_studio_option_keys.json).
class StudioKeys
{
public:
    // A missing or invalid file gives an empty set.
    static StudioKeys load(const boost::filesystem::path& file);

    explicit StudioKeys(std::set<std::string> keys = {}) : m_keys(std::move(keys)) {}

    // "inherits", "base_id" and "filament_id" always count as known.
    bool   knows(const std::string& key) const;
    bool   empty() const { return m_keys.empty(); }
    size_t size() const { return m_keys.size(); }

private:
    std::set<std::string> m_keys;
};

} // namespace Slic3r::BblPresetSync
