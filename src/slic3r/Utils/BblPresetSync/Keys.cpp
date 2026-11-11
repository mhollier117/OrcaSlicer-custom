#include "Keys.hpp"

#include "libslic3r/Preset.hpp"

#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <iterator>
#include <utility>
#include <vector>

namespace Slic3r::BblPresetSync {

namespace {

std::set<std::string> build_orca_keys(const std::vector<std::string>& options)
{
    std::set<std::string> keys(options.begin(), options.end());
    for (const char* extra : {"inherits", "base_id", "filament_id", "print_settings_id", "filament_settings_id", "printer_settings_id",
                              "compatible_printers", "compatible_printers_condition", "compatible_prints", "compatible_prints_condition"})
        keys.insert(extra);
    return keys;
}

} // namespace

bool orca_knows(PresetType type, const std::string& key)
{
    static const std::set<std::string> print    = build_orca_keys(Preset::print_options());
    static const std::set<std::string> filament = build_orca_keys(Preset::filament_options());
    static const std::set<std::string> printer  = build_orca_keys(Preset::printer_options());
    switch (type) {
    case PresetType::Print: return print.count(key) > 0;
    case PresetType::Filament: return filament.count(key) > 0;
    case PresetType::Printer: return printer.count(key) > 0;
    }
    return false;
}

StudioKeys StudioKeys::load(const boost::filesystem::path& file)
{
    std::set<std::string> keys;
    try {
        boost::nowide::ifstream in(file.string(), std::ios::binary);
        if (!in.is_open()) return StudioKeys();
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto        root = nlohmann::json::parse(text, nullptr, false);
        if (root.is_discarded() || !root.is_object()) return StudioKeys();
        const auto it = root.find("keys");
        if (it == root.end() || !it->is_array()) return StudioKeys();
        for (const auto& k : *it)
            if (k.is_string()) keys.insert(k.get<std::string>());
    } catch (...) {
        return StudioKeys();
    }
    return StudioKeys(std::move(keys));
}

bool StudioKeys::knows(const std::string& key) const
{
    return key == "inherits" || key == "base_id" || key == "filament_id" || m_keys.count(key) > 0;
}

} // namespace Slic3r::BblPresetSync
