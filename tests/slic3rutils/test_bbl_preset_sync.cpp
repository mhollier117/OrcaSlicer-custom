#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/BblPresetSync/BodyCheck.hpp"
#include "slic3r/Utils/BblPresetSync/FilamentId.hpp"
#include "slic3r/Utils/BblPresetSync/Hash.hpp"
#include "slic3r/Utils/BblPresetSync/Ledger.hpp"
#include "slic3r/Utils/BblPresetSync/Planner.hpp"
#include "slic3r/Utils/BblPresetSync/Types.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <iterator>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace Slic3r::BblPresetSync;
namespace fs = boost::filesystem;

namespace {

struct TempDir
{
    fs::path path;
    TempDir() : path(fs::temp_directory_path() / fs::unique_path("bblsync-%%%%-%%%%-%%%%")) { fs::create_directories(path); }
    ~TempDir()
    {
        boost::system::error_code ec;
        fs::remove_all(path, ec);
    }
};

std::string read_all(const fs::path& p)
{
    boost::nowide::ifstream in(p.string(), std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_all(const fs::path& p, const std::string& text)
{
    fs::create_directories(p.parent_path());
    boost::nowide::ofstream out(p.string(), std::ios::binary | std::ios::trunc);
    out << text;
}

Key fk(const std::string& name) { return Key{PresetType::Filament, name}; }

LocalPreset local_preset(const std::string& name, const std::string& hash, long long mtime = 0, LocalState st = LocalState::Eligible)
{
    LocalPreset l;
    l.key   = fk(name);
    l.state = st;
    l.hash  = hash;
    l.mtime = mtime;
    return l;
}

RemoteRow remote_row(const std::string& name, const std::string& id, long long update_time, bool body = true, const std::string& body_hash = "")
{
    RemoteRow r;
    r.key         = fk(name);
    r.setting_id  = id;
    r.update_time = update_time;
    r.has_body    = body;
    r.body_hash   = body_hash;
    return r;
}

LedgerEntry linked(const std::string& id, long long update_time, const std::string& hash, int absent = 0)
{
    LedgerEntry e;
    e.setting_id    = id;
    e.update_time   = update_time;
    e.synced_hash   = hash;
    e.absent_passes = absent;
    return e;
}

struct Fixture
{
    std::vector<LocalPreset> locals;
    std::vector<RemoteRow>   rows;
    LedgerView               view;

    Fixture() { view.first_run_confirmed = true; }
    void entry(const std::string& name, const LedgerEntry& e) { view.entries[fk(name)] = e; }
    Plan plan() const { return make_plan(locals, rows, view); }
};

const Action* find_action(const Plan& plan, const std::string& name)
{
    for (const Action& a : plan.actions)
        if (a.key == fk(name)) return &a;
    return nullptr;
}

bool has_note(const Plan& plan, const std::string& text)
{
    for (const std::string& n : plan.notes)
        if (n == text) return true;
    return false;
}

} // namespace

TEST_CASE("Preset type strings and keys round-trip", "[BblPresetSync]")
{
    SECTION("type strings")
    {
        for (PresetType t : {PresetType::Print, PresetType::Filament, PresetType::Printer}) {
            auto parsed = parse_type(type_string(t));
            REQUIRE(parsed.has_value());
            CHECK(*parsed == t);
        }
        CHECK(type_string(PresetType::Print) == "print");
        CHECK(type_string(PresetType::Filament) == "filament");
        CHECK(type_string(PresetType::Printer) == "printer");
        CHECK_FALSE(parse_type("machine").has_value());
    }
    SECTION("key with a slash in the name")
    {
        Key k{PresetType::Filament, "A/B @x"};
        CHECK(k.str() == "filament/A/B @x");
        auto parsed = Key::parse(k.str());
        REQUIRE(parsed.has_value());
        CHECK(*parsed == k);
        CHECK_FALSE(Key::parse("noslash").has_value());
        CHECK_FALSE(Key::parse("bogus/name").has_value());
    }
    SECTION("ordering is by type then name")
    {
        CHECK(Key{PresetType::Print, "z"} < Key{PresetType::Filament, "a"});
        CHECK(Key{PresetType::Filament, "a"} < Key{PresetType::Filament, "b"});
        CHECK_FALSE(Key{PresetType::Filament, "a"} < Key{PresetType::Filament, "a"});
    }
}

TEST_CASE("Upload hash ignores metadata and normalizes base_id", "[BblPresetSync]")
{
    SECTION("md5 vector") { CHECK(md5_hex("abc") == "900150983cd24fb0d6963f7d28e17f72"); }

    ValueMap base{{"filament_type", "PETG"}, {"nozzle_temperature", "240"}};
    const std::string h = upload_hash(base);

    SECTION("ignored keys do not change the hash")
    {
        for (const std::string& key : hash_ignored_keys()) {
            ValueMap v = base;
            v[key]     = "something";
            CAPTURE(key);
            CHECK(upload_hash(v) == h);
        }
        CHECK(hash_ignored_keys().size() == 10);
    }
    SECTION("empty base_id equals no base_id")
    {
        ValueMap v = base;
        v["base_id"] = "";
        CHECK(upload_hash(v) == h);
    }
    SECTION("base_id, inherits and filament_id change it")
    {
        for (const char* key : {"base_id", "inherits", "filament_id"}) {
            ValueMap v = base;
            v[key]     = "GFB00";
            CAPTURE(key);
            CHECK(upload_hash(v) != h);
        }
    }
    SECTION("a value change changes it and order of insertion does not matter")
    {
        ValueMap a, b;
        a["x"] = "1";
        a["y"] = "2";
        b["y"] = "2";
        b["x"] = "1";
        CHECK(upload_hash(a) == upload_hash(b));
        b["x"] = "3";
        CHECK(upload_hash(a) != upload_hash(b));
    }
}

TEST_CASE("Filament ids follow Bambu Studio's model", "[BblPresetSync]")
{
    SECTION("base name")
    {
        CHECK(filament_base_name("Overture PETG @BBL H2D") == "Overture PETG");
        CHECK(filament_base_name("Overture PETG") == "Overture PETG");
        CHECK(filament_base_name("A @B @C") == "A");
        CHECK(filament_base_name(" @H2D") == " @H2D");
        CHECK(filament_base_name("") == "");
    }
    SECTION("make_filament_id")
    {
        CHECK(make_filament_id("Foo", "123") == "P" + md5_hex("Foo@123").substr(0, 7));
        CHECK(make_filament_id("Foo", "123", "s") == "P" + md5_hex("Foo@123s").substr(0, 7));
        CHECK(make_filament_id("Overture PETG High Speed Tuned", "1234567890") == "P844690c");
    }
    SECTION("reuse of an existing root's id")
    {
        std::vector<ExistingRoot> roots{{"Pabcdefg", "Other"}, {"P1234567", "Foo"}, {"P7654321", "Foo"}};
        CHECK(choose_filament_id("Foo @BBL H2D", "123", roots, [] { return std::string("x"); }) == "P1234567");
    }
    SECTION("a non-P or wrong-length id is not reused")
    {
        std::vector<ExistingRoot> roots{{"GFA00", "Foo"}};
        CHECK(choose_filament_id("Foo", "123", roots, [] { return std::string("x"); }) == make_filament_id("Foo", "123"));
    }
    SECTION("a clash with another base name is resolved by the salt")
    {
        const std::string         clash = make_filament_id("Foo", "123");
        std::vector<ExistingRoot> roots{{clash, "Other"}};
        int                       calls = 0;
        auto                      salt  = [&calls] { return std::string("s") + std::to_string(++calls); };
        const std::string         id    = choose_filament_id("Foo", "123", roots, salt);
        CHECK(calls == 1);
        CHECK(id == make_filament_id("Foo", "123", "s1"));
    }
    SECTION("sixteen failing salts give up")
    {
        std::vector<ExistingRoot> roots;
        roots.push_back({make_filament_id("Foo", "123"), "Other"});
        for (int i = 1; i <= 16; ++i) roots.push_back({make_filament_id("Foo", "123", "s" + std::to_string(i)), "Other"});
        int  calls = 0;
        auto salt  = [&calls] { return std::string("s") + std::to_string(++calls); };
        CHECK(choose_filament_id("Foo", "123", roots, salt).empty());
        CHECK(calls == 16);
    }
}

TEST_CASE("Body check classifies drained bodies", "[BblPresetSync]")
{
    ValueMap child{{"type", "filament"}, {"setting_id", "PFUS1"}, {"name", "x"}, {"base_id", "GFSA"}, {"inherits", "p"},
                   {"filament_settings_id", "x"}};
    ValueMap root{{"type", "filament"}, {"setting_id", "PFUS1"}, {"filament_id", "P123"}, {"filament_type", "PETG"}};

    SECTION("a child with one content key is valid")
    {
        CHECK(check_body(child, PresetType::Filament, "PFUS1", false) == BodyStatus::Valid);
    }
    SECTION("a valid root")
    {
        CHECK(check_body(root, PresetType::Filament, "PFUS1", true) == BodyStatus::Valid);
    }
    SECTION("wrong type or setting id, including missing")
    {
        CHECK(check_body(child, PresetType::Print, "PFUS1", false) == BodyStatus::WrongIdentity);
        CHECK(check_body(child, PresetType::Filament, "PFUS2", false) == BodyStatus::WrongIdentity);
        ValueMap no_type = child;
        no_type.erase("type");
        CHECK(check_body(no_type, PresetType::Filament, "PFUS1", false) == BodyStatus::WrongIdentity);
        ValueMap no_id = child;
        no_id.erase("setting_id");
        CHECK(check_body(no_id, PresetType::Filament, "PFUS1", false) == BodyStatus::WrongIdentity);
    }
    SECTION("metadata only")
    {
        ValueMap meta{{"type", "filament"}, {"setting_id", "PFUS1"}, {"name", "x"}, {"updated_time", "1"}, {"version", "1.0"},
                      {"base_id", ""},      {"inherits", ""},        {"filament_id", "P1"}, {"user_id", "u"}, {"update_time", "2"}};
        CHECK(check_body(meta, PresetType::Filament, "PFUS1", false) == BodyStatus::MetadataOnly);
        CHECK(metadata_keys().size() == 10);
    }
    SECTION("a root filament without filament_type")
    {
        ValueMap r = root;
        r.erase("filament_type");
        r["nozzle_temperature"] = "240";
        CHECK(check_body(r, PresetType::Filament, "PFUS1", true) == BodyStatus::RootMissingType);
        CHECK(check_body(r, PresetType::Filament, "PFUS1", false) == BodyStatus::Valid);
    }
    SECTION("a root of another type needs no filament_type")
    {
        ValueMap p{{"type", "print"}, {"setting_id", "PPUS1"}, {"layer_height", "0.2"}};
        CHECK(check_body(p, PresetType::Print, "PPUS1", true) == BodyStatus::Valid);
    }
}

TEST_CASE("Ledger persists state atomically", "[BblPresetSync]")
{
    TempDir  dir;
    fs::path file = Ledger::file_path(dir.path, "user1", "uid1");

    SECTION("file names are per account and safe")
    {
        CHECK(file == dir.path / "bbl_sync" / "ledger_user1_uid1.json");
        CHECK(Ledger::file_path(dir.path, "a/b c", "x.y").filename().string() == "ledger_a_b_c_x_y.json");
        CHECK(Ledger::file_path(dir.path, "u-1_2", "9").filename().string() == "ledger_u-1_2_9.json");
        CHECK(Ledger::file_path(dir.path, "u2", "uid1") != file);
        CHECK(Ledger::backup_dir(dir.path) == dir.path / "bbl_sync" / "backup");
    }
    SECTION("a missing file loads empty")
    {
        Ledger l(file);
        std::string err;
        CHECK(l.load(&err));
        CHECK(l.entries().empty());
        CHECK_FALSE(l.first_run_confirmed());
        CHECK(l.halted().empty());
        CHECK(l.clock_offset() == 0);
    }
    SECTION("save then load round-trips every field")
    {
        LedgerEntry e;
        e.setting_id        = "PFUS1";
        e.update_time       = 1762790000;
        e.synced_hash       = "abc";
        e.state             = "unresolvable";
        e.reason            = "parent missing";
        e.absent_passes     = 1;
        e.retry_after       = 99;
        e.fail_count        = 3;
        e.last_error        = "boom";
        e.converted_from    = "OFabc";
        e.k_copied_printers = {"sn1", "sn2"};

        Ledger a(file);
        a.put(Key{PresetType::Filament, "A/B"}, e);
        a.put(fk("plain"), LedgerEntry{});
        a.set_first_run_confirmed(true);
        a.set_halted("mass_delete");
        a.set_clock_offset(-7);
        std::string err;
        REQUIRE(a.save(&err));
        CHECK_FALSE(fs::exists(fs::path(file.string() + ".tmp")));

        Ledger b(file);
        REQUIRE(b.load(&err));
        CHECK(b.first_run_confirmed());
        CHECK(b.halted() == "mass_delete");
        CHECK(b.clock_offset() == -7);
        REQUIRE(b.entries().size() == 2);
        auto got = b.get(Key{PresetType::Filament, "A/B"});
        REQUIRE(got.has_value());
        CHECK(got->setting_id == "PFUS1");
        CHECK(got->update_time == 1762790000);
        CHECK(got->synced_hash == "abc");
        CHECK(got->state == "unresolvable");
        CHECK(got->reason == "parent missing");
        CHECK(got->absent_passes == 1);
        CHECK(got->retry_after == 99);
        CHECK(got->fail_count == 3);
        CHECK(got->last_error == "boom");
        CHECK(got->converted_from == "OFabc");
        CHECK(got->k_copied_printers == std::vector<std::string>{"sn1", "sn2"});
        CHECK(b.get(fk("plain"))->state == "linked");

        const auto json = nlohmann::json::parse(read_all(file));
        CHECK(json["version"] == 1);
        CHECK(json["entries"].contains("filament/A/B"));
    }
    SECTION("erase and a second save replace the file")
    {
        Ledger a(file);
        a.put(fk("x"), linked("P1", 1, "h"));
        REQUIRE(a.save());
        a.erase(fk("x"));
        REQUIRE(a.save());
        Ledger b(file);
        REQUIRE(b.load());
        CHECK(b.entries().empty());
        CHECK_FALSE(a.get(fk("x")).has_value());
    }
    SECTION("a corrupt file is renamed and the ledger is empty")
    {
        write_all(file, "{ not json");
        Ledger l(file);
        l.put(fk("stale"), linked("P1", 1, "h"));
        std::string err;
        CHECK_FALSE(l.load(&err));
        CHECK_FALSE(err.empty());
        CHECK(l.entries().empty());
        CHECK_FALSE(fs::exists(file));
        int corrupt = 0;
        for (fs::directory_iterator it(file.parent_path()); it != fs::directory_iterator(); ++it)
            if (it->path().filename().string().find(".corrupt-") != std::string::npos) ++corrupt;
        CHECK(corrupt == 1);
    }
    SECTION("version 2 is treated as corrupt")
    {
        write_all(file, R"({"version": 2, "entries": {}})");
        Ledger      l(file);
        std::string err;
        CHECK_FALSE(l.load(&err));
        CHECK_FALSE(err.empty());
        CHECK_FALSE(fs::exists(file));
    }
    SECTION("unknown fields are ignored, missing fields default, bad keys are skipped")
    {
        write_all(file, R"({"version": 1, "future": [1,2], "entries": {
            "filament/ok": {"setting_id": "P9", "mystery": true},
            "filament/odd": "not an object",
            "noslash": {"setting_id": "P1"},
            "gadget/x": {"setting_id": "P2"}}})");
        Ledger l(file);
        REQUIRE(l.load());
        REQUIRE(l.entries().size() == 1);
        auto e = l.get(fk("ok"));
        REQUIRE(e.has_value());
        CHECK(e->setting_id == "P9");
        CHECK(e->state == "linked");
        CHECK(e->update_time == 0);
        CHECK(e->k_copied_printers.empty());
        CHECK_FALSE(l.first_run_confirmed());
    }
    SECTION("concurrent puts from four threads are all kept")
    {
        Ledger                   l(file);
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t)
            threads.emplace_back([&l, t] {
                for (int i = 0; i < 50; ++i) l.put(fk("t" + std::to_string(t) + "_" + std::to_string(i)), linked("P", i, "h"));
            });
        for (auto& th : threads) th.join();
        CHECK(l.entries().size() == 200);
    }
}

