#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/BblPresetSync/Cloud.hpp"
#include "slic3r/Utils/BblPresetSync/Eligibility.hpp"
#include "slic3r/Utils/BblPresetSync/BaseStore.hpp"
#include "slic3r/Utils/BblPresetSync/Hash.hpp"
#include "slic3r/Utils/BblPresetSync/Keys.hpp"
#include "slic3r/Utils/BblPresetSync/Merge.hpp"
#include "slic3r/Utils/BblPresetSync/Ledger.hpp"
#include "slic3r/Utils/BblPresetSync/Mirror.hpp"
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
#include <deque>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sync = Slic3r::BblPresetSync;
using namespace Slic3r;
namespace fs = boost::filesystem;

namespace {

// Add an in-memory preset (no file) with the given inherits value (empty => root preset).
Preset& add_inmemory_preset(PresetCollection& coll, const std::string& name, const std::string& inherits = {})
{
    DynamicPrintConfig config(coll.default_preset().config);
    config.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS, true)->value = inherits;
    return coll.load_preset(std::string(), name, config, /*select=*/false);
}

std::unique_ptr<PresetCollection> make_collection(Preset::Type type)
{
    const auto& defaults = static_cast<const PrintRegionConfig&>(FullPrintConfig::defaults());
    switch (type) {
    case Preset::TYPE_PRINT: return std::make_unique<PresetCollection>(type, Preset::print_options(), defaults);
    case Preset::TYPE_FILAMENT: return std::make_unique<PresetCollection>(type, Preset::filament_options(), defaults);
    default: return std::make_unique<PresetCollection>(type, Preset::printer_options(), defaults, "Default Printer");
    }
}

void set_compatible_printers(Preset& p, const std::vector<std::string>& names)
{
    p.config.option<ConfigOptionStrings>("compatible_printers", true)->values = names;
}

// The ids of Orca filaments that have a Bambu id.
sync::HasBambuId mapped_ids(std::set<std::string> ids)
{
    return [ids](const std::string& id) { return ids.count(id) > 0; };
}

// Three collections holding BBL and non-BBL system presets.
struct Library
{
    VendorProfile                     bbl{"BBL"};
    VendorProfile                     other{"Other"};
    std::unique_ptr<PresetCollection> prints{make_collection(Preset::TYPE_PRINT)};
    std::unique_ptr<PresetCollection> filaments{make_collection(Preset::TYPE_FILAMENT)};
    std::unique_ptr<PresetCollection> printers{make_collection(Preset::TYPE_PRINTER)};

    static void fill(PresetCollection& c, const std::string& name, const VendorProfile* vendor, const std::string& setting_id,
                     const std::string& filament_id = {})
    {
        Preset& p      = add_inmemory_preset(c, name);
        p.is_system    = true;
        p.vendor       = vendor;
        p.setting_id   = setting_id;
        p.filament_id  = filament_id;
    }

    Library()
    {
        fill(*printers, "BBL X1C", &bbl, "pr1");
        fill(*printers, "Other Printer", &other, "pr2");
        fill(*prints, "BBL Process", &bbl, "pc1");
        fill(*prints, "Lib Process", &other, "pc2");
        fill(*filaments, "BBL PLA", &bbl, "f1", "SYS1");   // its id maps
        fill(*filaments, "BBL Odd", &bbl, "f2", "SYS2");   // BBL vendor, id does not map
        fill(*filaments, "Lib PLA", &other, "f3", "LIB1"); // library filament
    }

    sync::EligibilityContext context(const sync::HasBambuId& has_id) const
    {
        sync::EligibilityContext ctx;
        ctx.prints        = prints.get();
        ctx.filaments     = filaments.get();
        ctx.printers      = printers.get();
        ctx.has_bambu_id  = has_id;
        ctx.id_map_usable = sync::compute_id_map_usable(*filaments, has_id);
        return ctx;
    }

    static Preset& user(PresetCollection& c, const std::string& name, const std::string& inherits)
    {
        return add_inmemory_preset(c, name, inherits);
    }
};

sync::Classified classify_named(PresetCollection& c, const std::string& name, const sync::EligibilityContext& ctx)
{
    const Preset* p = c.find_preset(name, false);
    REQUIRE(p != nullptr);
    return sync::classify(*p, c, ctx);
}

struct PresetState
{
    std::string name, setting_id, sync_info, file;
    long long   updated_time;
    bool operator==(const PresetState& o) const
    {
        return name == o.name && setting_id == o.setting_id && sync_info == o.sync_info && file == o.file && updated_time == o.updated_time;
    }
};

