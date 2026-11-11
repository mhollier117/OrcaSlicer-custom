#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/Utils/BblFilament/Linker.hpp"
#include "slic3r/Utils/BblFilament/Spool.hpp"

#include "bbl_filament_test_utils.hpp"

#include <set>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fil = Slic3r::BblFilament;
using namespace BblFilamentTest;

namespace {

fil::TrayFacts make_tray(const std::string& slot = "0", int ams_id = 0)
{
    fil::TrayFacts t;
    t.ams_id      = ams_id;
    t.ams_type    = 1;
    t.slot_id     = slot;
    t.occupied    = true;
    t.filament_id = "GFA00";
    t.type        = "PLA";
    t.sub_brands  = "PLA Basic";
    t.color       = "#FFFFFFFF";
    t.total_g     = 1000;
    t.remain      = 50;
    return t;
}

fil::TrayFacts make_rfid_tray(const std::string& slot, const std::string& uuid)
{
    fil::TrayFacts t = make_tray(slot);
    t.uuid           = uuid;
    t.tag_uid        = "0123456789AB01CD"; // 16 characters, "01" at offset 12
    t.official       = true;
    return t;
}

fil::PrinterView make_printer(const std::string& dev = "dev1")
{
    fil::PrinterView p;
    p.dev_id       = dev;
    p.name         = "Printer " + dev;
    p.bound        = true;
    p.fresh        = true;
    p.observations = 1;
    p.ams_sn[0]    = "sn0";
    return p;
}

fil::Spool make_row(const std::string& id, const std::string& rfid = "")
{
    fil::Spool s;
    s.id               = id;
    s.rfid             = rfid;
    s.filament_id      = "GFA00";
    s.vendor           = "Bambu Lab";
    s.type             = "PLA";
    s.name             = "PLA Basic";
    s.color            = "#FFFFFFFF";
    s.total_net_weight = 1000;
    s.net_weight       = 750;
    return s;
}

fil::Spool mounted_row(fil::Spool s, const std::string& dev, const std::string& sn, int ams_id, const std::string& slot, int ams_type = 1)
{
    s.in_printer = true;
    s.dev_id     = dev;
    s.ams_sn     = sn;
    s.ams_id     = ams_id;
    s.slot_id    = slot;
    s.ams_type   = ams_type;
    return s;
}

fil::Assignment make_assignment(const std::string& dev, const std::string& slot, const std::string& spool_id, long long created_at)
{
    fil::Assignment a;
    a.slot        = fil::SlotKey{dev, 0, slot};
    a.spool_id    = spool_id;
    a.filament_id = "GFA00";
    a.colors      = {"#FFFFFFFF"};
    a.created_at  = created_at;
    return a;
}

struct Ctx
{
    fil::LinkerState   state;
    fil::LinkConfig    config;
    long long          now{1000};
    unsigned long long serial{5};

    fil::LinkPlan plan(const std::vector<fil::PrinterView>& printers, const std::vector<fil::Spool>& rows, const std::set<std::string>& busy = {})
    {
        return fil::plan_links(printers, rows, busy, serial, state, config, now);
    }
};

fil::PrinterView with_tray(fil::PrinterView p, const fil::TrayFacts& t)
{
    p.trays.push_back(t);
    return p;
}

} // namespace

TEST_CASE("The linker plans nothing without a list or when it is off", "[BblFilament]")
{
    Ctx c;
    c.serial = 0;
    auto p   = with_tray(make_printer(), make_rfid_tray("0", "UUID1"));
    auto plan = c.plan({p}, {make_row("1", "UUID1")});
    CHECK_FALSE(plan.has_actions());
    CHECK(plan.notes.size() == 1);
    CHECK(plan.trays.empty());

    c.serial      = 5;
    c.config.mode = fil::LinkMode::Off;
    plan          = c.plan({p}, {make_row("1", "UUID1")});
    CHECK_FALSE(plan.has_actions());
    CHECK(plan.notes.size() == 1);
}

TEST_CASE("A printer that is not bound, not fresh or LAN-only and not selected takes no part", "[BblFilament]")
{
    const int variant = GENERATE(0, 1, 2);
    Ctx  c;
    auto p = with_tray(make_printer(), make_rfid_tray("0", "UUID1"));
    if (variant == 0) p.bound = false;
    if (variant == 1) p.fresh = false;
    if (variant == 2) p.lan_only = true;
    // A row that claims a slot of that printer is never unbound either.
    const auto claim = mounted_row(make_row("2"), "dev1", "sn0", 0, "3");
    for (int i = 0; i < 6; ++i) {
        p.observations = static_cast<unsigned long long>(i + 1);
        c.now += 20;
        c.serial += 1;
        const auto plan = c.plan({p}, {make_row("1", "UUID1"), claim});
        CHECK_FALSE(plan.has_actions());
        CHECK(plan.trays.empty());
    }
    CHECK(c.state.unmatched.empty());
}

TEST_CASE("A selected LAN printer takes part", "[BblFilament]")
{
    Ctx  c;
    auto p     = with_tray(make_printer(), make_rfid_tray("0", "UUID1"));
    p.lan_only = true;
    p.selected = true;
    CHECK(c.plan({p}, {make_row("1", "UUID1")}).mounts.size() == 1);
}

