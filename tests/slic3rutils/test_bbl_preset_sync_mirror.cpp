#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/BblPresetSync/Eligibility.hpp"
#include "slic3r/Utils/BblPresetSync/Snapshot.hpp"
#include "slic3r/Utils/BblPresetSync/Types.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
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
    CHECK(foreign->upload.empty());
    CHECK(foreign->hash.empty());
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