std::vector<PresetState> states(PresetCollection& c)
{
    std::vector<PresetState> out;
    for (auto it = c.begin(); it != c.end(); ++it)
        out.push_back({it->name, it->setting_id, it->sync_info, it->file, it->updated_time});
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// Loader seam
// ---------------------------------------------------------------------------

namespace {

struct SeamFixture
{
    ScopedTemporaryDir                temp;
    std::unique_ptr<PresetCollection> filaments{make_collection(Preset::TYPE_FILAMENT)};
    std::map<std::string, std::string> body; // what the cloud would send for "Child"

    static void add_parent(PresetCollection& c)
    {
        Preset& parent = add_inmemory_preset(c, "Base PLA");
        parent.is_system  = true;
        parent.setting_id = "base-id";
    }

    SeamFixture()
    {
        add_parent(*filaments);
        auto other = make_collection(Preset::TYPE_FILAMENT);
        add_parent(*other);
        Preset& child = add_inmemory_preset(*other, "Child", "Base PLA");
        child.version = Semver(1, 0, 0);
        child.config.option<ConfigOptionFloats>("filament_cost", true)->values = {42.5};
        REQUIRE(other->get_differed_values_to_update(child, body) == 0);
        body[BBL_JSON_KEY_SETTING_ID]     = "cloud-1";
        body[BBL_JSON_KEY_USER_ID]        = "u1";
        body[ORCA_JSON_KEY_UPDATE_TIME]   = "200";
    }
};

} // namespace

TEST_CASE("Materializing a child body returns the parent config plus the diff and leaves the collection alone", "[BblPresetSync]")
{
    SeamFixture f;
    PresetsConfigSubstitutions subs;
    const auto                 before = states(*f.filaments);

    DynamicPrintConfig out;
    REQUIRE(f.filaments->load_user_preset("Child", f.body, subs, ForwardCompatibilitySubstitutionRule::Disable,
                                          PresetOrigin(PresetOrigin::Kind::User), &out));

    const Preset* parent = f.filaments->find_preset("Base PLA", false);
    REQUIRE(parent != nullptr);
    CHECK(out.option<ConfigOptionFloats>("filament_cost")->values.at(0) == 42.5);
    CHECK(out.opt_serialize("filament_density") == parent->config.opt_serialize("filament_density"));
    CHECK(out.option<ConfigOptionString>(BBL_JSON_KEY_INHERITS)->value == "Base PLA");

    CHECK(states(*f.filaments) == before);
    CHECK(f.filaments->find_preset("Child", false, true) == nullptr);
}

TEST_CASE("Materializing leaves an existing preset of that name alone even when it is newer", "[BblPresetSync]")
{
    SeamFixture f;
    PresetsConfigSubstitutions subs;

    const fs::path file = f.temp.path() / "Child.json";
    Preset&        mine = add_inmemory_preset(*f.filaments, "Child", "Base PLA");
    mine.file           = file.string();
    mine.setting_id     = "mine";
    mine.sync_info      = "update";
    mine.updated_time   = 300; // newer than the body's 200
    REQUIRE(mine.save(nullptr));
    const auto before = states(*f.filaments);
    auto read_file = [](const fs::path& p) {
        std::ifstream in(p.string(), std::ios::binary);
        return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    };
    const std::string json_before = read_file(file), info_before = read_file(f.temp.path() / "Child.info");

    DynamicPrintConfig out;
    CHECK(f.filaments->load_user_preset("Child", f.body, subs, ForwardCompatibilitySubstitutionRule::Disable,
                                        PresetOrigin(PresetOrigin::Kind::User), &out));
    CHECK(out.option<ConfigOptionFloats>("filament_cost")->values.at(0) == 42.5);
    CHECK(states(*f.filaments) == before);
    CHECK(read_file(file) == json_before);
    CHECK(read_file(f.temp.path() / "Child.info") == info_before);

    // Without the parameter the newer local preset wins and the call reports no load.
    CHECK_FALSE(f.filaments->load_user_preset("Child", f.body, subs, ForwardCompatibilitySubstitutionRule::Disable));
}

TEST_CASE("Materializing an unresolvable inherits returns false", "[BblPresetSync]")
{
    SeamFixture f;
    PresetsConfigSubstitutions subs;
    f.body[BBL_JSON_KEY_INHERITS] = "No Such Parent";

    DynamicPrintConfig out;
    CHECK_FALSE(f.filaments->load_user_preset("Child", f.body, subs, ForwardCompatibilitySubstitutionRule::Disable,
                                              PresetOrigin(PresetOrigin::Kind::User), &out));
    CHECK(f.filaments->find_preset("Child", false, true) == nullptr);
}

TEST_CASE("Loading a cloud preset without the seam parameter updates an older local preset", "[BblPresetSync]")
{
    SeamFixture f;
    PresetsConfigSubstitutions subs;

    const fs::path dir = f.temp.path() / "base";
    fs::create_directories(dir);
    Preset& mine      = add_inmemory_preset(*f.filaments, "Child", "Base PLA");
    mine.file         = (dir / "Child.json").string();
    mine.setting_id   = "cloud-1";
    mine.user_id      = "u1";
    mine.updated_time = 100;
    REQUIRE(mine.save(nullptr));

    f.filaments->load_user_preset("Child", f.body, subs, ForwardCompatibilitySubstitutionRule::Disable);

    const Preset* updated = f.filaments->find_preset("Child", false, true);
    REQUIRE(updated != nullptr);
    CHECK(updated->updated_time == 200);
    CHECK(updated->config.option<ConfigOptionFloats>("filament_cost")->values.at(0) == 42.5);
}

// ---------------------------------------------------------------------------
// Eligibility
// ---------------------------------------------------------------------------

TEST_CASE("compute_id_map_usable needs a BBL system filament that has a Bambu id", "[BblPresetSync]")
{
    Library lib;
    CHECK_FALSE(sync::compute_id_map_usable(*lib.filaments, mapped_ids({})));
    // A mapped id on a non-BBL filament does not count.
    CHECK_FALSE(sync::compute_id_map_usable(*lib.filaments, mapped_ids({"LIB1"})));
    // An unmapped BBL id does not count either.
    CHECK_FALSE(sync::compute_id_map_usable(*lib.filaments, mapped_ids({"OTHER"})));
    CHECK(sync::compute_id_map_usable(*lib.filaments, mapped_ids({"SYS1"})));
}

TEST_CASE("classify rejects presets that are not plain user presets", "[BblPresetSync]")
{
    Library lib;
    const auto ctx = lib.context(mapped_ids({"SYS1"}));

    SECTION("a system preset")
    {
        auto c = classify_named(*lib.filaments, "BBL PLA", ctx);
        CHECK(c.eligibility == sync::Eligibility::Ineligible);
        CHECK(c.reason == "not_a_user_preset");
    }
    SECTION("a preset embedded in the project")
    {
        Library::user(*lib.filaments, "Embedded", "BBL PLA");
        lib.filaments->find_preset("Embedded", false, true)->is_project_embedded = true;
        auto c = classify_named(*lib.filaments, "Embedded", ctx);
        CHECK(c.eligibility == sync::Eligibility::Ineligible);
        CHECK(c.reason == "not_a_user_preset");
    }
    SECTION("a preset that came from a bundle")
    {
        Library::user(*lib.filaments, "Bundled", "BBL PLA");
        lib.filaments->find_preset("Bundled", false, true)->bundle_id = "bundle-1";
        auto c = classify_named(*lib.filaments, "Bundled", ctx);
        CHECK(c.eligibility == sync::Eligibility::Ineligible);
        CHECK(c.reason == "not_a_user_preset");
    }
}

TEST_CASE("classify handles a child filament by what its parent is", "[BblPresetSync]")
{
    Library lib;

    SECTION("child of a BBL system filament whose id maps is eligible")
    {
        Library::user(*lib.filaments, "Mine", "BBL PLA");
        auto c = classify_named(*lib.filaments, "Mine", lib.context(mapped_ids({"SYS1"})));
        CHECK(c.eligibility == sync::Eligibility::Eligible);
        CHECK(c.shape == sync::Shape::Child);
    }
    SECTION("the id does not map and it targets a BBL printer: a candidate")
    {
        Preset& p = Library::user(*lib.filaments, "Mine", "BBL Odd");
        set_compatible_printers(p, {"BBL X1C"});
        auto c = classify_named(*lib.filaments, "Mine", lib.context(mapped_ids({"SYS1"})));
        CHECK(c.eligibility == sync::Eligibility::Candidate);
        CHECK(c.shape == sync::Shape::Child);
        CHECK(c.reason == "parent_unknown_to_bambu");
    }
    SECTION("the id map is unusable")
    {
        Preset& p = Library::user(*lib.filaments, "Mine", "BBL PLA");
        set_compatible_printers(p, {"BBL X1C"});
        auto c = classify_named(*lib.filaments, "Mine", lib.context(mapped_ids({})));
        CHECK(c.eligibility == sync::Eligibility::Ineligible);
        CHECK(c.reason == "id_map_missing");
    }
    SECTION("child of a library filament targeting a BBL printer is a candidate")
    {
        Preset& p = Library::user(*lib.filaments, "Mine", "Lib PLA");
        set_compatible_printers(p, {"BBL X1C"});
        auto c = classify_named(*lib.filaments, "Mine", lib.context(mapped_ids({"SYS1"})));
        CHECK(c.eligibility == sync::Eligibility::Candidate);
        CHECK(c.reason == "parent_unknown_to_bambu");
    }
    SECTION("child of a library filament targeting a non-BBL printer")
    {
        Preset& p = Library::user(*lib.filaments, "Mine", "Lib PLA");
        set_compatible_printers(p, {"Other Printer"});
        auto c = classify_named(*lib.filaments, "Mine", lib.context(mapped_ids({"SYS1"})));
        CHECK(c.eligibility == sync::Eligibility::Ineligible);
        CHECK(c.reason == "not_bambu_based");
    }
    SECTION("child of a library filament with no compatible printers")
    {
        Library::user(*lib.filaments, "Mine", "Lib PLA");
        auto c = classify_named(*lib.filaments, "Mine", lib.context(mapped_ids({"SYS1"})));
        CHECK(c.eligibility == sync::Eligibility::Ineligible);
        CHECK(c.reason == "not_bambu_based");
    }
    SECTION("compatible with a BBL printer and a non-BBL one")
    {
        Preset& p = Library::user(*lib.filaments, "Mine", "Lib PLA");
        set_compatible_printers(p, {"BBL X1C", "Other Printer"});
        auto c = classify_named(*lib.filaments, "Mine", lib.context(mapped_ids({"SYS1"})));
        CHECK(c.reason == "not_bambu_based");
    }
    SECTION("child of a user preset")
    {
        Library::user(*lib.filaments, "Middle", "BBL PLA");
        Library::user(*lib.filaments, "Mine", "Middle");
        auto c = classify_named(*lib.filaments, "Mine", lib.context(mapped_ids({"SYS1"})));
        CHECK(c.eligibility == sync::Eligibility::Ineligible);
        CHECK(c.reason == "unsupported_chain");
    }
    SECTION("child whose parent is gone")
    {
        Library::user(*lib.filaments, "Mine", "Vanished Parent");
        auto c = classify_named(*lib.filaments, "Mine", lib.context(mapped_ids({"SYS1"})));
        CHECK(c.eligibility == sync::Eligibility::Ineligible);
        CHECK(c.reason == "parent_missing");
    }
    SECTION("child of a BBL filament without a setting id")
    {
        lib.filaments->find_preset("BBL PLA", false, true)->setting_id.clear();
        Library::user(*lib.filaments, "Mine", "BBL PLA");
        auto c = classify_named(*lib.filaments, "Mine", lib.context(mapped_ids({"SYS1"})));
        CHECK(c.eligibility == sync::Eligibility::Ineligible);
        CHECK(c.reason == "not_bambu_based");
    }
}

TEST_CASE("classify handles a root filament", "[BblPresetSync]")
{
    Library lib;
    const auto ctx = lib.context(mapped_ids({"SYS1"}));

    SECTION("with an id and BBL printers is eligible")
    {
        Preset& p     = Library::user(*lib.filaments, "Mine", "");
        p.filament_id = "P1234567";
        set_compatible_printers(p, {"BBL X1C"});
        auto c = classify_named(*lib.filaments, "Mine", ctx);
        CHECK(c.eligibility == sync::Eligibility::Eligible);
        CHECK(c.shape == sync::Shape::Root);
    }
    SECTION("without an id is a candidate")
    {
        Preset& p = Library::user(*lib.filaments, "Mine", "");
        set_compatible_printers(p, {"BBL X1C"});
        auto c = classify_named(*lib.filaments, "Mine", ctx);
        CHECK(c.eligibility == sync::Eligibility::Candidate);
        CHECK(c.shape == sync::Shape::Root);
        CHECK(c.reason == "no_filament_id");
    }
    SECTION("for a non-BBL printer")
    {
        Preset& p     = Library::user(*lib.filaments, "Mine", "");
        p.filament_id = "P1234567";
        set_compatible_printers(p, {"Other Printer"});
        auto c = classify_named(*lib.filaments, "Mine", ctx);
        CHECK(c.eligibility == sync::Eligibility::Ineligible);
        CHECK(c.reason == "not_bambu_based");
    }
    SECTION("for any printer")
    {
        Preset& p     = Library::user(*lib.filaments, "Mine", "");
        p.filament_id = "P1234567";
        auto c = classify_named(*lib.filaments, "Mine", ctx);
        CHECK(c.eligibility == sync::Eligibility::Ineligible);
        CHECK(c.reason == "not_bambu_based");
    }
}

TEST_CASE("classify handles process and printer presets", "[BblPresetSync]")
{
    Library lib;
    const auto ctx = lib.context(mapped_ids({"SYS1"}));

    SECTION("a process child of a BBL process is eligible")
    {
        Library::user(*lib.prints, "Mine", "BBL Process");
        auto c = classify_named(*lib.prints, "Mine", ctx);
        CHECK(c.eligibility == sync::Eligibility::Eligible);
        CHECK(c.shape == sync::Shape::Child);
    }
    SECTION("a process child of a library process")
    {
        Library::user(*lib.prints, "Mine", "Lib Process");
        auto c = classify_named(*lib.prints, "Mine", ctx);
        CHECK(c.eligibility == sync::Eligibility::Ineligible);
        CHECK(c.reason == "not_bambu_based");
    }
    SECTION("a process root")
    {
        Library::user(*lib.prints, "Mine", "");
        auto c = classify_named(*lib.prints, "Mine", ctx);
        CHECK(c.eligibility == sync::Eligibility::Ineligible);
        CHECK(c.reason == "root_not_supported");
    }
    SECTION("a printer child of a BBL printer needs no filament id map")
    {
        Library::user(*lib.printers, "Mine", "BBL X1C");
        auto c = classify_named(*lib.printers, "Mine", lib.context(mapped_ids({})));
        CHECK(c.eligibility == sync::Eligibility::Eligible);
    }
    SECTION("a printer root")
    {
        Library::user(*lib.printers, "Mine", "");
        auto c = classify_named(*lib.printers, "Mine", ctx);
        CHECK(c.eligibility == sync::Eligibility::Ineligible);
        CHECK(c.reason == "root_not_supported");
    }
}

// ---------------------------------------------------------------------------
// Snapshot
// ---------------------------------------------------------------------------

TEST_CASE("build_snapshot lists user presets with eligibility, upload values and file times", "[BblPresetSync]")
{
    ScopedTemporaryDir temp;
    VendorProfile      bbl{"BBL"};
    VendorProfile      other{"Other"};
    PresetBundle       bundle;

    auto make_system = [](PresetCollection& c, const std::string& name, const VendorProfile* v, const std::string& setting_id,
                          const std::string& filament_id = {}) {
        Preset& p     = add_inmemory_preset(c, name);
        p.is_system   = true;
        p.vendor      = v;
        p.setting_id  = setting_id;
        p.filament_id = filament_id;
    };
    make_system(bundle.printers, "BBL X1C", &bbl, "pr1");
    make_system(bundle.prints, "BBL Process", &bbl, "pc1");
    make_system(bundle.filaments, "BBL PLA", &bbl, "f1", "SYS1");
    make_system(bundle.filaments, "Lib PLA", &other, "f3", "LIB1");

    const fs::path file = temp.path() / "Mine.json";
    Preset& mine = add_inmemory_preset(bundle.filaments, "Mine", "BBL PLA");
    mine.file    = file.string();
    mine.version = Semver(1, 0, 0);
    mine.config.option<ConfigOptionFloats>("filament_cost", true)->values = {7.0};
    REQUIRE(mine.save(nullptr));
    fs::last_write_time(file, 1700000000);
    add_inmemory_preset(bundle.filaments, "Foreign", "Lib PLA");
    add_inmemory_preset(bundle.prints, "My Process", "BBL Process").version = Semver(1, 0, 0);
    bundle.filaments.select_preset_by_name("Mine", true);
    bundle.filaments.get_edited_preset().is_dirty = true;

    const auto snap = sync::build_snapshot(bundle, mapped_ids({"SYS1"}));

    CHECK(snap.id_map_usable);
    CHECK(snap.filaments_dirty);
    CHECK_FALSE(snap.prints_dirty);
    CHECK_FALSE(snap.printers_dirty);

    auto find = [&](sync::PresetType t, const std::string& name) -> const sync::LocalItem* {
        for (const auto& i : snap.items)
            if (i.key == sync::Key{t, name}) return &i;
        return nullptr;
    };
    // Only user presets are listed.
    CHECK(snap.items.size() == 3);
    CHECK(find(sync::PresetType::Filament, "BBL PLA") == nullptr);

    const auto* m = find(sync::PresetType::Filament, "Mine");
    REQUIRE(m != nullptr);
    CHECK(m->cls.eligibility == sync::Eligibility::Eligible);
    CHECK(m->selected);
    CHECK(m->mtime == 1700000000);
    CHECK(m->file == file.string());
    CHECK(m->upload.at("filament_cost") == "7");
    CHECK(m->upload.count(BBL_JSON_KEY_VERSION) == 1);
    CHECK(m->upload.at(BBL_JSON_KEY_TYPE) == "filament");
    CHECK(m->hash.size() == 32);

    const auto* foreign = find(sync::PresetType::Filament, "Foreign");
    REQUIRE(foreign != nullptr);
    CHECK(foreign->cls.eligibility == sync::Eligibility::Ineligible);
    CHECK_FALSE(foreign->upload.empty()); // every user preset has its upload map
    CHECK(foreign->hash.size() == 32);
    CHECK(foreign->mtime == 0); // no file
    CHECK_FALSE(foreign->selected);

    const auto* process = find(sync::PresetType::Print, "My Process");
    REQUIRE(process != nullptr);
    CHECK(process->cls.eligibility == sync::Eligibility::Eligible);

    const auto locals = sync::to_local_presets(snap);
    REQUIRE(locals.size() == snap.items.size());
    for (size_t i = 0; i < locals.size(); ++i) {
        CHECK(locals[i].key == snap.items[i].key);
        if (snap.items[i].cls.eligibility == sync::Eligibility::Eligible) {
            CHECK(locals[i].state == sync::LocalState::Eligible);
            CHECK(locals[i].hash == snap.items[i].hash);
            CHECK(locals[i].mtime == snap.items[i].mtime);
        } else {
            CHECK(locals[i].state == sync::LocalState::Ineligible);
        }
    }
}

TEST_CASE("to_local_presets reports candidates as ineligible", "[BblPresetSync]")
{
    sync::LocalSnapshot snap;
    sync::LocalItem     cand;
    cand.key                 = sync::Key{sync::PresetType::Filament, "Cand"};
    cand.cls.eligibility     = sync::Eligibility::Candidate;
    cand.hash                = "ignored";
    snap.items.push_back(cand);

    const auto locals = sync::to_local_presets(snap);
    REQUIRE(locals.size() == 1);
    CHECK(locals[0].state == sync::LocalState::Ineligible);
}

// ---------------------------------------------------------------------------
// Cloud interface and mirror pass: fakes
// ---------------------------------------------------------------------------

namespace {

using sync::Key;
using sync::PresetType;

struct FakeCloud : sync::ICloud
{
    std::vector<sync::CloudRow>            rows;
    std::map<std::string, sync::ValueMap>  bodies;
    bool                                   list_ok{true};
    std::string                            list_error{"boom"};
    int                                    list_calls{0};
    std::vector<Key>                       wanted; // rows whose body the last list asked for
    std::vector<std::string>               calls;  // "create <name>", "update <id>", "remove <id>"
    std::deque<sync::WriteResult>          create_script, update_script;
    bool                                   remove_ok{true};
    int                                    next_id{1};
    long long                              server_time{5000};
    std::function<void(const std::string&)> before_update;
    std::vector<sync::ValueMap>            created_values;
    std::vector<sync::ValueMap>            updated_values;
    bool                                   drain_wanted_only{false}; // the drain returns only the bodies that were asked for

    sync::ListResult list(const std::string&, const std::function<bool(const sync::CloudRow&)>& want_body) override
    {
        ++list_calls;
        sync::ListResult r;
        if (!list_ok) {
            r.error = list_error;
            return r;
        }
        wanted.clear();
        for (const auto& row : rows)
            if (want_body(row)) wanted.push_back(Key{row.type, row.name});
        r.ok             = true;
        r.rows           = rows;
        r.bodies_by_name = bodies;
        if (drain_wanted_only) {
            r.bodies_by_name.clear();
            for (const auto& k : wanted)
                if (bodies.count(k.name)) r.bodies_by_name[k.name] = bodies.at(k.name);
        }
        return r;
    }
    sync::WriteResult create(const std::string& name, sync::ValueMap values) override
    {
        calls.push_back("create " + name);
        created_values.push_back(values);
        if (!create_script.empty()) {
            auto w = create_script.front();
            create_script.pop_front();
            return w;
        }
        sync::WriteResult w;
        w.ok          = true;
        w.http_code   = 200;
        w.setting_id  = "PF" + std::to_string(next_id++);
        w.update_time = server_time;
        return w;
    }
    sync::WriteResult update(const std::string& id, const std::string&, sync::ValueMap values) override
    {
        calls.push_back("update " + id);
        updated_values.push_back(values);
        if (before_update) before_update(id);
        if (!update_script.empty()) {
            auto w = update_script.front();
            update_script.pop_front();
            return w;
        }
        sync::WriteResult w;
        w.ok          = true;
        w.http_code   = 200;
        w.update_time = server_time;
        return w;
    }
    bool remove(const std::string& id) override
    {
        calls.push_back("remove " + id);
        return remove_ok;
    }
    int count(const std::string& prefix) const
    {
        int n = 0;
        for (const auto& c : calls)
            if (c.rfind(prefix, 0) == 0) ++n;
        return n;
    }
};

struct FakeStore : sync::ILocalStore
{
    std::optional<sync::LocalSnapshot>               snap{sync::LocalSnapshot{}};
    int                                              snapshot_calls{0};
    std::vector<sync::PullOp>                        pulls;
    std::vector<sync::DeleteOp>                      deletes;
    sync::OpOutcome                                  outcome{sync::OpResult::Applied, "", "newhash"};
    bool                                             defer{false};
    std::vector<std::function<void(sync::OpOutcome)>> held;
    std::vector<std::string>                         order; // "begin", "pull", "delete", "end"

    std::optional<sync::LocalSnapshot> snapshot(const sync::Session&) override
    {
        ++snapshot_calls;
        return snap;
    }
    void begin_batch() override { order.push_back("begin"); }
    void end_batch() override { order.push_back("end"); }
    void apply_pull(sync::PullOp op, std::function<void(sync::OpOutcome)> done) override
    {
        order.push_back("pull");
        pulls.push_back(std::move(op));
        if (defer) held.push_back(std::move(done));
        else done(outcome);
    }
    void delete_local(sync::DeleteOp op, std::function<void(sync::OpOutcome)> done) override
    {
        order.push_back("delete");
        deletes.push_back(std::move(op));
        if (defer) held.push_back(std::move(done));
        else done(outcome);
    }
};

struct FakeHost : sync::IHost
{
    std::optional<sync::Session>  sess{sync::Session{1, "orca1", "bambu1"}};
    bool                          cancel_now{false};
    int                           cancel_after{-1}; // cancelled() turns true after this many calls
    int                           cancel_calls{0};
    int                           session_changes_at_call{-1}; // session() returns another session from this call on
    int                           session_calls{0};
    long long                     t{1000000};
    std::vector<int>              sleeps;
    std::string                   bundle{"02.00.00.00"};
    fs::path                      dir;
    fs::path                      res;
    std::vector<sync::FirstRunSummary> first_runs;
    struct Halt { std::string kind; std::vector<Key> local, remote; };
    std::vector<Halt>             halts;
    std::vector<std::vector<Key>> candidate_calls;
    std::vector<std::pair<std::string, std::string>> notes;

    std::optional<sync::Session> session() override
    {
        ++session_calls;
        if (session_changes_at_call > 0 && session_calls >= session_changes_at_call) return sync::Session{99, "orca1", "bambu1"};
        return sess;
    }
    bool cancelled() override
    {
        ++cancel_calls;
        return cancel_now || (cancel_after >= 0 && cancel_calls > cancel_after);
    }
    long long   now() override { return t; }
    void        sleep_ms(int ms) override { sleeps.push_back(ms); }
    std::string bundle_version() override { return bundle; }
    fs::path    data_dir() override { return dir; }
    fs::path    resources_dir() override { return res; }
    void        on_first_run(const sync::FirstRunSummary& s) override { first_runs.push_back(s); }
    void        on_candidates(const std::vector<Key>& c) override { candidate_calls.push_back(c); }
    void        on_halt(const std::string& kind, const std::vector<Key>& l, const std::vector<Key>& r) override { halts.push_back({kind, l, r}); }
    void        notify(const std::string& code, const std::string& detail) override { notes.emplace_back(code, detail); }
    int         notified(const std::string& code) const
    {
        int n = 0;
        for (const auto& p : notes)
            if (p.first == code) ++n;
        return n;
    }
};

Key fkey(const std::string& name) { return Key{PresetType::Filament, name}; }

sync::ValueMap valid_body(const std::string& setting_id, const std::string& cost = "1")
{
    return {{"type", "filament"}, {"setting_id", setting_id}, {"inherits", "BBL PLA"}, {"base_id", "f1"}, {"version", "1.0.0.0"},
            {"filament_cost", cost}};
}

struct Env
{
    ScopedTemporaryDir dir{"orca-bblmirror"};
    FakeHost           host;
    FakeCloud          cloud;
    FakeStore          store;
    sync::Mirror       mirror{host, cloud, store};

    // Keys the fake Bambu Studio knows. "filament_flow_ratio" is an OrcaSlicer key it does not know; "studio_only" is not an OrcaSlicer key.
    static std::vector<std::string> studio_key_list()
    {
        return {"filament_cost", "filament_density", "filament_type", "filament_vendor", "layer_height", "studio_only"};
    }
    Env()
    {
        host.dir = dir.path();
        host.res = dir.path() / "res";
        fs::create_directories(host.res / "printers");
        nlohmann::json j;
        j["source"]         = "test";
        j["studio_version"] = "0";
        j["keys"]           = studio_key_list();
        boost::nowide::ofstream out((host.res / "printers" / "bambu_studio_option_keys.json").string());
        out << j.dump();
    }
    sync::BaseStore base_store(const std::string& orca = "orca1", const std::string& bambu = "bambu1") const
    {
        return sync::BaseStore(sync::BaseStore::dir(dir.path(), orca, bambu));
    }

    fs::path ledger_path(const std::string& orca = "orca1", const std::string& bambu = "bambu1") const
    {
        return sync::Ledger::file_path(dir.path(), orca, bambu);
    }
    void seed(const std::map<Key, sync::LedgerEntry>& entries, bool confirmed = true, const std::string& orca = "orca1",
              const std::string& bambu = "bambu1")
    {
        sync::Ledger l(ledger_path(orca, bambu));
        for (const auto& [k, e] : entries) l.put(k, e);
        l.set_first_run_confirmed(confirmed);
        REQUIRE(l.save());
    }
    std::unique_ptr<sync::Ledger> reload(const std::string& orca = "orca1", const std::string& bambu = "bambu1") const
    {
        auto l = std::make_unique<sync::Ledger>(ledger_path(orca, bambu));
        l->load();
        return l;
    }
    void add_local(const std::string& name, const std::string& hash, long long mtime = 0)
    {
        sync::LocalItem item;
        item.key                 = fkey(name);
        item.cls.eligibility     = sync::Eligibility::Eligible;
        item.upload              = {{"filament_cost", hash}, {"type", "filament"}};
        item.hash                = hash;
        item.mtime               = mtime;
        store.snap->items.push_back(item);
    }
    // A local preset whose upload map is exactly `body` (it equals the remote copy).
    void add_local_body(const std::string& name, const sync::ValueMap& body)
    {
        sync::LocalItem item;
        item.key             = fkey(name);
        item.cls.eligibility = sync::Eligibility::Eligible;
        item.upload          = body;
        item.hash            = sync::upload_hash(body);
        store.snap->items.push_back(item);
    }
    void add_candidate(const std::string& name)
    {
        sync::LocalItem item;
        item.key             = fkey(name);
        item.cls.eligibility = sync::Eligibility::Candidate;
        store.snap->items.push_back(item);
    }
    void remove_local(const std::string& name)
    {
        auto& v = store.snap->items;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const sync::LocalItem& i) { return i.key == fkey(name); }), v.end());
    }
    void set_local_hash(const std::string& name, const std::string& hash)
    {
        for (auto& i : store.snap->items)
            if (i.key == fkey(name)) {
                i.hash                  = hash;
                i.upload["filament_cost"] = hash;
            }
    }
    void add_remote(const std::string& name, const std::string& id, long long time, bool with_body = true)
    {
        cloud.rows.push_back({PresetType::Filament, name, id, time});
        if (with_body) cloud.bodies[name] = valid_body(id, name);
    }
    void pass(sync::Trigger t = sync::Trigger::Tick) { mirror.run_pass(t); }

    // The ledger file holds what the mirror holds.
    void check_disk()
    {
        const auto live = mirror.ledger();
        REQUIRE(live);
        const auto disk = reload();
        const auto a    = live->entries();
        const auto b    = disk->entries();
        REQUIRE(a.size() == b.size());
        for (const auto& [k, e] : a) {
            auto it = b.find(k);
            REQUIRE(it != b.end());
            CHECK(it->second.setting_id == e.setting_id);
            CHECK(it->second.update_time == e.update_time);
            CHECK(it->second.synced_hash == e.synced_hash);
            CHECK(it->second.state == e.state);
            CHECK(it->second.reason == e.reason);
            CHECK(it->second.absent_passes == e.absent_passes);
            CHECK(it->second.absent_since == e.absent_since);
        }
        CHECK(disk->halted() == live->halted());
        CHECK(disk->first_run_confirmed() == live->first_run_confirmed());
        CHECK(disk->clock_offset() == live->clock_offset());
    }
};

