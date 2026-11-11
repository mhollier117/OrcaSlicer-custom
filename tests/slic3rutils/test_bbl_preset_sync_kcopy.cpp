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
