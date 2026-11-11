#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/BblPresetSync/KCopy.hpp"

#include <string>
#include <vector>

namespace sync = Slic3r::BblPresetSync;

namespace {

sync::KEntry entry(const std::string& filament_id, const std::string& name, int extruder = 0, int volume = 0, float k = 0.04f)
{
    sync::KEntry e;
    e.filament_id        = filament_id;
    e.name               = name;
    e.extruder_id        = extruder;
    e.nozzle_volume_type = volume;
    e.k_value            = k;
    e.n_coef             = 1.4f;
    e.nozzle_diameter    = 0.4f;
    e.cali_idx           = 7;
    return e;
}

} // namespace

TEST_CASE("plan_k_copy has nothing to copy when the old id has no entries", "[BblPresetSync]")
{
    const auto plan = sync::plan_k_copy({entry("OTHER", "a")}, "OLD", "NEW", true);
    CHECK(plan.to_write.empty());
    CHECK(plan.skipped_existing == 0);
    CHECK(plan.skipped_full == 0);
}

TEST_CASE("plan_k_copy copies entries on different extruders under the new id", "[BblPresetSync]")
{
    const auto plan = sync::plan_k_copy({entry("OLD", "left", 0, 0, 0.03f), entry("OTHER", "x"), entry("OLD", "right", 1, 0, 0.05f)}, "OLD", "NEW", true);
    REQUIRE(plan.to_write.size() == 2);
    CHECK(plan.to_write[0].filament_id == "NEW");
    CHECK(plan.to_write[0].name == "left");
    CHECK(plan.to_write[0].extruder_id == 0);
    CHECK(plan.to_write[0].k_value == 0.03f);
    CHECK(plan.to_write[0].cali_idx == -1);
    CHECK(plan.to_write[1].name == "right");
    CHECK(plan.to_write[1].extruder_id == 1);
    CHECK(plan.to_write[1].n_coef == 1.4f);
}

TEST_CASE("plan_k_copy skips a duplicate under the new id but not the same name on another extruder", "[BblPresetSync]")
{
    const auto plan = sync::plan_k_copy({entry("OLD", "same", 0), entry("NEW", "same", 0), entry("OLD", "same", 1)}, "OLD", "NEW", true);
    CHECK(plan.skipped_existing == 1);
    REQUIRE(plan.to_write.size() == 1);
    CHECK(plan.to_write[0].extruder_id == 1);
}

TEST_CASE("plan_k_copy counts entries it plans itself as duplicates", "[BblPresetSync]")
{
    const auto plan = sync::plan_k_copy({entry("OLD", "dup", 0, 0, 0.03f), entry("OLD", "dup", 0, 0, 0.04f)}, "OLD", "NEW", true);
    CHECK(plan.to_write.size() == 1);
    CHECK(plan.skipped_existing == 1);
}

TEST_CASE("plan_k_copy respects the table limit per nozzle only when limited", "[BblPresetSync]")
{
    // 13 other entries and the 2 to copy: 15 on extruder 0, room for one more. Extruder 1 is empty apart from its own entry.
    std::vector<sync::KEntry> table;
    for (int i = 0; i < sync::K_TABLE_LIMIT - 3; ++i) table.push_back(entry("FILL", "fill" + std::to_string(i)));
    table.push_back(entry("OLD", "a"));
    table.push_back(entry("OLD", "b"));
    table.push_back(entry("OLD", "c", 1, 0));

    SECTION("limited")
    {
        const auto plan = sync::plan_k_copy(table, "OLD", "NEW", true);
        REQUIRE(plan.to_write.size() == 2);
        CHECK(plan.to_write[0].name == "a");
        CHECK(plan.to_write[1].name == "c"); // the other extruder has room
        CHECK(plan.skipped_full == 1);
    }
    SECTION("not limited")
    {
        const auto plan = sync::plan_k_copy(table, "OLD", "NEW", false);
        CHECK(plan.skipped_full == 0);
        CHECK(plan.to_write.size() == 3);
    }
}

TEST_CASE("plan_k_copy of equal or empty ids is empty", "[BblPresetSync]")
{
    const std::vector<sync::KEntry> table{entry("OLD", "a")};
    CHECK(sync::plan_k_copy(table, "OLD", "OLD", true).to_write.empty());
    CHECK(sync::plan_k_copy(table, "", "NEW", true).to_write.empty());
}

namespace {

// A connected cloud printer with its info ready, the PA flag set and one 0.4 nozzle.
sync::KPrinterFacts printer(const std::string& dev_id, bool selected = false)
{
    sync::KPrinterFacts f;
    f.dev_id     = dev_id;
    f.name       = "name-" + dev_id;
    f.selected   = selected;
    f.connected  = true;
    f.info_ready = true;
    f.pa_flag    = true;
    f.diameters  = {0.4f};
    return f;
}

std::vector<std::string> ids(const std::vector<sync::KPrinter>& printers)
{
    std::vector<std::string> out;
    for (const sync::KPrinter& p : printers) out.push_back(p.dev_id);
    return out;
}

} // namespace