// The mirror's change-detection hash of a local preset made by Env::add_local(name, literal).
std::string H(const std::string& literal)
{
    return sync::upload_hash({{"filament_cost", literal}});
}

sync::LedgerEntry linked(const std::string& id, long long time, const std::string& hash)
{
    sync::LedgerEntry e;
    e.setting_id  = id;
    e.update_time = time;
    e.synced_hash = H(hash);
    return e;
}

bool has_note(const sync::Status& st, const std::string& needle)
{
    for (const auto& n : st.notes)
        if (n.find(needle) != std::string::npos) return true;
    return false;
}

bool has_waiting(const sync::Status& st, const std::string& text)
{
    return std::find(st.waiting.begin(), st.waiting.end(), text) != st.waiting.end();
}

} // namespace

// ---------------------------------------------------------------------------
// Steps 1 to 5: session, list decision, list
// ---------------------------------------------------------------------------

TEST_CASE("A pass is skipped when the host preconditions are not met", "[BblPresetSync]")
{
    Env e;
    e.host.sess.reset();
    e.pass();
    CHECK(e.mirror.status().last_result == "skipped: preconditions");
    CHECK(e.mirror.status().last_pass == e.host.t);
    CHECK(e.store.snapshot_calls == 0);
    CHECK(e.cloud.list_calls == 0);
}

TEST_CASE("A pass without a snapshot is skipped", "[BblPresetSync]")
{
    Env e;
    e.store.snap.reset();
    e.pass();
    CHECK(e.mirror.status().last_result == "skipped: no snapshot");
    CHECK(e.cloud.list_calls == 0);
}

TEST_CASE("A session change loads the ledger of the new session and clears in-memory state", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("A"), linked("PA", 100, "ha")}});
    e.seed({{fkey("B"), linked("PB", 100, "hb")}}, true, "orca2", "bambu2");
    e.add_local("A", "ha");
    e.add_remote("A", "PA", 100);

    e.pass();
    REQUIRE(e.mirror.ledger());
    CHECK(e.mirror.ledger()->get(fkey("A")).has_value());
    CHECK_FALSE(e.mirror.ledger()->get(fkey("B")).has_value());
    CHECK(e.mirror.ledger()->file() == e.ledger_path());

    // A failing create puts the key into backoff.
    e.add_local("Fresh", "hf");
    e.cloud.create_script.push_back(sync::WriteResult{false, 500, "", "", 0});
    e.pass(sync::Trigger::Manual);
    CHECK(e.cloud.count("create") == 1);
    e.pass(sync::Trigger::Manual);
    CHECK(e.cloud.count("create") == 1); // still backing off

    // Another account: other ledger file, the backoff is gone.
    e.host.sess = sync::Session{2, "orca2", "bambu2"};
    e.remove_local("A");
    e.add_local("B", "hb");
    e.cloud.rows.clear();
    e.add_remote("B", "PB", 100);
    e.pass();
    CHECK(e.mirror.ledger()->file() == e.ledger_path("orca2", "bambu2"));
    CHECK(e.mirror.ledger()->get(fkey("B")).has_value());
    CHECK_FALSE(e.mirror.ledger()->get(fkey("A")).has_value());
    CHECK(e.cloud.count("create") == 2); // "Fresh" is tried at once in the new session
}

TEST_CASE("A corrupt ledger file starts an empty ledger", "[BblPresetSync]")
{
    Env e;
    fs::create_directories(e.ledger_path().parent_path());
    {
        boost::nowide::ofstream out(e.ledger_path().string(), std::ios::binary);
        out << "{ this is not json";
    }
    e.pass();
    CHECK(e.mirror.status().last_result == "dry run");
    CHECK(e.mirror.ledger()->entries().empty());
}

TEST_CASE("The cloud is listed only when something changed or the interval passed", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("K"), linked("P1", 100, "h1")}});
    e.add_local("K", "h1");
    e.add_remote("K", "P1", 100);

    e.pass();
    CHECK(e.cloud.list_calls == 1); // first pass in a session
    CHECK(e.mirror.status().last_result == "ok");

    e.pass();
    CHECK(e.cloud.list_calls == 1);
    CHECK(e.mirror.status().last_result == "ok");

    SECTION("a manual trigger lists")
    {
        e.pass(sync::Trigger::Manual);
        CHECK(e.cloud.list_calls == 2);
    }
    SECTION("the interval passing lists")
    {
        e.host.t += sync::LIST_INTERVAL_SECONDS - 1;
        e.pass();
        CHECK(e.cloud.list_calls == 1);
        e.host.t += 1;
        e.pass();
        CHECK(e.cloud.list_calls == 2);
    }
    SECTION("a new eligible local preset lists")
    {
        e.add_local("New", "hn");
        e.pass();
        CHECK(e.cloud.list_calls == 2);
    }
    SECTION("a changed hash of a linked preset lists")
    {
        e.set_local_hash("K", "h2");
        e.pass();
        CHECK(e.cloud.list_calls == 2);
        CHECK(e.cloud.count("update P1") == 1);
    }
    SECTION("a linked entry without a local preset lists")
    {
        e.remove_local("K");
        e.pass();
        CHECK(e.cloud.list_calls == 2);
    }
    SECTION("an ineligible local preset does not")
    {
        sync::LocalItem item;
        item.key                 = fkey("Ineligible");
        item.cls.eligibility     = sync::Eligibility::Ineligible;
        e.store.snap->items.push_back(item);
        e.pass();
        CHECK(e.cloud.list_calls == 1);
    }
}

TEST_CASE("A failed list changes nothing and notifies once until a list succeeds", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    e.add_local("New", "hn");
    e.cloud.list_ok = false;

    e.pass();
    CHECK(e.mirror.status().last_result == "list failed: boom");
    CHECK(e.host.notified("list_failed") == 1);
    CHECK(e.host.notes.back().second == "boom");
    e.pass(sync::Trigger::Manual);
    CHECK(e.host.notified("list_failed") == 1);
    CHECK(e.cloud.calls.empty());
    CHECK(e.mirror.status().planned.empty());
    CHECK(e.mirror.ledger()->entries().empty());

    e.cloud.list_ok = true;
    e.pass(sync::Trigger::Manual);
    CHECK(e.mirror.status().last_result == "ok");
    CHECK(e.cloud.count("create New") == 1);

    e.cloud.list_ok = false;
    e.pass(sync::Trigger::Manual);
    CHECK(e.host.notified("list_failed") == 2);
}

