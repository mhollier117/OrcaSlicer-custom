#pragma once

#include <map>
#include <optional>
#include <string>

namespace Slic3r::BblPresetSync {

enum class PresetType { Print, Filament, Printer };

// "print", "filament", "printer" (the BBL_JSON_KEY_TYPE values)
inline std::string type_string(PresetType t)
{
    switch (t) {
    case PresetType::Print: return "print";
    case PresetType::Filament: return "filament";
    case PresetType::Printer: return "printer";
    }
    return "";
}

inline std::optional<PresetType> parse_type(const std::string& s)
{
    if (s == "print") return PresetType::Print;
    if (s == "filament") return PresetType::Filament;
    if (s == "printer") return PresetType::Printer;
    return std::nullopt;
}

using ValueMap = std::map<std::string, std::string>;

struct Key
{
    PresetType  type{PresetType::Print};
    std::string name;

    // "<type>/<name>"
    std::string str() const { return type_string(type) + "/" + name; }

    // Splits at the first '/'.
    static std::optional<Key> parse(const std::string& s)
    {
        const size_t slash = s.find('/');
        if (slash == std::string::npos) return std::nullopt;
        const auto type = parse_type(s.substr(0, slash));
        if (!type) return std::nullopt;
        return Key{*type, s.substr(slash + 1)};
    }

    bool operator<(const Key& o) const
    {
        if (type != o.type) return static_cast<int>(type) < static_cast<int>(o.type);
        return name < o.name;
    }
    bool operator==(const Key& o) const { return type == o.type && name == o.name; }
};

} // namespace Slic3r::BblPresetSync
