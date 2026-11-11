#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/Utils/BblFilament/LinkDriver.hpp"
#include "slic3r/Utils/BblFilament/Linker.hpp"
#include "slic3r/Utils/BblFilament/Spool.hpp"
#include "slic3r/Utils/BblFilament/SpoolCloud.hpp"
#include "slic3r/Utils/BblFilament/SpoolService.hpp"

#include "bbl_filament_test_utils.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <set>
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
    // Two occupied trays with the same RFID are evidence for neither (see the test of two printers below).
    Ctx  c;
    auto p = make_printer();
    p.trays.push_back(make_rfid_tray("0", "UUID1"));
    p.trays.push_back(make_rfid_tray("1", "UUID1"));
    const auto plan = c.plan({p}, {make_row("1", "UUID1")});
    CHECK(plan.mounts.empty());
    CHECK(plan.trays[0].spool_id.empty());
    CHECK(plan.trays[1].spool_id.empty());
    CHECK(plan.registrations.empty());

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

TEST_CASE("The grams the printer reports win over the percent", "[BblFilament]")
{
    auto t     = make_tray(); // 50 % of 1000 g
    t.remain_g = 304;
    CHECK(fil::measured_grams(t, make_row("1")).value() == 304);
    t.remain_g = 0;
    CHECK(fil::measured_grams(t, make_row("1")).value() == 0);
    t.remain   = -1; // no percent: the grams alone are enough
    t.remain_g = 120;
    CHECK(fil::measured_grams(t, make_row("1")).value() == 120);
    t.reading = true; // nothing while the tray is being read
    CHECK_FALSE(fil::measured_grams(t, make_row("1")).has_value());
}

TEST_CASE("A weight push uses the grams the printer reports", "[BblFilament]")
{
    Ctx  c;
    auto t     = make_rfid_tray("0", "UUID1");
    t.remain   = 30;
    t.remain_g = 300;
    auto row       = mounted_row(make_row("1", "UUID1"), "dev1", "sn0", 0, "0");
    row.net_weight = 304; // its percent is 30 too, but the printer reports grams
    const auto plan = c.plan({with_tray(make_printer(), t)}, {row});
    REQUIRE(plan.weights.size() == 1);
    CHECK(plan.weights[0].grams == 300);
}

TEST_CASE("A percent does not overrule a row whose own percent is the same", "[BblFilament]")
{
    auto row       = make_row("1", "UUID1");
    row.net_weight = 304; // of 1000 g
    auto t         = make_rfid_tray("0", "UUID1");
    t.remain       = 30;
    {
        Ctx c;
        CHECK_FALSE(c.plan({with_tray(make_printer(), t)}, {mounted_row(row, "dev1", "sn0", 0, "0")}).has_actions());
    }
    {
        // A mount sends the row's own weight.
        Ctx        c;
        const auto plan = c.plan({with_tray(make_printer(), t)}, {row});
        REQUIRE(plan.mounts.size() == 1);
        CHECK_FALSE(plan.mounts[0].grams.has_value());
        CHECK(fil::ams_item_from_row(row, "sn0", 0, 1, "0", plan.mounts[0].grams).net_weight == 304);
    }
    {
        // A percent that differs still counts.
        Ctx c;
        t.remain        = 29;
        const auto plan = c.plan({with_tray(make_printer(), t)}, {mounted_row(row, "dev1", "sn0", 0, "0")});
        REQUIRE(plan.weights.size() == 1);
        CHECK(plan.weights[0].grams == 290);
    }
}

TEST_CASE("A percent a little above the stored weight does not raise it", "[BblFilament]")
{
    const auto plan_for = [](long long row_g, int remain, int remain_g = -1) {
        auto row       = mounted_row(make_row("1", "UUID1"), "dev1", "sn0", 0, "0");
        row.net_weight = static_cast<double>(row_g); // of 1000 g
        auto t         = make_rfid_tray("0", "UUID1");
        t.remain       = remain;
        t.remain_g     = remain_g;
        Ctx c;
        return c.plan({with_tray(make_printer(), t)}, {row});
    };
    const auto has_note = [](const fil::LinkPlan& plan) {
        for (const std::string& n : plan.notes)
            if (n == "The printer's estimate for spool 1 is a little above the stored weight: kept.") return true;
        return false;
    };
    {
        const auto plan = plan_for(90, 11); // 110 g against 90 g stored
        CHECK(plan.weights.empty());
        CHECK_FALSE(plan.has_actions());
        CHECK(has_note(plan));
        REQUIRE(plan.trays.size() == 1);
        CHECK_FALSE(plan.trays[0].note.empty());
    }
    {
        const auto plan = plan_for(90, 14); // 50 g above: still inside 5 % of 1000 g
        CHECK(plan.weights.empty());
        CHECK(has_note(plan));
    }
    {
        const auto plan = plan_for(90, 15);
        REQUIRE(plan.weights.size() == 1);
        CHECK(plan.weights[0].grams == 150);
        CHECK_FALSE(has_note(plan));
    }
    {
        const auto plan = plan_for(0, 60); // nothing stored for a spool the printer sees more than half full
        REQUIRE(plan.weights.size() == 1);
        CHECK(plan.weights[0].grams == 600);
    }
    {
        const auto plan = plan_for(110, 9); // lowering is sent whatever the difference
        REQUIRE(plan.weights.size() == 1);
        CHECK(plan.weights[0].grams == 90);
        CHECK_FALSE(has_note(plan));
    }
    {
        const auto plan = plan_for(90, 11, 110); // grams the printer reports are sent
        REQUIRE(plan.weights.size() == 1);
        CHECK(plan.weights[0].grams == 110);
        CHECK_FALSE(has_note(plan));
    }
    {
        // An RFID mount carries no weight for such a rise.
        auto row       = make_row("1", "UUID1");
        row.net_weight = 90;
        auto t         = make_rfid_tray("0", "UUID1");
        t.remain       = 11;
        Ctx        c;
        const auto plan = c.plan({with_tray(make_printer(), t)}, {row});
        REQUIRE(plan.mounts.size() == 1);
        CHECK(plan.mounts[0].rfid);
        CHECK_FALSE(plan.mounts[0].grams.has_value());
    }
}

TEST_CASE("A claim on an occupied slot whose spool cannot be identified yet is left alone", "[BblFilament]")
{
    const auto run = [](const fil::TrayFacts& t, const fil::Spool& claim) {
        Ctx  c;
        auto p = with_tray(make_printer(), t);
        for (int i = 0; i < 6; ++i) {
            p.observations = static_cast<unsigned long long>(i + 1);
            c.now += 20;
            c.serial += 1;
            const auto plan = c.plan({p}, {claim});
            CHECK(plan.unbinds.empty());
            CHECK_FALSE(plan.want_pull);
        }
        CHECK(c.state.unmatched.empty());
    };
    // A tray being read (an RFID row whose tag is not read yet).
    auto reading    = make_tray("0");
    reading.reading = true;
    reading.color   = "#123456FF"; // would not agree with a manual row either
    run(reading, mounted_row(make_row("2", "UUID2"), "dev1", "sn0", 0, "0"));
    run(reading, mounted_row(make_row("2"), "dev1", "sn0", 0, "0"));
    // A tray that reports no RFID while the row has one (after an AMS restart).
    auto no_tag = make_tray("0");
    run(no_tag, mounted_row(make_row("2", "UUID2"), "dev1", "sn0", 0, "0"));
    no_tag.uuid = "00000000000000000000000000000000";
    run(no_tag, mounted_row(make_row("2", "UUID2"), "dev1", "sn0", 0, "0"));
}