// ---------------------------------------------------------------------------
// Step 6: remote rows and bodies
// ---------------------------------------------------------------------------

TEST_CASE("A name used by two preset types yields no body and one note", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    e.cloud.rows.push_back({PresetType::Filament, "Dup", "PA", 100});
    e.cloud.rows.push_back({PresetType::Print, "Dup", "PB", 100});
    e.cloud.bodies["Dup"] = valid_body("PA");
    e.pass();
    CHECK(has_note(e.mirror.status(), "name used by several preset types: Dup"));
    int n = 0;
    for (const auto& note : e.mirror.status().notes)
        if (note == "name used by several preset types: Dup") ++n;
    CHECK(n == 1);
    CHECK(e.store.pulls.empty());
}

TEST_CASE("A body with metadata only is not used", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    e.cloud.rows.push_back({PresetType::Filament, "Meta", "PM", 100});
    e.cloud.bodies["Meta"] = {{"type", "filament"}, {"setting_id", "PM"}, {"version", "1.0.0.0"}, {"inherits", "BBL PLA"}};
    e.pass();
    CHECK(e.store.pulls.empty());
    CHECK(has_note(e.mirror.status(), "body unavailable for filament/Meta: metadata only"));
}

TEST_CASE("A body for another setting id is not used", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    e.cloud.rows.push_back({PresetType::Filament, "Other", "PO", 100});
    e.cloud.bodies["Other"] = valid_body("SOMETHING-ELSE");
    e.pass();
    CHECK(e.store.pulls.empty());
    CHECK(has_note(e.mirror.status(), "body unavailable for filament/Other: wrong identity"));
}

// ---------------------------------------------------------------------------
// Step 8: dry run and first run confirmation
// ---------------------------------------------------------------------------

TEST_CASE("The first run is a dry run that shows its summary and executes nothing", "[BblPresetSync]")
{
    Env e;
    e.add_local("Up", "hup");
    const auto body = valid_body("PD", "d");
    e.cloud.rows.push_back({PresetType::Filament, "Down", "PD", 100});
    e.cloud.bodies["Down"] = body;
    e.add_local_body("Same", valid_body("PS", "s"));
    e.cloud.rows.push_back({PresetType::Filament, "Same", "PS", 100});
    e.cloud.bodies["Same"] = valid_body("PS", "s");
    sync::LocalItem cand;
    cand.key             = fkey("Cand");
    cand.cls.eligibility = sync::Eligibility::Candidate;
    e.store.snap->items.push_back(cand);

    e.pass();
    CHECK(e.mirror.status().last_result == "dry run");
    REQUIRE(e.host.first_runs.size() == 1);
    const auto& s = e.host.first_runs[0];
    CHECK(s.uploads == std::vector<Key>{fkey("Up")});
    CHECK(s.downloads == std::vector<Key>{fkey("Down")});
    CHECK(s.links == std::vector<Key>{fkey("Same")});
    CHECK(s.candidates == std::vector<Key>{fkey("Cand")});
    CHECK(e.cloud.calls.empty());
    CHECK(e.store.pulls.empty());
    CHECK(e.mirror.ledger()->entries().empty());
    CHECK_FALSE(e.mirror.status().first_run_confirmed);

    // An unchanged summary is shown once, a changed one again.
    e.pass(sync::Trigger::Manual);
    CHECK(e.host.first_runs.size() == 1);
    e.add_local("Up2", "hup2");
    e.pass(sync::Trigger::Manual);
    CHECK(e.host.first_runs.size() == 2);
    CHECK(e.host.first_runs[1].uploads.size() == 2);
}

TEST_CASE("The dry run summary lists conflicts and remote deletes", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("Gone"), linked("P9", 100, "h9")}, {fkey("Both"), linked("P8", 100, "old")},
            {fkey("Both2"), linked("P7", 100, "old")}},
           /*confirmed=*/false);
    e.add_remote("Gone", "P9", 100, false);
    // Local changed and remote changed, local newer.
    e.add_local("Both", "new", 2000000);
    e.add_remote("Both", "P8", 200);
    // Local changed and remote changed, remote newer.
    e.add_local("Both2", "new", 1);
    e.add_remote("Both2", "P7", 5000000);
    e.pass();
    REQUIRE(e.host.first_runs.size() == 1);
    const auto& s = e.host.first_runs[0];
    CHECK(s.remote_deletes == std::vector<Key>{fkey("Gone")});
    CHECK(s.conflicts_local_wins == std::vector<Key>{fkey("Both")});
    CHECK(s.conflicts_remote_wins == std::vector<Key>{fkey("Both2")});
}

TEST_CASE("Declining the first run stops the summary and confirming lets the next pass upload", "[BblPresetSync]")
{
    Env e;
    e.add_local("Up", "hup");
    e.pass();
    REQUIRE(e.host.first_runs.size() == 1);

    SECTION("declined")
    {
        e.mirror.confirm_first_run(false);
        e.add_local("Up2", "hup2");
        e.pass(sync::Trigger::Manual);
        CHECK(e.host.first_runs.size() == 1);
        CHECK(e.mirror.status().last_result == "dry run");
        CHECK_FALSE(e.reload()->first_run_confirmed());
    }
    SECTION("confirmed")
    {
        e.mirror.confirm_first_run(true);
        CHECK(e.mirror.status().first_run_confirmed);
        CHECK(e.reload()->first_run_confirmed());
        e.pass();
        CHECK(e.cloud.count("create Up") == 1);
        CHECK(e.mirror.status().last_result == "ok");
        e.check_disk();
    }
}

TEST_CASE("reset_first_run_prompt makes the next dry run show its summary again", "[BblPresetSync]")
{
    Env e;
    e.add_local("Up", "hup");
    e.pass();
    e.pass(sync::Trigger::Manual);
    CHECK(e.host.first_runs.size() == 1);

    e.mirror.reset_first_run_prompt();
    e.pass(sync::Trigger::Manual);
    CHECK(e.host.first_runs.size() == 2);

    // It also clears a decline.
    e.mirror.confirm_first_run(false);
    e.pass(sync::Trigger::Manual);
    CHECK(e.host.first_runs.size() == 2);
    e.mirror.reset_first_run_prompt();
    e.pass(sync::Trigger::Manual);
    CHECK(e.host.first_runs.size() == 3);
}

TEST_CASE("Candidates that appear after the first run are announced once per change of the list", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    e.add_candidate("Zed");
    e.add_candidate("Alpha");

    e.pass();
    REQUIRE(e.host.candidate_calls.size() == 1);
    CHECK(e.host.candidate_calls[0] == std::vector<Key>{fkey("Alpha"), fkey("Zed")}); // key order

    e.pass(sync::Trigger::Manual);
    e.pass();
    CHECK(e.host.candidate_calls.size() == 1); // unchanged

    e.add_candidate("Mid");
    e.pass();
    REQUIRE(e.host.candidate_calls.size() == 2);
    CHECK(e.host.candidate_calls[1].size() == 3);

    // An empty list resets the memory: the same candidate is announced again when it reappears.
    e.store.snap->items.clear();
    e.pass();
    e.add_candidate("Alpha");
    e.pass();
    CHECK(e.host.candidate_calls.size() == 3);
}

TEST_CASE("Candidates are not announced before the first run is confirmed or when they have an entry", "[BblPresetSync]")
{
    Env e;
    e.add_candidate("Cand");
    e.pass();
    CHECK(e.host.candidate_calls.empty());

    Env d;
    sync::LedgerEntry declined;
    declined.state = "declined";
    d.seed({{fkey("Refused"), declined}});
    d.add_candidate("Refused");
    d.pass();
    CHECK(d.host.candidate_calls.empty());
    d.add_candidate("Other");
    d.pass();
    REQUIRE(d.host.candidate_calls.size() == 1);
    CHECK(d.host.candidate_calls[0] == std::vector<Key>{fkey("Other")});
}

TEST_CASE("reset_candidates_prompt makes the next pass announce the candidates again", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    e.add_candidate("Cand");
    e.pass();
    CHECK(e.host.candidate_calls.size() == 1);
    e.pass(sync::Trigger::Manual);
    CHECK(e.host.candidate_calls.size() == 1);

    e.mirror.reset_candidates_prompt();
    e.pass();
    CHECK(e.host.candidate_calls.size() == 2);
}

TEST_CASE("The announced candidates are forgotten on a session change", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    e.seed({}, true, "orca2", "bambu2");
    e.add_candidate("Cand");
    e.pass();
    REQUIRE(e.host.candidate_calls.size() == 1);

    e.host.sess = sync::Session{2, "orca2", "bambu2"};
    e.pass();
    CHECK(e.host.candidate_calls.size() == 2);
}

TEST_CASE("Ledger conversions survive a save and load and are absent from old files", "[BblPresetSync]")
{
    ScopedTemporaryDir temp;
    const fs::path     file = temp.path() / "ledger.json";
    {
        sync::Ledger ledger(file);
        ledger.put_conversion(Key{PresetType::Filament, "Mine"}, {"OLD1", "PNEW1", {"X1C"}});
        REQUIRE(ledger.save());
    }
    sync::Ledger loaded(file);
    REQUIRE(loaded.load());
    const auto c = loaded.get_conversion(Key{PresetType::Filament, "Mine"});
    REQUIRE(c.has_value());
    CHECK(c->from_filament_id == "OLD1");
    CHECK(c->to_filament_id == "PNEW1");
    CHECK(c->k_copied_printers == std::vector<std::string>{"X1C"});
    CHECK(loaded.conversions().size() == 1);
    CHECK_FALSE(loaded.get_conversion(Key{PresetType::Filament, "Other"}).has_value());

    {
        boost::nowide::ofstream out(file.string(), std::ios::binary | std::ios::trunc);
        out << R"({"version":1,"entries":{}})";
    }
    REQUIRE(loaded.load());
    CHECK(loaded.conversions().empty());
}

// ---------------------------------------------------------------------------
// Step 10: executing actions
// ---------------------------------------------------------------------------

TEST_CASE("Create links the new Bambu id and sets the clock offset", "[BblPresetSync]")
{
    Env e;
    sync::LedgerEntry old = linked("", 0, "stale");
    old.state             = "too_large";
    old.converted_from    = "GFOLD";
    old.k_copied_printers = {"printer-1"};
    e.seed({{fkey("New"), old}});
    e.add_local("New", "hn");
    e.pass();

    REQUIRE(e.cloud.calls.size() == 1);
    CHECK(e.cloud.created_values[0].at("filament_cost") == "hn");
    const auto entry = e.mirror.ledger()->get(fkey("New"));
    REQUIRE(entry.has_value());
    CHECK(entry->setting_id == "PF1");
    CHECK(entry->update_time == 5000);
    CHECK(entry->synced_hash == H("hn"));
    CHECK(entry->state == "linked");
    CHECK(entry->absent_passes == 0);
    CHECK(entry->absent_since == 0);
    CHECK(entry->converted_from == "GFOLD");
    CHECK(entry->k_copied_printers == std::vector<std::string>{"printer-1"});
    CHECK(e.mirror.ledger()->clock_offset() == 5000 - e.host.t);
    e.check_disk();
}

TEST_CASE("Create without a server time leaves the clock offset", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    e.add_local("New", "hn");
    e.cloud.server_time = 0;
    e.pass();
    CHECK(e.mirror.ledger()->get(fkey("New"))->update_time == 0);
    CHECK(e.mirror.ledger()->clock_offset() == 0);
}

TEST_CASE("Update writes the local values to the linked Bambu id", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("Mine"), linked("P1", 100, "h1")}});
    e.add_local("Mine", "h2");
    e.add_remote("Mine", "P1", 100, false);
    e.pass();
    CHECK(e.cloud.count("update P1") == 1);
    const auto entry = e.mirror.ledger()->get(fkey("Mine"));
    REQUIRE(entry.has_value());
    CHECK(entry->setting_id == "P1");
    CHECK(entry->update_time == 5000);
    CHECK(entry->synced_hash == H("h2"));
    CHECK(entry->state == "linked");
    CHECK(e.mirror.ledger()->clock_offset() == 5000 - e.host.t);
    e.check_disk();
}

TEST_CASE("A change on both sides is a merging pull that saves both copies first", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("Mine"), linked("P1", 100, "h1")}});
    e.add_local("Mine", "h2", e.host.t); // local is newer
    e.add_remote("Mine", "P1", 500);
    const fs::path expected = sync::Ledger::backup_dir(e.dir.path()) / "19700112-134640_filament_Mine_remote.json";

    SECTION("the backup is written and the pull asks for the local side to win")
    {
        e.pass();
        CHECK(fs::exists(expected));
        CHECK(e.cloud.count("update") == 0);
        REQUIRE(e.store.pulls.size() == 1);
        CHECK(e.store.pulls[0].backup_local);
        CHECK(e.store.pulls[0].body.at("filament_cost") == "h2"); // no base: both changed, the local value wins
        std::ifstream in(expected.string());
        const auto    json = nlohmann::json::parse(in);
        CHECK(json.at("filament_cost").get<std::string>() == "Mine");
        CHECK(json.at("setting_id").get<std::string>() == "P1");
    }
    SECTION("an unwritable backup skips the pull")
    {
        {
            boost::nowide::ofstream out((sync::Ledger::backup_dir(e.dir.path())).string(), std::ios::binary);
            out << "a file where the folder should be";
        }
        e.pass();
        CHECK(e.store.pulls.empty());
        CHECK(has_note(e.mirror.status(), "pull skipped for filament/Mine"));
        CHECK(e.mirror.ledger()->get(fkey("Mine"))->synced_hash == H("h1"));
    }
}

