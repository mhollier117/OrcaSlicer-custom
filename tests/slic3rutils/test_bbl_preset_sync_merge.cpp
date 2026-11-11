#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/BblPresetSync/BaseStore.hpp"
#include "slic3r/Utils/BblPresetSync/Hash.hpp"
#include "slic3r/Utils/BblPresetSync/Keys.hpp"
#include "slic3r/Utils/BblPresetSync/Merge.hpp"
#include "slic3r/Utils/BblPresetSync/Types.hpp"

#include "test_utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <string>
#include <thread>
#include <vector>

namespace sync = Slic3r::BblPresetSync;
namespace fs   = boost::filesystem;
using sync::ValueMap;

namespace {

// OrcaSlicer knows everything except "studio_only"; Studio knows everything except "orca_only".
bool orca_k(const std::string& k) { return k != "studio_only"; }
bool studio_k(const std::string& k) { return k != "orca_only"; }

sync::MergeResult merge(const ValueMap& l, const ValueMap& r, const ValueMap& b, bool local_newer = false)
{
    return sync::merge_bodies(l, r, b, local_newer, orca_k, studio_k);
}

void write_file(const fs::path& file, const std::string& text)
{
    fs::create_directories(file.parent_path());
    boost::nowide::ofstream out(file.string(), std::ios::binary);
    out << text;
}

} // namespace

// ---------------------------------------------------------------------------
// Keys
// ---------------------------------------------------------------------------

TEST_CASE("orca_knows follows the option list of each preset type", "[BblPresetSync]")
{
    using sync::PresetType;
    CHECK(sync::orca_knows(PresetType::Filament, "filament_cost"));
    CHECK_FALSE(sync::orca_knows(PresetType::Print, "filament_cost"));
    CHECK(sync::orca_knows(PresetType::Print, "layer_height"));
    CHECK_FALSE(sync::orca_knows(PresetType::Filament, "layer_height"));
    CHECK(sync::orca_knows(PresetType::Printer, "nozzle_diameter"));
    CHECK_FALSE(sync::orca_knows(PresetType::Printer, "no_such_setting"));
    for (const PresetType t : {PresetType::Print, PresetType::Filament, PresetType::Printer}) {
        CHECK(sync::orca_knows(t, "inherits"));
        CHECK(sync::orca_knows(t, "base_id"));
        CHECK(sync::orca_knows(t, "filament_id"));
        CHECK(sync::orca_knows(t, "filament_settings_id"));
        CHECK(sync::orca_knows(t, "print_settings_id"));
        CHECK(sync::orca_knows(t, "printer_settings_id"));
        CHECK(sync::orca_knows(t, "compatible_printers"));
        CHECK(sync::orca_knows(t, "compatible_printers_condition"));
        CHECK(sync::orca_knows(t, "compatible_prints"));
        CHECK(sync::orca_knows(t, "compatible_prints_condition"));
    }
}

TEST_CASE("StudioKeys loads the key list and treats a bad file as empty", "[BblPresetSync]")
{
    ScopedTemporaryDir temp{"orca-bblkeys"};
    const fs::path     file = temp.path() / "keys.json";

    SECTION("a valid file")
    {
        write_file(file, R"({"source":"x","studio_version":"1","keys":["layer_height","filament_cost"]})");
        const auto keys = sync::StudioKeys::load(file);
        CHECK_FALSE(keys.empty());
        CHECK(keys.size() == 2);
        CHECK(keys.knows("layer_height"));
        CHECK_FALSE(keys.knows("filament_flow_ratio"));
    }
    SECTION("a missing file")
    {
        const auto keys = sync::StudioKeys::load(temp.path() / "nope.json");
        CHECK(keys.empty());
        CHECK_FALSE(keys.knows("layer_height"));
    }
    SECTION("an invalid file")
    {
        write_file(file, "not json");
        CHECK(sync::StudioKeys::load(file).empty());
        write_file(file, R"({"keys": 5})");
        CHECK(sync::StudioKeys::load(file).empty());
    }
    SECTION("inherits, base_id and filament_id always count as known")
    {
        const sync::StudioKeys keys;
        CHECK(keys.knows("inherits"));
        CHECK(keys.knows("base_id"));
        CHECK(keys.knows("filament_id"));
        CHECK_FALSE(keys.knows("filament_cost"));
    }
    SECTION("the shipped list loads")
    {
        const fs::path shipped = fs::path(__FILE__).parent_path().parent_path().parent_path() / "resources" / "printers" / "bambu_studio_option_keys.json";
        if (fs::exists(shipped)) {
            const auto keys = sync::StudioKeys::load(shipped);
            CHECK(keys.size() > 700);
            CHECK(keys.knows("layer_height"));
        }
    }
}