TEST_CASE("A tray with an RFID is matched to the row with that RFID and mounted", "[BblFilament]")
{
    Ctx  c;
    auto p    = with_tray(make_printer(), make_rfid_tray("0", "UUID1"));
    auto plan = c.plan({p}, {make_row("1", "UUID1")});
    REQUIRE(plan.mounts.size() == 1);
    CHECK(plan.mounts[0].spool_id == "1");
    CHECK(plan.mounts[0].rfid);
    CHECK(plan.mounts[0].ams_sn == "sn0");
    CHECK(plan.mounts[0].ams_type == 1);
    REQUIRE(plan.trays.size() == 1);
    CHECK(plan.trays[0].rule == "rfid");
    CHECK(plan.trays[0].spool_id == "1");
}

TEST_CASE("A tray with an unknown RFID never falls through to an assignment or a cloud pin", "[BblFilament]")
{
    Ctx  c;
    auto t        = make_tray("0");
    t.uuid        = "UNKNOWN";
    auto p        = with_tray(make_printer(), t);
    auto manual   = make_row("2");
    auto claiming = mounted_row(make_row("3"), "dev1", "sn0", 0, "0");
    c.state.assignments[fil::SlotKey{"dev1", 0, "0"}] = make_assignment("dev1", "0", "2", c.now);
    const auto plan = c.plan({p}, {manual, claiming});
    CHECK(plan.mounts.empty());
    REQUIRE(plan.trays.size() == 1);
    CHECK(plan.trays[0].rule.empty());
    CHECK(plan.trays[0].spool_id.empty());
}

TEST_CASE("An assignment matches only when the filament and the colour agree", "[BblFilament]")
{
    const auto run = [](const std::string& filament, const std::string& colour) {
        Ctx  c;
        auto t        = make_tray("0");
        t.filament_id = filament;
        t.color       = colour;
        c.state.assignments[fil::SlotKey{"dev1", 0, "0"}] = make_assignment("dev1", "0", "2", c.now);
        return c.plan({with_tray(make_printer(), t)}, {make_row("2")});
    };
    auto ok = run("GFA00", "#ffffffff");
    REQUIRE(ok.mounts.size() == 1);
    CHECK_FALSE(ok.mounts[0].rfid);
    CHECK_FALSE(ok.mounts[0].grams.has_value());
    CHECK(ok.trays[0].rule == "assignment");
    CHECK(run("GFB00", "#FFFFFFFF").mounts.empty());
    CHECK(run("GFA00", "#000000FF").mounts.empty());
    CHECK(run("", "#FFFFFFFF").mounts.empty());
    CHECK(run("GFA00", "").mounts.empty());
    CHECK(run("GFA00", "#FFFFFF").mounts.size() == 1); // alpha is ignored
}

TEST_CASE("An assignment with several colours needs the same list", "[BblFilament]")
{
    Ctx  c;
    auto t   = make_tray("0");
    t.colors = {"#FF0000FF", "#00FF00FF"};
    auto a   = make_assignment("dev1", "0", "2", c.now);
    a.colors = {"#ff0000", "#00ff00"};
    c.state.assignments[a.slot] = a;
    CHECK(c.plan({with_tray(make_printer(), t)}, {make_row("2")}).mounts.size() == 1);
    c.state.assignments[a.slot].colors = {"#ff0000"};
    CHECK(c.plan({with_tray(make_printer(), t)}, {make_row("2")}).mounts.empty());
}

TEST_CASE("The cloud's pin holds while the tray agrees and breaks when the filament or colour changes", "[BblFilament]")
{
    const auto claim = mounted_row(make_row("2"), "dev1", "sn0", 0, "0");
    {
        Ctx  c;
        auto plan = c.plan({with_tray(make_printer(), make_tray("0"))}, {claim});
        CHECK(plan.mounts.empty()); // mounted already
        REQUIRE(plan.trays.size() == 1);
        CHECK(plan.trays[0].rule == "cloud");
        CHECK(plan.trays[0].spool_id == "2");
    }
    {
        Ctx  c;
        auto t = make_tray("0");
        t.filament_id.clear();
        t.color.clear();
        CHECK(c.plan({with_tray(make_printer(), t)}, {claim}).trays[0].rule == "cloud"); // unknown values agree
    }
    {
        Ctx  c;
        auto t        = make_tray("0");
        t.filament_id = "GFB00";
        CHECK(c.plan({with_tray(make_printer(), t)}, {claim}).trays[0].rule.empty());
    }
    {
        Ctx  c;
        auto t  = make_tray("0");
        t.color = "#123456FF";
        CHECK(c.plan({with_tray(make_printer(), t)}, {claim}).trays[0].rule.empty());
    }
    {
        Ctx  c;
        auto t     = make_tray("0");
        t.occupied = false;
        CHECK(c.plan({with_tray(make_printer(), t)}, {claim}).trays[0].rule.empty());
    }
    {
        Ctx  c;
        auto row = mounted_row(make_row("2", "UUIDX"), "dev1", "sn0", 0, "0"); // a row with RFID is not pinned by the cloud
        CHECK(c.plan({with_tray(make_printer(), make_tray("0"))}, {row}).trays[0].rule.empty());
    }
}