TEST_CASE("A 422 on update leaves the entry and forces a list on the next pass", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("Mine"), linked("P1", 100, "h1")}});
    e.add_local("Mine", "h2");
    e.add_remote("Mine", "P1", 100, false);
    e.cloud.update_script.push_back(sync::WriteResult{false, 422, "", "", 0});
    e.pass();
    CHECK(has_note(e.mirror.status(), "remote copy missing for filament/Mine"));
    CHECK(e.mirror.ledger()->get(fkey("Mine"))->synced_hash == H("h1"));
    CHECK(e.cloud.list_calls == 1);
    e.pass(); // no manual trigger, no interval
    CHECK(e.cloud.list_calls == 2);
    CHECK(e.cloud.count("update P1") == 2); // not in backoff
}

TEST_CASE("Link pairs a local preset with an identical remote row", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    const auto body = valid_body("PL", "x");
    e.add_local_body("Same", body);
    e.cloud.rows.push_back({PresetType::Filament, "Same", "PL", 321});
    e.cloud.bodies["Same"] = body;
    e.pass();
    const auto entry = e.mirror.ledger()->get(fkey("Same"));
    REQUIRE(entry.has_value());
    CHECK(entry->setting_id == "PL");
    CHECK(entry->update_time == 321);
    CHECK(entry->synced_hash == sync::upload_hash(body));
    CHECK(entry->state == "linked");
    CHECK(e.cloud.calls.empty());
    e.check_disk();
}

TEST_CASE("DropEntry erases the entry of a preset that is gone on both sides", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("Gone"), linked("P1", 100, "h1")}, {fkey("Keep"), linked("P2", 100, "h2")}});
    e.add_local("Keep", "h2");
    e.add_remote("Keep", "P2", 100, false);
    e.pass();
    CHECK_FALSE(e.mirror.ledger()->get(fkey("Gone")).has_value());
    CHECK(e.mirror.ledger()->get(fkey("Keep")).has_value());
    e.check_disk();
}

TEST_CASE("SetAbsent records that a row was not in the list", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("Seen"), linked("P1", 100, "h1")}, {fkey("Lost"), linked("P2", 100, "h2")}});
    e.add_local("Seen", "h1");
    e.add_remote("Seen", "P1", 100, false);
    e.add_local("Lost", "h2");
    e.pass();
    const auto entry = e.mirror.ledger()->get(fkey("Lost"));
    REQUIRE(entry.has_value());
    CHECK(entry->absent_passes == 1);
    CHECK(entry->absent_since == e.host.t);
    CHECK(entry->state == "linked");
    e.check_disk();
}

TEST_CASE("MarkUnresolvable keeps the entry as unresolvable with the planner reason", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("A"), linked("P1", 100, "ha")}});
    e.add_local("A", "ha");
    e.add_local("B", "hb");
    e.add_remote("B", "P1", 200, false); // the row of A was renamed onto B
    e.pass();
    const auto entry = e.mirror.ledger()->get(fkey("A"));
    REQUIRE(entry.has_value());
    CHECK(entry->state == "unresolvable");
    CHECK(entry->reason == "remote renamed onto an existing name");
    CHECK(entry->setting_id.empty());
    e.check_disk();
}

// ---------------------------------------------------------------------------
// Pull and DeleteLocal through the local store
// ---------------------------------------------------------------------------

TEST_CASE("A pull marks its key pending until the store is done", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    e.add_remote("Remote", "P5", 200);
    e.store.defer = true;

    e.pass();
    REQUIRE(e.store.pulls.size() == 1);
    const auto& op = e.store.pulls[0];
    CHECK(op.key == fkey("Remote"));
    CHECK(op.body == valid_body("P5", "Remote"));
    CHECK(op.session == sync::Session{1, "orca1", "bambu1"});
    CHECK(op.expected_local_hash.empty());
    CHECK_FALSE(op.backup_local);
    CHECK_FALSE(op.rename_from_set);
    CHECK(e.mirror.status().planned == std::vector<std::string>{"pull filament/Remote"});

    e.pass(sync::Trigger::Manual);
    CHECK(e.store.pulls.size() == 1);
    CHECK(e.mirror.status().planned.empty());
    CHECK(has_waiting(e.mirror.status(), "filament/Remote: pending"));

    sync::OpOutcome applied{sync::OpResult::Applied, "", "hx"};
    applied.new_upload = e.cloud.bodies["Remote"]; // the preset now holds what Bambu holds
    e.store.held[0](applied);
    const auto entry = e.mirror.ledger()->get(fkey("Remote"));
    REQUIRE(entry.has_value());
    CHECK(entry->setting_id == "P5");
    CHECK(entry->update_time == 200);
    CHECK(entry->synced_hash == sync::upload_hash(e.cloud.bodies["Remote"])); // a plain download uploads nothing later
    CHECK(entry->state == "linked");
    e.check_disk();
}

TEST_CASE("Each pull outcome has its effect", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    e.add_remote("Remote", "P5", 200);

    SECTION("applied")
    {
        e.store.outcome            = {sync::OpResult::Applied, "", "hx"};
        e.store.outcome.new_upload = e.cloud.bodies["Remote"];
        e.pass();
        CHECK(e.mirror.ledger()->get(fkey("Remote"))->synced_hash == sync::upload_hash(e.cloud.bodies["Remote"]));
        e.check_disk();
    }
    SECTION("unresolvable")
    {
        e.store.outcome = {sync::OpResult::Unresolvable, "needs a parent", ""};
        e.pass();
        const auto entry = e.mirror.ledger()->get(fkey("Remote"));
        REQUIRE(entry.has_value());
        CHECK(entry->state == "unresolvable");
        CHECK(entry->reason == "needs a parent");
        CHECK(entry->setting_id == "P5");
        CHECK(entry->update_time == 200);
        CHECK(e.host.notes.empty()); // the controller reports unresolvable presets once per batch
        e.check_disk();
    }
    SECTION("postponed")
    {
        e.store.outcome = {sync::OpResult::Postponed, "editing", ""};
        e.pass();
        CHECK_FALSE(e.mirror.ledger()->get(fkey("Remote")).has_value());
        CHECK(has_waiting(e.mirror.status(), "filament/Remote: editing"));
        e.pass(sync::Trigger::Manual); // not pending and not in backoff: planned again
        CHECK(e.store.pulls.size() == 2);
    }
    SECTION("failed")
    {
        e.store.outcome = {sync::OpResult::Failed, "disk", ""};
        e.pass();
        CHECK_FALSE(e.mirror.ledger()->get(fkey("Remote")).has_value());
        e.host.t += 59;
        e.pass(sync::Trigger::Manual);
        CHECK(e.store.pulls.size() == 1);
        CHECK(has_waiting(e.mirror.status(), "filament/Remote: backoff"));
        e.host.t += 1;
        e.pass(sync::Trigger::Manual);
        CHECK(e.store.pulls.size() == 2);
    }
}

TEST_CASE("A pull that finishes after a session change does nothing", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    e.add_remote("Remote", "P5", 200);
    e.store.defer = true;
    e.pass();
    REQUIRE(e.store.held.size() == 1);

    e.host.sess = sync::Session{2, "orca1", "bambu1"};
    e.store.snap->items.clear();
    e.cloud.rows.clear();
    e.pass();
    e.store.held[0](sync::OpOutcome{sync::OpResult::Applied, "", "hx"});
    CHECK_FALSE(e.mirror.ledger()->get(fkey("Remote")).has_value());
    CHECK_FALSE(e.reload()->get(fkey("Remote")).has_value());
}

TEST_CASE("A rename pull erases the old entry once applied", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("Old"), linked("P7", 100, "h")}});
    e.add_local("Old", "h");
    e.add_remote("New", "P7", 200);
    e.pass();

    REQUIRE(e.store.pulls.size() == 1);
    CHECK(e.store.pulls[0].key == fkey("New"));
    CHECK(e.store.pulls[0].rename_from_set);
    CHECK(e.store.pulls[0].rename_from == fkey("Old"));
    CHECK(e.store.pulls[0].backup_local);
    CHECK(e.store.pulls[0].expected_local_hash.empty()); // nothing at the new name yet
    CHECK_FALSE(e.mirror.ledger()->get(fkey("Old")).has_value());
    const auto entry = e.mirror.ledger()->get(fkey("New"));
    REQUIRE(entry.has_value());
    CHECK(entry->setting_id == "P7");
    e.check_disk();
}

TEST_CASE("A pull over a local preset passes its snapshot hash", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("Mine"), linked("P1", 100, "h1")}});
    e.add_local("Mine", "h1");
    e.add_remote("Mine", "P1", 300);
    e.pass();
    REQUIRE(e.store.pulls.size() == 1);
    CHECK(e.store.pulls[0].expected_local_hash == "h1");
}

namespace {

// Four linked presets, three of which are absent from the list long enough to be deleted locally.
void local_delete_scenario(Env& e)
{
    const long long since = e.host.t - 1000;
    std::map<Key, sync::LedgerEntry> entries;
    for (const std::string n : {"K1", "K2", "K3"}) {
        auto entry          = linked("Q" + n, 100, "h" + n);
        entry.absent_passes = 1;
        entry.absent_since  = since;
        entries[fkey(n)]    = entry;
        e.add_local(n, "h" + n);
    }
    entries[fkey("K4")] = linked("Q4", 100, "h4");
    e.add_local("K4", "h4");
    e.add_remote("K4", "Q4", 100, false);
    e.seed(entries);
}

// Four linked presets, three of which are gone locally while their rows are unchanged.
void remote_delete_scenario(Env& e)
{
    std::map<Key, sync::LedgerEntry> entries;
    for (const std::string n : {"R1", "R2", "R3"}) {
        entries[fkey(n)] = linked("Q" + n, 100, "h" + n);
        e.add_remote(n, "Q" + n, 100);
    }
    entries[fkey("K4")] = linked("Q4", 100, "h4");
    e.add_local("K4", "h4");
    e.add_remote("K4", "Q4", 100, false);
    e.seed(entries);
}

} // namespace

TEST_CASE("DeleteLocal outcomes", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("Gone"), [] {
                 auto x          = linked("P2", 100, "hg");
                 x.absent_passes = 1;
                 x.absent_since  = 1000000 - 1000;
                 return x;
             }()},
            {fkey("Keep"), linked("P3", 100, "hk")}});
    e.add_local("Gone", "hg");
    e.add_local("Keep", "hk");
    e.add_remote("Keep", "P3", 100, false);

    SECTION("applied erases the entry")
    {
        e.pass();
        REQUIRE(e.store.deletes.size() == 1);
        CHECK(e.store.deletes[0].key == fkey("Gone"));
        CHECK(e.store.deletes[0].expected_local_hash == "hg");
        CHECK(e.store.deletes[0].backup_local);
        CHECK_FALSE(e.mirror.ledger()->get(fkey("Gone")).has_value());
        e.check_disk();
    }
    SECTION("postponed waits")
    {
        e.store.outcome = {sync::OpResult::Postponed, "in use", ""};
        e.pass();
        CHECK(e.mirror.ledger()->get(fkey("Gone")).has_value());
        CHECK(has_waiting(e.mirror.status(), "filament/Gone: in use"));
    }
    SECTION("failed backs off")
    {
        e.store.outcome = {sync::OpResult::Failed, "", ""};
        e.pass();
        CHECK(e.mirror.ledger()->get(fkey("Gone")).has_value());
        e.pass(sync::Trigger::Manual);
        CHECK(e.store.deletes.size() == 1);
        e.host.t += 60;
        e.pass(sync::Trigger::Manual);
        CHECK(e.store.deletes.size() == 2);
    }
}

// ---------------------------------------------------------------------------
// Errors, backoff, limits
// ---------------------------------------------------------------------------

TEST_CASE("A 401 blocks the mirror until the Bambu account changes", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    e.seed({}, true, "orca1", "bambu2");
    e.add_local("New1", "h1");
    e.add_local("New2", "h2");
    e.cloud.create_script.push_back(sync::WriteResult{false, 401, "", "", 0});

    e.pass();
    CHECK(e.cloud.count("create") == 1); // the pass stopped
    CHECK(e.host.notified("auth") == 1);

    const int snapshots = e.store.snapshot_calls;
    e.pass();
    CHECK(e.mirror.status().last_result == "skipped: auth");
    CHECK(e.store.snapshot_calls == snapshots);

    // A new generation of the same account does not unblock.
    e.host.sess = sync::Session{2, "orca1", "bambu1"};
    e.pass();
    CHECK(e.mirror.status().last_result == "skipped: auth");

    e.host.sess = sync::Session{3, "orca1", "bambu2"};
    e.pass();
    CHECK(e.mirror.status().last_result == "ok");
    CHECK(e.cloud.count("create") == 3);
}

TEST_CASE("Preset limit code 14 blocks only creates", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("Mine"), linked("P1", 100, "h1")}});
    e.add_local("Mine", "h2");
    e.add_remote("Mine", "P1", 100, false);
    e.add_local("New1", "n1");
    e.add_local("New2", "n2");
    e.cloud.create_script.push_back(sync::WriteResult{false, 400, "14", "", 0});

    e.pass();
    CHECK(e.cloud.count("update P1") == 1); // updates still go through
    CHECK(e.cloud.count("create") == 1);    // New2 is not even tried
    CHECK(e.host.notified("preset_limit") == 1);
    CHECK(has_waiting(e.mirror.status(), "filament/New2: preset limit"));

    e.pass(sync::Trigger::Manual);
    CHECK(e.host.notified("preset_limit") == 1);
    CHECK(e.cloud.count("create") == 1);
    CHECK(e.mirror.status().last_result == "ok");
}

TEST_CASE("A 413 marks the entry too large and is not retried until the hash changes", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    e.add_local("Big", "hb");
    e.cloud.create_script.push_back(sync::WriteResult{false, 413, "", "", 0});
    e.pass();
    const auto entry = e.mirror.ledger()->get(fkey("Big"));
    REQUIRE(entry.has_value());
    CHECK(entry->state == "too_large");
    CHECK(entry->synced_hash == H("hb"));
    CHECK(entry->setting_id.empty());
    e.check_disk();

    e.pass(sync::Trigger::Manual);
    CHECK(e.cloud.count("create") == 1);

    e.set_local_hash("Big", "hb2");
    e.pass();
    CHECK(e.cloud.count("create") == 2);
    CHECK(e.mirror.ledger()->get(fkey("Big"))->state == "linked");
}