// ---------------------------------------------------------------------------
// merge_bodies
// ---------------------------------------------------------------------------

TEST_CASE("merge_bodies takes each setting from the side that changed it", "[BblPresetSync]")
{
    SECTION("unchanged")
    {
        const auto m = merge({{"a", "1"}}, {{"a", "1"}}, {{"a", "1"}});
        CHECK(m.merged == ValueMap{{"a", "1"}});
        CHECK(m.taken_remote == 0);
        CHECK(m.kept_local == 0);
        CHECK(m.conflicts == 0);
        CHECK_FALSE(m.needs_upload);
    }
    SECTION("remote-only change")
    {
        const auto m = merge({{"a", "1"}}, {{"a", "2"}}, {{"a", "1"}});
        CHECK(m.merged == ValueMap{{"a", "2"}});
        CHECK(m.taken_remote == 1);
        CHECK(m.conflicts == 0);
        CHECK_FALSE(m.needs_upload);
    }
    SECTION("local-only change")
    {
        const auto m = merge({{"a", "2"}}, {{"a", "1"}}, {{"a", "1"}});
        CHECK(m.merged == ValueMap{{"a", "2"}});
        CHECK(m.taken_remote == 0);
        CHECK(m.conflicts == 0);
        CHECK(m.needs_upload);
    }
    SECTION("both changed to the same value")
    {
        const auto m = merge({{"a", "2"}}, {{"a", "2"}}, {{"a", "1"}});
        CHECK(m.merged == ValueMap{{"a", "2"}});
        CHECK(m.conflicts == 0);
        CHECK_FALSE(m.needs_upload);
    }
    SECTION("both changed to different values, the local side newer")
    {
        const auto m = merge({{"a", "L"}}, {{"a", "R"}}, {{"a", "1"}}, true);
        CHECK(m.merged == ValueMap{{"a", "L"}});
        CHECK(m.conflicts == 1);
        CHECK(m.kept_local == 1);
        CHECK(m.taken_remote == 0);
        CHECK(m.needs_upload);
    }
    SECTION("both changed to different values, the remote side newer")
    {
        const auto m = merge({{"a", "L"}}, {{"a", "R"}}, {{"a", "1"}}, false);
        CHECK(m.merged == ValueMap{{"a", "R"}});
        CHECK(m.conflicts == 1);
        CHECK(m.kept_local == 0);
        CHECK(m.taken_remote == 1);
        CHECK_FALSE(m.needs_upload);
    }
    SECTION("remote removed a key Studio knows: the preset inherits it again")
    {
        const auto m = merge({{"a", "1"}, {"b", "x"}}, {{"b", "x"}}, {{"a", "1"}, {"b", "x"}});
        CHECK(m.merged == ValueMap{{"b", "x"}});
        CHECK(m.taken_remote == 1);
        CHECK(m.kept_unknown_to_studio.empty());
        CHECK_FALSE(m.needs_upload);
    }
    SECTION("remote dropped a key Studio does not know: the local value stays and is listed")
    {
        const auto m = merge({{"orca_only", "7"}, {"b", "x"}}, {{"b", "x"}}, {{"orca_only", "7"}, {"b", "x"}});
        CHECK(m.merged == ValueMap{{"orca_only", "7"}, {"b", "x"}});
        CHECK(m.kept_local == 1);
        CHECK(m.kept_unknown_to_studio == std::vector<std::string>{"orca_only"});
        CHECK_FALSE(m.needs_upload); // Bambu cannot hold it
    }
    SECTION("a key only the remote has and OrcaSlicer does not know stays out")
    {
        const auto m = merge({}, {{"studio_only", "9"}, {"a", "1"}}, {});
        CHECK(m.merged == ValueMap{{"a", "1"}});
        CHECK_FALSE(m.needs_upload);
    }
    SECTION("an empty base (pairing) unites both sides, the newer side wins overlaps")
    {
        const ValueMap l{{"a", "1"}, {"b", "2"}}, r{{"a", "3"}, {"c", "4"}};
        const auto     remote_wins = merge(l, r, {}, false);
        CHECK(remote_wins.merged == ValueMap{{"a", "3"}, {"b", "2"}, {"c", "4"}});
        CHECK(remote_wins.conflicts == 1);
        const auto local_wins = merge(l, r, {}, true);
        CHECK(local_wins.merged == ValueMap{{"a", "1"}, {"b", "2"}, {"c", "4"}});
        CHECK(local_wins.needs_upload);
    }
    SECTION("metadata keys are neither merged nor compared")
    {
        const auto m = merge({{"name", "x"}, {"version", "1"}, {"updated_time", "5"}}, {{"name", "y"}, {"setting_id", "P"}}, {});
        CHECK(m.merged.empty());
        CHECK_FALSE(m.needs_upload);
    }
    SECTION("an absent key and an empty value differ")
    {
        // Local holds "", the remote and base nothing: only the local side changed.
        const auto a = merge({{"a", ""}}, {}, {});
        CHECK(a.merged == ValueMap{{"a", ""}});
        CHECK(a.needs_upload);
        // The remote cleared a key that was "" in the base: it counts as a remote change, Studio inherits it again.
        const auto b = merge({{"a", ""}}, {}, {{"a", ""}});
        CHECK(b.merged.empty());
        CHECK(b.taken_remote == 1);
    }
}