TEST_CASE("An unidentified spool holds an unbind streak where it is", "[BblFilament]")
{
    Ctx  c;
    auto t     = make_tray("0");
    t.occupied = false;
    auto p     = with_tray(make_printer(), t);
    const auto claim = mounted_row(make_row("2", "UUID2"), "dev1", "sn0", 0, "0");
    p.observations   = 1;
    c.plan({p}, {claim});
    REQUIRE(c.state.unmatched["2"].count == 1);
    p.trays[0].occupied = true; // a spool without a readable tag
    for (int i = 0; i < 4; ++i) {
        p.observations += 1;
        c.now += 20;
        c.serial += 1;
        CHECK(c.plan({p}, {claim}).unbinds.empty());
    }
    CHECK(c.state.unmatched["2"].count == 1);
    // A tray that is empty again lets the streak grow.
    p.trays[0].occupied = false;
    p.observations += 1;
    c.plan({p}, {claim});
    CHECK(c.state.unmatched["2"].count == 2);
}

TEST_CASE("Something the cloud accepted lately is not planned again", "[BblFilament]")
{
    const fil::SlotKey S{"dev1", 0, "0"};
    {
        Ctx c;
        c.state.sent_until[fil::sent_key_mount(S, "1")] = c.now + 10;
        auto plan = c.plan({with_tray(make_printer(), make_rfid_tray("0", "UUID1"))}, {make_row("1", "UUID1")});
        CHECK(plan.mounts.empty());
        CHECK(plan.skipped_sent == 1);
        CHECK_FALSE(plan.notes.empty());
        c.now += 10; // the memory ran out
        plan = c.plan({with_tray(make_printer(), make_rfid_tray("0", "UUID1"))}, {make_row("1", "UUID1")});
        CHECK(plan.mounts.size() == 1);
        CHECK(c.state.sent_until.empty());
    }
    {
        // A weight push of the same grams, on an idle printer too.
        Ctx c;
        c.state.sent_until[fil::sent_key_weight("UUID1", 500)] = c.now + 10;
        const auto row = mounted_row(make_row("1", "UUID1"), "dev1", "sn0", 0, "0");
        CHECK(c.plan({with_tray(make_printer(), make_rfid_tray("0", "UUID1"))}, {row}).weights.empty());
        auto t   = make_rfid_tray("0", "UUID1");
        t.remain = 40; // other grams are sent
        CHECK(c.plan({with_tray(make_printer(), t)}, {row}).weights.size() == 1);
    }
    {
        // An unbind of the same slot.
        Ctx  c;
        auto p = make_printer();
        const auto claim = mounted_row(make_row("2"), "dev1", "sn0", 0, "0");
        c.state.sent_until[fil::sent_key_unbind(S)] = c.now + 100000;
        fil::LinkPlan plan;
        for (int i = 0; i < 4; ++i) {
            p.observations = static_cast<unsigned long long>(i + 1);
            c.now += 11;
            plan = c.plan({p}, {claim});
        }
        c.serial += 1;
        p.observations = 9;
        plan           = c.plan({p}, {claim});
        CHECK(plan.unbinds.empty());
        CHECK(plan.skipped_sent == 1);
    }
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

TEST_CASE("A tray being read is not matched and not registered", "[BblFilament]")
{
    Ctx c;
    auto t = make_rfid_tray("0", "UUID1");
    t.reading = true;
    auto plan = c.plan({with_tray(make_printer(), t)}, {make_row("1", "UUID1")});
    CHECK(plan.mounts.empty());
    REQUIRE(plan.trays.size() == 1);
    CHECK(plan.trays[0].spool_id.empty());
    // An unknown official spool waits for the read to end too.
    Ctx d;
    plan = d.plan({with_tray(make_printer(), t)}, {});
    CHECK(plan.registrations.empty());
    t.reading = false;
    CHECK(d.plan({with_tray(make_printer(), t)}, {}).registrations.size() == 1);
}

TEST_CASE("An RFID left behind on an empty tray matches nothing", "[BblFilament]")
{
    auto stale     = make_rfid_tray("0", "UUID1");
    stale.occupied = false; // the spool was taken out; a diff of only the exist bits kept its data
    {
        Ctx  c;
        auto plan = c.plan({with_tray(make_printer(), stale)}, {make_row("1", "UUID1")});
        CHECK(plan.mounts.empty());
        REQUIRE(plan.trays.size() == 1);
        CHECK(plan.trays[0].spool_id.empty());
        Ctx d;
        CHECK(d.plan({with_tray(make_printer(), stale)}, {}).registrations.empty());
    }
    {
        // The same spool newly reported by an occupied tray of the same printer is mounted there.
        Ctx  c;
        auto p = with_tray(make_printer(), stale);
        p.trays.push_back(make_rfid_tray("1", "UUID1"));
        const auto plan = c.plan({p}, {mounted_row(make_row("1", "UUID1"), "dev1", "sn0", 0, "0")});
        REQUIRE(plan.mounts.size() == 1);
        CHECK(plan.mounts[0].slot == (fil::SlotKey{"dev1", 0, "1"}));
    }
    {
        // ... or of another printer.
        Ctx  c;
        auto q      = make_printer("dev2");
        q.ams_sn[0] = "sn2";
        q.trays.push_back(make_rfid_tray("3", "UUID1"));
        const auto plan = c.plan({with_tray(make_printer(), stale), q}, {mounted_row(make_row("1", "UUID1"), "dev1", "sn0", 0, "0")});
        REQUIRE(plan.mounts.size() == 1);
        CHECK(plan.mounts[0].slot == (fil::SlotKey{"dev2", 0, "3"}));
        CHECK(plan.unbinds.empty());
    }
}

TEST_CASE("Two occupied trays with the same RFID leave its row alone", "[BblFilament]")
{
    Ctx  c;
    auto p1 = with_tray(make_printer("dev1"), make_rfid_tray("0", "UUID1"));
    auto p2 = make_printer("dev2");
    p2.ams_sn[0] = "sn2";
    p2.trays.push_back(make_rfid_tray("0", "UUID1"));
    const auto row = mounted_row(make_row("1", "UUID1"), "dev1", "sn0", 0, "3"); // claims a slot no tray reports
    for (int i = 0; i < 6; ++i) {
        p1.observations = static_cast<unsigned long long>(i + 1);
        c.now += 20;
        c.serial += 1;
        const auto plan = c.plan({p1, p2}, {row});
        CHECK_FALSE(plan.has_actions());
        CHECK_FALSE(plan.want_pull);
        CHECK_FALSE(plan.notes.empty());
        REQUIRE(plan.trays.size() == 2);
        CHECK(plan.trays[0].spool_id.empty());
        CHECK(plan.trays[1].spool_id.empty());
        CHECK_FALSE(plan.trays[0].note.empty());
    }
    CHECK(c.state.unmatched.empty());
    // Two unknown official spools with one RFID are not registered either.
    Ctx d;
    CHECK(d.plan({p1, p2}, {}).registrations.empty());
}

TEST_CASE("An unbind streak belongs to the mount it was counted for", "[BblFilament]")
{
    Ctx  c;
    auto p = make_printer();
    for (const char* slot : {"0", "1"}) {
        auto t     = make_tray(slot);
        t.occupied = false;
        p.trays.push_back(t);
    }
    const auto at_a = mounted_row(make_row("2"), "dev1", "sn0", 0, "0");
    for (int i = 0; i < 4; ++i) {
        p.observations = static_cast<unsigned long long>(i + 1);
        c.now += 11;
        CHECK(c.plan({p}, {at_a}).unbinds.empty());
    }
    REQUIRE(c.state.unmatched["2"].count == 4); // observations and seconds enough at A; only a newer list is missing

    // A newer list shows the row at B, where no tray matches either: B starts its own streak.
    const auto at_b = mounted_row(make_row("2"), "dev1", "sn0", 0, "1");
    const auto b_list = c.serial + 1;
    c.serial          = b_list;
    p.observations    = 5;
    c.now += 11;
    CHECK(c.plan({p}, {at_b}).unbinds.empty());
    CHECK(c.state.unmatched["2"].count == 1);
    CHECK(c.state.unmatched["2"].slot_id == "1");
    for (int i = 0; i < 3; ++i) {
        p.observations += 1;
        c.now += 11; // three observations and 33 seconds at B, but still the list B's streak began with
        CHECK(c.plan({p}, {at_b}).unbinds.empty());
    }
    c.serial += 1;
    p.observations += 1;
    const auto plan = c.plan({p}, {at_b});
    REQUIRE(plan.unbinds.size() == 1);
    CHECK(plan.unbinds[0].slot.slot_id == "1");
}

TEST_CASE("A huge or non-finite row weight is bounded before it is rounded", "[BblFilament]")
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    CHECK(fil::bounded_grams(1e30) == 999999999);
    CHECK(fil::bounded_grams(-5.0) == 0);
    CHECK(fil::bounded_grams(nan) == 0);
    CHECK(fil::bounded_grams(std::numeric_limits<double>::infinity()) == 0);
    CHECK(fil::bounded_grams(749.6) == 750);

    auto t    = make_tray(); // 50 %, no total of its own: the row's total counts
    t.total_g = 0;
    auto row  = make_row("1");
    row.total_net_weight = 1e30;
    CHECK(fil::measured_grams(t, row).value() == 500000000);
    row.total_net_weight = nan;
    CHECK_FALSE(fil::measured_grams(t, row).has_value());

    row.total_net_weight = 1e30;
    row.net_weight       = nan;
    auto item            = fil::ams_item_from_row(row, "sn0", 0, 1, "0", std::nullopt);
    CHECK(item.total_net_weight == 999999999);
    CHECK(item.net_weight == 0);
    row.total_net_weight = nan;
    row.net_weight       = 1e30;
    item                 = fil::ams_item_from_row(row, "sn0", 0, 1, "0", std::nullopt);
    CHECK(item.total_net_weight == 0);
    CHECK(item.net_weight == 999999999);

    // A weight push compares with the bounded value.
    Ctx  c;
    auto mounted       = mounted_row(make_row("1", "UUID1"), "dev1", "sn0", 0, "0");
    mounted.net_weight = 1e30;
    const auto plan    = c.plan({with_tray(make_printer(), make_rfid_tray("0", "UUID1"))}, {mounted});
    REQUIRE(plan.weights.size() == 1);
    CHECK(plan.weights[0].grams == 500);
}

