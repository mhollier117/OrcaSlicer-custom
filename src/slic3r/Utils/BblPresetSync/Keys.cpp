#include "Keys.hpp"

#include "libslic3r/Preset.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <cctype>
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

StudioKeys StudioKeys::load(const boost::filesystem::path& file, std::string* source)
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
        if (source != nullptr) {
            const auto v = root.find("studio_version");
            *source      = "bundled" + (v != root.end() && v->is_string() ? " " + v->get<std::string>() : std::string());
        }
    } catch (...) {
        return StudioKeys();
    }
    return StudioKeys(std::move(keys));
}

bool StudioKeys::knows(const std::string& key) const
{
    return key == "inherits" || key == "base_id" || key == "filament_id" || m_keys.count(key) > 0;
}

std::set<std::string> parse_studio_print_config(const std::string& text)
{
    std::set<std::string> keys;
    const std::string     marker = "this->add";
    size_t                pos    = 0;
    while ((pos = text.find(marker, pos)) != std::string::npos) {
        size_t i = pos + marker.size();
        pos      = i;
        if (text.compare(i, 10, "_nullable(") == 0) i += 10;
        else if (i < text.size() && text[i] == '(') i += 1;
        else continue;
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i >= text.size() || text[i] != '"') continue;
        ++i;
        const size_t start = i;
        while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_')) ++i;
        if (i == start || i >= text.size() || text[i] != '"') continue;
        keys.insert(text.substr(start, i - start));
    }
    return keys;
}

bool plausible_studio_keys(const std::set<std::string>& keys)
{
    if (keys.size() < 400) return false;
    for (const char* k : {"layer_height", "nozzle_temperature", "filament_type", "printable_area", "sparse_infill_density", "compatible_printers"})
        if (!keys.count(k)) return false;
    return true;
}

std::optional<StudioKeyFile> read_studio_key_file(const boost::filesystem::path& file)
{
    try {
        boost::nowide::ifstream in(file.string(), std::ios::binary);
        if (!in.is_open()) return std::nullopt;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const auto        root = nlohmann::json::parse(text, nullptr, false);
        if (root.is_discarded() || !root.is_object()) return std::nullopt;
        const auto it = root.find("keys");
        if (it == root.end() || !it->is_array()) return std::nullopt;
        StudioKeyFile out;
        for (const auto& k : *it)
            if (k.is_string()) out.keys.insert(k.get<std::string>());
        if (!plausible_studio_keys(out.keys)) return std::nullopt;
        const auto tag     = root.find("source_tag");
        const auto version = root.find("studio_version");
        const auto fetched = root.find("fetched_at");
        if (tag != root.end() && tag->is_string()) out.source = "BambuStudio " + tag->get<std::string>();
        else out.source = "bundled" + (version != root.end() && version->is_string() ? " " + version->get<std::string>() : std::string());
        if (fetched != root.end() && fetched->is_number_integer()) out.fetched_at = fetched->get<long long>();
        return out;
    } catch (...) {
        return std::nullopt;
    }
}

bool write_studio_key_file(const boost::filesystem::path& file, const std::set<std::string>& keys, const std::string& source_tag,
                           long long fetched_at)
{
    namespace fs = boost::filesystem;
    try {
        nlohmann::json root;
        root["source_tag"] = source_tag;
        root["fetched_at"] = fetched_at;
        root["keys"]       = keys;

        boost::system::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        fs::path tmp = file;
        tmp += ".tmp";
        {
            boost::nowide::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
            if (!out.is_open()) return false;
            out << root.dump(1);
            out.flush();
            if (!out.good()) {
                out.close();
                fs::remove(tmp, ec);
                return false;
            }
        }
        fs::rename(tmp, file, ec);
        if (ec) {
            fs::remove(file, ec);
            boost::system::error_code ec2;
            fs::rename(tmp, file, ec2);
            if (ec2) {
                fs::remove(tmp, ec);
                return false;
            }
        }
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace Slic3r::BblPresetSync
