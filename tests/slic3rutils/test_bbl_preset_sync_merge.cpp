#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/BblPresetSync/BaseStore.hpp"
#include "slic3r/Utils/BblPresetSync/Hash.hpp"
#include "slic3r/Utils/BblPresetSync/Keys.hpp"
#include "slic3r/Utils/BblPresetSync/Merge.hpp"
#include "slic3r/Utils/BblPresetSync/Types.hpp"

#include "test_utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <set>
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

// Normal merge: local, local base, remote, remote base.
sync::MergeResult merge(const ValueMap& l, const ValueMap& lb, const ValueMap& r, const ValueMap& rb, bool local_newer = false)
{
    return sync::merge_bodies(sync::MergeInput{l, lb, r, rb, false, local_newer, orca_k, studio_k});
}

// First meeting of two same-named presets: there are no bases.
sync::MergeResult merge_pairing(const ValueMap& l, const ValueMap& r, bool local_newer = false)
{
    const ValueMap none;
    return sync::merge_bodies(sync::MergeInput{l, none, r, none, true, local_newer, orca_k, studio_k});
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

TEST_CASE("merge_bodies exchanges edits against two bases", "[BblPresetSync]")
{
    SECTION("nothing changed")
    {
        const auto m = merge({{"a", "1"}}, {{"a", "1"}}, {{"a", "1"}}, {{"a", "1"}});
        CHECK(m.merged == ValueMap{{"a", "1"}});
        CHECK(m.pending_local.empty());
        CHECK(m.taken_remote == 0);
        CHECK(m.conflicts == 0);
    }
    SECTION("a difference that equals its own base on both sides is not a change")
    {
        // OrcaSlicer formats the value its own way; neither side edited it.
        const auto m = merge({{"a", "1.0"}}, {{"a", "1.0"}}, {{"a", "1"}}, {{"a", "1"}});
        CHECK(m.merged == ValueMap{{"a", "1.0"}});
        CHECK(m.pending_local.empty());
        CHECK(m.taken_remote == 0);
    }
    SECTION("an extra local key that was already in the local base is not an edit")
    {
        const auto m = merge({{"a", "1"}, {"extra", "x"}}, {{"a", "1"}, {"extra", "x"}}, {{"a", "1"}}, {{"a", "1"}});
        CHECK(m.merged == ValueMap{{"a", "1"}, {"extra", "x"}});
        CHECK(m.pending_local.empty());
    }
    SECTION("remote-only edit")
    {
        const auto m = merge({{"a", "1.0"}}, {{"a", "1.0"}}, {{"a", "2"}}, {{"a", "1"}});
        CHECK(m.merged == ValueMap{{"a", "2"}});
        CHECK(m.taken_remote == 1);
        CHECK(m.pending_local.empty());
    }
    SECTION("local-only edit is kept and pending")
    {
        const auto m = merge({{"a", "2"}}, {{"a", "1"}}, {{"a", "1"}}, {{"a", "1"}});
        CHECK(m.merged == ValueMap{{"a", "2"}});
        CHECK(m.pending_local == std::set<std::string>{"a"});
        CHECK(m.taken_remote == 0);
    }
    SECTION("a local edit of an OrcaSlicer-only key is pending")
    {
        const auto m = merge({{"orca_only", "9"}}, {{"orca_only", "7"}}, {}, {});
        CHECK(m.merged == ValueMap{{"orca_only", "9"}});
        CHECK(m.pending_local == std::set<std::string>{"orca_only"});
    }
    SECTION("both edited to the same value")
    {
        const auto m = merge({{"a", "2"}}, {{"a", "1"}}, {{"a", "2"}}, {{"a", "1"}});
        CHECK(m.merged == ValueMap{{"a", "2"}});
        CHECK(m.conflicts == 0);
        CHECK(m.pending_local.empty());
    }
    SECTION("conflict, the local side newer")
    {
        const auto m = merge({{"a", "L"}}, {{"a", "1"}}, {{"a", "R"}}, {{"a", "1"}}, true);
        CHECK(m.merged == ValueMap{{"a", "L"}});
        CHECK(m.conflicts == 1);
        CHECK(m.kept_local == 1);
        CHECK(m.pending_local == std::set<std::string>{"a"});
    }
    SECTION("conflict, the remote side newer")
    {
        const auto m = merge({{"a", "L"}}, {{"a", "1"}}, {{"a", "R"}}, {{"a", "1"}}, false);
        CHECK(m.merged == ValueMap{{"a", "R"}});
        CHECK(m.conflicts == 1);
        CHECK(m.taken_remote == 1);
        CHECK(m.pending_local.empty());
    }
    SECTION("remote removed a key Studio knows: the preset inherits it again")
    {
        const auto m = merge({{"a", "1"}}, {{"a", "1"}}, {}, {{"a", "1"}});
        CHECK(m.merged.empty());
        CHECK(m.taken_remote == 1);
        CHECK(m.kept_unknown_to_studio.empty());
    }
    SECTION("remote dropped a key Studio does not know: kept and listed, nothing pending")
    {
        const auto m = merge({{"orca_only", "7"}}, {{"orca_only", "7"}}, {}, {{"orca_only", "7"}});
        CHECK(m.merged == ValueMap{{"orca_only", "7"}});
        CHECK(m.kept_local == 1);
        CHECK(m.kept_unknown_to_studio == std::vector<std::string>{"orca_only"});
        CHECK(m.pending_local.empty());
    }
    SECTION("a key unknown to OrcaSlicer never reaches the preset")
    {
        const auto m = merge({}, {}, {{"studio_only", "9"}, {"a", "1"}}, {});
        CHECK(m.merged == ValueMap{{"a", "1"}});
        CHECK(m.pending_local.empty());
    }
    SECTION("metadata keys are neither merged nor compared")
    {
        const auto m = merge({{"name", "x"}, {"version", "1"}}, {}, {{"name", "y"}, {"setting_id", "P"}}, {});
        CHECK(m.merged.empty());
    }
    SECTION("an absent key and an empty value differ")
    {
        const auto a = merge({{"a", ""}}, {}, {}, {});
        CHECK(a.merged == ValueMap{{"a", ""}});
        CHECK(a.pending_local == std::set<std::string>{"a"});
        const auto b = merge({{"a", ""}}, {{"a", ""}}, {}, {{"a", ""}});
        CHECK(b.merged.empty());
        CHECK(b.taken_remote == 1);
    }
    SECTION("a missing local preset takes everything of the remote body")
    {
        const ValueMap none;
        const auto     m = merge(none, none, {{"a", "1"}, {"b", "2"}, {"studio_only", "s"}}, none);
        CHECK(m.merged == ValueMap{{"a", "1"}, {"b", "2"}});
        CHECK(m.taken_remote == 2);
        CHECK(m.pending_local.empty());
    }
}

TEST_CASE("merge_bodies of a pairing has no bases", "[BblPresetSync]")
{
    SECTION("equal keys")
    {
        const auto m = merge_pairing({{"a", "1"}}, {{"a", "1"}});
        CHECK(m.merged == ValueMap{{"a", "1"}});
        CHECK(m.conflicts == 0);
        CHECK(m.pending_local.empty());
    }
    SECTION("keys only on Bambu are taken, keys only local are kept and not pending")
    {
        const auto m = merge_pairing({{"a", "1"}, {"local_only", "L"}}, {{"a", "1"}, {"remote_only", "R"}});
        CHECK(m.merged == ValueMap{{"a", "1"}, {"local_only", "L"}, {"remote_only", "R"}});
        CHECK(m.pending_local.empty());
        CHECK(m.taken_remote == 2);
    }
    SECTION("a disagreement is settled by the newer side")
    {
        const auto remote_wins = merge_pairing({{"a", "L"}}, {{"a", "R"}}, false);
        CHECK(remote_wins.merged == ValueMap{{"a", "R"}});
        CHECK(remote_wins.conflicts == 1);
        CHECK(remote_wins.pending_local.empty());
        const auto local_wins = merge_pairing({{"a", "L"}}, {{"a", "R"}}, true);
        CHECK(local_wins.merged == ValueMap{{"a", "L"}});
        CHECK(local_wins.conflicts == 1);
        CHECK(local_wins.pending_local == std::set<std::string>{"a"});
    }
    SECTION("a key unknown to OrcaSlicer stays out")
    {
        const auto m = merge_pairing({}, {{"studio_only", "s"}});
        CHECK(m.merged.empty());
    }
}

TEST_CASE("apply_local_edits changes only what was edited locally", "[BblPresetSync]")
{
    const ValueMap remote{{"a", "R-a"}, {"b", "R-b"}, {"c", "R-c"}, {"studio_only", "s"}, {"name", "n"}, {"setting_id", "P"},
                          {"type", "filament"},  {"version", "1.0.0.0"}};
    const ValueMap local_base{{"a", "1"}, {"b", "1"}, {"c", "1"}};

    SECTION("untouched keys keep the Bambu value, whatever the local map holds")
    {
        const ValueMap local{{"a", "different"}, {"b", "1"}, {"c", "1"}, {"type", "filament"}};
        const ValueMap lb{{"a", "different"}, {"b", "1"}, {"c", "1"}};
        const auto     up = sync::apply_local_edits(remote, local, lb, orca_k);
        CHECK(up.at("a") == "R-a");
        CHECK(up.at("b") == "R-b");
        CHECK(up.at("c") == "R-c");
    }
    SECTION("an edited key takes the local value, a removed key is removed")
    {
        const ValueMap local{{"a", "edited"}, {"b", "1"}, {"type", "filament"}}; // c removed
        const auto     up = sync::apply_local_edits(remote, local, local_base, orca_k);
        CHECK(up.at("a") == "edited");
        CHECK(up.at("b") == "R-b");
        CHECK(up.count("c") == 0);
    }
    SECTION("a key added locally is added")
    {
        const ValueMap local{{"a", "1"}, {"b", "1"}, {"c", "1"}, {"d", "new"}};
        const auto     up = sync::apply_local_edits(remote, local, local_base, orca_k);
        CHECK(up.at("d") == "new");
    }
    SECTION("the envelope fields of the Bambu copy are dropped, unknown keys survive")
    {
        ValueMap copy = remote;
        copy["update_time"]  = "123";
        copy["updated_time"] = "123";
        copy["user_id"]      = "u";
        copy["nickname"]     = "nick";
        copy["public"]       = "false";
        copy["code"]         = "0";
        const ValueMap local{{"a", "edited"}, {"b", "1"}, {"c", "1"}, {"name", "local-name"}};
        const auto     up = sync::apply_local_edits(copy, local, local_base, orca_k);
        CHECK(up.at("studio_only") == "s");
        for (const char* k : {"update_time", "updated_time", "setting_id", "user_id", "name", "nickname", "public", "code"})
            CHECK(up.count(k) == 0);
    }
    SECTION("an OrcaSlicer-unknown key edited locally is not applied")
    {
        const ValueMap local{{"studio_only", "x"}, {"a", "1"}, {"b", "1"}, {"c", "1"}};
        CHECK(sync::apply_local_edits(remote, local, local_base, orca_k).at("studio_only") == "s");
    }
    SECTION("type comes from the local map, version stays unless the copy has none")
    {
        const ValueMap local{{"a", "1"}, {"b", "1"}, {"c", "1"}, {"type", "print"}, {"version", "9.9.9.9"}};
        const auto     up = sync::apply_local_edits(remote, local, local_base, orca_k);
        CHECK(up.at("type") == "print");
        CHECK(up.at("version") == "1.0.0.0");
        ValueMap without = remote;
        without.erase("version");
        CHECK(sync::apply_local_edits(without, local, local_base, orca_k).at("version") == "9.9.9.9");
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
            store.put("PF1", {{{"a", "1"}, {"b", ""}}, {{"x", "y"}}});
            CHECK(store.get("PF1")->remote.at("a") == "1");
            CHECK(store.has("PF1"));
        }
        sync::BaseStore again(dir);
        const auto      got = again.get("PF1");
        REQUIRE(got.has_value());
        CHECK(got->remote == ValueMap{{"a", "1"}, {"b", ""}});
        CHECK(got->local == ValueMap{{"x", "y"}});
        CHECK(fs::exists(dir / "PF1.json"));
        CHECK_FALSE(fs::exists(dir / "PF1.json.tmp"));
    }
    SECTION("accounts have their own directories")
    {
        CHECK(sync::BaseStore::dir(temp.path(), "o", "b1") != sync::BaseStore::dir(temp.path(), "o", "b2"));
        CHECK(sync::BaseStore::dir(temp.path(), "o1", "b") != sync::BaseStore::dir(temp.path(), "o2", "b"));
        sync::BaseStore one(sync::BaseStore::dir(temp.path(), "o", "b1")), two(sync::BaseStore::dir(temp.path(), "o", "b2"));
        one.put("PF1", {{{"a", "1"}}, {}});
        CHECK_FALSE(two.get("PF1").has_value());
    }
    SECTION("a file in the old single-map form reads as the remote base")
    {
        write_file(dir / "PF1.json", R"({"a": "1", "b": "2"})");
        sync::BaseStore store(dir);
        const auto      got = store.get("PF1");
        REQUIRE(got.has_value());
        CHECK(got->remote == ValueMap{{"a", "1"}, {"b", "2"}});
        CHECK(got->local.empty());
    }
    SECTION("a corrupt file counts as missing")
    {
        write_file(dir / "PF1.json", "{ not json");
        write_file(dir / "PF2.json", R"({"a": 5})"); // not a body of strings
        sync::BaseStore store(dir);
        CHECK_FALSE(store.get("PF1").has_value());
        CHECK_FALSE(store.get("PF2").has_value());
        store.put("PF1", {{{"a", "1"}}, {}});
        CHECK(store.get("PF1").has_value());
    }
    SECTION("erase removes the file and the cached copy")
    {
        sync::BaseStore store(dir);
        store.put("PF1", {{{"a", "1"}}, {}});
        store.erase("PF1");
        CHECK_FALSE(store.get("PF1").has_value());
        CHECK_FALSE(fs::exists(dir / "PF1.json"));
        store.erase("PF-never"); // harmless
    }
    SECTION("reads come from the cache once loaded")
    {
        sync::BaseStore store(dir);
        store.put("PF1", {{{"a", "1"}}, {}});
        fs::remove(dir / "PF1.json");
        CHECK(store.get("PF1").has_value());
    }
    SECTION("two threads can write and read")
    {
        sync::BaseStore store(dir);
        auto            work = [&store](const std::string& prefix) {
            for (int i = 0; i < 40; ++i) {
                const std::string id = prefix + std::to_string(i % 5);
                store.put(id, {{{"n", std::to_string(i)}}, {{"m", std::to_string(i)}}});
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
                const auto a = store.get(id), b = reread.get(id);
                CHECK(a.has_value() == b.has_value());
                if (a && b) {
                    CHECK(a->remote == b->remote);
                    CHECK(a->local == b->local);
                }
            }
    }
}

// ---------------------------------------------------------------------------
// Studio key list: parsing, plausibility, files
// ---------------------------------------------------------------------------

namespace {

std::set<std::string> numbered_keys(size_t n)
{
    std::set<std::string> keys;
    for (size_t i = 0; i < n; ++i) keys.insert("key_" + std::to_string(i));
    return keys;
}

const char* const essential_keys[] = {"layer_height", "nozzle_temperature", "filament_type", "printable_area", "sparse_infill_density", "compatible_printers"};

std::set<std::string> full_key_set()
{
    std::set<std::string> keys = numbered_keys(400);
    for (const char* k : essential_keys) keys.insert(k);
    return keys;
}

} // namespace

TEST_CASE("parse_studio_print_config finds the keys defined with this->add and this->add_nullable", "[BblPresetSync]")
{
    const std::string source = R"(
    def = this->add("layer_height", coFloat);
    def = this->add_nullable("filament_retraction_length", coFloats);
    def = this->add( "sparse_infill_density" , coPercent);
    // this->add("old_key", coInt);   (a comment is still found by the plain scan: harmless)
    def = m_other.add("not_this", coInt);
    def = this->add(key_variable, coInt);
    def = this->add_nullable(
        "on_next_line", coInt);
    def = this->add("with-dash", coInt);
    )";
    const auto keys = sync::parse_studio_print_config(source);
    CHECK(keys.count("layer_height") == 1);
    CHECK(keys.count("filament_retraction_length") == 1);
    CHECK(keys.count("sparse_infill_density") == 1);
    CHECK(keys.count("old_key") == 1);
    CHECK(keys.count("on_next_line") == 1);
    CHECK(keys.count("not_this") == 0);
    CHECK(keys.count("key_variable") == 0);
    CHECK(keys.count("with-dash") == 0);
    CHECK(keys.size() == 5);
}