TEST_CASE("One row is never matched by two trays", "[BblFilament]")
{
    Ctx  c;
    auto p = make_printer();
    p.trays.push_back(make_rfid_tray("0", "UUID1"));
    p.trays.push_back(make_rfid_tray("1", "UUID1"));
    const auto plan = c.plan({p}, {make_row("1", "UUID1")});
    REQUIRE(plan.mounts.size() == 1);
    CHECK(plan.mounts[0].slot.slot_id == "0");
    CHECK(plan.trays[1].spool_id.empty());
    CHECK(plan.registrations.empty()); // the account has that spool

    Ctx  d;
    auto q = make_printer();
    q.trays.push_back(make_tray("0"));
    q.trays.push_back(make_tray("1"));
    const auto plan2 = d.plan({q}, {mounted_row(make_row("2"), "dev1", "sn0", 0, "0")});
    CHECK(plan2.trays[0].spool_id == "2");
    CHECK(plan2.trays[1].spool_id.empty());
}

TEST_CASE("A mount for a manual row has no grams and one for an RFID row has the measured grams", "[BblFilament]")
{
    Ctx  c;
    auto p = make_printer();
    p.trays.push_back(make_rfid_tray("0", "UUID1"));
    p.trays.push_back(make_tray("1"));
    c.state.assignments[fil::SlotKey{"dev1", 0, "1"}] = make_assignment("dev1", "1", "2", c.now);
    const auto plan = c.plan({p}, {make_row("1", "UUID1"), make_row("2")});
    REQUIRE(plan.mounts.size() == 2);
    CHECK(plan.mounts[0].rfid);
    REQUIRE(plan.mounts[0].grams.has_value());
    CHECK(plan.mounts[0].grams.value() == 500);
    CHECK_FALSE(plan.mounts[1].rfid);
    CHECK_FALSE(plan.mounts[1].grams.has_value());
    CHECK(plan.mounts[1].spool_id == "2");
}

TEST_CASE("A spool that is mounted already needs no mount", "[BblFilament]")
{
    Ctx  c;
    auto t = make_rfid_tray("0", "UUID1");
    t.remain = 75; // equals the row's 750 g
    const auto row = mounted_row(make_row("1", "UUID1"), "dev1", "sn0", 0, "0");
    CHECK_FALSE(c.plan({with_tray(make_printer(), t)}, {row}).has_actions());
    // Any difference in the mount plans one.
    for (int i = 0; i < 5; ++i) {
        auto r = row;
        if (i == 0) r.dev_id = "other";
        if (i == 1) r.ams_sn = "snX";
        if (i == 2) r.ams_id = 1;
        if (i == 3) r.slot_id = "2";
        if (i == 4) r.ams_type = 4;
        Ctx d;
        CHECK(d.plan({with_tray(make_printer(), t)}, {r}).mounts.size() == 1);
    }
    auto r = row;
    r.in_printer = false;
    Ctx d;
    CHECK(d.plan({with_tray(make_printer(), t)}, {r}).mounts.size() == 1);
}

TEST_CASE("Without an AMS serial nothing is sent and nothing is unbound", "[BblFilament]")
{
    Ctx  c;
    auto p = with_tray(make_printer(), make_rfid_tray("0", "UUID1"));
    p.ams_sn.clear();
    auto stale = mounted_row(make_row("2"), "dev1", "", 0, "3"); // claims a slot, but carries no serial
    for (int i = 0; i < 6; ++i) {
        p.observations = static_cast<unsigned long long>(i + 1);
        c.now += 20;
        c.serial += 1;
        const auto plan = c.plan({p}, {make_row("1", "UUID1"), stale});
        CHECK_FALSE(plan.has_actions());
        REQUIRE(plan.trays.size() == 1);
        CHECK(plan.trays[0].spool_id == "1"); // matching still counts
    }
    // An external tray gets nothing either.
    Ctx  d;
    auto t     = make_rfid_tray("0", "UUID1");
    t.external = true;
    t.ams_id   = 255;
    auto q     = with_tray(make_printer(), t);
    q.ams_sn[255] = "sn255";
    CHECK_FALSE(d.plan({q}, {make_row("1", "UUID1")}).has_actions());
}

TEST_CASE("A busy row is left alone", "[BblFilament]")
{
    Ctx  c;
    auto p = with_tray(make_printer(), make_rfid_tray("0", "UUID1"));
    const auto plan = c.plan({p}, {make_row("1", "UUID1")}, {"1"});
    CHECK_FALSE(plan.has_actions());
    CHECK_FALSE(plan.notes.empty());

    // A busy row that claims a slot is not unbound and its streak does not move.
    Ctx  d;
    auto q     = make_printer();
    auto claim = mounted_row(make_row("2"), "dev1", "sn0", 0, "3");
    d.plan({q}, {claim}, {"2"});
    CHECK(d.state.unmatched.empty());
}