TEST_CASE("Failed writes back off 60, 120, 240 seconds up to an hour and a success resets", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    e.add_local("Flaky", "hf");

    const std::vector<long long> delays = {60, 120, 240, 480, 960, 1920, 3600, 3600};
    for (long long delay : delays) {
        e.cloud.create_script.push_back(sync::WriteResult{false, 500, "", "", 0});
        const int before = e.cloud.count("create");
        e.pass(sync::Trigger::Manual);
        REQUIRE(e.cloud.count("create") == before + 1);
        e.host.t += delay - 1;
        e.pass(sync::Trigger::Manual);
        REQUIRE(e.cloud.count("create") == before + 1);
        e.host.t += 1;
    }

    // A success clears the backoff for good.
    e.pass(sync::Trigger::Manual);
    e.add_remote("Flaky", "PF1", 5000, false);
    CHECK(e.mirror.ledger()->get(fkey("Flaky"))->state == "linked");

    e.set_local_hash("Flaky", "hf2");
    e.cloud.update_script.push_back(sync::WriteResult{false, 500, "", "", 0});
    e.pass();
    CHECK(e.cloud.count("update") == 1);
    e.host.t += 59;
    e.pass();
    CHECK(e.cloud.count("update") == 1);
    e.host.t += 1;
    e.pass();
    CHECK(e.cloud.count("update") == 2); // first backoff again: 60 seconds
}

TEST_CASE("A failed remote delete backs off", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("Gone"), linked("P9", 100, "h9")}, {fkey("Keep"), linked("P1", 100, "h1")}});
    e.add_remote("Gone", "P9", 100, false);
    e.add_remote("Keep", "P1", 100, false);
    e.add_local("Keep", "h1");
    e.cloud.remove_ok = false;
    e.pass();
    CHECK(e.cloud.count("remove P9") == 1);
    CHECK(e.mirror.ledger()->get(fkey("Gone")).has_value());
    e.pass(sync::Trigger::Manual);
    CHECK(e.cloud.count("remove") == 1);

    e.cloud.remove_ok = true;
    e.host.t += 60;
    e.pass(sync::Trigger::Manual);
    CHECK(e.cloud.count("remove") == 2);
    CHECK_FALSE(e.mirror.ledger()->get(fkey("Gone")).has_value());
    e.check_disk();
}

// ---------------------------------------------------------------------------
// Halts
// ---------------------------------------------------------------------------

TEST_CASE("A halting plan sets the flag, reports it and still runs the other actions", "[BblPresetSync]")
{
    Env e;
    local_delete_scenario(e);
    e.add_local("Fresh", "hf");
    e.pass();

    CHECK(e.mirror.status().last_result == "halted: mass_delete");
    CHECK(e.mirror.status().halted == "mass_delete");
    CHECK(e.mirror.ledger()->halted() == "mass_delete");
    CHECK(e.reload()->halted() == "mass_delete");
    REQUIRE(e.host.halts.size() == 1);
    CHECK(e.host.halts[0].kind == "mass_delete");
    CHECK(e.host.halts[0].local == std::vector<Key>{fkey("K1"), fkey("K2"), fkey("K3")});
    CHECK(e.host.halts[0].remote.empty());
    CHECK(e.store.deletes.empty());
    CHECK(e.cloud.count("create Fresh") == 1);

    const int snapshots = e.store.snapshot_calls;
    e.pass(sync::Trigger::Manual);
    CHECK(e.mirror.status().last_result == "halted: mass_delete");
    CHECK(e.store.snapshot_calls == snapshots);
    CHECK(e.host.halts.size() == 1);
}

TEST_CASE("Halt choices on halted local deletes", "[BblPresetSync]")
{
    Env e;
    local_delete_scenario(e);
    e.pass();
    REQUIRE(e.mirror.ledger()->halted() == "mass_delete");

    SECTION("later keeps the halt")
    {
        e.mirror.resolve_halt(sync::HaltChoice::Later);
        CHECK(e.mirror.ledger()->halted() == "mass_delete");
        e.pass();
        CHECK(e.mirror.status().last_result == "halted: mass_delete");
    }
    SECTION("proceed runs the deletes once")
    {
        e.mirror.resolve_halt(sync::HaltChoice::ProceedLocalDeletes);
        CHECK(e.mirror.ledger()->halted().empty());
        CHECK(e.reload()->halted().empty());
        e.pass(sync::Trigger::Manual);
        CHECK(e.mirror.status().last_result == "ok");
        REQUIRE(e.store.deletes.size() == 3);
        for (const auto& d : e.store.deletes) CHECK(d.backup_local);
        for (const char* n : {"K1", "K2", "K3"}) CHECK_FALSE(e.mirror.ledger()->get(fkey(n)).has_value());
        CHECK(e.host.halts.size() == 1);
        e.check_disk();
    }
    SECTION("proceed is one-shot")
    {
        e.store.outcome = {sync::OpResult::Postponed, "later", ""}; // the entries stay, the plan halts again
        e.mirror.resolve_halt(sync::HaltChoice::ProceedLocalDeletes);
        e.pass(sync::Trigger::Manual);
        REQUIRE(e.store.deletes.size() == 3);
        e.pass(sync::Trigger::Manual);
        CHECK(e.store.deletes.size() == 3);
        CHECK(e.mirror.status().last_result == "halted: mass_delete");
        CHECK(e.host.halts.size() == 2);
    }
    SECTION("keep local declines the presets")
    {
        e.mirror.resolve_halt(sync::HaltChoice::KeepLocal);
        for (const char* n : {"K1", "K2", "K3"}) {
            const auto entry = e.mirror.ledger()->get(fkey(n));
            REQUIRE(entry.has_value());
            CHECK(entry->state == "declined");
            CHECK(entry->setting_id.empty());
        }
        CHECK(e.mirror.ledger()->halted().empty());
        e.check_disk();
        e.pass(sync::Trigger::Manual);
        CHECK(e.mirror.status().last_result == "ok");
        CHECK(e.store.deletes.empty());
        CHECK(e.cloud.calls.empty());
    }
}

TEST_CASE("Halt choices on halted remote deletes", "[BblPresetSync]")
{
    Env e;
    remote_delete_scenario(e);
    e.pass();
    REQUIRE(e.mirror.ledger()->halted() == "mass_delete");
    REQUIRE(e.host.halts.size() == 1);
    CHECK(e.host.halts[0].remote == std::vector<Key>{fkey("R1"), fkey("R2"), fkey("R3")});
    CHECK(e.cloud.count("remove") == 0);

    SECTION("proceed deletes the rows")
    {
        e.mirror.resolve_halt(sync::HaltChoice::ProceedRemoteDeletes);
        e.pass();
        CHECK(e.cloud.count("remove") == 3);
        CHECK(e.mirror.status().last_result == "ok");
        for (const char* n : {"R1", "R2", "R3"}) CHECK_FALSE(e.mirror.ledger()->get(fkey(n)).has_value());
        e.check_disk();
    }
    SECTION("restore forgets the entries so the presets are pulled again")
    {
        e.mirror.resolve_halt(sync::HaltChoice::RestoreFromBambu);
        for (const char* n : {"R1", "R2", "R3"}) CHECK_FALSE(e.mirror.ledger()->get(fkey(n)).has_value());
        e.check_disk();
        e.store.defer = true;
        e.pass(sync::Trigger::Manual);
        CHECK(e.store.pulls.size() == 3);
        CHECK(e.cloud.count("remove") == 0);
    }
}

TEST_CASE("An empty list never deletes local presets", "[BblPresetSync]")
{
    Env e;
    std::map<Key, sync::LedgerEntry> entries;
    for (const char* n : {"K1", "K2"}) {
        auto entry          = linked(std::string("Q") + n, 100, "h");
        entry.absent_passes = 1;
        entry.absent_since  = e.host.t - 1000;
        entries[fkey(n)]    = entry;
        e.add_local(n, "h");
    }
    e.seed(entries);
    e.pass();
    CHECK(e.mirror.status().last_result == "halted: empty_list");
    CHECK(e.store.deletes.empty());
}

TEST_CASE("set_declined records a refused conversion", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    e.add_local("Refused", "hr");
    e.pass();
    REQUIRE(e.cloud.count("create Refused") == 1);

    e.mirror.set_declined(fkey("Refused"));
    const auto entry = e.mirror.ledger()->get(fkey("Refused"));
    REQUIRE(entry.has_value());
    CHECK(entry->state == "declined");
    CHECK(entry->setting_id.empty());
    e.check_disk();

    e.set_local_hash("Refused", "changed");
    e.pass(sync::Trigger::Manual);
    CHECK(e.cloud.count("create") == 1);
    CHECK(e.cloud.count("update") == 0);
}

// ---------------------------------------------------------------------------
// Cancellation, pacing, status
// ---------------------------------------------------------------------------

TEST_CASE("Cancellation stops the pass between actions", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    e.add_local("New1", "h1");
    e.add_local("New2", "h2");
    e.host.cancel_after = 1;
    e.pass();
    CHECK(e.cloud.count("create") == 1);
    CHECK(e.mirror.status().last_result == "stopped: cancelled");
    CHECK(e.mirror.ledger()->get(fkey("New1")).has_value());
    CHECK_FALSE(e.mirror.ledger()->get(fkey("New2")).has_value());
    e.check_disk();
}

TEST_CASE("A session change stops the pass between actions", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    e.add_local("New1", "h1");
    e.add_local("New2", "h2");
    e.host.session_changes_at_call = 3; // pass start, before action 1, before action 2
    e.pass();
    CHECK(e.cloud.count("create") == 1);
    CHECK(e.mirror.status().last_result == "stopped: session changed");
}

TEST_CASE("Network writes are spaced out", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    e.add_local("New1", "h1");
    e.pass();
    CHECK(e.host.sleeps.empty());

    e.add_local("New2", "h2");
    e.add_local("New3", "h3");
    e.pass();
    CHECK(e.host.sleeps == std::vector<int>{sync::WRITE_GAP_MS});
    CHECK(e.cloud.count("create") == 3);
}

TEST_CASE("Status reflects the last pass", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("K"), linked("P1", 100, "h1")}});
    e.add_local("K", "h1");
    e.add_remote("K", "P1", 100, false);
    e.add_local("New", "hn");
    e.host.t += 7;
    e.pass();

    const auto st = e.mirror.status();
    CHECK(st.last_pass == e.host.t);
    CHECK(st.last_result == "ok");
    CHECK(st.planned == std::vector<std::string>{"create filament/New"});
    CHECK(st.first_run_confirmed);
    CHECK(st.linked == 2);
    CHECK(st.halted.empty());
}

// ---------------------------------------------------------------------------
// Halt recovery, forced lists, forget, hash of children
// ---------------------------------------------------------------------------

TEST_CASE("A halt saved in the ledger is cleared on load and announced again by the next plan", "[BblPresetSync]")
{
    Env e;
    local_delete_scenario(e);
    {
        sync::Ledger l(e.ledger_path());
        REQUIRE(l.load());
        l.set_halted("mass_delete");
        REQUIRE(l.save());
    }
    e.pass();
    CHECK(e.mirror.status().last_result == "halted: mass_delete");
    REQUIRE(e.host.halts.size() == 1);
    CHECK(e.host.halts[0].local == std::vector<Key>{fkey("K1"), fkey("K2"), fkey("K3")});
    CHECK(e.store.deletes.empty());
    CHECK(e.reload()->halted() == "mass_delete"); // set again by the new plan
}

TEST_CASE("A saved halt is dropped on load when the plan no longer halts", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("K"), linked("P1", 100, "h1")}});
    {
        sync::Ledger l(e.ledger_path());
        REQUIRE(l.load());
        l.set_halted("mass_delete");
        REQUIRE(l.save());
    }
    e.add_local("K", "h1");
    e.add_remote("K", "P1", 100, false);
    e.pass();
    CHECK(e.mirror.status().last_result == "ok");
    CHECK(e.reload()->halted().empty());
    CHECK(e.host.halts.empty());
}

TEST_CASE("Forcing calls make the next tick list while the controls do not", "[BblPresetSync]")
{
    auto lists_after = [](const std::function<void(Env&)>& call) {
        Env e;
        e.seed({{fkey("K"), linked("P1", 100, "h1")}});
        e.add_local("K", "h1");
        e.add_remote("K", "P1", 100, false);
        e.pass();
        e.pass();
        call(e);
        e.pass();
        return e.cloud.list_calls;
    };
    CHECK(lists_after([](Env&) {}) == 1);
    CHECK(lists_after([](Env& e) { e.mirror.resolve_halt(sync::HaltChoice::Later); }) == 1);
    CHECK(lists_after([](Env& e) { e.mirror.confirm_first_run(false); }) == 1);
    CHECK(lists_after([](Env& e) { e.mirror.resolve_halt(sync::HaltChoice::KeepLocal); }) == 2);
    CHECK(lists_after([](Env& e) { e.mirror.resolve_halt(sync::HaltChoice::RestoreFromBambu); }) == 2);
    CHECK(lists_after([](Env& e) { e.mirror.resolve_halt(sync::HaltChoice::ProceedLocalDeletes); }) == 2);
    CHECK(lists_after([](Env& e) { e.mirror.resolve_halt(sync::HaltChoice::ProceedRemoteDeletes); }) == 2);
    CHECK(lists_after([](Env& e) { e.mirror.confirm_first_run(true); }) == 2);
    CHECK(lists_after([](Env& e) { e.mirror.set_declined(fkey("Other")); }) == 2);
    CHECK(lists_after([](Env& e) { e.mirror.forget(fkey("Other")); }) == 2);
}