TEST_CASE("want_body asks only for unknown or newer rows", "[BblPresetSync]")
{
    LedgerView view;
    view.entries[fk("A")] = linked("P1", 100, "h");
    CHECK(want_body(fk("A"), "P-unknown", 5, view));
    CHECK(want_body(fk("A"), "P1", 101, view));
    CHECK_FALSE(want_body(fk("A"), "P1", 100, view));
    CHECK_FALSE(want_body(fk("A"), "P1", 99, view));
}

TEST_CASE("Planner handles linked entries", "[BblPresetSync]")
{
    Fixture f;
    f.entry("A", linked("P1", 100, "h0"));

    SECTION("neither changed gives nothing")
    {
        f.locals = {local_preset("A", "h0")};
        f.rows   = {remote_row("A", "P1", 100)};
        CHECK(f.plan().actions.empty());
    }
    SECTION("neither changed resets a nonzero absent count")
    {
        f.entry("A", linked("P1", 100, "h0", 1));
        f.locals = {local_preset("A", "h0")};
        f.rows   = {remote_row("A", "P1", 100)};
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::SetAbsent);
        CHECK(plan.actions[0].absent_passes == 0);
    }
    SECTION("local changed updates")
    {
        f.locals = {local_preset("A", "h1")};
        f.rows   = {remote_row("A", "P1", 100, false)};
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::Update);
        CHECK(plan.actions[0].setting_id == "P1");
        CHECK_FALSE(plan.actions[0].backup_remote);
    }
    SECTION("remote changed pulls")
    {
        f.locals = {local_preset("A", "h0")};
        f.rows   = {remote_row("A", "P1", 200)};
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::Pull);
        CHECK(plan.actions[0].setting_id == "P1");
        CHECK_FALSE(plan.actions[0].backup_local);
    }
    SECTION("remote changed without a body gives a note and no action")
    {
        f.locals = {local_preset("A", "h0")};
        f.rows   = {remote_row("A", "P1", 200, false)};
        auto plan = f.plan();
        CHECK(plan.actions.empty());
        CHECK(has_note(plan, "body unavailable for filament/A"));
    }
    SECTION("both changed: tie window edges")
    {
        f.locals = {local_preset("A", "h1", 1000)};
        f.rows   = {remote_row("A", "P1", 1120)};
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::Update);
        CHECK(plan.actions[0].backup_remote);

        f.rows = {remote_row("A", "P1", 1121)};
        plan   = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::Pull);
        CHECK(plan.actions[0].backup_local);
    }
    SECTION("both changed: clock_offset shifts the result")
    {
        f.locals = {local_preset("A", "h1", 1000)};
        f.rows   = {remote_row("A", "P1", 1130)};
        CHECK(f.plan().actions[0].type == ActionType::Pull);
        f.view.clock_offset = 10;
        CHECK(f.plan().actions[0].type == ActionType::Update);
        f.view.clock_offset = -10;
        f.rows              = {remote_row("A", "P1", 1111)};
        CHECK(f.plan().actions[0].type == ActionType::Pull);
    }
    SECTION("both changed without a body gives a note and no action")
    {
        f.locals = {local_preset("A", "h1", 1000)};
        f.rows   = {remote_row("A", "P1", 1120, false)};
        auto plan = f.plan();
        CHECK(plan.actions.empty());
        CHECK(has_note(plan, "body unavailable for filament/A"));
    }
    SECTION("local missing: remote unchanged deletes remote, changed pulls")
    {
        f.rows = {remote_row("A", "P1", 100)};
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::DeleteRemote);
        CHECK(plan.actions[0].setting_id == "P1");

        f.rows = {remote_row("A", "P1", 101)};
        plan   = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::Pull);

        f.rows = {remote_row("A", "P1", 101, false)};
        plan   = f.plan();
        CHECK(plan.actions.empty());
        CHECK(has_note(plan, "body unavailable for filament/A"));
    }
    SECTION("row absent: first pass counts")
    {
        f.locals = {local_preset("A", "h0")};
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::SetAbsent);
        CHECK(plan.actions[0].absent_passes == 1);
    }
    SECTION("row absent: second pass deletes locally with backup when unchanged")
    {
        f.entry("A", linked("P1", 100, "h0", 1));
        f.locals = {local_preset("A", "h0")};
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::DeleteLocal);
        CHECK(plan.actions[0].backup_local);
    }
    SECTION("row absent: second pass creates again when local changed")
    {
        f.entry("A", linked("P1", 100, "h0", 1));
        f.locals = {local_preset("A", "h1")};
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::Create);
        CHECK(plan.actions[0].setting_id.empty());
    }
    SECTION("local missing and row absent drops the entry")
    {
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::DropEntry);
    }
    SECTION("ineligible never produces actions, with the row present or gone")
    {
        f.locals = {local_preset("A", "", 0, LocalState::Ineligible)};
        CHECK(f.plan().actions.empty());
        f.rows = {remote_row("A", "P1", 500)};
        CHECK(f.plan().actions.empty());
    }
    SECTION("a reappearing row is handled like any present row")
    {
        f.entry("A", linked("P1", 100, "h0", 1));
        f.locals = {local_preset("A", "h0")};
        f.rows   = {remote_row("A", "P1", 100)};
        CHECK(f.plan().actions[0].type == ActionType::SetAbsent);
        CHECK(f.plan().actions[0].absent_passes == 0);
    }
}