TEST_CASE("A weight is pushed when the measured value is valid and differs", "[BblFilament]")
{
    const auto row = mounted_row(make_row("1", "UUID1"), "dev1", "sn0", 0, "0"); // 750 g
    {
        Ctx  c;
        auto t = make_rfid_tray("0", "UUID1");
        t.remain = 50;
        const auto plan = c.plan({with_tray(make_printer(), t)}, {row});
        REQUIRE(plan.weights.size() == 1);
        CHECK(plan.weights[0].grams == 500);
        CHECK(plan.weights[0].spool_id == "1");
        CHECK(plan.mounts.empty());
    }
    {
        Ctx  c;
        auto t = make_rfid_tray("0", "UUID1");
        t.remain = 75;
        CHECK_FALSE(c.plan({with_tray(make_printer(), t)}, {row}).has_actions());
    }
    for (int variant = 0; variant < 5; ++variant) {
        Ctx  c;
        auto t = make_rfid_tray("0", "UUID1");
        if (variant == 0) t.reading = true;
        if (variant == 1) t.remain = -1;
        if (variant == 2) t.remain = 101;
        if (variant == 3) t.total_g = 0;
        auto r = row;
        if (variant == 3) r.total_net_weight = 0;
        if (variant == 4) t.remain = 75; // equals the row's 750 g
        CHECK_FALSE(c.plan({with_tray(make_printer(), t)}, {r}).has_actions());
    }
    {
        // The total falls back to the row's when the tray reports none.
        Ctx  c;
        auto t   = make_rfid_tray("0", "UUID1");
        t.total_g = 0;
        t.remain  = 50;
        const auto plan = c.plan({with_tray(make_printer(), t)}, {row});
        REQUIRE(plan.weights.size() == 1);
        CHECK(plan.weights[0].grams == 500);
    }
}

TEST_CASE("A busy printer gets a weight at most every ten minutes", "[BblFilament]")
{
    const auto row = mounted_row(make_row("1", "UUID1"), "dev1", "sn0", 0, "0");
    Ctx  c;
    auto p = with_tray(make_printer(), make_rfid_tray("0", "UUID1"));
    p.busy = true;
    c.state.pushed["UUID1"] = {c.now - 100, 500};
    auto plan = c.plan({p}, {row});
    CHECK(plan.weights.empty());
    CHECK(plan.skipped_cooldown["dev1"] == 1);
    CHECK_FALSE(plan.notes.empty());

    c.now += 600;
    CHECK(c.plan({p}, {row}).weights.size() == 1);

    // An idle printer is not held back.
    Ctx d;
    d.state.pushed["UUID1"] = {d.now - 100, 500};
    p.busy                  = false;
    CHECK(d.plan({p}, {row}).weights.size() == 1);
    // A busy printer with nothing pushed yet is not held back either.
    Ctx e;
    p.busy = true;
    CHECK(e.plan({p}, {row}).weights.size() == 1);
}

TEST_CASE("An unbind needs three different observations, thirty seconds and a newer list", "[BblFilament]")
{
    Ctx  c;
    auto p     = make_printer();
    auto t     = make_tray("0");
    t.occupied = false;
    p.trays.push_back(t);
    const auto claim = mounted_row(make_row("2"), "dev1", "sn0", 0, "0");

    p.observations = 1;
    CHECK_FALSE(c.plan({p}, {claim}).has_actions());
    CHECK(c.state.unmatched["2"].count == 1);
    c.now += 20;
    CHECK_FALSE(c.plan({p}, {claim}).has_actions()); // the same observation again does not count
    CHECK(c.state.unmatched["2"].count == 1);
    p.observations = 2;
    c.now += 1;
    c.plan({p}, {claim});
    CHECK(c.state.unmatched["2"].count == 2);
    p.observations = 3; // count 3, but only 21 seconds
    CHECK_FALSE(c.plan({p}, {claim}).has_actions());
    p.observations = 4;
    c.now += 10; // 31 s and count 4, but the list is the one the streak began with
    auto plan = c.plan({p}, {claim});
    CHECK(plan.unbinds.empty());
    CHECK(plan.want_pull);
    c.serial += 1;
    p.observations = 5;
    plan           = c.plan({p}, {claim});
    REQUIRE(plan.unbinds.size() == 1);
    CHECK_FALSE(plan.want_pull);
    CHECK(plan.unbinds[0].spool_id == "2");
    CHECK(plan.unbinds[0].ams_sn == "sn0");
    CHECK(plan.unbinds[0].ams_type == 1);
    CHECK(plan.unbinds[0].slot.slot_id == "0");
}

TEST_CASE("The unbind streak counts a restarted observation number as a new observation", "[BblFilament]")
{
    Ctx  c;
    auto p = make_printer();
    const auto claim = mounted_row(make_row("2"), "dev1", "sn0", 0, "0");
    p.observations   = 50;
    c.plan({p}, {claim});
    p.observations = 2; // a full state restarted the count
    c.plan({p}, {claim});
    CHECK(c.state.unmatched["2"].count == 2);
}

TEST_CASE("A row matched by another printer's tray is not unbound", "[BblFilament]")
{
    Ctx  c;
    auto p1 = make_printer("dev1");
    auto p2 = make_printer("dev2");
    p2.ams_sn[0] = "sn2";
    p2.trays.push_back(make_rfid_tray("0", "UUID1"));
    const auto row = mounted_row(make_row("1", "UUID1"), "dev1", "sn0", 0, "0"); // the cloud says dev1, the tray is on dev2
    for (int i = 0; i < 6; ++i) {
        p1.observations = static_cast<unsigned long long>(i + 1);
        c.now += 20;
        c.serial += 1;
        const auto plan = c.plan({p1, p2}, {row});
        CHECK(plan.unbinds.empty());
        REQUIRE(plan.mounts.size() == 1);
        CHECK(plan.mounts[0].slot.dev_id == "dev2");
    }
    CHECK(c.state.unmatched.empty());
}