TEST_CASE("upload_with_foreign adds the Studio-only keys of the base", "[BblPresetSync]")
{
    const ValueMap local{{"a", "1"}, {"type", "filament"}, {"version", "2"}};
    const ValueMap base{{"a", "0"}, {"studio_only", "s"}, {"name", "n"}, {"setting_id", "P"}};

    const auto up = sync::upload_with_foreign(local, base, orca_k);
    CHECK(up.at("studio_only") == "s");
    CHECK(up.at("a") == "1");                  // an OrcaSlicer-known key of the base is not copied over the local one
    CHECK(up.at("type") == "filament");        // the local map goes up as it is
    CHECK(up.count("name") == 0);              // metadata of the base is not added
    CHECK(up.count("setting_id") == 0);

    // Only keys OrcaSlicer knows: nothing is added.
    CHECK(sync::upload_with_foreign(local, {{"a", "0"}}, orca_k) == local);

    // The local side wins on a key both hold.
    const auto both = sync::upload_with_foreign({{"studio_only", "L"}}, {{"studio_only", "B"}}, orca_k);
    CHECK(both.at("studio_only") == "L");
}

TEST_CASE("fingerprint ignores settings OrcaSlicer holds and Bambu lost", "[BblPresetSync]")
{
    const ValueMap base_without{{"a", "1"}};
    const ValueMap base_with{{"a", "1"}, {"orca_only", "7"}};
    const ValueMap values{{"a", "1"}, {"orca_only", "7"}};

    // An OrcaSlicer-only key missing from the base does not count.
    CHECK(sync::fingerprint(values, base_without, orca_k, studio_k) == sync::upload_hash({{"a", "1"}}));
    // The same key present in the base does.
    CHECK(sync::fingerprint(values, base_with, orca_k, studio_k) == sync::upload_hash(values));
    // A key Studio knows always counts.
    CHECK(sync::fingerprint({{"a", "2"}}, {}, orca_k, studio_k) == sync::upload_hash({{"a", "2"}}));
    CHECK(sync::fingerprint({{"a", "2"}}, {}, orca_k, studio_k) != sync::fingerprint({{"a", "1"}}, {}, orca_k, studio_k));
    // A key OrcaSlicer does not know is kept (it came from the base).
    CHECK(sync::fingerprint({{"studio_only", "s"}}, {}, orca_k, studio_k) == sync::upload_hash({{"studio_only", "s"}}));
}