TEST_CASE("Planner handles remote renames", "[BblPresetSync]")
{
    Fixture f;
    f.entry("A", linked("P1", 100, "h0"));

    SECTION("a free new name is pulled and the old preset is renamed away")
    {
        f.locals = {local_preset("A", "h0")};
        f.rows   = {remote_row("B", "P1", 150)};
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 1);
        const Action& a = plan.actions[0];
        CHECK(a.type == ActionType::Pull);
        CHECK(a.key == fk("B"));
        CHECK(a.setting_id == "P1");
        CHECK(a.rename_from_set);
        CHECK(a.rename_from == fk("A"));
        CHECK(a.backup_local);
    }
    SECTION("without a body there is a note and no action")
    {
        f.locals = {local_preset("A", "h0")};
        f.rows   = {remote_row("B", "P1", 150, false)};
        auto plan = f.plan();
        CHECK(plan.actions.empty());
        CHECK(has_note(plan, "body unavailable for filament/B"));
    }
    SECTION("a taken new name marks the entry unresolvable and pairs the row")
    {
        f.locals = {local_preset("A", "h0", 0), local_preset("B", "hb", 5000)};
        f.rows   = {remote_row("B", "P1", 150, true, "hb")};
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 2);
        CHECK(plan.actions[0].type == ActionType::MarkUnresolvable);
        CHECK(plan.actions[0].key == fk("A"));
        CHECK(plan.actions[0].reason == "remote renamed onto an existing name");
        CHECK(plan.actions[1].type == ActionType::Link);
        CHECK(plan.actions[1].key == fk("B"));
        CHECK(plan.actions[1].setting_id == "P1");
    }
    SECTION("a missing old preset drops the entry and the row is pulled as new")
    {
        f.rows = {remote_row("B", "P1", 150)};
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 2);
        CHECK(plan.actions[0].type == ActionType::DropEntry);
        CHECK(plan.actions[0].key == fk("A"));
        CHECK(plan.actions[1].type == ActionType::Pull);
        CHECK(plan.actions[1].key == fk("B"));
        CHECK_FALSE(plan.actions[1].rename_from_set);
    }
    SECTION("an ineligible old preset is left alone and the row is not re-planned")
    {
        f.locals = {local_preset("A", "", 0, LocalState::Ineligible)};
        f.rows   = {remote_row("B", "P1", 150)};
        CHECK(f.plan().actions.empty());
    }
}

