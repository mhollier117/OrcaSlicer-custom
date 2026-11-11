#include "FmPresets.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

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

} // namespace

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
    const std::string color_code = ColorCodes::normalize_hex(color);
    if (color_code.empty())
        return nullptr;
    std::vector<std::string> list;
    if (colors.size() > 1) {
        for (const std::string& c : colors) {
            const std::string hex = ColorCodes::normalize_hex(c);
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
                      {"series", o->name},
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