TEST_CASE("plausible_studio_keys wants enough keys and the essential ones", "[BblPresetSync]")
{
    CHECK(sync::plausible_studio_keys(full_key_set()));

    auto few_keys = numbered_keys(393);
    for (const char* k : essential_keys) few_keys.insert(k);
    REQUIRE(few_keys.size() == 399);
    CHECK_FALSE(sync::plausible_studio_keys(few_keys));

    auto without = full_key_set();
    without.erase("layer_height");
    for (size_t i = 1000; without.size() < 405; ++i) without.insert("pad_" + std::to_string(i));
    CHECK_FALSE(sync::plausible_studio_keys(without));
}

TEST_CASE("The bundled Studio key list is plausible", "[BblPresetSync]")
{
    const fs::path bundled = fs::path(std::string(PROFILES_DIR)).parent_path() / "printers" / "bambu_studio_option_keys.json";
    const auto     file    = sync::read_studio_key_file(bundled);
    REQUIRE(file.has_value());
    CHECK(file->fetched_at == 0);
    CHECK(file->source.rfind("bundled ", 0) == 0);
    CHECK(sync::plausible_studio_keys(file->keys));
}

TEST_CASE("Studio key files round-trip and bad ones are rejected", "[BblPresetSync]")
{
    ScopedTemporaryDir temp{"orca-bblstudiokeys"};
    const fs::path     file = temp.path() / "sub" / "studio_keys.json";

    SECTION("write then read")
    {
        const std::map<std::string, std::string> types{{"layer_height", "coFloat"}, {"filament_type", "coStrings"}};
        REQUIRE(sync::write_studio_key_file(file, full_key_set(), types, "v02.09.00.70", 1700000000));
        CHECK_FALSE(fs::exists(fs::path(file.string() + ".tmp")));
        const auto got = sync::read_studio_key_file(file);
        REQUIRE(got.has_value());
        CHECK(got->keys == full_key_set());
        CHECK(got->types == types);
        CHECK(got->source == "BambuStudio v02.09.00.70");
        CHECK(got->fetched_at == 1700000000);
    }
    SECTION("the bundled form has no fetched_at")
    {
        nlohmann::json j;
        j["studio_version"] = "v1";
        j["keys"]           = full_key_set();
        write_file(file, j.dump());
        const auto got = sync::read_studio_key_file(file);
        REQUIRE(got.has_value());
        CHECK(got->fetched_at == 0);
        CHECK(got->source == "bundled v1");
        CHECK(got->types.empty()); // a file without "types"
    }
    SECTION("only string types are read")
    {
        nlohmann::json j;
        j["keys"]                   = full_key_set();
        j["types"]["layer_height"]  = "coFloat";
        j["types"]["filament_type"] = 5;
        write_file(file, j.dump());
        const auto got = sync::read_studio_key_file(file);
        REQUIRE(got.has_value());
        CHECK(got->types == std::map<std::string, std::string>{{"layer_height", "coFloat"}});
    }
    SECTION("missing, invalid and implausible files")
    {
        CHECK_FALSE(sync::read_studio_key_file(temp.path() / "none.json").has_value());
        write_file(file, "not json");
        CHECK_FALSE(sync::read_studio_key_file(file).has_value());
        REQUIRE(sync::write_studio_key_file(file, numbered_keys(10), {}, "v1", 5));
        CHECK_FALSE(sync::read_studio_key_file(file).has_value());
    }
}