TEST_CASE("Planner handles non-linked ledger states", "[BblPresetSync]")
{
    Fixture f;

    SECTION("declined never acts")
    {
        LedgerEntry e = linked("P1", 100, "h0");
        e.state       = "declined";
        f.entry("A", e);
        f.locals = {local_preset("A", "h9")};
        f.rows   = {remote_row("A", "P1", 900)};
        CHECK(f.plan().actions.empty());
        f.rows.clear();
        CHECK(f.plan().actions.empty());
    }
    SECTION("too_large updates when the hash changed and the row exists, else creates")
    {
        LedgerEntry e = linked("P1", 100, "h0");
        e.state       = "too_large";
        f.entry("A", e);
        f.locals = {local_preset("A", "h1")};
        f.rows   = {remote_row("A", "P1", 100)};
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::Update);
        f.rows.clear();
        plan = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::Create);
    }
    SECTION("too_large with an unchanged hash or an ineligible preset does nothing")
    {
        LedgerEntry e = linked("P1", 100, "h0");
        e.state       = "too_large";
        f.entry("A", e);
        f.locals = {local_preset("A", "h0")};
        CHECK(f.plan().actions.empty());
        f.locals = {local_preset("A", "h1", 0, LocalState::Ineligible)};
        CHECK(f.plan().actions.empty());
        f.locals.clear();
        CHECK(f.plan().actions.empty());
    }
    SECTION("unresolvable with an unchanged row does nothing and the row is not re-planned")
    {
        LedgerEntry e = linked("P1", 100, "h0");
        e.state       = "unresolvable";
        f.entry("A", e);
        f.rows = {remote_row("A", "P1", 100)};
        auto plan = f.plan();
        CHECK(plan.actions.empty());
        CHECK(plan.notes.empty());
    }
    SECTION("unresolvable with a newer row is released to step B")
    {
        LedgerEntry e = linked("P1", 100, "h0");
        e.state       = "unresolvable";
        f.entry("A", e);
        f.rows = {remote_row("A", "P1", 101)};
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::Pull);
        CHECK(plan.actions[0].key == fk("A"));
        CHECK(plan.notes.empty());
    }
}

