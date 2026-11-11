#pragma once

#include "Types.hpp"

#include <boost/filesystem/path.hpp>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace Slic3r::BblPresetSync {

// Keys OrcaSlicer can hold for a preset type: the print, filament or printer options, plus the linkage keys
// ("inherits", "base_id", "filament_id", the three "*_settings_id") and the compatibility keys.
bool orca_knows(PresetType type, const std::string& key);

// The type token of OrcaSlicer's own definition of a setting ("coFloat", "coStrings", ...); empty when OrcaSlicer has no such key.
std::string orca_type_name(const std::string& key);

// The setting keys Bambu Studio understands (resources/printers/bambu_studio_option_keys.json), with the type token
// of each where the list has one.
class StudioKeys
{
public:
    // A missing or invalid file gives an empty set. `source`, when given, is set to "bundled <studio_version>" for a loaded file.
    static StudioKeys load(const boost::filesystem::path& file, std::string* source = nullptr);

    explicit StudioKeys(std::set<std::string> keys = {}, std::map<std::string, std::string> types = {})
        : m_keys(std::move(keys)), m_types(std::move(types))
    {}

    // "inherits", "base_id" and "filament_id" always count as known.
    bool   knows(const std::string& key) const;
    bool   empty() const { return m_keys.empty(); }
    size_t size() const { return m_keys.size(); }

    // Studio's type token for the key ("coFloat", ...); empty when unknown.
    const std::string&                        type_of(const std::string& key) const;
    bool                                      has_types() const { return !m_types.empty(); }
    const std::map<std::string, std::string>& types() const { return m_types; }

private:
    std::set<std::string>              m_keys;
    std::map<std::string, std::string> m_types;
};

// Every key defined as  this->add("key", ...)  or  this->add_nullable("key", ...)  in the text of Studio's PrintConfig.cpp.
// A plain scan: find `this->add(` or `this->add_nullable(`, skip spaces, expect a quote, read [A-Za-z0-9_]+, expect a quote.
std::set<std::string> parse_studio_print_config(const std::string& source_text);

// The type of each key found by the same scan: after the key's closing quote skip spaces, expect a comma, skip spaces and
// read [A-Za-z0-9_]+; a token starting with "co" is the key's type. The first occurrence of a key wins; a key without such a
// token is not in the map.
std::map<std::string, std::string> parse_studio_print_config_types(const std::string& source_text);

// A parsed set is accepted only when it has at least 400 keys and contains layer_height, nozzle_temperature, filament_type,
// printable_area, sparse_infill_density and compatible_printers.
bool plausible_studio_keys(const std::set<std::string>& keys);

struct StudioKeyFile
{
    std::set<std::string>              keys;
    std::map<std::string, std::string> types;          // key -> Studio's type token; empty when the file has none
    std::string                        source;         // "bundled <studio_version>" or "BambuStudio <source_tag>"
    long long                          fetched_at{0};  // unix seconds; 0 for the bundled file
};

// Reads {"keys": [...], "types"?: {key: token}, "studio_version"?: "...", "source_tag"?: "...", "fetched_at"?: n}.
// nullopt when missing, invalid or not plausible.
std::optional<StudioKeyFile> read_studio_key_file(const boost::filesystem::path& file);

// Atomic write (temp + rename) of {"source_tag", "fetched_at", "keys", "types"}; false on failure.
bool write_studio_key_file(const boost::filesystem::path& file, const std::set<std::string>& keys,
                           const std::map<std::string, std::string>& types, const std::string& source_tag, long long fetched_at);

} // namespace Slic3r::BblPresetSync
