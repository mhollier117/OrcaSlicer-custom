#include "FmPresets.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <cctype>
#include <cstddef>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI::Fm {

namespace {

std::string bambu_of(const ToBambuId& to_bambu, const std::string& id)
{
    return to_bambu ? to_bambu(id) : id;
}

// A preset as preset_tree() offers it.
struct Offered
{
    std::string bid, vendor, type, name;
    bool        user{false};
};

// A system root preset whose id Bambu knows, or a user preset, with a vendor and a name; nullopt for any other.
std::optional<Offered> offered(PresetCollection& filaments, Preset& preset, const ToBambuId& to_bambu)
{
    Offered o;
    o.user = !preset.is_system && !preset.is_default;
    if (!o.user && filaments.get_preset_base(preset) != &preset)
        return std::nullopt;
    o.bid = bambu_of(to_bambu, preset.filament_id);
    if (o.bid.empty() || o.bid.rfind("OF", 0) == 0)
        return std::nullopt;
    o.vendor = preset.config.get_filament_vendor();
    if (o.vendor.empty())
        return std::nullopt;
    o.type = preset.config.get_filament_type();
    if (o.type.empty())
        o.type = "Other";
    o.name = filaments.get_preset_alias(preset, true);
    if (o.name.empty())
        o.name = preset.name;
    if (o.name.empty())
        return std::nullopt;
    return o;
}

std::string trimmed(const std::string& s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

bool starts_with(const std::string& s, const std::string& prefix) { return s.compare(0, prefix.size(), prefix) == 0; }

// formatTypeSeries of the page.
std::string format_type_series(const std::string& type, const std::string& series)
{
    const std::string t = trimmed(type), s = trimmed(series);
    if (t.empty())
        return s;
    if (s.empty() || s == t)
        return t;
    if (starts_with(s, t + " "))
        return s;
    return trimmed(t + " " + s);
}

// The colour as the page writes a tray's (canonicalizeHex): "#RRGGBB" in upper case, or "" when it is not valid.
std::string page_hex(const std::string& colour)
{
    const std::string hex = ColorCodes::normalize_hex(colour);
    return hex.empty() ? hex : hex.substr(0, 7);
}

} // namespace

std::string series_for_preset_name(const std::string& name, const std::string& vendor, const std::string& type)
{
    std::string value = trimmed(name);
    // Without a trailing " @..." part: from the first run of spaces followed by '@'.
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (!std::isspace(static_cast<unsigned char>(value[i])))
            continue;
        std::size_t j = i;
        while (j < value.size() && std::isspace(static_cast<unsigned char>(value[j])))
            ++j;
        if (j < value.size() && value[j] == '@') {
            value.erase(i);
            break;
        }
        i = j - 1;
    }
    const std::string fallback = format_type_series(type, name); // the page's series is the same name
    if (value.empty())
        return fallback;

    std::string compact;
    for (char c : vendor)
        if (!std::isspace(static_cast<unsigned char>(c)))
            compact.push_back(c);
    std::vector<std::string> aliases{vendor, compact};
    if (vendor == "Bambu Lab")
        aliases.emplace_back("Bambu");
    for (const std::string& alias : aliases) {
        if (alias.empty())
            continue;
        if (value == alias)
            return fallback;
        if (starts_with(value, alias + " ")) {
            value = trimmed(value.substr(alias.size() + 1));
            break;
        }
    }
    return value.empty() ? fallback : value;
}

