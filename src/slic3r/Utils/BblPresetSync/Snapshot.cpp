#include "Snapshot.hpp"

#include "Hash.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"

#include <boost/filesystem/operations.hpp>

#include <ctime>
#include <optional>

namespace Slic3r::BblPresetSync {

namespace {

std::optional<PresetType> to_sync_type(Preset::Type t)
{
    switch (t) {
    case Preset::TYPE_PRINT: return PresetType::Print;
    case Preset::TYPE_FILAMENT: return PresetType::Filament;
    case Preset::TYPE_PRINTER: return PresetType::Printer;
    default: return std::nullopt;
    }
}

long long file_mtime(const std::string& file)
{
    if (file.empty()) return 0;
    boost::system::error_code ec;
    const std::time_t         t = boost::filesystem::last_write_time(boost::filesystem::path(file), ec);
    return ec ? 0 : static_cast<long long>(t);
}

} // namespace

EligibilityContext make_context(PresetBundle& bundle, const HasBambuId& has_bambu_id)
{
    EligibilityContext ctx;
    ctx.prints        = &bundle.prints;
    ctx.filaments     = &bundle.filaments;
    ctx.printers      = &bundle.printers;
    ctx.has_bambu_id  = has_bambu_id;
    ctx.id_map_usable = compute_id_map_usable(bundle.filaments, has_bambu_id);
    return ctx;
}

LocalItem make_item(const Preset& preset, PresetCollection& collection, PresetBundle& bundle, const EligibilityContext& ctx)
{
    LocalItem item;
    item.key             = Key{to_sync_type(preset.type).value_or(PresetType::Print), preset.name};
    item.cls             = classify(preset, collection, ctx);
    item.mtime           = file_mtime(preset.file);
    item.orca_setting_id = preset.setting_id;
    item.filament_id     = preset.filament_id;
    item.file            = preset.file;
    item.selected        = preset.name == collection.get_selected_preset_name();

    if (item.cls.eligibility == Eligibility::Eligible) {
        Preset copy = preset;
        if (bundle.get_differed_values_to_update(copy, item.upload) != 0) {
            item.upload.clear();
            item.cls.eligibility = Eligibility::Ineligible;
            item.cls.reason      = "no_upload_values";
        } else {
            item.hash = upload_hash(item.upload);
        }
    }
    return item;
}

LocalSnapshot build_snapshot(PresetBundle& bundle, const HasBambuId& has_bambu_id)
{
    LocalSnapshot snap;

    const EligibilityContext ctx = make_context(bundle, has_bambu_id);
    snap.id_map_usable           = ctx.id_map_usable;

    snap.prints_dirty    = bundle.prints.get_edited_preset().is_dirty;
    snap.filaments_dirty = bundle.filaments.get_edited_preset().is_dirty;
    snap.printers_dirty  = bundle.printers.get_edited_preset().is_dirty;

    auto add_collection = [&](PresetCollection& collection) {
        for (auto it = collection.begin(); it != collection.end(); ++it) {
            const Preset& preset = *it;
            if (!preset.is_user()) continue;
            if (!to_sync_type(preset.type)) continue;
            snap.items.push_back(make_item(preset, collection, bundle, ctx));
        }
    };
    add_collection(bundle.prints);
    add_collection(bundle.filaments);
    add_collection(bundle.printers);
    return snap;
}

std::vector<LocalPreset> to_local_presets(const LocalSnapshot& snapshot)
{
    std::vector<LocalPreset> out;
    out.reserve(snapshot.items.size());
    for (const LocalItem& item : snapshot.items) {
        LocalPreset l;
        l.key = item.key;
        if (item.cls.eligibility == Eligibility::Eligible) {
            l.state = LocalState::Eligible;
            l.hash  = item.hash;
            l.mtime = item.mtime;
        } else {
            l.state = LocalState::Ineligible;
        }
        out.push_back(std::move(l));
    }
    return out;
}

} // namespace Slic3r::BblPresetSync
