#include "LocalOps.hpp"

#include "FilamentId.hpp"
#include "Snapshot.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r_version.h"

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/filesystem/operations.hpp>

#include <algorithm>
#include <ctime>
#include <string>
#include <vector>

namespace Slic3r::BblPresetSync {

namespace fs = boost::filesystem;

namespace {

OpOutcome outcome(OpResult result, const std::string& reason = "", const std::string& new_hash = "")
{
    OpOutcome o;
    o.result   = result;
    o.reason   = reason;
    o.new_hash = new_hash;
    return o;
}

PresetCollection& collection_for(PresetBundle& bundle, PresetType type)
{
    switch (type) {
    case PresetType::Print: return bundle.prints;
    case PresetType::Filament: return bundle.filaments;
    case PresetType::Printer: return bundle.printers;
    }
    return bundle.prints;
}

Preset* find_user_preset(PresetCollection& collection, const std::string& name)
{
    Preset* preset = collection.find_preset(name, false, true);
    return (preset != nullptr && preset->name == name && preset->is_user()) ? preset : nullptr;
}

std::string current_hash(const LocalOpsContext& ctx, const Key& key)
{
    PresetCollection& collection = collection_for(*ctx.bundle, key.type);
    const Preset*     preset     = find_user_preset(collection, key.name);
    if (preset == nullptr) return "";
    const EligibilityContext ectx = make_context(*ctx.bundle, ctx.has_bambu_id);
    return make_item(*preset, collection, *ctx.bundle, ectx).hash;
}

std::string type_name(Preset::Type t)
{
    switch (t) {
    case Preset::TYPE_PRINT: return "print";
    case Preset::TYPE_FILAMENT: return "filament";
    default: return "printer";
    }
}

std::string safe_name(const std::string& s)
{
    std::string out = s;
    for (char& c : out) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) c = '_';
    }
    return out;
}

// Copies the preset file and its .info into the backup folder; false on any failure.
bool backup_files(const LocalOpsContext& ctx, const Preset& preset, const std::string& tag)
{
    boost::system::error_code ec;
    fs::create_directories(ctx.backup_dir, ec);
    if (ec) return false;

    std::string stamp = boost::posix_time::to_iso_string(boost::posix_time::from_time_t(static_cast<std::time_t>(ctx.now())));
    std::replace(stamp.begin(), stamp.end(), 'T', '-');
    const std::string stem = stamp + "_" + type_name(preset.type) + "_" +
                             safe_name(preset.name) + "_" + tag;

    const fs::path src(preset.file);
    if (preset.file.empty() || !fs::exists(src, ec)) return false;
    fs::copy_file(src, ctx.backup_dir / (stem + ".json"), fs::copy_options::overwrite_existing, ec);
    if (ec) return false;

    fs::path info = src;
    info.replace_extension(".info");
    if (fs::exists(info, ec)) {
        fs::copy_file(info, ctx.backup_dir / (stem + ".info"), fs::copy_options::overwrite_existing, ec);
        if (ec) return false;
    }
    return true;
}

std::string in_use(const LocalOpsContext& ctx, PresetCollection& collection, const std::string& name)
{
    if (collection.get_selected_preset_name() == name) return "selected";
    if (collection.type() == Preset::TYPE_FILAMENT) {
        const auto& used = ctx.bundle->filament_presets;
        if (std::find(used.begin(), used.end(), name) != used.end()) return "used by the project";
    }
    for (auto it = collection.begin(); it != collection.end(); ++it)
        if (it->inherits() == name) return "has presets based on it";
    return "";
}

bool dirty(PresetCollection& collection)
{
    return collection.get_edited_preset().is_dirty;
}

std::string value_of(const ValueMap& values, const char* key)
{
    auto it = values.find(key);
    return it == values.end() ? std::string() : it->second;
}

// Steps 4-6 of delete_local, shared with the rename part of apply_pull. The preset is known to exist and not be in use.
OpOutcome delete_checked(const LocalOpsContext& ctx, PresetCollection& collection, const Preset& preset, bool backup_local)
{
    const std::string name = preset.name;
    if (backup_local && !backup_files(ctx, preset, "deleted")) return outcome(OpResult::Failed, "backup failed");
    if (ctx.queue_orca_cloud_delete) ctx.queue_orca_cloud_delete(preset);
    if (!collection.delete_preset(name)) return outcome(OpResult::Failed, "could not delete");
    return outcome(OpResult::Applied);
}

} // namespace

