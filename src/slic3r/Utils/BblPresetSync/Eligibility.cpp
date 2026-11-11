#include "Eligibility.hpp"

#include "libslic3r/Config.hpp"

namespace Slic3r::BblPresetSync {

namespace {

bool is_bbl_vendor(const Preset& p) { return p.vendor != nullptr && p.vendor->id == "BBL"; }

bool is_bbl_printer(const PresetCollection* printers, const std::string& name)
{
    if (printers == nullptr) return false;
    const Preset* p = printers->find_preset(name, false);
    if (p == nullptr || p->name != name) return false;
    const VendorProfile* v = printers->get_preset_with_vendor_profile(*p).vendor;
    return v != nullptr && v->id == "BBL";
}

bool compatible_only_with_bbl_printers(const Preset& preset, const EligibilityContext& ctx)
{
    const auto* opt = preset.config.option<ConfigOptionStrings>("compatible_printers");
    if (opt == nullptr || opt->values.empty()) return false;
    for (const std::string& name : opt->values)
        if (!is_bbl_printer(ctx.printers, name)) return false;
    return true;
}

Classified make(Eligibility e, Shape s, const char* reason = "")
{
    Classified c;
    c.eligibility = e;
    c.shape       = s;
    c.reason      = reason;
    return c;
}

} // namespace

bool compute_id_map_usable(const PresetCollection& filaments, const HasBambuId& has_bambu_id)
{
    if (!has_bambu_id) return false;
    for (auto it = filaments.begin(); it != filaments.end(); ++it)
        if (it->is_system && is_bbl_vendor(*it) && has_bambu_id(it->filament_id)) return true;
    return false;
}

Classified classify(const Preset& preset, const PresetCollection& collection, const EligibilityContext& ctx)
{
    if (!preset.is_user() || preset.is_project_embedded || !preset.bundle_id.empty())
        return make(Eligibility::Ineligible, Shape::Child, "not_a_user_preset");

    const bool is_filament = preset.type == Preset::TYPE_FILAMENT;

    if (!preset.inherits().empty()) {
        const Preset* parent = collection.find_preset2(preset.inherits(), true);
        if (parent == nullptr) return make(Eligibility::Ineligible, Shape::Child, "parent_missing");
        if (!parent->is_system) return make(Eligibility::Ineligible, Shape::Child, "unsupported_chain");

        bool known = is_bbl_vendor(*parent) && !parent->setting_id.empty();
        if (known && is_filament) known = ctx.id_map_usable && ctx.has_bambu_id && ctx.has_bambu_id(parent->filament_id);
        if (known) return make(Eligibility::Eligible, Shape::Child);
        if (is_filament) {
            if (!ctx.id_map_usable) return make(Eligibility::Ineligible, Shape::Child, "id_map_missing");
            if (compatible_only_with_bbl_printers(preset, ctx))
                return make(Eligibility::Candidate, Shape::Child, "parent_unknown_to_bambu");
        }
        return make(Eligibility::Ineligible, Shape::Child, "not_bambu_based");
    }

    if (!is_filament) return make(Eligibility::Ineligible, Shape::Root, "root_not_supported");
    if (!compatible_only_with_bbl_printers(preset, ctx)) return make(Eligibility::Ineligible, Shape::Root, "not_bambu_based");
    if (preset.filament_id.empty()) return make(Eligibility::Candidate, Shape::Root, "no_filament_id");
    return make(Eligibility::Eligible, Shape::Root);
}

} // namespace Slic3r::BblPresetSync