TEST_CASE("A replacement mounts the new spool and never unbinds the old one", "[BblFilament]")
{
    Ctx  c;
    auto p = with_tray(make_printer(), make_rfid_tray("0", "UUIDB"));
    const auto a = mounted_row(make_row("1", "UUIDA"), "dev1", "sn0", 0, "0");
    for (int i = 0; i < 6; ++i) {
        p.observations = static_cast<unsigned long long>(i + 1);
        c.now += 20;
        c.serial += 1;
        const auto plan = c.plan({p}, {a, make_row("2", "UUIDB")});
        CHECK(plan.unbinds.empty());
        REQUIRE(plan.mounts.size() == 1);
        CHECK(plan.mounts[0].spool_id == "2");
    }
    CHECK(c.state.unmatched.empty());
}

TEST_CASE("A stale claim on a slot whose tray matched another spool is never unbound", "[BblFilament]")
{
    Ctx  c;
    auto p = with_tray(make_printer(), make_rfid_tray("0", "UUIDB"));
    const auto a = mounted_row(make_row("1"), "dev1", "sn0", 0, "0");
    const auto b = mounted_row(make_row("2", "UUIDB"), "dev1", "sn0", 0, "0"); // mounted already: no mount planned
    for (int i = 0; i < 6; ++i) {
        p.observations = static_cast<unsigned long long>(i + 1);
        c.now += 20;
        c.serial += 1;
        const auto plan = c.plan({p}, {a, b});
        CHECK(plan.unbinds.empty());
        CHECK(plan.mounts.empty());
        CHECK_FALSE(plan.want_pull);
    }
    CHECK(c.state.unmatched.empty());
}

TEST_CASE("A row whose AMS unit is gone is unbound by the same rule", "[BblFilament]")
{
    Ctx  c;
    auto p = with_tray(make_printer(), make_tray("0"));
    const auto gone = mounted_row(make_row("2"), "dev1", "snGone", 1, "0");
    fil::LinkPlan plan;
    for (int i = 0; i < 4; ++i) {
        p.observations = static_cast<unsigned long long>(i + 1);
        c.now += 11;
        plan = c.plan({p}, {gone});
    }
    CHECK(plan.want_pull);
    c.serial += 1;
    p.observations = 9;
    plan           = c.plan({p}, {gone});
    REQUIRE(plan.unbinds.size() == 1);
    CHECK(plan.unbinds[0].slot.ams_id == 1);
    CHECK(plan.unbinds[0].ams_sn == "snGone");
}

TEST_CASE("The unbind rules respect the slot choice, refusals and an assignment", "[BblFilament]")
{
    const auto claim = mounted_row(make_row("2"), "dev1", "sn0", 0, "0");
    const auto run   = [&](Ctx& c, bool assigned) {
        auto p = make_printer();
        if (assigned) c.state.assignments[fil::SlotKey{"dev1", 0, "0"}] = make_assignment("dev1", "0", "9", c.now + 100000);
        fil::LinkPlan plan;
        for (int i = 0; i < 4; ++i) {
            p.observations = static_cast<unsigned long long>(i + 1);
            c.now += 11;
            plan = c.plan({p}, {claim});
        }
        c.serial += 1;
        p.observations = 9;
        return c.plan({p}, {claim});
    };
    {
        Ctx c;
        CHECK(run(c, false).unbinds.size() == 1);
    }
    {
        Ctx c;
        c.config.only_slots = {"dev1:0:1"};
        CHECK(run(c, false).unbinds.empty());
    }
    {
        Ctx c;
        c.config.only_slots = {"dev1:0:0"};
        CHECK(run(c, false).unbinds.size() == 1);
    }
    {
        Ctx c;
        c.state.refused["dev1:0:0|"] = {c.now + 100000, 3};
        CHECK(run(c, false).unbinds.empty());
    }
    {
        Ctx c;
        CHECK(run(c, true).unbinds.empty());
    }
}

TEST_CASE("A new official spool is registered only when every condition holds", "[BblFilament]")
{
    const auto run = [](const std::function<void(fil::TrayFacts&, fil::PrinterView&, Ctx&)>& tweak) {
        Ctx  c;
        auto t = make_rfid_tray("0", "UUID1");
        auto p = make_printer();
        tweak(t, p, c);
        p.trays.push_back(t);
        return c.plan({p}, {}).registrations;
    };
    const auto regs = run([](auto&, auto&, auto&) {});
    REQUIRE(regs.size() == 1);
    CHECK(regs[0].ams_sn == "sn0");
    CHECK(regs[0].tray.uuid == "UUID1");
    CHECK(run([](auto& t, auto&, auto&) { t.official = false; }).empty());
    CHECK(run([](auto& t, auto&, auto&) { t.uuid.clear(); }).empty());
    CHECK(run([](auto& t, auto&, auto&) { t.uuid = "0000000000000000"; }).empty());
    CHECK(run([](auto&, auto& p, auto&) { p.lan_only = true; p.selected = true; }).empty());
    CHECK(run([](auto& t, auto&, auto&) { t.ams_type = 2; }).empty());
    CHECK(run([](auto&, auto& p, auto&) { p.ams_sn.clear(); }).empty());
    CHECK(run([](auto& t, auto&, auto&) { t.external = true; }).empty());
    CHECK(run([](auto&, auto&, auto& c) { c.state.registrations["UUID1"].done = true; }).empty());
    CHECK(run([](auto&, auto&, auto& c) { c.state.registrations["UUID1"].next_at = c.now + 10; }).empty());
    CHECK(run([](auto&, auto&, auto& c) { c.state.registrations["UUID1"].next_at = c.now; }).size() == 1);
    CHECK(run([](auto&, auto&, auto& c) { c.config.only_slots = {"dev1:0:1"}; }).empty());
}

