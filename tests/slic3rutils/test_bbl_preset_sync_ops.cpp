#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/BblPresetSync/BodyCheck.hpp"
#include "slic3r/Utils/BblPresetSync/FilamentId.hpp"
#include "slic3r/Utils/BblPresetSync/LocalOps.hpp"
#include "slic3r/Utils/BblPresetSync/Snapshot.hpp"
#include "slic3r/Utils/BblPresetSync/Types.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace sync = Slic3r::BblPresetSync;
using namespace Slic3r;
namespace fs = boost::filesystem;
using sync::Key;
using sync::OpResult;
using sync::PresetType;

namespace {

Preset& add_inmemory(PresetCollection& coll, const std::string& name, const std::string& inherits = {})
{
    DynamicPrintConfig config(coll.default_preset().config);
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;
    return coll.load_preset(std::string(), name, config, /*select=*/false);
}

std::string read_file(const fs::path& p)
{
    boost::nowide::ifstream in(p.string(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool has_backup(const fs::path& dir, const std::string& suffix)
{
    if (!fs::exists(dir)) return false;
    for (fs::directory_iterator it(dir); it != fs::directory_iterator(); ++it) {
        const std::string n = it->path().filename().string();
        if (n.size() >= suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0) return true;
    }
    return false;
}

struct OpsFixture
{
    ScopedTemporaryDir       temp{"orca-bblops"};
    VendorProfile            bbl{"BBL"};
    VendorProfile            other{"Other"};
    PresetBundle             bundle;
    std::vector<std::string> queued;
    long long                clock{1700000000};

    OpsFixture()
    {
        const std::string dir = (temp.path() / "user").string();
        fs::create_directories(dir);
        bundle.prints.update_user_presets_directory(dir, "process");
        bundle.filaments.update_user_presets_directory(dir, "filament");
        bundle.printers.update_user_presets_directory(dir, "machine");

        auto sys = [](PresetCollection& c, const std::string& name, const VendorProfile* v, const std::string& setting_id,
                      const std::string& filament_id = {}) {
            Preset& p     = add_inmemory(c, name);
            p.is_system   = true;
            p.vendor      = v;
            p.setting_id  = setting_id;
            p.filament_id = filament_id;
        };
        sys(bundle.printers, "BBL X1C", &bbl, "pr1");
        sys(bundle.prints, "BBL Process", &bbl, "pc1");
        sys(bundle.filaments, "BBL PLA", &bbl, "f1", "SYS1");
        sys(bundle.filaments, "BBL PETG", &bbl, "f2", "SYS2");
        sys(bundle.filaments, "Lib PLA", &other, "f3", "LIB1");
    }

    sync::LocalOpsContext ctx()
    {
        sync::LocalOpsContext c;
        c.bundle                 = &bundle;
        c.has_bambu_id           = [](const std::string& id) { return id == "SYS1" || id == "SYS2"; };
        c.backup_dir             = temp.path() / "backup";
        c.bambu_uid              = "uid";
        c.now                    = [this] { return clock; };
        c.queue_orca_cloud_delete = [this](const Preset& p) { queued.push_back(p.name); };
        return c;
    }

    PresetCollection& filaments() { return bundle.filaments; }

    // A user preset with a file; a child saves its diff against the parent.
    Preset& add_user(PresetCollection& coll, const std::string& name, const std::string& inherits, double cost = 7.0)
    {
        Preset& p = add_inmemory(coll, name, inherits);
        p.file    = coll.path_from_name(name, inherits.empty());
        p.version = Semver(1, 0, 0);
        if (coll.type() == Preset::TYPE_FILAMENT) p.config.option<ConfigOptionFloats>("filament_cost", true)->values = {cost};
        Preset* parent = inherits.empty() ? nullptr : coll.find_preset(inherits, false);
        if (parent) p.base_id = parent->setting_id;
        REQUIRE(p.save(parent ? &parent->config : nullptr));
        return p;
    }

    std::string hash_of(PresetType type, const std::string& name)
    {
        PresetCollection& c = type == PresetType::Print ? bundle.prints : type == PresetType::Filament ? bundle.filaments : bundle.printers;
        const Preset* p     = c.find_preset(name, false, true);
        REQUIRE(p != nullptr);
        const auto ectx = sync::make_context(bundle, ctx().has_bambu_id);
        return sync::make_item(*p, c, bundle, ectx).hash;
    }

    sync::LocalItem item_of(const std::string& name)
    {
        const Preset* p = bundle.filaments.find_preset(name, false, true);
        REQUIRE(p != nullptr);
        const auto ectx = sync::make_context(bundle, ctx().has_bambu_id);
        return sync::make_item(*p, bundle.filaments, bundle, ectx);
    }

    // A cloud body for a child of a BBL filament, built like the upload does.
    static sync::ValueMap child_body(const std::string& parent, const std::string& parent_setting_id, double cost)
    {
        PresetCollection coll(Preset::TYPE_FILAMENT, Preset::filament_options(),
                              static_cast<const PrintRegionConfig&>(FullPrintConfig::defaults()));
        Preset& par     = add_inmemory(coll, parent);
        par.is_system   = true;
        par.setting_id  = parent_setting_id;
        Preset& child   = add_inmemory(coll, "Body Source", parent);
        child.version   = Semver(1, 0, 0);
        child.base_id   = parent_setting_id;
        child.config.option<ConfigOptionFloats>("filament_cost", true)->values = {cost};
        sync::ValueMap body;
        REQUIRE(coll.get_differed_values_to_update(child, body) == 0);
        body[BBL_JSON_KEY_SETTING_ID] = "PFUSbambu1";
        body[BBL_JSON_KEY_USER_ID]    = "bambu-user";
        body[BBL_JSON_KEY_FILAMENT_ID] = "PBODY99";
        return body;
    }

    static sync::ValueMap root_body(double cost, const std::string& filament_id)
    {
        PresetCollection coll(Preset::TYPE_FILAMENT, Preset::filament_options(),
                              static_cast<const PrintRegionConfig&>(FullPrintConfig::defaults()));
        Preset& root = add_inmemory(coll, "Body Source");
        root.version = Semver(1, 0, 0);
        root.config.option<ConfigOptionFloats>("filament_cost", true)->values = {cost};
        root.config.option<ConfigOptionStrings>("compatible_printers", true)->values = {"BBL X1C"};
        root.filament_id = filament_id;
        sync::ValueMap body;
        REQUIRE(coll.get_differed_values_to_update(root, body) == 0);
        body[BBL_JSON_KEY_SETTING_ID]  = "PFUSbambu2";
        body[BBL_JSON_KEY_USER_ID]     = "bambu-user";
        body[BBL_JSON_KEY_FILAMENT_ID] = filament_id;
        body.erase(BBL_JSON_KEY_BASE_ID);
        body.erase(BBL_JSON_KEY_INHERITS);
        return body;
    }

    static sync::PullOp pull(const std::string& name, sync::ValueMap body, const std::string& expected_hash = {})
    {
        sync::PullOp op;
        op.key                 = Key{PresetType::Filament, name};
        op.body                = std::move(body);
        op.expected_local_hash = expected_hash;
        return op;
    }

    static sync::DeleteOp del(const std::string& name, const std::string& expected_hash)
    {
        sync::DeleteOp op;
        op.key                 = Key{PresetType::Filament, name};
        op.expected_local_hash = expected_hash;
        return op;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// apply_pull
// ---------------------------------------------------------------------------

TEST_CASE("apply_pull creates a child preset as a diff without an Orca setting id", "[BblPresetSync]")
{
    OpsFixture f;
    const auto out = sync::apply_pull(f.ctx(), OpsFixture::pull("New Child", OpsFixture::child_body("BBL PLA", "f1", 42.5)));
    REQUIRE(out.result == OpResult::Applied);

    const Preset* saved = f.filaments().find_preset("New Child", false, true);
    REQUIRE(saved != nullptr);
    CHECK(saved->inherits() == "BBL PLA");
    CHECK(saved->setting_id.empty());
    CHECK(saved->sync_info == "create");
    CHECK(saved->filament_id == "SYS1"); // the parent's, not the body's
    CHECK(saved->config.option<ConfigOptionFloats>("filament_cost")->values.at(0) == 42.5);

    const fs::path file = f.filaments().path_from_name("New Child", false);
    REQUIRE(fs::exists(file));
    CHECK(saved->file == file);
    const std::string json = read_file(file);
    CHECK(json.find("\"inherits\": \"BBL PLA\"") != std::string::npos);
    CHECK(json.find("filament_cost") != std::string::npos);
    CHECK(json.find("filament_density") == std::string::npos); // a diff, not the full config
    fs::path info = file;
    info.replace_extension(".info");
    const std::string info_text = read_file(info);
    CHECK(info_text.find("sync_info = create") != std::string::npos);
    CHECK(info_text.find("setting_id = ") != std::string::npos);
    CHECK(info_text.find("setting_id = P") == std::string::npos);
    CHECK(info_text.find("setting_id = o") == std::string::npos);
}

TEST_CASE("apply_pull replaces a synced child keeping its Orca identity", "[BblPresetSync]")
{
    OpsFixture f;
    Preset&    mine = f.add_user(f.filaments(), "Mine", "BBL PLA", 7.0);
    mine.setting_id   = "orca-1";
    mine.user_id      = "orca-user";
    mine.updated_time = 100;
    mine.save(&f.filaments().find_preset("BBL PLA", false)->config);
    const std::string expected = f.hash_of(PresetType::Filament, "Mine");

    const auto out = sync::apply_pull(f.ctx(), OpsFixture::pull("Mine", OpsFixture::child_body("BBL PLA", "f1", 55.0), expected));
    REQUIRE(out.result == OpResult::Applied);

    const Preset* saved = f.filaments().find_preset("Mine", false, true);
    REQUIRE(saved != nullptr);
    CHECK(saved->setting_id == "orca-1");
    CHECK(saved->user_id == "orca-user");
    CHECK(saved->updated_time == 100);
    CHECK(saved->sync_info == "update");
    CHECK(saved->config.option<ConfigOptionFloats>("filament_cost")->values.at(0) == 55.0);
    CHECK(read_file(saved->file).find("55") != std::string::npos);
    fs::path info = saved->file;
    info.replace_extension(".info");
    const std::string info_text = read_file(info);
    CHECK(info_text.find("setting_id = orca-1") != std::string::npos);
    CHECK(info_text.find("sync_info = update") != std::string::npos);
}

TEST_CASE("apply_pull of a root filament body creates a root with the body's id", "[BblPresetSync]")
{
    OpsFixture f;
    const auto out = sync::apply_pull(f.ctx(), OpsFixture::pull("Custom Root", OpsFixture::root_body(9.0, "PROOT01")));
    REQUIRE(out.result == OpResult::Applied);

    const Preset* saved = f.filaments().find_preset("Custom Root", false, true);
    REQUIRE(saved != nullptr);
    CHECK(saved->inherits().empty());
    CHECK(saved->filament_id == "PROOT01");
    CHECK(saved->base_id.empty());
    CHECK(saved->file == f.filaments().path_from_name("Custom Root", true));
    CHECK(fs::exists(saved->file));
    CHECK(read_file(saved->file).find("PROOT01") != std::string::npos);
    CHECK(f.item_of("Custom Root").cls.eligibility == sync::Eligibility::Eligible);
}

TEST_CASE("apply_pull of a root body over an existing child moves the file to base and leaves no old files", "[BblPresetSync]")
{
    OpsFixture f;
    f.add_user(f.filaments(), "Mine", "BBL PLA");
    const fs::path old_file = f.filaments().path_from_name("Mine", false);
    fs::path       old_info = old_file;
    old_info.replace_extension(".info");
    REQUIRE(fs::exists(old_file));
    const std::string expected = f.hash_of(PresetType::Filament, "Mine");

    const auto out = sync::apply_pull(f.ctx(), OpsFixture::pull("Mine", OpsFixture::root_body(9.0, "PROOT02"), expected));
    REQUIRE(out.result == OpResult::Applied);

    const Preset* saved = f.filaments().find_preset("Mine", false, true);
    REQUIRE(saved != nullptr);
    CHECK(saved->file == f.filaments().path_from_name("Mine", true));
    CHECK(fs::exists(saved->file));
    CHECK_FALSE(fs::exists(old_file));
    CHECK_FALSE(fs::exists(old_info));
    CHECK(saved->inherits().empty());
}

TEST_CASE("apply_pull gives a child filament its parent's filament id", "[BblPresetSync]")
{
    OpsFixture f;
    REQUIRE(sync::apply_pull(f.ctx(), OpsFixture::pull("Kid", OpsFixture::child_body("BBL PETG", "f2", 3.0))).result == OpResult::Applied);
    const Preset* saved = f.filaments().find_preset("Kid", false, true);
    REQUIRE(saved != nullptr);
    CHECK(saved->filament_id == "SYS2");
    CHECK(saved->base_id == "f2");
}

TEST_CASE("apply_pull cannot resolve a body whose parent is missing and changes nothing", "[BblPresetSync]")
{
    OpsFixture f;
    f.add_user(f.filaments(), "Mine", "BBL PLA");
    const std::string expected = f.hash_of(PresetType::Filament, "Mine");
    auto              body     = OpsFixture::child_body("BBL PLA", "f1", 55.0);
    body[BBL_JSON_KEY_INHERITS] = "No Such Parent";

    const auto out = sync::apply_pull(f.ctx(), OpsFixture::pull("Mine", body, expected));
    CHECK(out.result == OpResult::Unresolvable);
    CHECK(f.hash_of(PresetType::Filament, "Mine") == expected);
    CHECK(f.filaments().find_preset("Mine", false, true)->config.option<ConfigOptionFloats>("filament_cost")->values.at(0) == 7.0);
}

TEST_CASE("apply_pull postpones on unsaved edits and on a changed local preset", "[BblPresetSync]")
{
    OpsFixture f;
    f.add_user(f.filaments(), "Mine", "BBL PLA");
    const std::string expected = f.hash_of(PresetType::Filament, "Mine");
    const auto        body     = OpsFixture::child_body("BBL PLA", "f1", 55.0);

    SECTION("dirty")
    {
        f.filaments().get_edited_preset().is_dirty = true;
        const auto out                             = sync::apply_pull(f.ctx(), OpsFixture::pull("Mine", body, expected));
        CHECK(out.result == OpResult::Postponed);
        CHECK(out.reason == "unsaved edits");
    }
    SECTION("hash mismatch")
    {
        const auto out = sync::apply_pull(f.ctx(), OpsFixture::pull("Mine", body, "not-the-hash"));
        CHECK(out.result == OpResult::Postponed);
        CHECK(out.reason == "changed since planning");
    }
    CHECK(f.filaments().find_preset("Mine", false, true)->config.option<ConfigOptionFloats>("filament_cost")->values.at(0) == 7.0);
}

TEST_CASE("apply_pull backs up the local files when asked", "[BblPresetSync]")
{
    OpsFixture f;
    f.add_user(f.filaments(), "Mine", "BBL PLA");
    auto op         = OpsFixture::pull("Mine", OpsFixture::child_body("BBL PLA", "f1", 55.0), f.hash_of(PresetType::Filament, "Mine"));
    op.backup_local = true;
    REQUIRE(sync::apply_pull(f.ctx(), op).result == OpResult::Applied);
    CHECK(has_backup(f.temp.path() / "backup", "_filament_Mine_local.json"));
    CHECK(has_backup(f.temp.path() / "backup", "_filament_Mine_local.info"));
}

TEST_CASE("apply_pull restores the previous selection and keeps a selected target", "[BblPresetSync]")
{
    OpsFixture f;
    f.add_user(f.filaments(), "Mine", "BBL PLA");
    f.add_user(f.filaments(), "Other", "BBL PLA");

    SECTION("another preset selected")
    {
        f.filaments().select_preset_by_name("Other", true);
        REQUIRE(sync::apply_pull(f.ctx(), OpsFixture::pull("Mine", OpsFixture::child_body("BBL PLA", "f1", 55.0),
                                                           f.hash_of(PresetType::Filament, "Mine"))).result == OpResult::Applied);
        CHECK(f.filaments().get_selected_preset_name() == "Other");
    }
    SECTION("the target selected")
    {
        f.filaments().select_preset_by_name("Mine", true);
        REQUIRE(sync::apply_pull(f.ctx(), OpsFixture::pull("Mine", OpsFixture::child_body("BBL PLA", "f1", 55.0),
                                                           f.hash_of(PresetType::Filament, "Mine"))).result == OpResult::Applied);
        CHECK(f.filaments().get_selected_preset_name() == "Mine");
        CHECK_FALSE(f.filaments().get_edited_preset().is_dirty);
        CHECK(f.filaments().get_edited_preset().config.option<ConfigOptionFloats>("filament_cost")->values.at(0) == 55.0);
    }
}

TEST_CASE("apply_pull rename deletes the old preset and queues its Orca delete", "[BblPresetSync]")
{
    OpsFixture f;
    Preset&    old = f.add_user(f.filaments(), "Old Name", "BBL PLA");
    const fs::path old_file = old.file;

    auto op            = OpsFixture::pull("New Name", OpsFixture::child_body("BBL PLA", "f1", 55.0));
    op.rename_from_set = true;
    op.rename_from     = Key{PresetType::Filament, "Old Name"};

    SECTION("applied")
    {
        REQUIRE(sync::apply_pull(f.ctx(), op).result == OpResult::Applied);
        CHECK(f.filaments().find_preset("New Name", false, true) != nullptr);
        CHECK(f.filaments().find_preset("Old Name", false, true) == nullptr);
        CHECK_FALSE(fs::exists(old_file));
        CHECK(f.queued == std::vector<std::string>{"Old Name"});
    }
    SECTION("old preset selected")
    {
        f.filaments().select_preset_by_name("Old Name", true);
        const auto out = sync::apply_pull(f.ctx(), op);
        CHECK(out.result == OpResult::Postponed);
        CHECK(out.reason == "old preset selected");
        CHECK(f.filaments().find_preset("New Name", false, true) == nullptr);
        CHECK(fs::exists(old_file));
        CHECK(f.queued.empty());
    }
}

TEST_CASE("apply_pull reports the hash the next snapshot shows", "[BblPresetSync]")
{
    OpsFixture f;
    const auto out = sync::apply_pull(f.ctx(), OpsFixture::pull("New Child", OpsFixture::child_body("BBL PLA", "f1", 42.5)));
    REQUIRE(out.result == OpResult::Applied);
    CHECK_FALSE(out.new_hash.empty());

    const auto snap = sync::build_snapshot(f.bundle, f.ctx().has_bambu_id);
    const sync::LocalItem* found = nullptr;
    for (const auto& i : snap.items)
        if (i.key == Key{PresetType::Filament, "New Child"}) found = &i;
    REQUIRE(found != nullptr);
    CHECK(found->hash == out.new_hash);
}

// ---------------------------------------------------------------------------
// delete_local
// ---------------------------------------------------------------------------

TEST_CASE("delete_local removes the preset, queues the Orca delete once and keeps the selection", "[BblPresetSync]")
{
    OpsFixture f;
    Preset&    mine = f.add_user(f.filaments(), "Mine", "BBL PLA");
    f.add_user(f.filaments(), "Keep", "BBL PLA");
    const fs::path file = mine.file;
    f.filaments().select_preset_by_name("Keep", true);

    auto op         = OpsFixture::del("Mine", f.hash_of(PresetType::Filament, "Mine"));
    op.backup_local = true;
    REQUIRE(sync::delete_local(f.ctx(), op).result == OpResult::Applied);

    CHECK(f.filaments().find_preset("Mine", false, true) == nullptr);
    CHECK_FALSE(fs::exists(file));
    CHECK(f.queued == std::vector<std::string>{"Mine"});
    CHECK(f.filaments().get_selected_preset_name() == "Keep");
    CHECK(has_backup(f.temp.path() / "backup", "_filament_Mine_deleted.json"));
}

TEST_CASE("delete_local of a missing preset is applied", "[BblPresetSync]")
{
    OpsFixture f;
    CHECK(sync::delete_local(f.ctx(), OpsFixture::del("Nope", "")).result == OpResult::Applied);
    CHECK(f.queued.empty());
}

TEST_CASE("delete_local postpones for each reason", "[BblPresetSync]")
{
    OpsFixture f;
    f.add_user(f.filaments(), "Mine", "BBL PLA");
    f.add_user(f.filaments(), "Other", "BBL PLA");
    const std::string hash = f.hash_of(PresetType::Filament, "Mine");
    f.filaments().select_preset_by_name("Other", true);

    SECTION("changed")
    {
        const auto out = sync::delete_local(f.ctx(), OpsFixture::del("Mine", "stale"));
        CHECK(out.result == OpResult::Postponed);
        CHECK(out.reason == "changed since planning");
    }
    SECTION("dirty")
    {
        f.filaments().get_edited_preset().is_dirty = true;
        const auto out                             = sync::delete_local(f.ctx(), OpsFixture::del("Mine", hash));
        CHECK(out.result == OpResult::Postponed);
        CHECK(out.reason == "unsaved edits");
    }
    SECTION("selected")
    {
        f.filaments().select_preset_by_name("Mine", true);
        const auto out = sync::delete_local(f.ctx(), OpsFixture::del("Mine", hash));
        CHECK(out.result == OpResult::Postponed);
        CHECK(out.reason == "selected");
    }
    SECTION("used by the project")
    {
        f.bundle.filament_presets = {"Mine"};
        const auto out            = sync::delete_local(f.ctx(), OpsFixture::del("Mine", hash));
        CHECK(out.result == OpResult::Postponed);
        CHECK(out.reason == "used by the project");
    }
    SECTION("has presets based on it")
    {
        f.add_user(f.filaments(), "Grand", "Mine");
        const auto out = sync::delete_local(f.ctx(), OpsFixture::del("Mine", f.hash_of(PresetType::Filament, "Mine")));
        CHECK(out.result == OpResult::Postponed);
        CHECK(out.reason == "has presets based on it");
    }
    CHECK(f.filaments().find_preset("Mine", false, true) != nullptr);
    CHECK(f.queued.empty());
}

// ---------------------------------------------------------------------------
// convert_filament
// ---------------------------------------------------------------------------

namespace {

Preset& add_candidate_child(OpsFixture& f, const std::string& name)
{
    Preset& p = f.add_user(f.filaments(), name, "Lib PLA");
    p.config.option<ConfigOptionStrings>("compatible_printers", true)->values = {"BBL X1C"};
    p.filament_id                                                             = "LIB1"; // what loading a child gives it
    p.setting_id                                                              = "orca-9";
    p.user_id                                                                 = "orca-user";
    p.updated_time                                                            = 5;
    REQUIRE(p.save(&f.filaments().find_preset("Lib PLA", false)->config));
    return p;
}

Preset& add_candidate_root(OpsFixture& f, const std::string& name)
{
    Preset& p = f.add_user(f.filaments(), name, "");
    p.config.option<ConfigOptionStrings>("compatible_printers", true)->values = {"BBL X1C"};
    REQUIRE(p.save(nullptr));
    return p;
}

} // namespace

TEST_CASE("convert_filament turns a child candidate into a root with a new filament id", "[BblPresetSync]")
{
    OpsFixture f;
    add_candidate_child(f, "Cand @BBL X1C");
    f.add_user(f.filaments(), "Grand", "Cand @BBL X1C");
    const fs::path old_file = f.filaments().path_from_name("Cand @BBL X1C", false);
    fs::path       old_info = old_file;
    old_info.replace_extension(".info");
    REQUIRE(f.item_of("Cand @BBL X1C").cls.eligibility == sync::Eligibility::Candidate);

    const auto r = sync::convert_filament(f.ctx(), "Cand @BBL X1C");
    REQUIRE(r.outcome.result == OpResult::Applied);
    CHECK(r.from_filament_id == "LIB1");
    CHECK(r.to_filament_id == sync::make_filament_id("Cand", "uid"));
    CHECK(r.to_filament_id.at(0) == 'P');

    const Preset* saved = f.filaments().find_preset("Cand @BBL X1C", false, true);
    REQUIRE(saved != nullptr);
    CHECK(saved->inherits().empty());
    CHECK(saved->filament_id == r.to_filament_id);
    CHECK(saved->file == f.filaments().path_from_name("Cand @BBL X1C", true));
    CHECK(fs::exists(saved->file));
    CHECK_FALSE(fs::exists(old_file));
    CHECK_FALSE(fs::exists(old_info));
    CHECK(saved->setting_id == "orca-9");
    CHECK(saved->user_id == "orca-user");
    CHECK(saved->sync_info == "update");
    CHECK(saved->config.option<ConfigOptionStrings>("compatible_printers")->values == std::vector<std::string>{"BBL X1C"});
    CHECK(read_file(saved->file).find(r.to_filament_id) != std::string::npos);

    const auto item = f.item_of("Cand @BBL X1C");
    CHECK(item.cls.eligibility == sync::Eligibility::Eligible);
    CHECK(item.cls.shape == sync::Shape::Root);

    CHECK(f.filaments().find_preset("Grand", false, true)->filament_id == r.to_filament_id);
}

TEST_CASE("convert_filament gives a candidate root without an id one in place", "[BblPresetSync]")
{
    OpsFixture f;
    add_candidate_root(f, "Plain @BBL X1C");
    const fs::path file = f.filaments().path_from_name("Plain @BBL X1C", true);
    f.filaments().select_preset_by_name("BBL PLA", true);

    const auto r = sync::convert_filament(f.ctx(), "Plain @BBL X1C");
    REQUIRE(r.outcome.result == OpResult::Applied);
    CHECK(r.from_filament_id.empty());

    const Preset* saved = f.filaments().find_preset("Plain @BBL X1C", false, true);
    REQUIRE(saved != nullptr);
    CHECK(saved->filament_id == r.to_filament_id);
    CHECK(saved->file == file);
    CHECK(saved->sync_info == "create"); // never uploaded to Orca
    CHECK(f.item_of("Plain @BBL X1C").cls.eligibility == sync::Eligibility::Eligible);
    CHECK(f.filaments().get_selected_preset_name() == "BBL PLA");
}

TEST_CASE("convert_filament refuses what is not a candidate and what has unsaved edits", "[BblPresetSync]")
{
    OpsFixture f;
    f.add_user(f.filaments(), "Mine", "BBL PLA"); // eligible, not a candidate
    add_candidate_root(f, "Plain @BBL X1C");

    CHECK(sync::convert_filament(f.ctx(), "Mine").outcome.result == OpResult::Failed);
    CHECK(sync::convert_filament(f.ctx(), "Mine").outcome.reason == "not a conversion candidate");
    CHECK(sync::convert_filament(f.ctx(), "Missing").outcome.result == OpResult::Failed);

    f.filaments().get_edited_preset().is_dirty = true;
    const auto r                               = sync::convert_filament(f.ctx(), "Plain @BBL X1C");
    CHECK(r.outcome.result == OpResult::Postponed);
    CHECK(r.outcome.reason == "unsaved edits");
    CHECK(f.filaments().find_preset("Plain @BBL X1C", false, true)->filament_id.empty());
}

TEST_CASE("convert_filament writes a backup first", "[BblPresetSync]")
{
    OpsFixture f;
    add_candidate_child(f, "Cand @BBL X1C");
    REQUIRE(sync::convert_filament(f.ctx(), "Cand @BBL X1C").outcome.result == OpResult::Applied);
    CHECK(has_backup(f.temp.path() / "backup", "_before-convert.json"));
    CHECK(has_backup(f.temp.path() / "backup", "_before-convert.info"));
}

TEST_CASE("convert_filament reuses the id for a second preset with the same base name", "[BblPresetSync]")
{
    OpsFixture f;
    add_candidate_root(f, "Same @BBL X1C");
    add_candidate_root(f, "Same @Other");
    const auto a = sync::convert_filament(f.ctx(), "Same @BBL X1C");
    const auto b = sync::convert_filament(f.ctx(), "Same @Other");
    REQUIRE(a.outcome.result == OpResult::Applied);
    REQUIRE(b.outcome.result == OpResult::Applied);
    CHECK(a.to_filament_id == b.to_filament_id);
}

TEST_CASE("apply_pull keeps the intended parent when the new preset sorts first or last among many", "[BblPresetSync]")
{
    OpsFixture f;
    for (const char* name : {"BBL ABS", "BBL ASA", "BBL PC", "BBL TPU", "BBL PVA", "BBL PA"}) {
        Preset& p     = add_inmemory(f.filaments(), name);
        p.is_system   = true;
        p.vendor      = &f.bbl;
        p.setting_id  = std::string("sys-") + name;
        p.filament_id = "SYS1";
    }
    for (const char* name : {"Mine B", "Mine C", "Mine D", "Mine E", "Mine F", "Mine G"}) f.add_user(f.filaments(), name, "BBL PLA");

    for (const char* target : {"AAA first", "zzz last"}) {
        DYNAMIC_SECTION(target)
        {
            const auto out = sync::apply_pull(f.ctx(), OpsFixture::pull(target, OpsFixture::child_body("BBL PLA", "f1", 61.5)));
            REQUIRE(out.result == OpResult::Applied);

            const Preset* saved = f.filaments().find_preset(target, false, true);
            REQUIRE(saved != nullptr);
            CHECK(saved->inherits() == "BBL PLA");
            CHECK(saved->filament_id == "SYS1");
            CHECK(saved->base_id == "f1");

            // The file holds the diff against the intended parent, which is what the upload also sends.
            Preset copy = *saved;
            sync::ValueMap diff;
            REQUIRE(f.bundle.get_differed_values_to_update(copy, diff) == 0);
            const auto json = nlohmann::json::parse(read_file(saved->file));
            CHECK(json.at("inherits") == "BBL PLA");
            CHECK(json.at("filament_cost").at(0) == "61.5");
            CHECK(json.contains("filament_density") == false);
            // Everything the file holds is part of that diff (the diff may add nil entries for per-extruder overrides).
            for (auto it = json.begin(); it != json.end(); ++it) {
                if (sync::metadata_keys().count(it.key()) || it.key() == "version" || it.key() == "name" || it.key() == "from" ||
                    it.key() == "filament_settings_id")
                    continue;
                INFO(it.key());
                CHECK(diff.count(it.key()) == 1);
            }
        }
    }
}