TEST_CASE("Planner pairs and creates for unknown rows and locals", "[BblPresetSync]")
{
    Fixture f;

    SECTION("a lower-id duplicate of a linked row is dropped and noted")
    {
        f.entry("A", linked("P1", 100, "h0"));
        f.locals = {local_preset("A", "h0")};
        f.rows   = {remote_row("A", "P1", 100), remote_row("A", "P2", 50)};
        auto plan = f.plan();
        CHECK(plan.actions.empty());
        CHECK(has_note(plan, "duplicate remote rows for filament/A"));
    }
    SECTION("unlinked row note when the entry has another id")
    {
        f.entry("A", linked("P1", 100, "h0"));
        f.locals = {local_preset("A", "h0")};
        f.rows   = {remote_row("A", "P9", 100)};
        auto plan = f.plan();
        CHECK(has_note(plan, "unlinked remote row for filament/A"));
        REQUIRE(find_action(plan, "A") != nullptr);
        CHECK(find_action(plan, "A")->type == ActionType::SetAbsent);
    }
    SECTION("ineligible local with an unknown row: no action")
    {
        f.locals = {local_preset("A", "", 0, LocalState::Ineligible)};
        f.rows   = {remote_row("A", "P1", 100)};
        CHECK(f.plan().actions.empty());
    }
    SECTION("pairing with equal hashes links")
    {
        f.locals = {local_preset("A", "h")};
        f.rows   = {remote_row("A", "P1", 100, true, "h")};
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::Link);
        CHECK(plan.actions[0].setting_id == "P1");
    }
    SECTION("pairing with different hashes: local wins updates with remote backup")
    {
        f.locals = {local_preset("A", "h", 1000)};
        f.rows   = {remote_row("A", "P1", 1120, true, "other")};
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::Update);
        CHECK(plan.actions[0].setting_id == "P1");
        CHECK(plan.actions[0].backup_remote);
    }
    SECTION("pairing with different hashes: remote wins pulls with local backup")
    {
        f.locals = {local_preset("A", "h", 1000)};
        f.rows   = {remote_row("A", "P1", 1121, true, "other")};
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::Pull);
        CHECK(plan.actions[0].backup_local);
    }
    SECTION("pairing without a body gives a note and no action")
    {
        f.locals = {local_preset("A", "h")};
        f.rows   = {remote_row("A", "P1", 100, false)};
        auto plan = f.plan();
        CHECK(plan.actions.empty());
        CHECK(has_note(plan, "body unavailable for filament/A"));
    }
    SECTION("a row with no local preset is pulled as new, or noted without a body")
    {
        f.rows = {remote_row("A", "P1", 100)};
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::Pull);
        f.rows = {remote_row("A", "P1", 100, false)};
        plan   = f.plan();
        CHECK(plan.actions.empty());
        CHECK(has_note(plan, "body unavailable for filament/A"));
    }
    SECTION("an eligible local with nothing on either side is created")
    {
        f.locals = {local_preset("A", "h"), local_preset("B", "", 0, LocalState::Ineligible)};
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::Create);
        CHECK(plan.actions[0].key == fk("A"));
        CHECK(plan.actions[0].setting_id.empty());
    }
    SECTION("no create when a ledger entry or a remote row exists at the key")
    {
        f.locals = {local_preset("A", "h"), local_preset("B", "h")};
        f.entry("A", [] { LedgerEntry e = linked("P1", 1, "h"); e.state = "declined"; return e; }());
        f.rows = {remote_row("B", "P2", 1, true, "h")};
        auto plan = f.plan();
        CHECK(find_action(plan, "A") == nullptr);
        REQUIRE(find_action(plan, "B") != nullptr);
        CHECK(find_action(plan, "B")->type == ActionType::Link);
    }
}