json preset_tree(PresetCollection& filaments, const ToBambuId& to_bambu)
{
    std::map<std::string, std::map<std::string, std::vector<json>>> tree;
    std::set<std::string>                                          seen;

    for (Preset& preset : filaments) {
        const std::optional<Offered> o = offered(filaments, preset, to_bambu);
        if (!o)
            continue;
        const std::string key = o->user ? o->vendor + "\n" + o->type + "\n" + preset.name : o->bid;
        if (seen.count(key))
            continue;
        seen.insert(key);
        json item = {{"name", o->name}, {"series", o->name}, {"filament_id", o->bid}};
        if (o->user)
            item["is_user"] = true;
        tree[o->vendor][o->type].push_back(std::move(item));
    }

    json vendors = json::array();
    for (auto& [vendor, types] : tree) {
        json type_list = json::array();
        for (auto& [type, items] : types) {
            json series = json::array();
            json item_list = json::array();
            for (json& item : items) {
                series.push_back(item["name"]);
                item_list.push_back(std::move(item));
            }
            type_list.push_back({{"name", type}, {"series", std::move(series)}, {"items", std::move(item_list)}});
        }
        vendors.push_back({{"name", vendor}, {"types", std::move(type_list)}});
    }
    return {{"vendors", std::move(vendors)}};
}

json page_spool_for_filament(PresetCollection& filaments, const std::string& preset_filament_id, const ToBambuId& to_bambu,
                             const std::string& color, const std::vector<std::string>& colors, int color_type)
{
    if (preset_filament_id.empty())
        return nullptr;
    const std::string color_code = page_hex(color);
    if (color_code.empty())
        return nullptr;
    std::vector<std::string> list;
    if (colors.size() > 1) {
        for (const std::string& c : colors) {
            const std::string hex = page_hex(c);
            if (hex.empty())
                return nullptr;
            list.push_back(hex);
        }
    }
    for (Preset& preset : filaments) {
        if (preset.filament_id != preset_filament_id)
            continue;
        const std::optional<Offered> o = offered(filaments, preset, to_bambu);
        if (!o)
            continue;
        json spool = {{"brand", o->vendor},
                      {"material_type", o->type},
                      {"series", series_for_preset_name(o->name, o->vendor, o->type)},
                      {"setting_id", o->bid},
                      {"color_code", color_code},
                      {"color_type", list.empty() ? 2 : BblFilament::normalize_color_type(color_type, list.size())},
                      {"initial_weight", 1000},
                      {"net_weight", 1000},
                      {"spool_weight", 0},
                      {"entry_method", "manual"},
                      {"note", ""}};
        if (!list.empty())
            spool["colors"] = list;
        return spool;
    }
    return nullptr;
}

std::string tray_id_name_for(const ColorCodes& colors, const std::string& bambu_id, const std::vector<std::string>& hex, int color_type)
{
    if (bambu_id.size() <= 2)
        return {};
    const ColorEntry* e = colors.find(bambu_id, hex, color_type);
    if (e == nullptr || e->color_code.empty())
        return {};
    return bambu_id.substr(2) + "-" + e->color_code;
}

BblFilament::FilamentLookup filament_lookup(const PresetCollection* filaments, ToBambuId to_bambu, const ColorCodes* colors)
{
    BblFilament::FilamentLookup lookup;
    if (filaments != nullptr) {
        auto first_match = [filaments, to_bambu](const std::string& bid) -> const Preset* {
            if (bid.empty())
                return nullptr;
            for (const Preset& p : *filaments)
                if (bambu_of(to_bambu, p.filament_id) == bid)
                    return &p;
            return nullptr;
        };
        lookup.is_support = [first_match](const std::string& bid) -> std::optional<bool> {
            const Preset* p = first_match(bid);
            if (p == nullptr)
                return std::nullopt;
            const auto* opt = dynamic_cast<const ConfigOptionBools*>(p->config.option("filament_is_support"));
            if (opt == nullptr || opt->values.empty())
                return std::nullopt;
            return opt->values.front() != 0;
        };
        lookup.filament_type = [first_match](const std::string& bid) -> std::string {
            const Preset* p = first_match(bid);
            return p == nullptr ? std::string() : p->config.get_filament_type();
        };
    }
    if (colors != nullptr)
        lookup.tray_id_name = [colors](const std::string& bid, const std::vector<std::string>& hex, int color_type) {
            return tray_id_name_for(*colors, bid, hex, color_type);
        };
    return lookup;
}

} // namespace Slic3r::GUI::Fm