TEST_CASE("forget erases the entry and the next pass treats the preset as new", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("Mine"), linked("P1", 100, "h1")}});
    e.add_local("Mine", "h1");
    e.add_remote("Mine", "P1", 100, false);
    e.pass();
    REQUIRE(e.cloud.calls.empty());

    SECTION("no row left: it is created again")
    {
        e.cloud.rows.clear();
        e.mirror.forget(fkey("Mine"));
        CHECK_FALSE(e.mirror.ledger()->get(fkey("Mine")).has_value());
        CHECK_FALSE(e.reload()->get(fkey("Mine")).has_value());
        e.pass();
        CHECK(e.cloud.count("create Mine") == 1);
    }
    SECTION("an identical row is still there: it is paired")
    {
        e.cloud.rows.clear();
        e.cloud.rows.push_back({PresetType::Filament, "Mine", "P2", 400});
        e.cloud.bodies["Mine"] = valid_body("P2", "x");
        e.store.snap->items[0].upload = valid_body("P2", "x");
        e.store.snap->items[0].hash   = sync::upload_hash(valid_body("P2", "x"));
        e.mirror.forget(fkey("Mine"));
        e.pass();
        const auto entry = e.mirror.ledger()->get(fkey("Mine"));
        REQUIRE(entry.has_value());
        CHECK(entry->setting_id == "P2");
        CHECK(e.cloud.calls.empty());
    }
    SECTION("it clears the backoff of the key")
    {
        e.set_local_hash("Mine", "h2");
        e.cloud.update_script.push_back(sync::WriteResult{false, 500, "", "", 0});
        e.pass();
        CHECK(e.cloud.count("update") == 1);
        e.mirror.forget(fkey("Mine"));
        e.cloud.rows.clear();
        e.pass(); // no 60 second wait
        CHECK(e.cloud.count("create Mine") == 1);
    }
}

TEST_CASE("The upload hash of a child ignores filament_id and a root keeps it", "[BblPresetSync]")
{
    const sync::ValueMap child   = {{"inherits", "BBL PLA"}, {"base_id", "f1"}, {"filament_cost", "3"}};
    sync::ValueMap       with_id = child;
    with_id["filament_id"]       = "GFA00";
    CHECK(sync::upload_hash(child) == sync::upload_hash(with_id));

    const sync::ValueMap root_a = {{"filament_cost", "3"}, {"filament_id", "P1111111"}};
    sync::ValueMap       root_b = root_a;
    root_b["filament_id"]       = "P2222222";
    CHECK(sync::upload_hash(root_a) != sync::upload_hash(root_b));

    sync::ValueMap empty_a = root_a;
    empty_a["inherits"]    = "";
    sync::ValueMap empty_b = root_b;
    empty_b["inherits"]    = "";
    CHECK(sync::upload_hash(empty_a) != sync::upload_hash(empty_b));
}

// ---------------------------------------------------------------------------
// Edits exchanged against two bases
// ---------------------------------------------------------------------------

namespace {

// A valid Bambu body (a child of "BBL PLA") with extra settings.
sync::ValueMap body_with(const std::string& id, const sync::ValueMap& extra)
{
    sync::ValueMap b = valid_body(id, "1");
    for (const auto& [k, v] : extra) b[k] = v;
    return b;
}

// OrcaSlicer's representation of a body: three keys Studio omits and a value formatted its own way.
sync::ValueMap orca_repr(const sync::ValueMap& body)
{
    sync::ValueMap m = body;
    m["filament_flow_ratio"]          = "0.98";
    m["filament_max_volumetric_speed"] = "15";
    m["default_filament_colour"]      = "";
    m["filament_cost"]                = "1.00";
    return m;
}

void put_bases(Env& e, const std::string& id, const sync::ValueMap& remote, const sync::ValueMap& local)
{
    e.base_store().put(id, sync::Bases{remote, local});
}

// A linked preset "Mine" (id P1) whose bases are `remote` and `local`; the snapshot holds `local`.
void seed_linked(Env& e, const sync::ValueMap& remote, const sync::ValueMap& local, long long time = 100)
{
    sync::LedgerEntry le;
    le.setting_id  = "P1";
    le.update_time = time;
    le.synced_hash = sync::upload_hash(local);
    e.seed({{fkey("Mine"), le}});
    put_bases(e, "P1", remote, local);
    e.add_local_body("Mine", local);
}

void set_upload(Env& e, const std::string& name, const sync::ValueMap& upload)
{
    for (auto& i : e.store.snap->items)
        if (i.key == fkey(name)) {
            i.upload = upload;
            i.hash   = sync::upload_hash(upload);
        }
}

void set_mtime(Env& e, const std::string& name, long long mtime)
{
    for (auto& i : e.store.snap->items)
        if (i.key == fkey(name)) i.mtime = mtime;
}

void remote_row(Env& e, const std::string& name, const std::string& id, long long time, const sync::ValueMap& body)
{
    e.cloud.rows.clear();
    e.cloud.rows.push_back({PresetType::Filament, name, id, time});
    e.cloud.bodies[name] = body;
}

} // namespace

TEST_CASE("OrcaSlicer's own representation of a downloaded preset is not an edit", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    const auto rem = valid_body("P1", "1");
    const auto loc = orca_repr(rem);
    remote_row(e, "Mine", "P1", 200, rem);
    e.store.outcome.new_upload = loc;

    e.pass(sync::Trigger::Manual);
    REQUIRE(e.store.pulls.size() == 1);
    const auto bases = e.base_store().get("P1");
    REQUIRE(bases.has_value());
    CHECK(bases->remote == rem);
    CHECK(bases->local == loc);
    CHECK(e.mirror.ledger()->get(fkey("Mine"))->synced_hash == sync::upload_hash(loc));

    e.add_local_body("Mine", loc);
    for (int i = 0; i < 3; ++i) {
        e.pass(sync::Trigger::Manual);
        CHECK(e.cloud.calls.empty());
        CHECK(e.store.pulls.size() == 1);
    }

    // The remote body changes one key: the pull applies that key only.
    auto rem2             = rem;
    rem2["filament_density"] = "2";
    remote_row(e, "Mine", "P1", 300, rem2);
    auto loc2                = loc;
    loc2["filament_density"] = "2";
    e.store.outcome.new_upload = loc2;
    e.pass(sync::Trigger::Manual);
    REQUIRE(e.store.pulls.size() == 2);
    CHECK(e.store.pulls[1].body.at("filament_density") == "2");
    CHECK(e.store.pulls[1].body.at("filament_cost") == "1.00"); // the local representation stays
    set_upload(e, "Mine", loc2);
    for (int i = 0; i < 3; ++i) {
        e.pass(sync::Trigger::Manual);
        CHECK(e.cloud.calls.empty());
        CHECK(e.store.pulls.size() == 2);
    }

    // A real local edit of one key goes up alone, on top of the Bambu copy.
    auto loc3                = loc2;
    loc3["filament_density"] = "3";
    set_upload(e, "Mine", loc3);
    e.pass(sync::Trigger::Manual);
    REQUIRE(e.cloud.updated_values.size() == 1);
    auto expected                = rem2;
    expected.erase("setting_id"); // the envelope never goes back up
    expected["filament_density"] = "3";
    CHECK(e.cloud.updated_values[0] == expected); // none of the extra keys, no reformatted value
    for (const char* k : {"update_time", "updated_time", "setting_id", "user_id", "name"}) CHECK(e.cloud.updated_values[0].count(k) == 0);
    e.pass(sync::Trigger::Manual);
    CHECK(e.cloud.count("update P1") == 1);
}

TEST_CASE("A setting Studio dropped is kept locally and uploaded only when edited", "[BblPresetSync]")
{
    Env  e;
    auto rem_old = body_with("P1", {{"filament_flow_ratio", "0.9"}}); // OrcaSlicer-only key
    seed_linked(e, rem_old, rem_old);
    auto rem_new             = valid_body("P1", "1");
    rem_new["filament_cost"] = "2"; // Studio saved: new cost, flow ratio gone
    remote_row(e, "Mine", "P1", 200, rem_new);
    auto after             = rem_old;
    after["filament_cost"] = "2";
    e.store.outcome.new_upload = after;

    e.pass(sync::Trigger::Manual);
    REQUIRE(e.store.pulls.size() == 1);
    CHECK(e.store.pulls[0].body.at("filament_flow_ratio") == "0.9");
    CHECK(e.store.pulls[0].body.at("filament_cost") == "2");
    CHECK(has_note(e.mirror.status(), "kept 1 OrcaSlicer-only settings for filament/Mine"));
    CHECK(e.base_store().get("P1")->remote == rem_new);

    set_upload(e, "Mine", after);
    for (int i = 0; i < 3; ++i) {
        e.pass(sync::Trigger::Manual);
        CHECK(e.cloud.calls.empty());
    }

    auto edited                   = after;
    edited["filament_flow_ratio"] = "0.8";
    set_upload(e, "Mine", edited);
    e.pass(sync::Trigger::Manual);
    REQUIRE(e.cloud.updated_values.size() == 1);
    CHECK(e.cloud.updated_values[0].at("filament_flow_ratio") == "0.8");
    CHECK(e.cloud.updated_values[0].at("filament_cost") == "2");
}

TEST_CASE("A setting Studio removed is removed from the pulled body", "[BblPresetSync]")
{
    Env  e;
    auto base = body_with("P1", {{"filament_density", "1.3"}});
    seed_linked(e, base, base);
    remote_row(e, "Mine", "P1", 200, valid_body("P1", "1"));
    e.pass(sync::Trigger::Manual);
    REQUIRE(e.store.pulls.size() == 1);
    CHECK(e.store.pulls[0].body.count("filament_density") == 0);
    CHECK(e.store.pulls[0].body.at("filament_cost") == "1");
    CHECK(e.store.pulls[0].body.at("setting_id") == "P1");
}

TEST_CASE("Edits to different settings on both sides merge once, then upload only the local one", "[BblPresetSync]")
{
    Env  e;
    auto base = body_with("P1", {{"filament_density", "1.0"}});
    seed_linked(e, base, base);
    auto local             = base;
    local["filament_cost"] = "5";
    set_upload(e, "Mine", local);
    auto remote                = base;
    remote["filament_density"] = "2.0";
    remote_row(e, "Mine", "P1", 500, remote);
    auto merged                = local;
    merged["filament_density"] = "2.0";
    e.store.outcome.new_upload = merged;

    e.pass(sync::Trigger::Manual);
    REQUIRE(e.store.pulls.size() == 1);
    CHECK(e.store.pulls[0].backup_local);
    CHECK(e.store.pulls[0].body.at("filament_cost") == "5");
    CHECK(e.store.pulls[0].body.at("filament_density") == "2.0");
    CHECK(e.cloud.calls.empty());
    CHECK(fs::exists(sync::Ledger::backup_dir(e.dir.path()) / "19700112-134640_filament_Mine_remote.json"));
    CHECK(has_note(e.mirror.status(), "1 local edits of filament/Mine will be uploaded"));
    const auto bases = e.base_store().get("P1");
    REQUIRE(bases.has_value());
    CHECK(bases->remote == remote);
    CHECK(bases->local.at("filament_cost") == "1"); // the pending edit is still a difference from the local base
    CHECK(bases->local.at("filament_density") == "2.0");

    set_upload(e, "Mine", merged);
    e.pass(sync::Trigger::Manual);
    REQUIRE(e.cloud.updated_values.size() == 1);
    auto expected             = remote;
    expected.erase("setting_id"); // the envelope never goes back up
    expected["filament_cost"] = "5";
    CHECK(e.cloud.updated_values[0] == expected);
    e.pass(sync::Trigger::Manual);
    e.pass(sync::Trigger::Manual);
    CHECK(e.cloud.count("update P1") == 1);
    CHECK(e.store.pulls.size() == 1);
}

TEST_CASE("A conflict on one setting is settled by the newer side", "[BblPresetSync]")
{
    Env  e;
    auto base = valid_body("P1", "1");
    seed_linked(e, base, base);
    auto local             = base;
    local["filament_cost"] = "L";
    set_upload(e, "Mine", local);
    auto remote             = base;
    remote["filament_cost"] = "R";

    SECTION("local newer: the local value stays and goes up")
    {
        remote_row(e, "Mine", "P1", 500, remote);
        set_mtime(e, "Mine", e.host.t);
        e.store.outcome.new_upload = local;
        e.pass(sync::Trigger::Manual);
        REQUIRE(e.store.pulls.size() == 1);
        CHECK(e.store.pulls[0].body.at("filament_cost") == "L");
        e.pass(sync::Trigger::Manual);
        REQUIRE(e.cloud.updated_values.size() == 1);
        CHECK(e.cloud.updated_values[0].at("filament_cost") == "L");
    }
    SECTION("remote newer: the remote value is taken and nothing goes up")
    {
        remote_row(e, "Mine", "P1", e.host.t + 1000, remote);
        set_mtime(e, "Mine", 0);
        auto after             = local;
        after["filament_cost"] = "R";
        e.store.outcome.new_upload = after;
        e.pass(sync::Trigger::Manual);
        REQUIRE(e.store.pulls.size() == 1);
        CHECK(e.store.pulls[0].body.at("filament_cost") == "R");
        set_upload(e, "Mine", after);
        e.pass(sync::Trigger::Manual);
        CHECK(e.cloud.calls.empty());
    }
}

