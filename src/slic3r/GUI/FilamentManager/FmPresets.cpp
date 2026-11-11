#include "FmPresets.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <map>
#include <optional>
#include <set>
#include <utility>

namespace Slic3r::GUI::Fm {

namespace {

std::string bambu_of(const ToBambuId& to_bambu, const std::string& id)
{
    return to_bambu ? to_bambu(id) : id;
}

} // namespace

json preset_tree(PresetCollection& filaments, const ToBambuId& to_bambu)
{
    std::map<std::string, std::map<std::string, std::vector<json>>> tree;
    std::set<std::string>                                          seen;

    for (Preset& preset : filaments) {
        const bool user = !preset.is_system && !preset.is_default;
        if (!user && filaments.get_preset_base(preset) != &preset)
            continue;
        const std::string bid = bambu_of(to_bambu, preset.filament_id);
        if (bid.empty() || bid.rfind("OF", 0) == 0)
            continue;
        const std::string vendor = preset.config.get_filament_vendor();
        if (vendor.empty())
            continue;
        std::string type = preset.config.get_filament_type();
        if (type.empty())
            type = "Other";
        const std::string key = user ? vendor + "\n" + type + "\n" + preset.name : bid;
        if (seen.count(key))
            continue;
        std::string name = filaments.get_preset_alias(preset, true);
        if (name.empty())
            name = preset.name;
        if (name.empty())
            continue;
        seen.insert(key);
        json item = {{"name", name}, {"series", name}, {"filament_id", bid}};
        if (user)
            item["is_user"] = true;
        tree[vendor][type].push_back(std::move(item));
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