TEST_CASE("Only the chosen slots are touched", "[BblFilament]")
{
    Ctx  c;
    auto p = make_printer();
    p.trays.push_back(make_rfid_tray("0", "UUID1"));
    p.trays.push_back(make_rfid_tray("1", "UUID2"));
    c.config.only_slots = {"dev1:0:1"};
    const auto plan     = c.plan({p}, {make_row("1", "UUID1"), make_row("2", "UUID2")});
    REQUIRE(plan.mounts.size() == 1);
    CHECK(plan.mounts[0].spool_id == "2");
    CHECK(plan.registrations.empty());
}

TEST_CASE("A refusal keeps a spool away from its slot until it runs out", "[BblFilament]")
{
    Ctx  c;
    auto p = with_tray(make_printer(), make_rfid_tray("0", "UUID1"));
    c.state.refused["dev1:0:0|1"] = {c.now + 600, 5};
    CHECK_FALSE(c.plan({p}, {make_row("1", "UUID1")}).has_actions());
    c.now += 600;
    CHECK(c.plan({p}, {make_row("1", "UUID1")}).mounts.size() == 1);
    CHECK(c.state.refused.empty());
}

TEST_CASE("An assignment older than two minutes is dropped", "[BblFilament]")
{
    Ctx  c;
    c.state.assignments[fil::SlotKey{"dev1", 0, "0"}] = make_assignment("dev1", "0", "2", c.now);
    auto p = with_tray(make_printer(), make_tray("0"));
    c.now += fil::k_assignment_seconds;
    CHECK(c.plan({p}, {make_row("2")}).mounts.size() == 1);
    c.now += 1;
    const auto plan = c.plan({p}, {make_row("2")});
    CHECK(plan.mounts.empty());
    CHECK(c.state.assignments.empty());
    CHECK_FALSE(plan.notes.empty());
}

TEST_CASE("A matched assignment whose row is mounted is dropped", "[BblFilament]")
{
    Ctx  c;
    c.state.assignments[fil::SlotKey{"dev1", 0, "0"}] = make_assignment("dev1", "0", "2", c.now);
    auto p = with_tray(make_printer(), make_tray("0"));
    c.plan({p}, {make_row("2")});
    CHECK(c.state.assignments.size() == 1); // not mounted yet
    c.plan({p}, {mounted_row(make_row("2"), "dev1", "sn0", 0, "0")});
    CHECK(c.state.assignments.empty());
}

TEST_CASE("A dry run plans like a live run", "[BblFilament]")
{
    Ctx  c;
    auto p = with_tray(make_printer(), make_rfid_tray("0", "UUID1"));
    c.config.mode = fil::LinkMode::DryRun;
    CHECK(c.plan({p}, {make_row("1", "UUID1")}).mounts.size() == 1);
}

TEST_CASE("Plans list actions in printer and slot order", "[BblFilament]")
{
    Ctx  c;
    auto p2 = make_printer("dev2");
    p2.trays.push_back(make_rfid_tray("0", "UUID3"));
    auto p1 = make_printer("dev1");
    p1.trays.push_back(make_rfid_tray("1", "UUID2"));
    p1.trays.push_back(make_rfid_tray("0", "UUID1"));
    const auto plan = c.plan({p2, p1}, {make_row("1", "UUID1"), make_row("2", "UUID2"), make_row("3", "UUID3")});
    REQUIRE(plan.mounts.size() == 3);
    CHECK(plan.mounts[0].spool_id == "1");
    CHECK(plan.mounts[1].spool_id == "2");
    CHECK(plan.mounts[2].spool_id == "3");
}

TEST_CASE("An ams item from a row keeps the row's weight when no grams are given", "[BblFilament]")
{
    auto row   = make_row("1", "UUID1");
    row.note   = "n";
    row.tray_id_name = "A00-W1";
    auto item  = fil::ams_item_from_row(row, "sn0", 0, 1, "2", std::nullopt);
    CHECK(item.rfid == "UUID1");
    CHECK(item.net_weight == 750);
    CHECK(item.total_net_weight == 1000);
    CHECK(item.color_type == 2);
    REQUIRE(item.colors.size() == 1);
    CHECK(item.colors[0] == "#FFFFFFFF");
    CHECK(item.ams_sn == "sn0");
    CHECK(item.slot_id == "2");
    CHECK(item.ams_type == 1);
    CHECK(item.note == "n");
    CHECK_FALSE(item.create_new);
    CHECK_FALSE(item.is_support);
    CHECK(fil::ams_item_from_row(row, "sn0", 0, 1, "2", 123).net_weight == 123);

    row.colors     = {"#FF0000FF", "#00FF00FF"};
    row.color_type = 1;
    item           = fil::ams_item_from_row(row, "sn0", 0, 1, "2", std::nullopt);
    CHECK(item.color_type == 1);
    CHECK(item.colors.size() == 2);
}