TEST_CASE("k_copy_printers takes the selected printer alone", "[BblPresetSync]")
{
    sync::KPrinterFacts sel = printer("SEL", true);
    sel.diameters           = {0.4f, 0.0f, 0.6f, 0.4f};
    const auto out          = sync::k_copy_printers({sel});
    REQUIRE(out.size() == 1);
    CHECK(out[0].dev_id == "SEL");
    CHECK(out[0].name == "name-SEL");
    CHECK(out[0].diameters == std::vector<float>{0.4f, 0.6f});
    CHECK(out[0].limited);
}

TEST_CASE("k_copy_printers takes another cloud printer with its own facts, after the selected one", "[BblPresetSync]")
{
    sync::KPrinterFacts other = printer("OTHER");
    other.diameters           = {0.6f};
    const auto out            = sync::k_copy_printers({other, printer("SEL", true)});
    CHECK(ids(out) == std::vector<std::string>{"SEL", "OTHER"});
    REQUIRE(out.size() == 2);
    CHECK(out[1].diameters == std::vector<float>{0.6f});
}

TEST_CASE("k_copy_printers takes the tap's diameter and PA support when the printer's own are missing", "[BblPresetSync]")
{
    sync::KPrinterFacts other = printer("OTHER");
    other.pa_flag             = false;
    other.diameters           = {0.0f};
    other.tap_diameter        = 0.4;
    other.tap_pa_supported    = true;
    const auto out            = sync::k_copy_printers({printer("SEL", true), other});
    REQUIRE(out.size() == 2);
    CHECK(out[1].dev_id == "OTHER");
    CHECK(out[1].diameters == std::vector<float>{0.4f});

    SECTION("the tap's diameter wins over the model's for a printer that is not selected")
    {
        other.diameters  = {0.6f};
        const auto mixed = sync::k_copy_printers({other});
        REQUIRE(mixed.size() == 1);
        CHECK(mixed[0].diameters == std::vector<float>{0.4f});
    }
    SECTION("the model's diameters win over the tap's for the selected printer")
    {
        other.selected   = true;
        other.diameters  = {0.6f};
        const auto mixed = sync::k_copy_printers({other});
        REQUIRE(mixed.size() == 1);
        CHECK(mixed[0].diameters == std::vector<float>{0.6f});
    }
    SECTION("a printer that is not selected keeps the model's diameters while the tap has none")
    {
        other.diameters    = {0.6f};
        other.tap_diameter = 0;
        const auto mixed   = sync::k_copy_printers({other});
        REQUIRE(mixed.size() == 1);
        CHECK(mixed[0].diameters == std::vector<float>{0.6f});
    }
}

TEST_CASE("k_copy_printers leaves out a printer without PA support or without a known diameter", "[BblPresetSync]")
{
    sync::KPrinterFacts no_pa = printer("NOPA");
    no_pa.pa_flag             = false;
    sync::KPrinterFacts tap_no = printer("TAPNO");
    tap_no.pa_flag             = false;
    tap_no.tap_pa_supported    = false;
    sync::KPrinterFacts no_d = printer("NOD");
    no_d.diameters           = {};
    no_d.tap_diameter        = 0;
    sync::KPrinterFacts neither = printer("NEITHER");
    neither.pa_flag             = false;
    neither.diameters           = {0.0f};
    CHECK(sync::k_copy_printers({no_pa, tap_no, no_d, neither}).empty());
}

TEST_CASE("k_copy_printers counts the X1 series as supported without the flag, and only it as unlimited", "[BblPresetSync]")
{
    sync::KPrinterFacts x1 = printer("X1");
    x1.pa_flag             = false;
    x1.series_x1           = true;
    sync::KPrinterFacts other = printer("OTHER");
    const auto out            = sync::k_copy_printers({x1, other});
    REQUIRE(out.size() == 2);
    CHECK(out[0].dev_id == "X1");
    CHECK_FALSE(out[0].limited);
    CHECK(out[1].limited);
}

TEST_CASE("k_copy_printers leaves out a LAN printer that is not selected", "[BblPresetSync]")
{
    sync::KPrinterFacts lan = printer("LAN");
    lan.lan                 = true;
    CHECK(sync::k_copy_printers({lan}).empty());
    lan.selected = true;
    CHECK(ids(sync::k_copy_printers({lan})) == std::vector<std::string>{"LAN"});
}

TEST_CASE("k_copy_printers leaves out a printer that is not connected or not ready", "[BblPresetSync]")
{
    sync::KPrinterFacts off = printer("OFF", true);
    off.connected           = false;
    sync::KPrinterFacts not_ready = printer("NOTREADY");
    not_ready.info_ready          = false;
    CHECK(sync::k_copy_printers({off, not_ready}).empty());
}

TEST_CASE("k_copy_printers lists the selected printer once when the account lists it too", "[BblPresetSync]")
{
    sync::KPrinterFacts sel = printer("SEL", true);
    sel.diameters           = {0.6f};
    const auto out          = sync::k_copy_printers({printer("OTHER"), printer("SEL"), sel});
    CHECK(ids(out) == std::vector<std::string>{"SEL", "OTHER"});
    REQUIRE(out.size() == 2);
    CHECK(out[0].diameters == std::vector<float>{0.6f}); // the selected printer's entry counts
}