OpOutcome apply_pull(const LocalOpsContext& ctx, const PullOp& op)
{
    PresetCollection& collection = collection_for(*ctx.bundle, op.key.type);

    if (dirty(collection)) return outcome(OpResult::Postponed, "unsaved edits");
    if (current_hash(ctx, op.key) != op.expected_local_hash) return outcome(OpResult::Postponed, "changed since planning");

    Preset* old_preset = nullptr;
    if (op.rename_from_set) {
        old_preset = find_user_preset(collection, op.rename_from.name);
        if (old_preset != nullptr) {
            const std::string reason = in_use(ctx, collection, op.rename_from.name);
            if (!reason.empty()) return outcome(OpResult::Postponed, "old preset " + reason);
        }
    }

    // Materialize the body into a full config without touching the collection.
    ValueMap values = op.body;
    values.emplace(BBL_JSON_KEY_SETTING_ID, "");
    values.emplace(BBL_JSON_KEY_USER_ID, "");
    boost::optional<Semver> version;
    {
        auto it = values.find(BBL_JSON_KEY_VERSION);
        if (it != values.end()) version = Semver::parse(it->second);
        if (!version) {
            version                     = Semver::parse(SoftFever_VERSION);
            values[BBL_JSON_KEY_VERSION] = SoftFever_VERSION;
        }
    }
    DynamicPrintConfig         config;
    PresetsConfigSubstitutions subs;
    if (!collection.load_user_preset(op.key.name, values, subs, ForwardCompatibilitySubstitutionRule::Enable,
                                     PresetOrigin(PresetOrigin::Kind::User), &config))
        return outcome(OpResult::Unresolvable, "parent profile or version not usable");

    const bool is_root = value_of(op.body, BBL_JSON_KEY_BASE_ID).empty() && value_of(op.body, BBL_JSON_KEY_INHERITS).empty();
    Preset* parent = nullptr;
    if (!is_root) {
        parent = collection.find_preset2(Preset::inherits(config), true);
        if (parent == nullptr) return outcome(OpResult::Unresolvable, "parent profile not found");
    }
    const bool is_filament = op.key.type == PresetType::Filament;

    if (const Preset* existing = find_user_preset(collection, op.key.name); existing != nullptr && op.backup_local)
        if (!backup_files(ctx, *existing, "local")) return outcome(OpResult::Failed, "backup failed");

    Preset temp(collection.type(), op.key.name + " (bbl sync)");
    temp.config                 = config;
    temp.is_system = temp.is_default = false;
    temp.vendor                 = nullptr;
    temp.version                = *version;
    if (is_filament) temp.filament_id = is_root ? value_of(op.body, BBL_JSON_KEY_FILAMENT_ID) : parent->filament_id;

    const std::string prev = collection.get_selected_preset_name();
    collection.save_current_preset(op.key.name, /*detach*/ is_root, /*save_to_project*/ false, &temp);
    Preset* saved = collection.find_preset(op.key.name, false, true);
    if (saved == nullptr) return outcome(OpResult::Failed, "could not save");

    if (is_root) {
        if (is_filament) saved->filament_id = value_of(op.body, BBL_JSON_KEY_FILAMENT_ID);
        saved->base_id.clear();
    } else if (is_filament) {
        saved->filament_id = parent->filament_id;
    }
    if (saved->setting_id.empty()) saved->sync_info = "create";
    saved->save(parent != nullptr ? &parent->config : nullptr);

    if (prev != op.key.name && !prev.empty()) collection.select_preset_by_name(prev, true);

    if (old_preset != nullptr) {
        // The pointer may be stale after the save; look the old preset up again.
        Preset* old_again = find_user_preset(collection, op.rename_from.name);
        if (old_again != nullptr) {
            const OpOutcome deleted = delete_checked(ctx, collection, *old_again, op.backup_local);
            if (deleted.result != OpResult::Applied) return deleted;
        }
    }

    ctx.bundle->update_compatible(PresetSelectCompatibleType::Never);
    return outcome(OpResult::Applied, "", current_hash(ctx, op.key));
}