// ---------------------------------------------------------------------------
// BaseStore
// ---------------------------------------------------------------------------

TEST_CASE("BaseStore keeps one body per setting id and account", "[BblPresetSync]")
{
    ScopedTemporaryDir temp{"orca-bblbase"};
    const fs::path     dir = sync::BaseStore::dir(temp.path(), "orca/1", "b:1");

    SECTION("a round trip survives a new store")
    {
        {
            sync::BaseStore store(dir);
            CHECK_FALSE(store.get("PF1").has_value());
            store.put("PF1", {{"a", "1"}, {"b", ""}});
            CHECK(store.get("PF1")->at("a") == "1");
            CHECK(store.has("PF1"));
        }
        sync::BaseStore again(dir);
        const auto      got = again.get("PF1");
        REQUIRE(got.has_value());
        CHECK(*got == ValueMap{{"a", "1"}, {"b", ""}});
        CHECK(fs::exists(dir / "PF1.json"));
        CHECK_FALSE(fs::exists(dir / "PF1.json.tmp"));
    }
    SECTION("accounts have their own directories")
    {
        CHECK(sync::BaseStore::dir(temp.path(), "o", "b1") != sync::BaseStore::dir(temp.path(), "o", "b2"));
        CHECK(sync::BaseStore::dir(temp.path(), "o1", "b") != sync::BaseStore::dir(temp.path(), "o2", "b"));
        sync::BaseStore one(sync::BaseStore::dir(temp.path(), "o", "b1")), two(sync::BaseStore::dir(temp.path(), "o", "b2"));
        one.put("PF1", {{"a", "1"}});
        CHECK_FALSE(two.get("PF1").has_value());
    }
    SECTION("a corrupt file counts as missing")
    {
        write_file(dir / "PF1.json", "{ not json");
        write_file(dir / "PF2.json", R"({"a": 5})"); // not a body of strings
        sync::BaseStore store(dir);
        CHECK_FALSE(store.get("PF1").has_value());
        CHECK_FALSE(store.get("PF2").has_value());
        store.put("PF1", {{"a", "1"}});
        CHECK(store.get("PF1").has_value());
    }
    SECTION("erase removes the file and the cached copy")
    {
        sync::BaseStore store(dir);
        store.put("PF1", {{"a", "1"}});
        store.erase("PF1");
        CHECK_FALSE(store.get("PF1").has_value());
        CHECK_FALSE(fs::exists(dir / "PF1.json"));
        store.erase("PF-never"); // harmless
    }
    SECTION("reads come from the cache once loaded")
    {
        sync::BaseStore store(dir);
        store.put("PF1", {{"a", "1"}});
        fs::remove(dir / "PF1.json");
        CHECK(store.get("PF1").has_value());
    }
    SECTION("two threads can write and read")
    {
        sync::BaseStore store(dir);
        auto            work = [&store](const std::string& prefix) {
            for (int i = 0; i < 40; ++i) {
                const std::string id = prefix + std::to_string(i % 5);
                store.put(id, {{"n", std::to_string(i)}});
                store.get(id);
                if (i % 7 == 0) store.erase(id);
            }
        };
        std::thread t1(work, "A"), t2(work, "B");
        t1.join();
        t2.join();
        sync::BaseStore reread(dir);
        for (const char* prefix : {"A", "B"})
            for (int i = 0; i < 5; ++i) {
                const std::string id = std::string(prefix) + std::to_string(i);
                CHECK(store.get(id) == reread.get(id));
            }
    }
}