TEST_CASE("Planner reduces duplicate remote rows", "[BblPresetSync]")
{
    Fixture f;

    SECTION("the newest row wins, ties go to the greatest id, and a note is added once")
    {
        f.rows = {remote_row("A", "P1", 100), remote_row("A", "P3", 200), remote_row("A", "P2", 200)};
        auto plan = f.plan();
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::Pull);
        CHECK(plan.actions[0].setting_id == "P3");
        int notes = 0;
        for (const auto& n : plan.notes)
            if (n == "duplicate remote rows for filament/A") ++notes;
        CHECK(notes == 1);
    }
    SECTION("a row whose id is in the ledger is always kept")
    {
        f.entry("A", linked("P1", 100, "h0"));
        f.locals = {local_preset("A", "h0")};
        f.rows   = {remote_row("A", "P1", 100), remote_row("A", "P2", 300, true, "h0")};
        auto plan = f.plan();
        CHECK(has_note(plan, "duplicate remote rows for filament/A"));
        // The ledger's row is unchanged; the other row is an unlinked extra.
        CHECK(find_action(plan, "A") == nullptr);
        CHECK(has_note(plan, "unlinked remote row for filament/A"));
    }
}

TEST_CASE("Planner stops mass deletes", "[BblPresetSync]")
{
    // n entries are deleted/pulled per the scenario; the rest are unchanged linked entries.
    auto build_local_deletes = [](int deleted, int total) {
        Fixture f;
        for (int i = 0; i < total; ++i) {
            const std::string name = "k" + std::to_string(i);
            const bool        gone = i < deleted;
            f.entry(name, linked("P" + std::to_string(i), 100, "h", gone ? 1 : 0));
            f.locals.push_back(local_preset(name, "h"));
            if (!gone) f.rows.push_back(remote_row(name, "P" + std::to_string(i), 100));
        }
        return f;
    };
    auto build_remote_deletes = [](int deleted, int total) {
        Fixture f;
        for (int i = 0; i < total; ++i) {
            const std::string name = "k" + std::to_string(i);
            const bool        gone = i < deleted;
            f.entry(name, linked("P" + std::to_string(i), 100, "h"));
            if (!gone) f.locals.push_back(local_preset(name, "h"));
            f.rows.push_back(remote_row(name, "P" + std::to_string(i), 100));
        }
        return f;
    };

    SECTION("3 of 6 local deletes halt")
    {
        auto plan = build_local_deletes(3, 6).plan();
        CHECK(plan.halt == "mass_delete");
        CHECK(plan.halted_local_deletes.size() == 3);
        CHECK(plan.halted_remote_deletes.empty());
        CHECK(plan.actions.empty());
    }
    SECTION("3 of 7 local deletes go ahead")
    {
        auto plan = build_local_deletes(3, 7).plan();
        CHECK(plan.halt.empty());
        CHECK(plan.halted_local_deletes.empty());
        CHECK(plan.actions.size() == 3);
    }
    SECTION("2 of 2 local deletes are below the minimum")
    {
        auto plan = build_local_deletes(2, 2).plan();
        CHECK(plan.halt.empty());
        CHECK(plan.actions.size() == 2);
    }
    SECTION("3 of 6 remote deletes halt")
    {
        auto plan = build_remote_deletes(3, 6).plan();
        CHECK(plan.halt == "mass_delete");
        CHECK(plan.halted_remote_deletes.size() == 3);
        CHECK(plan.halted_local_deletes.empty());
        CHECK(plan.actions.empty());
    }
    SECTION("3 of 7 remote deletes go ahead")
    {
        auto plan = build_remote_deletes(3, 7).plan();
        CHECK(plan.halt.empty());
        CHECK(plan.halted_remote_deletes.empty());
        CHECK(plan.actions.size() == 3);
    }
    SECTION("the halt keeps the other actions")
    {
        Fixture f = build_local_deletes(3, 6);
        f.locals.push_back(local_preset("fresh", "h"));
        auto plan = f.plan();
        CHECK(plan.halt == "mass_delete");
        REQUIRE(plan.actions.size() == 1);
        CHECK(plan.actions[0].type == ActionType::Create);
    }
    SECTION("rename pulls are not counted as deletes")
    {
        Fixture f;
        for (int i = 0; i < 3; ++i) {
            const std::string old_name = "old" + std::to_string(i);
            f.entry(old_name, linked("P" + std::to_string(i), 100, "h"));
            f.locals.push_back(local_preset(old_name, "h"));
            f.rows.push_back(remote_row("new" + std::to_string(i), "P" + std::to_string(i), 150));
        }
        auto plan = f.plan();
        CHECK(plan.halt.empty());
        REQUIRE(plan.actions.size() == 3);
        for (const Action& a : plan.actions) CHECK(a.type == ActionType::Pull);
    }
}