OpOutcome delete_local(const LocalOpsContext& ctx, const DeleteOp& op)
{
    PresetCollection& collection = collection_for(*ctx.bundle, op.key.type);
    Preset*           preset     = find_user_preset(collection, op.key.name);
    if (preset == nullptr) return outcome(OpResult::Applied);

    if (current_hash(ctx, op.key) != op.expected_local_hash) return outcome(OpResult::Postponed, "changed since planning");
    if (dirty(collection)) return outcome(OpResult::Postponed, "unsaved edits");
    if (const std::string reason = in_use(ctx, collection, op.key.name); !reason.empty()) return outcome(OpResult::Postponed, reason);

    const std::string prev = collection.get_selected_preset_name();
    const OpOutcome   done = delete_checked(ctx, collection, *preset, op.backup_local);
    if (done.result != OpResult::Applied) return done;

    ctx.bundle->update_compatible(PresetSelectCompatibleType::Never);
    if (!prev.empty()) collection.select_preset_by_name(prev, true);
    return outcome(OpResult::Applied);
}

ConvertResult convert_filament(const LocalOpsContext& ctx, const std::string& preset_name)
{
    ConvertResult     result;
    PresetCollection& collection = ctx.bundle->filaments;

    Preset* preset = find_user_preset(collection, preset_name);
    if (preset == nullptr) {
        result.outcome = outcome(OpResult::Failed, "not a conversion candidate");
        return result;
    }
    const EligibilityContext ectx = make_context(*ctx.bundle, ctx.has_bambu_id);
    const LocalItem          item = make_item(*preset, collection, *ctx.bundle, ectx);
    if (item.cls.eligibility != Eligibility::Candidate) {
        result.outcome = outcome(OpResult::Failed, "not a conversion candidate");
        return result;
    }
    if (dirty(collection)) {
        result.outcome = outcome(OpResult::Postponed, "unsaved edits");
        return result;
    }
    if (!backup_files(ctx, *preset, "before-convert")) {
        result.outcome = outcome(OpResult::Failed, "backup failed");
        return result;
    }

    std::vector<ExistingRoot> roots;
    for (auto it = collection.begin(); it != collection.end(); ++it)
        if (it->is_user() && it->inherits().empty() && !it->filament_id.empty())
            roots.push_back({it->filament_id, filament_base_name(it->name)});
    int  n  = 0;
    auto to = choose_filament_id(preset_name, ctx.bambu_uid, roots, [&] { return std::to_string(ctx.now()) + std::to_string(n++); });
    if (to.empty()) {
        result.outcome = outcome(OpResult::Failed, "no free filament id");
        return result;
    }
    const std::string from = preset->filament_id;
    const std::string prev = collection.get_selected_preset_name();

    if (item.cls.shape == Shape::Child) {
        Preset copy = *preset;
        copy.name   = preset_name + " (bbl sync)";
        collection.save_current_preset(preset_name, /*detach*/ true, /*save_to_project*/ false, &copy);
        Preset* saved = collection.find_preset(preset_name, false, true);
        if (saved == nullptr) {
            result.outcome = outcome(OpResult::Failed, "could not save");
            return result;
        }
        saved->filament_id = to;
        saved->base_id.clear();
        saved->sync_info = saved->setting_id.empty() ? "create" : "update";
        saved->save(nullptr);
    } else {
        preset->filament_id = to;
        preset->sync_info   = preset->setting_id.empty() ? "create" : "update";
        preset->save(nullptr);
    }

    for (auto it = collection.begin(); it != collection.end(); ++it)
        if (it->inherits() == preset_name) it->filament_id = to;

    if (!prev.empty() && collection.get_selected_preset_name() != prev) collection.select_preset_by_name(prev, true);
    ctx.bundle->update_compatible(PresetSelectCompatibleType::Never);

    result.outcome          = outcome(OpResult::Applied);
    result.from_filament_id = from;
    result.to_filament_id   = to;
    return result;
}

} // namespace Slic3r::BblPresetSync