TEST_CASE("An ams item from a tray registers an official spool", "[BblFilament]")
{
    auto t = make_rfid_tray("2", "UUID1");
    t.tray_id_name = "A00-W1";
    t.remain       = 40;
    fil::FilamentLookup lookup;
    lookup.is_support = [](const std::string& id) { return std::optional<bool>(id == "GFA00"); };
    auto item         = fil::ams_item_from_tray(t, "sn0", lookup);
    CHECK(item.rfid == "UUID1");
    CHECK(item.vendor == "Bambu Lab");
    CHECK(item.type == "PLA");
    CHECK(item.name == "PLA Basic");
    CHECK(item.filament_id == "GFA00");
    CHECK(item.tray_id_name == "A00-W1");
    CHECK(item.total_net_weight == 1000);
    CHECK(item.net_weight == 400);
    CHECK(item.color_type == 2);
    CHECK(item.is_support);
    CHECK(item.slot_id == "2");
    CHECK(item.ams_sn == "sn0");
    CHECK_FALSE(item.create_new);

    t.reading = true;
    CHECK(fil::ams_item_from_tray(t, "sn0", {}).net_weight == 1000);
    CHECK_FALSE(fil::ams_item_from_tray(t, "sn0", {}).is_support);

    t.colors     = {"#FF0000FF", "#00FF00FF"};
    t.color_type = 0;
    CHECK(fil::ams_item_from_tray(t, "sn0", {}).color_type == 0);
    t.color_type = 7;
    CHECK(fil::ams_item_from_tray(t, "sn0", {}).color_type == 1);
}

TEST_CASE("Slot items carry the mount and the spool", "[BblFilament]")
{
    const auto b = fil::slot_bind("sn0", 1, 4, "2", make_row("77"));
    CHECK(b.spool_id == "77");
    CHECK(b.ams_sn == "sn0");
    CHECK(b.ams_id == 1);
    CHECK(b.ams_type == 4);
    CHECK(b.slot_id == "2");
    CHECK(b.rfid.empty());
    const auto u = fil::slot_unbind("sn0", 1, 4, "2");
    CHECK(u.spool_id.empty());
    CHECK(u.rfid.empty());
    CHECK(u.slot_id == "2");
    CHECK(fil::slot_bind("sn0", 0, 1, "0", make_row("5", "UUID1")).rfid == "UUID1");
}

TEST_CASE("Colours are compared by RGB only", "[BblFilament]")
{
    CHECK(fil::same_rgb("#FFAA00FF", "#ffaa00"));
    CHECK(fil::same_rgb("#FFAA0000", "#FFAA00FF"));
    CHECK_FALSE(fil::same_rgb("#FFAA00FF", "#FFAA01FF"));
    CHECK_FALSE(fil::same_rgb("", "#FFAA00"));
    CHECK_FALSE(fil::same_rgb("", ""));
}

TEST_CASE("The measured grams need a valid reading and a total", "[BblFilament]")
{
    auto t = make_tray();
    CHECK(fil::measured_grams(t, make_row("1")).value() == 500);
    t.remain = 0;
    CHECK(fil::measured_grams(t, make_row("1")).value() == 0);
    t.remain = 100;
    CHECK(fil::measured_grams(t, make_row("1")).value() == 1000);
    t.remain = -1;
    CHECK_FALSE(fil::measured_grams(t, make_row("1")).has_value());
    t.remain  = 50;
    t.reading = true;
    CHECK_FALSE(fil::measured_grams(t, make_row("1")).has_value());
    t.reading = false;
    t.total_g = 0;
    CHECK(fil::measured_grams(t, make_row("1")).value() == 500);
    auto row = make_row("1");
    row.total_net_weight = 0;
    CHECK_FALSE(fil::measured_grams(t, row).has_value());
}

TEST_CASE("RFID evidence moves a row claimed by a printer that does not take part", "[BblFilament]")
{
    Ctx c;
    auto excluded = make_printer("old");
    excluded.fresh = false;
    excluded.trays.push_back(make_rfid_tray("1", "IGNORED"));
    auto p = with_tray(make_printer(), make_rfid_tray("0", "UUID1"));
    const auto row = mounted_row(make_row("1", "UUID1"), "old", "old-sn", 0, "1");
    const auto plan = c.plan({excluded, p}, {row});
    REQUIRE(plan.mounts.size() == 1);
    CHECK(plan.mounts[0].slot == (fil::SlotKey{"dev1", 0, "0"}));
    CHECK(plan.unbinds.empty());
    CHECK(plan.weights.empty());
    CHECK(plan.trays.size() == 1);
    CHECK(c.state.unmatched.empty());
}