TEST_CASE("parse_studio_print_config_types reads the type token after each key", "[BblPresetSync]")
{
    const std::string source = R"(
    def = this->add("a", coFloat);
    def = this->add_nullable("b", coFloats);
    def = this->add( "c"   ,
        coPercent);
    def = this->add("no_token");
    def = this->add("not_co", ConfigOptionFloat);
    def = this->add("a", coString);
    def = this->add(key_variable, coInt);
    )";
    const auto types = sync::parse_studio_print_config_types(source);
    CHECK(types.size() == 3);
    CHECK(types.at("a") == "coFloat"); // the first occurrence wins
    CHECK(types.at("b") == "coFloats");
    CHECK(types.at("c") == "coPercent");
    CHECK(types.count("no_token") == 0);
    CHECK(types.count("not_co") == 0);
    // The key scan itself is unchanged.
    const auto keys = sync::parse_studio_print_config(source);
    CHECK(keys == std::set<std::string>{"a", "b", "c", "no_token", "not_co"});
}

TEST_CASE("StudioKeys carries the types of its file", "[BblPresetSync]")
{
    ScopedTemporaryDir temp{"orca-bblkeytypes"};
    const fs::path     file = temp.path() / "keys.json";

    write_file(file, R"({"keys":["layer_height","filament_notes"],"types":{"layer_height":"coFloat","filament_notes":"coString"}})");
    const auto keys = sync::StudioKeys::load(file);
    CHECK(keys.has_types());
    CHECK(keys.type_of("filament_notes") == "coString");
    CHECK(keys.type_of("layer_height") == "coFloat");
    CHECK(keys.type_of("unknown").empty());

    write_file(file, R"({"keys":["layer_height"]})");
    const auto without = sync::StudioKeys::load(file);
    CHECK_FALSE(without.has_types());
    CHECK(without.type_of("layer_height").empty());

    CHECK_FALSE(sync::StudioKeys({"a"}).has_types());
    CHECK(sync::StudioKeys({"a"}, {{"a", "coInt"}}).type_of("a") == "coInt");
}