TEST_CASE("Same-named presets meeting for the first time are paired or merged without uploading local-only settings", "[BblPresetSync]")
{
    Env e;
    e.seed({});

    SECTION("equal: a link that stores both bases")
    {
        const auto rem = valid_body("P1", "1");
        e.add_local_body("Mine", rem);
        remote_row(e, "Mine", "P1", 100, rem);
        e.pass(sync::Trigger::Manual);
        const auto entry = e.mirror.ledger()->get(fkey("Mine"));
        REQUIRE(entry.has_value());
        CHECK(entry->setting_id == "P1");
        CHECK(entry->synced_hash == sync::upload_hash(rem));
        const auto bases = e.base_store().get("P1");
        REQUIRE(bases.has_value());
        CHECK(bases->remote == rem);
        CHECK(bases->local == rem);
        CHECK(e.cloud.calls.empty());
        CHECK(e.store.pulls.empty());
    }
    SECTION("different, remote newer")
    {
        auto local                 = valid_body("P1", "L");
        local["filament_density"]  = "9"; // only local
        e.add_local_body("Mine", local);
        set_mtime(e, "Mine", 0);
        auto remote                = valid_body("P1", "R");
        remote["filament_vendor"]  = "V"; // only on Bambu
        remote_row(e, "Mine", "P1", e.host.t + 1000, remote);
        auto after                 = remote;
        after["filament_density"]  = "9";
        e.store.outcome.new_upload = after;

        e.pass(sync::Trigger::Manual);
        REQUIRE(e.store.pulls.size() == 1);
        CHECK(e.store.pulls[0].backup_local);
        CHECK(e.store.pulls[0].body.at("filament_cost") == "R");
        CHECK(e.store.pulls[0].body.at("filament_density") == "9");
        CHECK(e.store.pulls[0].body.at("filament_vendor") == "V");
        set_upload(e, "Mine", after);
        for (int i = 0; i < 3; ++i) {
            e.pass(sync::Trigger::Manual);
            CHECK(e.cloud.calls.empty());
        }
    }
    SECTION("different, local newer: only the disputed key goes up")
    {
        auto local                 = valid_body("P1", "L");
        local["filament_density"]  = "9";
        e.add_local_body("Mine", local);
        set_mtime(e, "Mine", e.host.t);
        auto remote                = valid_body("P1", "R");
        remote["filament_vendor"]  = "V";
        remote_row(e, "Mine", "P1", 500, remote);
        auto after                 = local;
        after["filament_vendor"]   = "V";
        e.store.outcome.new_upload = after;

        e.pass(sync::Trigger::Manual);
        REQUIRE(e.store.pulls.size() == 1);
        CHECK(e.store.pulls[0].body.at("filament_cost") == "L");
        set_upload(e, "Mine", after);
        e.pass(sync::Trigger::Manual);
        REQUIRE(e.cloud.updated_values.size() == 1);
        const auto& up = e.cloud.updated_values[0];
        CHECK(up.at("filament_cost") == "L");
        CHECK(up.at("filament_vendor") == "V");
        CHECK(up.count("filament_density") == 0); // only local: never uploaded
        e.pass(sync::Trigger::Manual);
        CHECK(e.cloud.count("update P1") == 1);
    }
}

TEST_CASE("An update without bases sends the whole local map", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("Mine"), linked("P1", 100, "h1")}});
    e.add_local("Mine", "h2");
    e.add_remote("Mine", "P1", 100, false);
    e.pass(sync::Trigger::Manual);
    REQUIRE(e.cloud.updated_values.size() == 1);
    CHECK(e.cloud.updated_values[0] == e.store.snap->items[0].upload);
    const auto bases = e.base_store().get("P1");
    REQUIRE(bases.has_value());
    CHECK(bases->local == e.store.snap->items[0].upload);
}

TEST_CASE("A linked preset that is not eligible still syncs both ways", "[BblPresetSync]")
{
    Env  e;
    auto base = body_with("P1", {{"studio_only", "S"}});
    auto local = valid_body("P1", "1");
    seed_linked(e, base, local);
    e.store.snap->items[0].cls.eligibility = sync::Eligibility::Ineligible;
    e.store.snap->items[0].cls.reason      = "parent_not_bambu";

    SECTION("a local edit is uploaded and a Studio-only key of the copy survives")
    {
        e.add_remote("Mine", "P1", 100, false);
        auto changed             = local;
        changed["filament_cost"] = "8";
        set_upload(e, "Mine", changed);
        e.pass(sync::Trigger::Manual);
        REQUIRE(e.cloud.updated_values.size() == 1);
        CHECK(e.cloud.updated_values[0].at("filament_cost") == "8");
        CHECK(e.cloud.updated_values[0].at("studio_only") == "S");
    }
    SECTION("a remote change is pulled")
    {
        auto remote             = base;
        remote["filament_cost"] = "3";
        remote_row(e, "Mine", "P1", 300, remote);
        e.pass(sync::Trigger::Manual);
        REQUIRE(e.store.pulls.size() == 1);
        CHECK(e.store.pulls[0].body.at("filament_cost") == "3");
    }
    SECTION("an unlinked ineligible preset is left alone")
    {
        Env other;
        other.seed({});
        other.add_local("Foreign", "hf");
        other.store.snap->items[0].cls.eligibility = sync::Eligibility::Ineligible;
        other.pass(sync::Trigger::Manual);
        CHECK(other.cloud.calls.empty());
    }
}

TEST_CASE("Bodies already downloaded are not asked for again on an unconfirmed first run", "[BblPresetSync]")
{
    Env e;
    e.cloud.drain_wanted_only = true;
    e.cloud.rows.push_back({PresetType::Filament, "Down", "PD", 100});
    e.cloud.bodies["Down"] = valid_body("PD", "d");

    e.pass(sync::Trigger::Manual);
    CHECK(e.cloud.wanted == std::vector<Key>{fkey("Down")});
    CHECK(e.mirror.status().planned == std::vector<std::string>{"pull filament/Down"});
    CHECK(e.mirror.status().last_result == "dry run");

    e.pass(sync::Trigger::Manual);
    CHECK(e.cloud.wanted.empty());
    CHECK(e.mirror.status().planned == std::vector<std::string>{"pull filament/Down"});
    CHECK(e.mirror.status().last_result == "dry run");

    // A newer row is downloaded again.
    e.cloud.rows[0].update_time = 150;
    e.pass(sync::Trigger::Manual);
    CHECK(e.cloud.wanted == std::vector<Key>{fkey("Down")});

    // Confirmed: the cached body is used for the real pull.
    e.mirror.confirm_first_run(true);
    e.pass(sync::Trigger::Manual);
    CHECK(e.cloud.wanted.empty());
    REQUIRE(e.store.pulls.size() == 1);
    CHECK(e.store.pulls[0].body.at("filament_cost") == "d");
}

TEST_CASE("The bases go away with their entry", "[BblPresetSync]")
{
    SECTION("remote delete")
    {
        Env  e;
        auto base = body_with("P1", {});
        e.seed({{fkey("Mine"), linked("P1", 100, "h1")}});
        put_bases(e, "P1", base, base);
        e.cloud.rows.push_back({PresetType::Filament, "Mine", "P1", 100});
        e.cloud.bodies["Mine"] = base;
        e.pass(sync::Trigger::Manual);
        CHECK(e.cloud.count("remove P1") == 1);
        CHECK_FALSE(e.base_store().get("P1").has_value());
    }
    SECTION("drop entry")
    {
        Env  e;
        auto base = body_with("P1", {});
        e.seed({{fkey("Mine"), linked("P1", 100, "h1")}});
        put_bases(e, "P1", base, base);
        e.pass(sync::Trigger::Manual); // gone on both sides
        CHECK_FALSE(e.mirror.ledger()->get(fkey("Mine")).has_value());
        CHECK_FALSE(e.base_store().get("P1").has_value());
    }
    SECTION("local delete")
    {
        Env  e;
        auto base  = body_with("P1", {});
        auto entry = linked("P1", 100, "h1");
        entry.absent_passes = 1;
        entry.absent_since  = 1000000 - 1000;
        e.seed({{fkey("Gone"), entry}, {fkey("Keep"), linked("P2", 100, "h2")}});
        put_bases(e, "P1", base, base);
        put_bases(e, "P2", base, base);
        e.add_local("Gone", "h1");
        e.add_local("Keep", "h2");
        e.cloud.rows.push_back({PresetType::Filament, "Keep", "P2", 100});
        e.pass(sync::Trigger::Manual);
        REQUIRE(e.store.deletes.size() == 1);
        CHECK_FALSE(e.base_store().get("P1").has_value());
        CHECK(e.base_store().get("P2").has_value());
    }
    SECTION("forget")
    {
        Env  e;
        auto base = body_with("P1", {});
        e.seed({{fkey("Mine"), linked("P1", 100, "h1")}});
        put_bases(e, "P1", base, base);
        e.add_local("Mine", "h1");
        e.pass(); // loads the session
        e.mirror.forget(fkey("Mine"));
        CHECK_FALSE(e.base_store().get("P1").has_value());
    }
}

TEST_CASE("A lost bases file merges as a first meeting and keeps both sides", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("Mine"), linked("P1", 100, "h1")}});
    e.add_local("Mine", "h1"); // unchanged since the last sync
    e.cloud.rows.push_back({PresetType::Filament, "Mine", "P1", 200});
    e.cloud.bodies["Mine"] = body_with("P1", {{"filament_density", "2"}});
    e.cloud.bodies["Mine"].erase("filament_cost");

    e.pass(sync::Trigger::Manual);
    REQUIRE(e.store.pulls.size() == 1);
    CHECK(e.store.pulls[0].body.at("filament_cost") == "h1");   // local value survives
    CHECK(e.store.pulls[0].body.at("filament_density") == "2"); // remote value arrives
}

TEST_CASE("set_studio_keys changes how the next pull treats a setting the remote removed", "[BblPresetSync]")
{
    Env  e;
    auto base = body_with("P1", {{"filament_density", "1.3"}});
    seed_linked(e, base, base);
    remote_row(e, "Mine", "P1", 200, valid_body("P1", "1")); // Studio removed filament_density

    SECTION("a set that knows the key: it is dropped")
    {
        e.mirror.set_studio_keys(sync::StudioKeys({"filament_cost", "filament_density"}), "BambuStudio vTest");
        e.pass(sync::Trigger::Manual);
        REQUIRE(e.store.pulls.size() == 1);
        CHECK(e.store.pulls[0].body.count("filament_density") == 0);
        CHECK(e.mirror.status().studio_keys_source == "BambuStudio vTest");
        CHECK(e.mirror.studio_key_count() == 2);
    }
    SECTION("a set that does not know it: the local value is kept")
    {
        e.mirror.set_studio_keys(sync::StudioKeys({"filament_cost"}), "BambuStudio vOther");
        e.pass(sync::Trigger::Manual);
        REQUIRE(e.store.pulls.size() == 1);
        CHECK(e.store.pulls[0].body.at("filament_density") == "1.3");
        CHECK(e.mirror.status().studio_keys_source == "BambuStudio vOther");
    }
    SECTION("without a set the bundled test file is used")
    {
        e.pass(sync::Trigger::Manual);
        CHECK(e.mirror.status().studio_keys_source == "bundled 0");
    }
    SECTION("a later set takes effect on the next pass")
    {
        e.pass(sync::Trigger::Manual);
        e.mirror.set_studio_keys(sync::StudioKeys({"filament_cost"}), "BambuStudio vLater");
        CHECK(e.mirror.status().studio_keys_source == "bundled 0");
        e.pass(sync::Trigger::Manual);
        CHECK(e.mirror.status().studio_keys_source == "BambuStudio vLater");
    }
}

// ---------------------------------------------------------------------------
// Batching
// ---------------------------------------------------------------------------

TEST_CASE("A pass brackets its local changes in one batch", "[BblPresetSync]")
{
    Env e;
    e.seed({{fkey("Gone"), [] {
                 auto x          = linked("P2", 100, "hg");
                 x.absent_passes = 1;
                 x.absent_since  = 1000000 - 1000;
                 return x;
             }()}});
    e.add_local("Gone", "hg");
    e.add_remote("Remote", "P5", 200);
    e.add_remote("Remote2", "P6", 200);

    SECTION("begin, the ops in order, end")
    {
        e.pass();
        REQUIRE(e.store.order.size() >= 4);
        CHECK(e.store.order.front() == "begin");
        CHECK(e.store.order.back() == "end");
        CHECK(std::count(e.store.order.begin(), e.store.order.end(), "begin") == 1);
        CHECK(std::count(e.store.order.begin(), e.store.order.end(), "end") == 1);
        CHECK(std::count(e.store.order.begin(), e.store.order.end(), "pull") == 2);
        CHECK(std::count(e.store.order.begin(), e.store.order.end(), "delete") == 1);
    }
    SECTION("an early stop still ends the batch")
    {
        e.host.cancel_after = 1; // the second action sees a cancelled sync
        e.pass();
        REQUIRE(e.store.order.size() >= 2);
        CHECK(e.store.order.front() == "begin");
        CHECK(e.store.order.back() == "end");
        CHECK(std::count(e.store.order.begin(), e.store.order.end(), "end") == 1);
        CHECK(e.mirror.status().last_result == "stopped: cancelled");
    }
    SECTION("a pass that does not execute does not open a batch")
    {
        e.pass(); // everything done
        e.store.order.clear();
        Env idle;
        idle.seed({});
        idle.pass();
        CHECK(idle.store.order.size() <= 2); // at most an empty bracket
    }
}

TEST_CASE("The mirror no longer notifies about unresolvable presets itself", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    e.add_remote("Remote", "P5", 200);
    e.store.outcome = {sync::OpResult::Unresolvable, "needs a parent", ""};
    e.pass();
    CHECK(e.mirror.ledger()->get(fkey("Remote"))->state == "unresolvable");
    CHECK(e.host.notified("unresolvable") == 0);
}

TEST_CASE("The mirror reports the loaded Studio keys, the cached bodies and the base store", "[BblPresetSync]")
{
    Env e;
    e.seed({});
    CHECK(e.mirror.studio_key_count() == 0);
    CHECK(e.mirror.cached_body_count() == 0);
    CHECK_FALSE(e.mirror.base_store());
    e.add_remote("Remote", "P5", 200);
    e.pass();
    CHECK(e.mirror.studio_key_count() == Env::studio_key_list().size());
    CHECK(e.mirror.cached_body_count() == 1);
    REQUIRE(e.mirror.base_store());
    CHECK(e.mirror.base_store()->has("P5")); // the pulled body became the base
}