TEST_CASE("An assignment outlives the slot's old contents and binds when the assigned filament shows", "[BblFilament]")
{
    Ctx  c;
    const fil::SlotKey S{"dev1", 0, "0"};
    c.state.assignments[S] = make_assignment("dev1", "0", "2", c.now);
    auto old        = make_tray("0");
    old.filament_id = "GFB00";
    old.color       = "#000000FF";
    for (int i = 0; i < 3; ++i) {
        c.now += 1;
        CHECK(c.plan({with_tray(make_printer(), old)}, {make_row("2")}).mounts.empty());
        REQUIRE(c.state.assignments.count(S) == 1);
    }
    CHECK(c.state.assignments[S].has_before);
    CHECK(c.state.assignments[S].before_filament_id == "GFB00");
    const auto plan = c.plan({with_tray(make_printer(), make_tray("0"))}, {make_row("2")});
    REQUIRE(plan.mounts.size() == 1);
    CHECK(plan.mounts[0].spool_id == "2");
}

TEST_CASE("An assignment ends when its slot shows yet another filament or empties", "[BblFilament]")
{
    const fil::SlotKey S{"dev1", 0, "0"};
    auto               old = make_tray("0");
    old.filament_id        = "GFB00";
    old.color              = "#000000FF";
    const int variant      = GENERATE(0, 1, 2); // a third filament, a third colour, the slot empties
    Ctx       c;
    c.state.assignments[S] = make_assignment("dev1", "0", "2", c.now);
    fil::TrayFacts next    = old;
    if (variant == 0) {
        CHECK(c.plan({with_tray(make_printer(), old)}, {make_row("2")}).mounts.empty()); // the old contents first
        next.filament_id = "GFC00";
    } else if (variant == 1) {
        CHECK(c.plan({with_tray(make_printer(), old)}, {make_row("2")}).mounts.empty());
        next.color = "#FF0000FF";
    } else {
        next.occupied = false;
    }
    const auto plan = c.plan({with_tray(make_printer(), next)}, {make_row("2")});
    CHECK(plan.mounts.empty());
    CHECK(c.state.assignments.empty());
    CHECK_FALSE(plan.notes.empty());
    // The assigned filament showing up later binds nothing.
    CHECK(c.plan({with_tray(make_printer(), make_tray("0"))}, {make_row("2")}).mounts.empty());
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

// ---------------------------------------------------------------------------------------------------------------------
// the driver: rounds through a real spool service, the fake cloud and the manual runner

namespace {

json rfid_row(long long id, const std::string& rfid)
{
    json r    = row_with(id);
    r["RFID"] = rfid;
    return r;
}

json mounted_json(json r, const std::string& dev, const std::string& sn, int ams_id, const std::string& slot, int ams_type = 1)
{
    FakeCloud::mount(r, dev, sn, slot, ams_id, ams_type);
    return r;
}

struct DriverRig
{
    Rig                              rig;
    long long                        t{5000};
    std::unique_ptr<fil::LinkDriver> driver;
    std::vector<fil::AutoPushSummary> pushes;
    std::vector<std::string>          registered;

    DriverRig()
    {
        fil::LinkDriverDeps deps;
        deps.service = rig.svc.get();
        deps.now     = [this] { return t; };
        driver       = std::make_unique<fil::LinkDriver>(std::move(deps));
        driver->on_auto_push  = [this](const fil::AutoPushSummary& s) { pushes.push_back(s); };
        driver->on_registered = [this](const fil::SlotKey& k, const std::string& rfid) { registered.push_back(k.text() + " " + rfid); };
    }
    void load() // the first list, then a clean log
    {
        rig.svc->request_pull();
        rig.settle();
        rig.cloud->log.clear();
    }
    void run(const std::vector<fil::PrinterView>& printers)
    {
        driver->tick(printers);
        rig.settle();
    }
    int count(const std::string& prefix) const { return rig.cloud->count(prefix); }
};

} // namespace

TEST_CASE("A dry run plans and sends nothing", "[BblFilament]")
{
    DriverRig d;
    d.rig.cloud->rows.push_back(rfid_row(1000001, "UUID1"));
    d.load();
    d.driver->config.mode = fil::LinkMode::DryRun;
    d.run({with_tray(make_printer(), make_rfid_tray("0", "UUID1"))});
    CHECK(d.driver->last_plan().mounts.size() == 1);
    CHECK(d.rig.cloud->log.empty());
}

TEST_CASE("A round sends the RFID mount and the manual bind, then one pull, and nothing once the list shows them", "[BblFilament]")
{
    DriverRig d;
    d.rig.cloud->rows.push_back(rfid_row(1000001, "UUID1"));
    d.rig.cloud->rows.push_back(row_with(1000002));
    d.rig.cloud->apply_mounts = true;
    d.load();
    auto p = make_printer();
    p.trays.push_back(make_rfid_tray("0", "UUID1")); // 50 % of 1000 g
    p.trays.push_back(make_tray("1"));
    d.driver->assign(make_assignment("dev1", "1", "1000002", 0));

    d.driver->tick({p});
    CHECK(d.driver->status_json({p})["round_in_flight"] == true);
    d.rig.settle();
    REQUIRE(d.rig.cloud->log.size() == 3);
    CHECK(d.rig.cloud->log[0] == "sync_ams dev1");
    CHECK(d.rig.cloud->log[1] == "sync_slots dev1");
    CHECK(d.count("list") == 1);
    REQUIRE(d.rig.cloud->ams_got.size() == 1);
    REQUIRE(d.rig.cloud->ams_got[0].size() == 1);
    CHECK(d.rig.cloud->ams_got[0][0].rfid == "UUID1");
    CHECK(d.rig.cloud->ams_got[0][0].net_weight == 500);
    CHECK(d.rig.cloud->ams_got[0][0].ams_sn == "sn0");
    CHECK(d.rig.cloud->ams_got[0][0].slot_id == "0");
    REQUIRE(d.rig.cloud->slots_got.size() == 1);
    REQUIRE(d.rig.cloud->slots_got[0].size() == 1);
    CHECK(d.rig.cloud->slots_got[0][0].spool_id == "1000002");
    CHECK(d.rig.cloud->slots_got[0][0].slot_id == "1");

    d.t += fil::LinkDriver::k_round_spacing_seconds;
    d.run({p});
    CHECK_FALSE(d.driver->last_plan().has_actions());
    CHECK(d.rig.cloud->log.size() == 3);
    CHECK(d.driver->status_json({p})["assignments"].empty());
}

TEST_CASE("No round starts while one is in flight, before its list arrives, or within five seconds", "[BblFilament]")
{
    DriverRig d;
    d.rig.cloud->rows.push_back(rfid_row(1000001, "UUID1"));
    d.load();
    const std::vector<fil::PrinterView> printers{with_tray(make_printer(), make_rfid_tray("0", "UUID1"))};
    d.rig.cloud->forced["sync_ams"].push_back(forced_error(fil::CloudError::Rejected)); // not taken: it is planned again

    d.driver->tick(printers);
    CHECK(d.rig.worker.size() == 1);
    d.t += 10;
    d.driver->tick(printers); // in flight
    d.rig.step_worker();
    d.rig.run_ui(); // the op is done, the round ends at t, its pull is waiting in the worker
    CHECK(d.count("sync_ams") == 1);
    CHECK(d.driver->status_json(printers)["waiting_for_list"] == true);
    d.driver->tick(printers); // the list has not arrived
    CHECK(d.rig.worker.size() == 1);
    d.rig.settle();
    CHECK(d.count("list") == 1);
    CHECK(d.count("sync_ams") == 1);

    const long long ended = d.t;
    d.t                   = ended + fil::LinkDriver::k_round_spacing_seconds - 1;
    d.run(printers);
    CHECK(d.count("sync_ams") == 1);
    d.t = ended + fil::LinkDriver::k_round_spacing_seconds;
    d.run(printers);
    CHECK(d.count("sync_ams") == 2);
}

TEST_CASE("A weight push is recorded only when the cloud took it", "[BblFilament]")
{
    DriverRig d;
    d.rig.cloud->rows.push_back(mounted_json(rfid_row(1000001, "UUID1"), "dev1", "sn0", 0, "0")); // 750 g
    d.load();
    auto p = with_tray(make_printer(), make_rfid_tray("0", "UUID1")); // measures 500 g
    p.busy = true;

    d.rig.cloud->forced["sync_ams"].push_back(forced_error(fil::CloudError::Rejected));
    d.run({p});
    CHECK(d.count("sync_ams") == 1);
    CHECK(d.pushes.empty());

    d.t += fil::LinkDriver::k_round_spacing_seconds;
    d.run({p}); // the failed push is sent again
    CHECK(d.count("sync_ams") == 2);
    REQUIRE(d.pushes.size() == 1);
    CHECK(d.pushes[0].pushed == 1);
    CHECK(d.pushes[0].busy);
    REQUIRE(d.rig.cloud->ams_got.size() == 2);
    CHECK(d.rig.cloud->ams_got[1][0].net_weight == 500);

    d.t += fil::LinkDriver::k_round_spacing_seconds;
    d.run({p}); // the busy printer is in its ten-minute cooldown
    CHECK(d.count("sync_ams") == 2);
    const auto& skipped = d.driver->last_plan().skipped_cooldown;
    REQUIRE(skipped.count("dev1") == 1);
    CHECK(skipped.at("dev1") == 1);

    d.driver->sync_now();
    d.run({p});
    CHECK(d.count("sync_ams") == 3);
}

TEST_CASE("A change the cloud took but the list does not show is not sent again for ten minutes", "[BblFilament]")
{
    DriverRig d;
    d.rig.cloud->rows.push_back(rfid_row(1000001, "UUID1"));                                            // not mounted
    d.rig.cloud->rows.push_back(mounted_json(rfid_row(1000003, "UUID3"), "dev1", "sn0", 0, "2"));       // 750 g
    d.rig.cloud->rows.push_back(row_with(1000002));
    d.load(); // apply_mounts is off: the lists after the round look as before
    auto p = make_printer();
    p.trays.push_back(make_rfid_tray("0", "UUID1"));
    p.trays.push_back(make_tray("1"));
    p.trays.push_back(make_rfid_tray("2", "UUID3")); // 500 g, an idle printer
    d.driver->assign(make_assignment("dev1", "1", "1000002", 0));

    const long long start = d.t;
    d.run({p});
    CHECK(d.count("sync_ams") == 1);
    CHECK(d.count("sync_slots") == 1);
    REQUIRE(d.rig.cloud->ams_got.size() == 1);
    CHECK(d.rig.cloud->ams_got[0].size() == 2);

    d.t += fil::LinkDriver::k_round_spacing_seconds;
    d.driver->assign(make_assignment("dev1", "1", "1000002", 0));
    d.run({p});
    CHECK(d.count("sync_") == 2);
    CHECK_FALSE(d.driver->last_plan().has_actions());
    CHECK(d.driver->last_plan().skipped_sent == 3);
    CHECK(d.driver->status_json({p})["planned"]["skipped_recently_sent"] == 3);

    d.t = start + fil::LinkDriver::k_refusal_seconds;
    d.driver->assign(make_assignment("dev1", "1", "1000002", 0));
    d.run({p});
    CHECK(d.count("sync_ams") == 2);
    CHECK(d.count("sync_slots") == 2);

    // sync_now forgets it.
    d.t += fil::LinkDriver::k_round_spacing_seconds;
    d.driver->assign(make_assignment("dev1", "1", "1000002", 0));
    d.run({p});
    CHECK(d.count("sync_") == 4);
    d.driver->sync_now();
    d.run({p});
    CHECK(d.count("sync_") == 6);
}

TEST_CASE("A change the cloud did not take is sent again in the next round", "[BblFilament]")
{
    DriverRig d;
    d.rig.cloud->rows.push_back(row_with(1000002));
    d.load();
    const std::vector<fil::PrinterView> printers{with_tray(make_printer(), make_tray("1"))};
    d.rig.cloud->forced["sync_slots"].push_back(forced_error(fil::CloudError::Rejected));
    d.driver->assign(make_assignment("dev1", "1", "1000002", 0));
    d.run(printers);
    CHECK(d.count("sync_slots") == 1);
    d.t += fil::LinkDriver::k_round_spacing_seconds;
    d.run(printers);
    CHECK(d.count("sync_slots") == 2);
}

TEST_CASE("An unbind the list does not show is not sent again for ten minutes", "[BblFilament]")
{
    DriverRig d;
    d.rig.cloud->rows.push_back(mounted_json(row_with(1000002), "dev1", "sn0", 0, "0"));
    d.load();
    auto t     = make_tray("0");
    t.occupied = false;
    auto p     = with_tray(make_printer(), t);
    for (int i = 0; i < 6 && d.count("sync_slots") == 0; ++i) {
        p.observations = static_cast<unsigned long long>(i + 1);
        d.t += 11;
        d.run({p});
        d.rig.svc->request_pull(); // a newer list
        d.rig.settle();
    }
    REQUIRE(d.count("sync_slots") == 1);
    for (int i = 0; i < 6; ++i) {
        p.observations += 1;
        d.t += 11;
        d.run({p});
        d.rig.svc->request_pull();
        d.rig.settle();
    }
    CHECK(d.count("sync_slots") == 1);
    CHECK(d.driver->last_plan().skipped_sent == 1);
}

TEST_CASE("The ams sync builder leaves out a row that became busy after planning", "[BblFilament]")
{
    DriverRig d;
    d.rig.cloud->rows.push_back(rfid_row(1000001, "UUID1"));
    d.rig.cloud->rows.push_back(rfid_row(1000002, "UUID2"));
    d.rig.cloud->rows.push_back(row_with(1000003));
    d.load();
    auto p = make_printer();
    p.trays.push_back(make_rfid_tray("0", "UUID1"));
    p.trays.push_back(make_rfid_tray("1", "UUID2"));

    d.rig.svc->update(json{{"spool_id", "1000003"}, {"note", "x"}}); // runs first: the link op waits behind it
    d.driver->tick({p});
    REQUIRE(d.driver->last_plan().mounts.size() == 2);
    d.rig.svc->update(json{{"spool_id", "1000001"}, {"note", "y"}}); // queued: row 1 is busy when the op starts
    d.rig.settle();
    REQUIRE(d.rig.cloud->ams_got.size() == 1);
    REQUIRE(d.rig.cloud->ams_got[0].size() == 1);
    CHECK(d.rig.cloud->ams_got[0][0].rfid == "UUID2");
}

TEST_CASE("Each registration is its own op, is done on success and backs off on failure", "[BblFilament]")
{
    DriverRig d;
    d.load(); // an empty account
    auto p = make_printer();
    p.trays.push_back(make_rfid_tray("0", "UUIDN1"));
    p.trays.push_back(make_rfid_tray("1", "UUIDN2"));

    d.rig.cloud->forced["sync_ams"].push_back(forced_error(fil::CloudError::Rejected)); // the first one fails
    const long long start = d.t;
    d.run({p});
    REQUIRE(d.rig.cloud->ams_got.size() == 2);
    REQUIRE(d.rig.cloud->ams_got[0].size() == 1);
    REQUIRE(d.rig.cloud->ams_got[1].size() == 1);
    CHECK(d.rig.cloud->ams_got[0][0].rfid == "UUIDN1");
    CHECK(d.rig.cloud->ams_got[1][0].rfid == "UUIDN2");
    REQUIRE(d.registered.size() == 1);
    CHECK(d.registered[0] == "dev1:0:1 UUIDN2");

    d.t += fil::LinkDriver::k_round_spacing_seconds;
    d.run({p});
    CHECK(d.driver->last_plan().registrations.empty()); // one is done, the other waits
    d.t = start + 119;
    d.run({p});
    CHECK(d.driver->last_plan().registrations.empty());
    d.t = start + 120; // 60 << 1 after the first failure
    d.run({p});
    CHECK(d.count("sync_ams") == 3);
    CHECK(d.rig.cloud->ams_got.back()[0].rfid == "UUIDN1");
}

TEST_CASE("A slot result with a non-zero code is not tried again for ten minutes", "[BblFilament]")
{
    DriverRig d;
    d.rig.cloud->rows.push_back(row_with(1000002));
    d.load();
    const std::vector<fil::PrinterView> printers{with_tray(make_printer(), make_tray("1"))};
    d.rig.cloud->forced["sync_slots"].push_back(FakeCloud::ok(json::parse(R"({"results":[{"amsSn":"sn0","slotId":"1","code":5}]})")));

    const long long start = d.t;
    d.driver->assign(make_assignment("dev1", "1", "1000002", 0));
    d.run(printers);
    CHECK(d.count("sync_slots") == 1);
    d.t += fil::LinkDriver::k_round_spacing_seconds;
    d.run(printers);
    CHECK(d.count("sync_slots") == 1);
    CHECK_FALSE(d.driver->last_plan().notes.empty());

    d.t = start + 300;
    d.driver->assign(make_assignment("dev1", "1", "1000002", 0));
    d.run(printers);
    CHECK(d.count("sync_slots") == 1);

    d.t = start + fil::LinkDriver::k_refusal_seconds;
    d.driver->assign(make_assignment("dev1", "1", "1000002", 0));
    d.run(printers);
    CHECK(d.count("sync_slots") == 2);
}

TEST_CASE("Unassign sends one unbind and later ticks do not bind the spool again", "[BblFilament]")
{
    DriverRig d;
    d.rig.cloud->rows.push_back(mounted_json(row_with(1000002), "dev1", "sn0", 0, "1"));
    d.rig.cloud->apply_mounts = true;
    d.load();
    auto p = with_tray(make_printer(), make_tray("1"));

    const fil::Accept a = d.driver->unassign(fil::SlotKey{"dev1", 0, "1"}, {p});
    CHECK(a.ok);
    d.rig.settle();
    REQUIRE(d.rig.cloud->slots_got.size() == 1);
    REQUIRE(d.rig.cloud->slots_got[0].size() == 1);
    CHECK(d.rig.cloud->slots_got[0][0].spool_id.empty());
    CHECK(d.rig.cloud->slots_got[0][0].slot_id == "1");
    CHECK(d.rig.cloud->slots_got[0][0].ams_sn == "sn0");

    for (int i = 0; i < 5; ++i) {
        d.t += 40;
        p.observations = static_cast<unsigned long long>(i + 2);
        d.run({p});
    }
    CHECK(d.count("sync_") == 1);
}

TEST_CASE("A planned unbind is sent only while the row has exactly the planned mount and the slot to itself", "[BblFilament]")
{
    const int variant = GENERATE(0, 1, 2, 3); // nothing changed; the AMS serial; the AMS type; another row claims the slot
    DriverRig d;
    d.rig.cloud->rows.push_back(mounted_json(row_with(1000002), "dev1", "sn0", 0, "0"));
    d.load();
    auto t     = make_tray("0");
    t.occupied = false;
    auto p     = with_tray(make_printer(), t);
    bool planned = false;
    for (int i = 0; i < 8 && !planned; ++i) {
        p.observations = static_cast<unsigned long long>(i + 1);
        d.t += 11;
        d.rig.svc->request_pull(); // a newer list; still running when a round starts, so the round's op waits behind it
        d.driver->tick({p});
        planned = !d.driver->last_plan().unbinds.empty();
        if (!planned) d.rig.settle();
    }
    REQUIRE(planned);
    REQUIRE(d.count("sync_slots") == 0);
    if (variant == 1) d.rig.cloud->rows[0]["amsSn"] = "sn9";
    if (variant == 2) d.rig.cloud->rows[0]["amsType"] = 2;
    if (variant == 3) d.rig.cloud->rows.push_back(mounted_json(row_with(1000003), "dev1", "sn0", 0, "0"));
    d.rig.settle(); // the pull brings the list of that moment, then the op starts
    CHECK(d.count("sync_slots") == (variant == 0 ? 1 : 0));
}

TEST_CASE("Unassign sends its unbind only while the row has the same mount and the slot to itself", "[BblFilament]")
{
    const int variant = GENERATE(0, 1, 2, 3); // nothing changed; the AMS serial; the AMS type; another row claims the slot
    DriverRig d;
    d.rig.cloud->rows.push_back(mounted_json(row_with(1000002), "dev1", "sn0", 0, "1"));
    d.load();
    const auto p = with_tray(make_printer(), make_tray("1"));
    d.rig.svc->request_pull(); // running: the unbind waits behind it
    REQUIRE(d.driver->unassign(fil::SlotKey{"dev1", 0, "1"}, {p}).ok);
    if (variant == 1) d.rig.cloud->rows[0]["amsSn"] = "sn9";
    if (variant == 2) d.rig.cloud->rows[0]["amsType"] = 2;
    if (variant == 3) d.rig.cloud->rows.push_back(mounted_json(row_with(1000003), "dev1", "sn0", 0, "1"));
    d.rig.settle();
    CHECK(d.count("sync_slots") == (variant == 0 ? 1 : 0));
}

TEST_CASE("Ops of a round still queued when the driver is reset send nothing", "[BblFilament]")
{
    DriverRig d;
    d.rig.cloud->rows.push_back(rfid_row(1000001, "UUID1"));
    d.rig.cloud->rows.push_back(row_with(1000002));
    d.rig.cloud->rows.push_back(row_with(1000003));
    d.load();
    auto p = make_printer();
    p.trays.push_back(make_rfid_tray("0", "UUID1"));
    p.trays.push_back(make_tray("1"));
    p.trays.push_back(make_rfid_tray("2", "UUIDN1")); // a new official spool
    d.driver->assign(make_assignment("dev1", "1", "1000002", 0));

    d.rig.svc->update(json{{"spool_id", "1000003"}, {"note", "x"}}); // runs first: the round's ops wait behind it
    d.driver->tick({p});
    REQUIRE(d.driver->last_plan().mounts.size() == 2);
    REQUIRE(d.driver->last_plan().registrations.size() == 1);
    d.driver->reset();
    d.rig.settle();
    CHECK(d.count("update") == 1);
    CHECK(d.count("sync_ams") == 0);
    CHECK(d.count("sync_slots") == 0);
}

TEST_CASE("Ops of a round still queued when the mode or the allow-list changes send nothing", "[BblFilament]")
{
    const int variant = GENERATE(0, 1, 2); // the mode goes Off; the mode goes DryRun; an allow-list is set
    DriverRig d;
    d.rig.cloud->rows.push_back(rfid_row(1000001, "UUID1"));
    d.rig.cloud->rows.push_back(row_with(1000002));
    d.rig.cloud->rows.push_back(row_with(1000003));
    d.load();
    auto p = make_printer();
    p.trays.push_back(make_rfid_tray("0", "UUID1"));
    p.trays.push_back(make_tray("1"));
    p.trays.push_back(make_rfid_tray("2", "UUIDN1")); // a new official spool
    d.driver->assign(make_assignment("dev1", "1", "1000002", 0));

    d.rig.svc->update(json{{"spool_id", "1000003"}, {"note", "x"}}); // runs first: the round's ops wait behind it
    d.driver->tick({p});
    REQUIRE(d.driver->last_plan().mounts.size() == 2);
    REQUIRE(d.driver->last_plan().registrations.size() == 1);
    REQUIRE(d.driver->status_json({p})["round_in_flight"] == true);
    if (variant == 0) d.driver->config.mode = fil::LinkMode::Off;
    if (variant == 1) d.driver->config.mode = fil::LinkMode::DryRun;
    if (variant == 2) d.driver->config.only_slots = {"dev1:0:3"};
    d.driver->tick({p});
    d.rig.settle();
    CHECK(d.count("update") == 1);
    CHECK(d.count("sync_ams") == 0);
    CHECK(d.count("sync_slots") == 0);
    CHECK(d.driver->status_json({p})["round_in_flight"] == false);
    CHECK(d.driver->status_json({p})["assignments"].size() == 1); // the rest of the state is kept
}

TEST_CASE("A tick with the same config leaves the queued round alone", "[BblFilament]")
{
    DriverRig d;
    d.rig.cloud->rows.push_back(rfid_row(1000001, "UUID1"));
    d.rig.cloud->rows.push_back(row_with(1000003));
    d.load();
    const auto p = with_tray(make_printer(), make_rfid_tray("0", "UUID1"));
    d.rig.svc->update(json{{"spool_id", "1000003"}, {"note", "x"}});
    d.driver->tick({p});
    d.driver->tick({p});
    d.rig.settle();
    CHECK(d.count("sync_ams") == 1);
}

TEST_CASE("An explicit write needs linking on and an allowed slot", "[BblFilament]")
{
    const fil::SlotKey slot{"dev1", 0, "1"};
    fil::LinkConfig    c;
    CHECK(fil::write_refusal(c, &slot).empty());
    CHECK(fil::write_refusal(c, nullptr).empty());
    c.only_slots = {"dev1:0:1"};
    CHECK(fil::write_refusal(c, &slot).empty());
    c.only_slots = {"dev1:0:2"};
    CHECK_FALSE(fil::write_refusal(c, &slot).empty());
    CHECK(fil::write_refusal(c, nullptr).empty()); // a write without a slot does not look at the allow-list
    c.only_slots.clear();
    c.mode = fil::LinkMode::DryRun;
    CHECK_FALSE(fil::write_refusal(c, &slot).empty());
    CHECK(fil::write_refusal(c, &slot, true).empty());
    c.mode = fil::LinkMode::Off;
    CHECK_FALSE(fil::write_refusal(c, &slot).empty());
    CHECK_FALSE(fil::write_refusal(c, &slot, true).empty());
}

TEST_CASE("An assignment is refused unless linking is on and the slot is allowed", "[BblFilament]")
{
    const int variant = GENERATE(0, 1, 2); // Off; DryRun; another slot allowed
    DriverRig d;
    if (variant == 0) d.driver->config.mode = fil::LinkMode::Off;
    if (variant == 1) d.driver->config.mode = fil::LinkMode::DryRun;
    if (variant == 2) d.driver->config.only_slots = {"dev1:0:2"};
    const fil::Accept a = d.driver->assign(make_assignment("dev1", "1", "1000002", 0));
    CHECK_FALSE(a.ok);
    CHECK(a.kind == fil::FailKind::Rejected);
    CHECK(d.driver->status_json({})["assignments"].empty());

    d.driver->config = fil::LinkConfig{};
    d.driver->config.only_slots = {"dev1:0:1"};
    CHECK(d.driver->assign(make_assignment("dev1", "1", "1000002", 0)).ok);
    CHECK(d.driver->status_json({})["assignments"].size() == 1);
}

TEST_CASE("Unassign is refused when linking is off or the slot is not allowed", "[BblFilament]")
{
    const int variant = GENERATE(0, 1); // Off; another slot allowed
    DriverRig d;
    d.rig.cloud->rows.push_back(mounted_json(row_with(1000002), "dev1", "sn0", 0, "1"));
    d.load();
    const auto p = with_tray(make_printer(), make_tray("1"));
    if (variant == 0) d.driver->config.mode = fil::LinkMode::Off;
    if (variant == 1) d.driver->config.only_slots = {"dev1:0:2"};
    const fil::Accept a = d.driver->unassign(fil::SlotKey{"dev1", 0, "1"}, {p});
    CHECK_FALSE(a.ok);
    CHECK(a.kind == fil::FailKind::Rejected);
    d.rig.settle();
    CHECK(d.rig.cloud->log.empty());
}

TEST_CASE("Unassign in a dry run answers the spool it would unbind and sends nothing", "[BblFilament]")
{
    DriverRig d;
    d.rig.cloud->rows.push_back(mounted_json(row_with(1000002), "dev1", "sn0", 0, "1"));
    d.load();
    const auto p = with_tray(make_printer(), make_tray("1"));
    d.driver->config.mode = fil::LinkMode::DryRun;
    std::string       would;
    const fil::Accept a = d.driver->unassign(fil::SlotKey{"dev1", 0, "1"}, {p}, &would);
    CHECK(a.ok);
    CHECK(would == "1000002");
    d.rig.settle();
    CHECK(d.rig.cloud->log.empty());
}

TEST_CASE("A spool created for a slot linking may not touch is created but not assigned", "[BblFilament]")
{
    const int variant = GENERATE(0, 1); // DryRun; another slot allowed
    DriverRig d;
    d.load();
    if (variant == 0) d.driver->config.mode = fil::LinkMode::DryRun;
    if (variant == 1) d.driver->config.only_slots = {"dev1:0:2"};
    REQUIRE(d.driver->create_and_assign(page_spool(), fil::SlotKey{"dev1", 0, "1"}).ok);
    d.rig.settle();
    CHECK(d.count("create") == 1);
    CHECK(d.driver->status_json({})["creations"].empty());
}

TEST_CASE("Unassign refuses an RFID spool and an empty slot", "[BblFilament]")
{
    DriverRig d;
    d.rig.cloud->rows.push_back(mounted_json(rfid_row(1000001, "UUID1"), "dev1", "sn0", 0, "0"));
    d.load();
    const auto p = with_tray(make_printer(), make_rfid_tray("0", "UUID1"));
    const fil::Accept rfid = d.driver->unassign(fil::SlotKey{"dev1", 0, "0"}, {p});
    CHECK_FALSE(rfid.ok);
    CHECK(rfid.kind == fil::FailKind::BadRequest);
    const fil::Accept empty = d.driver->unassign(fil::SlotKey{"dev1", 0, "2"}, {p});
    CHECK_FALSE(empty.ok);
    CHECK(empty.kind == fil::FailKind::BadRequest);
    d.rig.settle();
    CHECK(d.rig.cloud->log.empty());
}

TEST_CASE("Unassign says why it refused", "[BblFilament]")
{
    const auto check_clean = [](const std::string& why) {
        CHECK(why.find("sn0") == std::string::npos);
        CHECK(why.find("UUID1") == std::string::npos);
        CHECK(why.find("dev1") == std::string::npos);
    };
    SECTION("linking may not write there")
    for (int variant = 0; variant < 2; ++variant) { // Off; another slot allowed
        DriverRig d;
        d.rig.cloud->rows.push_back(mounted_json(row_with(1000002), "dev1", "sn0", 0, "1"));
        d.load();
        if (variant == 0) d.driver->config.mode = fil::LinkMode::Off;
        if (variant == 1) d.driver->config.only_slots = {"dev1:0:2"};
        const fil::SlotKey slot{"dev1", 0, "1"};
        const std::string  expected = fil::write_refusal(d.driver->config, &slot, true);
        REQUIRE_FALSE(expected.empty());
        std::string why;
        CHECK_FALSE(d.driver->unassign(slot, {with_tray(make_printer(), make_tray("1"))}, nullptr, &why).ok);
        CHECK(why == expected);
        check_clean(why);
    }
    SECTION("no spool is linked to the slot")
    {
        DriverRig d;
        d.load();
        std::string why;
        CHECK_FALSE(d.driver->unassign(fil::SlotKey{"dev1", 0, "1"}, {with_tray(make_printer(), make_tray("1"))}, nullptr, &why).ok);
        CHECK(why == "No spool is linked to that slot.");
    }
    SECTION("the spool has an RFID tag")
    {
        DriverRig d;
        d.rig.cloud->rows.push_back(mounted_json(rfid_row(1000001, "UUID1"), "dev1", "sn0", 0, "0"));
        d.load();
        std::string why;
        CHECK_FALSE(d.driver->unassign(fil::SlotKey{"dev1", 0, "0"}, {with_tray(make_printer(), make_rfid_tray("0", "UUID1"))}, nullptr, &why).ok);
        CHECK(why == "The spool in that slot has an RFID tag: the printer reports where it is.");
        check_clean(why);
    }
    SECTION("the AMS is not known")
    {
        DriverRig d;
        d.rig.cloud->rows.push_back(mounted_json(row_with(1000002), "dev1", "", 0, "1"));
        d.load();
        auto p = with_tray(make_printer(), make_tray("1"));
        p.ams_sn.clear();
        std::string why;
        CHECK_FALSE(d.driver->unassign(fil::SlotKey{"dev1", 0, "1"}, {p}, nullptr, &why).ok);
        CHECK(why == "The AMS of that slot is not known yet.");
    }
    SECTION("the service is missing")
    {
        fil::LinkDriver driver{fil::LinkDriverDeps{}};
        std::string     why;
        const fil::Accept a = driver.unassign(fil::SlotKey{"dev1", 0, "1"}, {}, nullptr, &why);
        CHECK_FALSE(a.ok);
        CHECK(a.kind == fil::FailKind::Unavailable);
        CHECK(why == "The Filament Manager is not available.");
    }
}

TEST_CASE("An assignment the tray agrees with binds the spool once", "[BblFilament]")
{
    DriverRig d;
    d.rig.cloud->rows.push_back(row_with(1000002));
    d.rig.cloud->apply_mounts = true;
    d.load();
    const std::vector<fil::PrinterView> printers{with_tray(make_printer(), make_tray("2"))};
    d.driver->assign(make_assignment("dev1", "2", "1000002", 0));
    for (int i = 0; i < 4; ++i) {
        d.run(printers);
        d.t += fil::LinkDriver::k_round_spacing_seconds;
    }
    CHECK(d.count("sync_slots") == 1);
    REQUIRE(d.rig.cloud->slots_got.size() == 1);
    CHECK(d.rig.cloud->slots_got[0][0].spool_id == "1000002");
    CHECK(d.driver->last_plan().trays[0].rule == "cloud"); // the cloud's own row holds it now
}

TEST_CASE("Reset forgets assignments, back-offs and the last plan", "[BblFilament]")
{
    DriverRig d;
    d.load();
    auto p = with_tray(make_printer(), make_rfid_tray("0", "UUIDN1"));
    p.trays.push_back(make_tray("1"));
    d.rig.cloud->forced["sync_ams"].push_back(forced_error(fil::CloudError::Rejected));
    d.run({p}); // the registration fails and backs off
    d.driver->assign(make_assignment("dev1", "1", "1000002", 0));
    d.t += fil::LinkDriver::k_round_spacing_seconds;
    d.run({p});
    CHECK(d.driver->last_plan().registrations.empty());

    d.driver->reset();
    CHECK(d.driver->status_json({p})["assignments"].empty());
    CHECK(d.driver->status_json({p})["last_round"].is_null());
    CHECK(d.driver->last_plan().notes.empty());
    d.driver->config.mode = fil::LinkMode::DryRun;
    d.run({p});
    CHECK(d.driver->last_plan().registrations.size() == 1);
}

TEST_CASE("The link status names printers and never shows a serial or an RFID", "[BblFilament]")
{
    const std::string dev  = "01P00A391800123";
    const std::string sn   = "19C06A4A2900456";
    const std::string rfid = "8A1F2E3D4C5B6A79";
    DriverRig         d;
    d.rig.cloud->rows.push_back(rfid_row(1000001, rfid));
    d.rig.cloud->rows.push_back(row_with(1000002));
    d.load();
    auto p      = make_printer(dev);
    p.name      = "Workshop";
    p.ams_sn[0] = sn;
    p.trays.push_back(make_rfid_tray("0", rfid));
    p.trays.push_back(make_tray("1"));
    d.driver->config.only_slots = {fil::SlotKey{dev, 0, "0"}.text(), fil::SlotKey{dev, 0, "1"}.text()};
    d.driver->assign(make_assignment(dev, "1", "1000002", 0));
    d.run({p});
    REQUIRE(d.count("sync_ams") == 1);

    const json        j    = d.driver->status_json({p});
    const std::string dump = j.dump();
    CHECK(dump.find(dev) == std::string::npos);
    CHECK(dump.find(sn) == std::string::npos);
    CHECK(dump.find(rfid) == std::string::npos);
    CHECK(j["mode"] == "on");
    REQUIRE(j["printers"].size() == 1);
    CHECK(j["printers"][0]["name"] == "Workshop");
    CHECK(j["printers"][0]["takes_part"] == true);
    REQUIRE(j["printers"][0]["trays"].size() == 2);
    CHECK(j["printers"][0]["trays"][0]["has_serial"] == true);
    CHECK(j["only_slots"][0] == "Workshop:0:0");
    CHECK(j["planned"]["mounts"] == 2);
    CHECK(j["plan"].size() == 2);
    REQUIRE(j["last_round"]["ops"].size() == 2);
    CHECK(j["last_round"]["ops"][0]["kind"] == "ams");
    CHECK(j["last_round"]["ops"][0]["ok"] == true);
}

// ---------------------------------------------------------------------------------------------------------------------
// create and assign

namespace {

bool has_note_containing(const json& status, const std::string& text)
{
    for (const json& n : status["notes"])
        if (n.is_string() && n.get<std::string>().find(text) != std::string::npos) return true;
    return false;
}

} // namespace

TEST_CASE("A spool created for a slot is assigned once the list shows it, and a tray that agrees binds it", "[BblFilament]")
{
    DriverRig d;
    d.rig.cloud->apply_mounts = true;
    d.load(); // an empty account
    const std::vector<fil::PrinterView> printers{with_tray(make_printer(), make_tray("1"))};

    REQUIRE(d.driver->create_and_assign(page_spool(), fil::SlotKey{"dev1", 0, "1"}).ok);
    CHECK(d.driver->status_json(printers)["creations"].size() == 1);
    d.rig.settle(); // the create, then the pull it asks for
    REQUIRE(d.count("create") == 1);

    d.run(printers);
    const json status = d.driver->status_json(printers);
    CHECK(status["creations"].empty());
    REQUIRE(d.rig.cloud->slots_got.size() == 1);
    REQUIRE(d.rig.cloud->slots_got[0].size() == 1);
    CHECK(d.rig.cloud->slots_got[0][0].spool_id == "2000001");
    CHECK(d.rig.cloud->slots_got[0][0].slot_id == "1");
}

TEST_CASE("Two new identical spools in one list leave the slot unassigned with a note", "[BblFilament]")
{
    DriverRig d;
    d.load();
    const std::vector<fil::PrinterView> printers{with_tray(make_printer(), make_tray("1"))};
    REQUIRE(d.driver->create_and_assign(page_spool(), fil::SlotKey{"dev1", 0, "1"}).ok);
    d.rig.cloud->rows.push_back(row_with(1000005)); // an identical spool made elsewhere in the meantime
    d.rig.settle();

    d.run(printers);
    const json status = d.driver->status_json(printers);
    CHECK(status["creations"].empty());
    CHECK(status["assignments"].empty());
    CHECK(has_note_containing(status, "could not tell which new spool belongs to the slot"));
    CHECK(d.count("sync_slots") == 0);

    d.driver->reset();
    CHECK_FALSE(has_note_containing(d.driver->status_json(printers), "could not tell"));
}

TEST_CASE("A created spool that does not show up within two minutes is forgotten", "[BblFilament]")
{
    DriverRig d;
    d.load();
    const std::vector<fil::PrinterView> printers{with_tray(make_printer(), make_tray("1"))};
    d.rig.cloud->forced["create"].push_back(FakeCloud::ok(json::object())); // accepted, but no row appears
    const long long start = d.t;
    REQUIRE(d.driver->create_and_assign(page_spool(), fil::SlotKey{"dev1", 0, "1"}).ok);
    d.rig.settle();
    REQUIRE(d.count("list") == 1); // a newer list, without the spool

    d.t = start + fil::k_assignment_seconds;
    d.run(printers);
    CHECK(d.driver->status_json(printers)["creations"].size() == 1);
    d.t = start + fil::k_assignment_seconds + 1;
    d.run(printers);
    CHECK(d.driver->status_json(printers)["creations"].empty());

    // A matching spool that shows up later is not assigned.
    d.rig.cloud->rows.push_back(row_with(1000005));
    d.rig.svc->request_pull();
    d.rig.settle();
    d.t += fil::LinkDriver::k_round_spacing_seconds;
    d.run(printers);
    CHECK(d.driver->status_json(printers)["assignments"].empty());
    CHECK(d.count("sync_slots") == 0);
}

TEST_CASE("A refused create remembers nothing", "[BblFilament]")
{
    DriverRig d;
    d.load();
    json no_filament = page_spool();
    no_filament.erase("setting_id");
    const fil::Accept bad = d.driver->create_and_assign(no_filament, fil::SlotKey{"dev1", 0, "1"});
    CHECK_FALSE(bad.ok);
    CHECK(bad.kind == fil::FailKind::BadRequest);
    json no_colour = page_spool();
    no_colour["color_code"] = "";
    CHECK_FALSE(d.driver->create_and_assign(no_colour, fil::SlotKey{"dev1", 0, "1"}).ok);
    CHECK_FALSE(d.driver->create_and_assign(json::array(), fil::SlotKey{"dev1", 0, "1"}).ok);

    d.rig.session.signed_in = false;
    const fil::Accept out = d.driver->create_and_assign(page_spool(), fil::SlotKey{"dev1", 0, "1"});
    CHECK_FALSE(out.ok);
    CHECK(out.kind == fil::FailKind::NotSignedIn);
    d.rig.settle();
    CHECK(d.count("create") == 0);
    CHECK(d.driver->status_json({})["creations"].empty());
}

TEST_CASE("The baseline keeps an older identical spool from being assigned", "[BblFilament]")
{
    DriverRig d;
    d.rig.cloud->rows.push_back(row_with(1000001)); // the same filament and colour, not in a printer
    d.load();
    const std::vector<fil::PrinterView> printers{with_tray(make_printer(), make_tray("1"))};
    REQUIRE(d.driver->create_and_assign(page_spool(), fil::SlotKey{"dev1", 0, "1"}).ok);
    d.rig.settle();
    d.run(printers);
    REQUIRE(d.rig.cloud->slots_got.size() == 1);
    REQUIRE(d.rig.cloud->slots_got[0].size() == 1);
    CHECK(d.rig.cloud->slots_got[0][0].spool_id == "2000001");
}

TEST_CASE("A spool created before the first list is created but not assigned", "[BblFilament]")
{
    DriverRig d; // no list yet
    REQUIRE(d.driver->create_and_assign(page_spool(), fil::SlotKey{"dev1", 0, "1"}).ok);
    CHECK(d.driver->status_json({})["creations"].empty());
    d.rig.settle();
    CHECK(d.count("create") == 1);
}