TEST_CASE("The bundled Studio key list has a type for its keys", "[BblPresetSync]")
{
    const fs::path bundled = fs::path(std::string(PROFILES_DIR)).parent_path() / "printers" / "bambu_studio_option_keys.json";
    const auto     file    = sync::read_studio_key_file(bundled);
    REQUIRE(file.has_value());
    CHECK(file->types.size() > 700);
    for (const auto& [key, type] : file->types) {
        CAPTURE(key);
        CHECK(file->keys.count(key) == 1);
        CHECK(type.rfind("co", 0) == 0);
    }
    CHECK(file->types.at("filament_notes") == "coString");
    CHECK(sync::StudioKeys::load(bundled).type_of("fan_max_speed") == "coInts");
}

TEST_CASE("orca_type_name spells OrcaSlicer's own type of a setting", "[BblPresetSync]")
{
    CHECK(sync::orca_type_name("layer_height") == "coFloat");
    CHECK(sync::orca_type_name("filament_notes") == "coStrings");
    CHECK(sync::orca_type_name("fan_max_speed") == "coFloats");
    CHECK(sync::orca_type_name("line_width") == "coFloatOrPercent");
    CHECK(sync::orca_type_name("enable_arc_fitting") == "coEnum");
    CHECK(sync::orca_type_name("no_such_setting").empty());
}
