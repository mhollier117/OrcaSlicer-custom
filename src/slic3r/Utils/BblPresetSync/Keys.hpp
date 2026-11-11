#pragma once

#include "Types.hpp"

#include <boost/filesystem/path.hpp>

#include <optional>
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
    // A missing or invalid file gives an empty set. `source`, when given, is set to "bundled <studio_version>" for a loaded file.
    static StudioKeys load(const boost::filesystem::path& file, std::string* source = nullptr);

    explicit StudioKeys(std::set<std::string> keys = {}) : m_keys(std::move(keys)) {}

    // "inherits", "base_id" and "filament_id" always count as known.
    bool   knows(const std::string& key) const;
    bool   empty() const { return m_keys.empty(); }
    size_t size() const { return m_keys.size(); }

private:
    std::set<std::string> m_keys;
};

// Every key defined as  this->add("key", ...)  or  this->add_nullable("key", ...)  in the text of Studio's PrintConfig.cpp.
// A plain scan: find `this->add(` or `this->add_nullable(`, skip spaces, expect a quote, read [A-Za-z0-9_]+, expect a quote.
std::set<std::string> parse_studio_print_config(const std::string& source_text);

// A parsed set is accepted only when it has at least 400 keys and contains layer_height, nozzle_temperature, filament_type,
// printable_area, sparse_infill_density and compatible_printers.
bool plausible_studio_keys(const std::set<std::string>& keys);

struct StudioKeyFile
{
    std::set<std::string> keys;
    std::string           source;         // "bundled <studio_version>" or "BambuStudio <source_tag>"
    long long             fetched_at{0};  // unix seconds; 0 for the bundled file
};

// Reads {"keys": [...], "studio_version"?: "...", "source_tag"?: "...", "fetched_at"?: n}. nullopt when missing, invalid or not plausible.
std::optional<StudioKeyFile> read_studio_key_file(const boost::filesystem::path& file);

// Atomic write (temp + rename) of {"source_tag", "fetched_at", "keys"}; false on failure.
bool write_studio_key_file(const boost::filesystem::path& file, const std::set<std::string>& keys, const std::string& source_tag,
                           long long fetched_at);

} // namespace Slic3r::BblPresetSync