TEST_CASE("An assignment moves a manual row from an excluded printer", "[BblFilament]")
{
    Ctx c;
    const auto a = make_assignment("dev1", "0", "1", c.now);
    c.state.assignments[a.slot] = a;
    const auto row = mounted_row(make_row("1"), "old", "old-sn", 0, "1");
    const auto plan = c.plan({with_tray(make_printer(), make_tray())}, {row});
    REQUIRE(plan.mounts.size() == 1);
    CHECK(plan.mounts[0].slot == a.slot);
    CHECK(plan.unbinds.empty());
}

TEST_CASE("An RFID mount omits a measurement that is still being read", "[BblFilament]")
{
    Ctx c;
    auto t = make_rfid_tray("0", "UUID1");
    t.reading = true;
    const auto plan = c.plan({with_tray(make_printer(), t)}, {make_row("1", "UUID1")});
    REQUIRE(plan.mounts.size() == 1);
    CHECK_FALSE(plan.mounts[0].grams.has_value());
}

TEST_CASE("An all-zero RFID permits an assignment match", "[BblFilament]")
{
    Ctx c;
    auto t = make_tray();
    t.uuid = "000000";
    const auto a = make_assignment("dev1", "0", "1", c.now);
    c.state.assignments[a.slot] = a;
    const auto plan = c.plan({with_tray(make_printer(), t)}, {make_row("1")});
    REQUIRE(plan.trays.size() == 1);
    CHECK(plan.trays[0].rule == "assignment");
}

TEST_CASE("RFID matches across printers take priority over earlier assignments", "[BblFilament]")
{
    Ctx c;
    const auto a = make_assignment("dev1", "0", "1", c.now);
    c.state.assignments[a.slot] = a;
    const auto plan = c.plan({with_tray(make_printer(), make_tray()),
                             with_tray(make_printer("dev2"), make_rfid_tray("0", "UUID1"))}, {make_row("1", "UUID1")});
    REQUIRE(plan.mounts.size() == 1);
    CHECK(plan.mounts[0].slot.dev_id == "dev2");
    CHECK(plan.trays[0].rule.empty());
    CHECK(plan.trays[1].rule == "rfid");
}

TEST_CASE("A multicolour assignment compares each colour in order", "[BblFilament]")
{
    Ctx c;
    auto a = make_assignment("dev1", "0", "1", c.now);
    a.colors = {"#FF0000", "#00FF00"};
    c.state.assignments[a.slot] = a;
    auto t = make_tray();
    t.colors = {"#00FF00", "#FF0000"};
    CHECK(c.plan({with_tray(make_printer(), t)}, {make_row("1")}).mounts.empty());
}

TEST_CASE("A single-colour assignment needs the tray colour itself", "[BblFilament]")
{
    Ctx c;
    const auto a = make_assignment("dev1", "0", "1", c.now);
    c.state.assignments[a.slot] = a;
    auto t = make_tray();
    t.colors = {t.color};
    t.color.clear();
    CHECK(c.plan({with_tray(make_printer(), t)}, {make_row("1")}).mounts.empty());
}

TEST_CASE("Unmatched streaks disappear when rows vanish or stop claiming an eligible printer", "[BblFilament]")
{
    Ctx c;
    auto p = make_printer();
    auto row = mounted_row(make_row("1"), "dev1", "sn0", 0, "0");
    c.plan({p}, {row});
    REQUIRE(c.state.unmatched.size() == 1);
    c.plan({p}, {});
    CHECK(c.state.unmatched.empty());
    c.plan({p}, {row});
    row.in_printer = false;
    c.plan({p}, {row});
    CHECK(c.state.unmatched.empty());
    row.in_printer = true;
    c.plan({p}, {row});
    p.fresh = false;
    c.plan({p}, {row});
    CHECK(c.state.unmatched.empty());
}

TEST_CASE("A matched row loses its unmatched streak even when it is busy", "[BblFilament]")
{
    Ctx c;
    auto row = mounted_row(make_row("1", "UUID1"), "dev1", "sn0", 0, "0");
    c.plan({make_printer()}, {row});
    REQUIRE(c.state.unmatched.size() == 1);
    c.plan({with_tray(make_printer(), make_rfid_tray("0", "UUID1"))}, {row}, {"1"});
    CHECK(c.state.unmatched.empty());
}

TEST_CASE("Item colour fallback preserves even an empty primary colour", "[BblFilament]")
{
    auto row = make_row("1");
    row.color.clear();
    auto item = fil::ams_item_from_row(row, "sn0", 0, 1, "0", std::nullopt);
    REQUIRE(item.colors.size() == 1);
    CHECK(item.colors[0].empty());
    auto tray = make_tray();
    tray.color.clear();
    item = fil::ams_item_from_tray(tray, "sn0", {});
    REQUIRE(item.colors.size() == 1);
    CHECK(item.colors[0].empty());
}

TEST_CASE("Slot keys compare device then AMS then slot", "[BblFilament]")
{
    const fil::SlotKey k{"dev1", 1, "2"};
    CHECK(k.text() == "dev1:1:2");
    CHECK(k == (fil::SlotKey{"dev1", 1, "2"}));
    CHECK(k < (fil::SlotKey{"dev2", 0, "0"}));
    CHECK(k < (fil::SlotKey{"dev1", 2, "0"}));
    CHECK(k < (fil::SlotKey{"dev1", 1, "3"}));
}