TEST_CASE("Planner flags a dry run until the first run is confirmed", "[BblPresetSync]")
{
    Fixture f;
    f.locals = {local_preset("A", "h")};
    f.view.first_run_confirmed = false;
    auto plan = f.plan();
    CHECK(plan.dry_run);
    REQUIRE(plan.actions.size() == 1);
    CHECK(plan.actions[0].type == ActionType::Create);
    f.view.first_run_confirmed = true;
    CHECK_FALSE(f.plan().dry_run);
}

TEST_CASE("Planner emits at most one action per key in a mixed scenario", "[BblPresetSync]")
{
    Fixture f;
    f.view.clock_offset = 3;
    // k0 unchanged, k1 local edit, k2 remote edit, k3 conflict, k4 remote delete, k5 absent once,
    // k6 absent twice, k7 gone both sides, k8 remote rename, k9 unknown row, k10 new local, k11 ineligible.
    f.entry("k0", linked("P0", 100, "h"));
    f.locals.push_back(local_preset("k0", "h"));
    f.rows.push_back(remote_row("k0", "P0", 100));

    f.entry("k1", linked("P1", 100, "h"));
    f.locals.push_back(local_preset("k1", "h2"));
    f.rows.push_back(remote_row("k1", "P1", 100));

    f.entry("k2", linked("P2", 100, "h"));
    f.locals.push_back(local_preset("k2", "h"));
    f.rows.push_back(remote_row("k2", "P2", 200));

    f.entry("k3", linked("P3", 100, "h"));
    f.locals.push_back(local_preset("k3", "h2", 5000));
    f.rows.push_back(remote_row("k3", "P3", 200));

    f.entry("k4", linked("P4", 100, "h"));
    f.rows.push_back(remote_row("k4", "P4", 100));

    f.entry("k5", linked("P5", 100, "h"));
    f.locals.push_back(local_preset("k5", "h"));

    f.entry("k6", linked("P6", 100, "h", 1));
    f.locals.push_back(local_preset("k6", "h"));

    f.entry("k7", linked("P7", 100, "h"));

    f.entry("k8", linked("P8", 100, "h"));
    f.locals.push_back(local_preset("k8", "h"));
    f.rows.push_back(remote_row("k8b", "P8", 150));

    f.rows.push_back(remote_row("k9", "P9", 10));

    f.locals.push_back(local_preset("k10", "h"));
    f.locals.push_back(local_preset("k11", "", 0, LocalState::Ineligible));

    auto plan = f.plan();
    std::set<Key> seen;
    for (const Action& a : plan.actions) {
        CAPTURE(a.key.str());
        CHECK(seen.insert(a.key).second);
    }
    CHECK(find_action(plan, "k0") == nullptr);
    CHECK(find_action(plan, "k1")->type == ActionType::Update);
    CHECK(find_action(plan, "k2")->type == ActionType::Pull);
    CHECK(find_action(plan, "k3")->type == ActionType::Update);
    CHECK(find_action(plan, "k4")->type == ActionType::DeleteRemote);
    CHECK(find_action(plan, "k5")->type == ActionType::SetAbsent);
    CHECK(find_action(plan, "k6")->type == ActionType::DeleteLocal);
    CHECK(find_action(plan, "k7")->type == ActionType::DropEntry);
    CHECK(find_action(plan, "k8") == nullptr);
    CHECK(find_action(plan, "k8b")->type == ActionType::Pull);
    CHECK(find_action(plan, "k9")->type == ActionType::Pull);
    CHECK(find_action(plan, "k10")->type == ActionType::Create);
    CHECK(find_action(plan, "k11") == nullptr);
    CHECK(plan.halt.empty());
}
