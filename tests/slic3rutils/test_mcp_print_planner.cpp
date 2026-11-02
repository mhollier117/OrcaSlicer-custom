// Tests for the print planner of the printer MCP tools (fork): the Print dialog's filament to tray mapping, its
// encodings and checks, on printers built from status JSON, without a Plater.

// Match the include environment that libslic3r_gui TUs get from pchheader.hpp (see test_dev_mapping.cpp).
#ifdef WIN32
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <Windows.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <wx/timer.h>

#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/DeviceCore/DevDefs.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevFirmware.h"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "slic3r/GUI/Mcp/McpPrinterCommon.hpp"
#include "slic3r/GUI/Mcp/McpPrintPlanner.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"
#include "slic3r/Utils/PublishCapture.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI::Mcp;
using nlohmann::json;
using Catch::Matchers::ContainsSubstring;

namespace {

struct TraySpec
{
    std::string color; // RRGGBBAA, empty: an empty slot
    std::string type;
};

struct AmsSpec
{
    std::string           id;
    unsigned              info; // bits 0-3 type (1 AMS, 4 AMS-HT), bits 8-11 extruder (0 right/main, 1 left/deputy)
    std::vector<TraySpec> trays;
};

void make_ready(MachineObject& obj)
{
    obj.m_full_msg_count = 1;
    obj.m_push_count     = 1;
    obj.last_push_time   = std::chrono::system_clock::now();
    obj.module_vers["ota"] = DevFirmwareVersionInfo();
}

void make_dual(MachineObject& obj)
{
    json ext = {{"state", 2}, {"info", json::array()}};
    for (int id = 0; id < 2; ++id)
        ext["info"].push_back({{"id", id}, {"filam_bak", json::array()}, {"info", 0}, {"temp", 0}, {"spre", 0}, {"snow", 0}, {"star", 0}, {"stat", 0}, {"hnow", id}});
    ExtderSystemParser::ParseV2_0(ext, obj.GetExtderSystem());
    REQUIRE(obj.GetExtderSystem()->GetTotalExtderCount() == 2);
}

void load_ams(MachineObject& obj, const std::vector<AmsSpec>& units)
{
    json ams = json::array();
    for (const AmsSpec& u : units) {
        char info[16];
        std::snprintf(info, sizeof(info), "%08X", u.info);
        json trays = json::array();
        for (size_t i = 0; i < u.trays.size(); ++i)
            trays.push_back({{"id", std::to_string(i)}, {"tray_color", u.trays[i].color.empty() ? "00000000" : u.trays[i].color}});
        ams.push_back({{"id", u.id}, {"info", info}, {"tray", trays}});
    }
    DevFilaSystemParser::ParseV1_0({{"ams", {{"tray_exist_bits", "0"}, {"ams", ams}}}}, &obj, obj.GetFilaSystem().get(), false);
    // The tray parse resolves the filament type through the GUI preset bundle, which tests do not have.
    for (const AmsSpec& u : units)
        for (size_t i = 0; i < u.trays.size(); ++i) {
            DevAmsTray* t = obj.GetFilaSystem()->GetAmsTray(u.id, std::to_string(i));
            REQUIRE(t != nullptr);
            t->m_fila_type = u.trays[i].type;
            t->is_exists   = !u.trays[i].color.empty();
        }
}

void add_ext_spool(MachineObject& obj, const std::string& id, const std::string& color, const std::string& type)
{
    DevAmsTray tray(id);
    tray.color       = color;
    tray.m_fila_type = type;
    tray.is_exists   = true;
    obj.vt_slot.push_back(tray);
}

PlanFilament fil(int id, const std::string& type, const std::string& color, int map = 0)
{
    PlanFilament f;
    f.id           = id;
    f.type         = type;
    f.color        = color;
    f.filament_id  = "GFA00";
    f.filament_map = map;
    f.nozzle       = map == 1 ? 1 : 0;
    return f;
}

PlanProject project_of(std::vector<PlanFilament> filaments, size_t slots, int nozzles = 1, std::vector<int> maps = {})
{
    PlanProject p;
    p.filaments        = std::move(filaments);
    p.slots            = slots;
    p.slot_filament_ids.assign(slots, "GFA00");
    p.nozzle_count     = nozzles;
    p.filament_maps    = std::move(maps);
    if (nozzles == 2) {
        p.nozzle_diameters    = {0.4, 0.4};
        p.nozzle_volume_types = {int(nvtStandard), int(nvtHighFlow)};
    }
    return p;
}

const PlanTarget& target(const PrintPlan& plan, int filament_id)
{
    for (const PlanTarget& t : plan.mapping)
        if (t.filament_id == filament_id)
            return t;
    FAIL("no target for filament " << filament_id);
    return plan.mapping.front();
}

bool has_check(const PrintPlan& plan, const std::string& id, const char* severity = nullptr)
{
    for (const PlanCheck& c : plan.checks)
        if (c.id == id && (severity == nullptr || c.severity == severity))
            return true;
    return false;
}

json no_overrides() { return json::array(); }

} // namespace

TEST_CASE("A single nozzle project maps to the AMS trays by colour and encodes tray indexes", "[McpPrintPlanner]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    make_ready(obj);
    load_ams(obj, {{"0", 0x1, {{"FF0000FF", "PLA"}, {"0000FFFF", "PLA"}, {"FFFFFFFF", "PLA"}, {"", ""}}}});

    // three project slots; slot 2 is not used by the plate
    PlanProject project = project_of({fil(0, "PLA", "#0000FFFF"), fil(2, "PLA", "#FFFFFFFF")}, 3);
    PrintPlan   plan    = make_plan(obj, project, PrinterFacts{}, PlanOptions{}, no_overrides());

    CHECK_FALSE(plan.dual_nozzle);
    CHECK(target(plan, 0).tray_index == 1);
    CHECK(target(plan, 0).ams_id == "0");
    CHECK(target(plan, 0).slot_id == "1");
    CHECK(target(plan, 0).source == "auto");
    CHECK(target(plan, 2).tray_index == 2);

    REQUIRE(plan.enc.available);
    CHECK(plan.enc.ams_mapping == json::array({1, -1, 2}));
    CHECK(plan.enc.ams_mapping2 == json::array({json{{"ams_id", 0}, {"slot_id", 1}}, json{{"ams_id", 255}, {"slot_id", 255}}, json{{"ams_id", 0}, {"slot_id", 2}}}));
    CHECK(plan.enc.ams_mapping_info[0]["ams"] == 1);
    CHECK(plan.enc.ams_mapping_info[0]["sourceColor"] == "#0000FFFF");
    CHECK(plan.enc.ams_mapping_info[0]["filamentType"] == "PLA");
    CHECK(plan.enc.ams_mapping_info[1]["ams"] == -1);
    CHECK_FALSE(plan.enc.ams_mapping_info[0].contains("nozzleId"));
    CHECK(plan.enc.nozzles_info.empty());
    CHECK(plan.enc.task_use_ams);
}

TEST_CASE("A filament type the AMS does not hold stays unmapped and blocks the print", "[McpPrintPlanner]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    make_ready(obj);
    load_ams(obj, {{"0", 0x1, {{"FF0000FF", "PLA"}, {"0000FFFF", "PLA"}}}});

    PlanProject project = project_of({fil(0, "PETG", "#FF0000FF")}, 1);
    PrintPlan   plan    = make_plan(obj, project, PrinterFacts{}, PlanOptions{}, no_overrides());

    CHECK(target(plan, 0).tray_index == -1);
    CHECK(target(plan, 0).ams_id.empty());
    CHECK(has_check(plan, "ams_mapping_invalid", "error"));
    CHECK_FALSE(plan.sendable);
    CHECK_FALSE(plan.enc.available);
}

TEST_CASE("Without an AMS a single nozzle printer takes the external spool", "[McpPrintPlanner]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    make_ready(obj);
    add_ext_spool(obj, "255", "FF0000FF", "PLA");

    PlanProject project = project_of({fil(0, "PETG", "#00FF00FF"), fil(1, "PLA", "#FF0000FF")}, 2);
    PrintPlan   plan    = make_plan(obj, project, PrinterFacts{}, PlanOptions{}, no_overrides());

    // the exact PLA match and, as the matcher's no-AMS special case does, the PETG one: there is no other choice
    CHECK(target(plan, 1).external);
    CHECK(target(plan, 1).tray_index == 255);
    CHECK(target(plan, 1).ams_id == "255");
    CHECK(target(plan, 0).external);
    REQUIRE(plan.enc.available);
    CHECK(plan.enc.ams_mapping == json::array({-1, -1}));
    CHECK(plan.enc.ams_mapping2[1] == json{{"ams_id", 255}, {"slot_id", 0}});
    CHECK(plan.enc.ams_mapping_info[1]["ams"] == 255);
    CHECK_FALSE(plan.enc.task_use_ams); // no AMS on the printer
    // the PETG filament on a PLA spool is warned about
    CHECK(has_check(plan, "external_filament_type_mismatch", "warning"));
}

TEST_CASE("On a dual nozzle printer each filament maps only into the AMS of its own side", "[McpPrintPlanner]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    make_ready(obj);
    make_dual(obj);
    // AMS 0 feeds the right (main) nozzle, AMS 1 the left (deputy) one; both hold a red PLA in slot 0
    load_ams(obj, {{"0", 0x1, {{"FF0000FF", "PLA"}, {"0000FFFF", "PLA"}}}, {"1", 0x101, {{"FF0000FF", "PLA"}, {"00FF00FF", "PLA"}}}});

    // filament 0 prints on the left nozzle (map 1), filament 1 on the right one (map 2)
    PlanProject project = project_of({fil(0, "PLA", "#FF0000FF", 1), fil(1, "PLA", "#0000FFFF", 2)}, 2, 2, {1, 2});
    PrintPlan   plan    = make_plan(obj, project, PrinterFacts{}, PlanOptions{}, no_overrides());

    CHECK(plan.dual_nozzle);
    CHECK(target(plan, 0).ams_id == "1"); // the left AMS' red, not AMS 0's identical red
    CHECK(target(plan, 0).tray_index == 4);
    CHECK(target(plan, 0).nozzle == 1);
    CHECK(target(plan, 1).ams_id == "0"); // the right AMS' blue
    CHECK(target(plan, 1).tray_index == 1);
    CHECK(target(plan, 1).nozzle == 0);

    REQUIRE(plan.enc.available);
    CHECK(plan.enc.ams_mapping == json::array({4, 1}));
    CHECK(plan.enc.ams_mapping_info[0]["nozzleId"] == 1); // left
    CHECK(plan.enc.ams_mapping_info[1]["nozzleId"] == 0); // right
    CHECK(plan.enc.nozzles_info.size() == 2);
    CHECK(plan.enc.nozzles_info[0]["id"] == 1);
    CHECK(plan.enc.nozzles_info[1]["id"] == 0);
    CHECK(plan.enc.nozzles_info[1]["flowSize"] == "high_flow");
    CHECK(plan.enc.nozzles_info[0]["type"].is_null());
    CHECK_FALSE(has_check(plan, "ams_mapping_invalid"));
}

TEST_CASE("A dual nozzle side without an AMS takes its own external spool and never the other side's", "[McpPrintPlanner]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    make_ready(obj);
    make_dual(obj);
    // only the right nozzle has an AMS; both external spools are loaded
    load_ams(obj, {{"0", 0x1, {{"FF0000FF", "PLA"}, {"0000FFFF", "PLA"}}}});
    add_ext_spool(obj, "254", "00FF00FF", "PLA");
    add_ext_spool(obj, "255", "00FF00FF", "PLA");

    // the left PLA matches the left spool; the right green PLA matches the right spool exactly but must take an AMS tray
    PlanProject project = project_of({fil(0, "PLA", "#00FF00FF", 1), fil(1, "PLA", "#00FF00FF", 2)}, 2, 2, {1, 2});
    PrintPlan   plan    = make_plan(obj, project, PrinterFacts{}, PlanOptions{}, no_overrides());

    CHECK(target(plan, 0).external);
    CHECK(target(plan, 0).ams_id == "254");
    CHECK(target(plan, 0).nozzle == 1);
    CHECK_FALSE(target(plan, 1).external);
    CHECK(target(plan, 1).ams_id == "0");
    REQUIRE(plan.enc.available);
    CHECK(plan.enc.ams_mapping == json::array({-1, target(plan, 1).tray_index}));
    CHECK(plan.enc.ams_mapping2[0] == json{{"ams_id", 254}, {"slot_id", 0}});
    CHECK(plan.enc.ams_mapping_info[0]["ams"] == 254);
}

TEST_CASE("An AMS-HT unit indexes its tray as ams id plus slot", "[McpPrintPlanner]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    make_ready(obj);
    load_ams(obj, {{"0", 0x1, {{"FF0000FF", "PLA"}}}, {"128", 0x4, {{"0000FFFF", "PETG"}}}});

    PlanProject project = project_of({fil(0, "PETG", "#0000FFFF")}, 1);
    PrintPlan   plan    = make_plan(obj, project, PrinterFacts{}, PlanOptions{}, no_overrides());

    CHECK(target(plan, 0).ams_id == "128");
    CHECK(target(plan, 0).tray_index == 128);
    CHECK(tray_index_of(obj, "128", "0") == 128);
    CHECK(tray_index_of(obj, "0", "0") == 0);
    CHECK(tray_index_of(obj, "5", "0") == -1);
    REQUIRE(plan.enc.available);
    CHECK(plan.enc.ams_mapping == json::array({128}));
    CHECK(plan.enc.ams_mapping2[0] == json{{"ams_id", 128}, {"slot_id", 0}});
}

TEST_CASE("Overrides are applied under the dialog's rules and refused with a reason otherwise", "[McpPrintPlanner]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    make_ready(obj);
    make_dual(obj);
    load_ams(obj, {{"0", 0x1, {{"FF0000FF", "PLA"}, {"0000FFFF", "PLA"}, {"00FF00FF", "PETG"}, {"", ""}}}, {"1", 0x101, {{"FFFF00FF", "PLA"}}}});
    add_ext_spool(obj, "255", "FFFFFFFF", "PLA");

    // filament 1 (slot number 2) prints on the right nozzle, filament 0 on the left one
    PlanProject project = project_of({fil(0, "PLA", "#FFFF00FF", 1), fil(1, "PLA", "#0000FFFF", 2)}, 2, 2, {1, 2});

    SECTION("a tray of the same type on the right side")
    {
        PrintPlan plan = make_plan(obj, project, PrinterFacts{}, PlanOptions{}, json::array({json{{"filament", 2}, {"ams_id", "0"}, {"slot_id", "0"}}}));
        CHECK(target(plan, 1).source == "override");
        CHECK(target(plan, 1).tray_index == 0);
        CHECK(target(plan, 0).source == "auto");
        CHECK(plan.enc.ams_mapping == json::array({4, 0}));
    }
    SECTION("the right external spool")
    {
        PrintPlan plan = make_plan(obj, project, PrinterFacts{}, PlanOptions{}, json::array({json{{"filament", 2}, {"external", "right"}}}));
        CHECK(target(plan, 1).external);
        CHECK(target(plan, 1).tray_index == 255);
        CHECK(target(plan, 1).source == "override");
    }
    SECTION("a tray on the wrong side")
    {
        CHECK_THROWS_WITH(make_plan(obj, project, PrinterFacts{}, PlanOptions{}, json::array({json{{"filament", 2}, {"ams_id", "1"}, {"slot_id", "0"}}})),
                          ContainsSubstring("feeds the left nozzle"));
        CHECK_THROWS_WITH(make_plan(obj, project, PrinterFacts{}, PlanOptions{}, json::array({json{{"filament", 2}, {"external", "left"}}})),
                          ContainsSubstring("cannot use the left external spool"));
    }
    SECTION("an empty tray")
    {
        CHECK_THROWS_WITH(make_plan(obj, project, PrinterFacts{}, PlanOptions{}, json::array({json{{"filament", 2}, {"ams_id", 0}, {"slot_id", 3}}})),
                          ContainsSubstring("is empty"));
    }
    SECTION("a tray of another filament type")
    {
        CHECK_THROWS_WITH(make_plan(obj, project, PrinterFacts{}, PlanOptions{}, json::array({json{{"filament", 2}, {"ams_id", "0"}, {"slot_id", "2"}}})),
                          ContainsSubstring("holds PETG"));
    }
    SECTION("a filament the plate does not use, and a tray that does not exist")
    {
        CHECK_THROWS_WITH(make_plan(obj, project, PrinterFacts{}, PlanOptions{}, json::array({json{{"filament", 5}, {"external", "right"}}})),
                          ContainsSubstring("not used by this plate"));
        CHECK_THROWS_WITH(make_plan(obj, project, PrinterFacts{}, PlanOptions{}, json::array({json{{"filament", 2}, {"ams_id", "7"}, {"slot_id", "0"}}})),
                          ContainsSubstring("no AMS 7"));
    }
}

TEST_CASE("The plan's checks follow the dialog's blocking errors and confirmation warnings", "[McpPrintPlanner]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    load_ams(obj, {{"0", 0x1, {{"FF0000FF", "PLA"}}}});
    add_ext_spool(obj, "255", "00FF00FF", "PETG");
    PlanProject project = project_of({fil(0, "PLA", "#FF0000FF")}, 1);

    SECTION("a printer that has not reported its state")
    {
        PrintPlan plan = make_plan(obj, project, PrinterFacts{}, PlanOptions{}, no_overrides());
        CHECK(has_check(plan, "printer_info_not_ready", "error"));
        CHECK_FALSE(plan.sendable);
    }

    make_ready(obj);

    SECTION("an empty plate and a missing agent end the checks early")
    {
        project.gcode_valid = false;
        CHECK(has_check(make_plan(obj, project, PrinterFacts{}, PlanOptions{}, no_overrides()), "blank_plate", "error"));
        project.gcode_valid = true;
        PrinterFacts facts;
        facts.agent_present = false;
        CHECK(has_check(make_plan(obj, project, facts, PlanOptions{}, no_overrides()), "no_network_agent", "error"));
    }
    SECTION("AMS and external spool together when the printer does not support it")
    {
        // filament 0 on AMS, filament 1 on the external spool
        PlanProject two = project_of({fil(0, "PLA", "#FF0000FF"), fil(1, "PETG", "#00FF00FF")}, 2);
        PrinterFacts facts;
        facts.ams_ext_mix_supported = false;
        PrintPlan plan = make_plan(obj, two, facts, PlanOptions{}, json::array({json{{"filament", 2}, {"external", "right"}}}));
        CHECK(has_check(plan, "ams_and_external_mixed", "error"));
        CHECK_FALSE(plan.sendable);
        facts.ams_ext_mix_supported = true;
        CHECK(has_check(make_plan(obj, two, facts, PlanOptions{}, json::array({json{{"filament", 2}, {"external", "right"}}})), "ams_and_external_on_one_extruder", "warning"));
    }
    SECTION("two filament types on one external spool need a confirmation")
    {
        PlanProject two = project_of({fil(0, "PLA", "#FF0000FF"), fil(1, "PETG", "#00FF00FF")}, 2);
        two.filaments[0].filament_id = "GFA00";
        two.filaments[1].filament_id = "GFG00";
        PrintPlan plan = make_plan(obj, two, PrinterFacts{}, PlanOptions{}, json::array({json{{"filament", 1}, {"external", "right"}}, json{{"filament", 2}, {"external", "right"}}}));
        REQUIRE(has_check(plan, "external_spool_shared", "warning"));
        CHECK(plan.needs_confirmation);
        // assisted external change makes the sharing intentional
        PlanOptions options;
        options.ext_change_assist = true;
        CHECK_FALSE(has_check(make_plan(obj, two, PrinterFacts{}, options, json::array({json{{"filament", 1}, {"external", "right"}}, json{{"filament", 2}, {"external", "right"}}})),
                              "external_spool_shared"));
    }
    SECTION("the filament blacklist: prohibitions block, warnings ask for confirmation")
    {
        PrinterFacts facts;
        facts.blacklist = [](const DevFilaBlacklist::CheckFilamentInfo&) {
            DevFilaBlacklist::CheckResult r;
            r.action_items["prohibition"].push_back({"prohibition", "Do not use this here", ""});
            r.action_items["warning"].push_back({"warning", "Careful with this", ""});
            return r;
        };
        PrintPlan plan = make_plan(obj, project, facts, PlanOptions{}, no_overrides());
        CHECK(has_check(plan, "filament_blacklisted_1", "error"));
        CHECK(has_check(plan, "filament_blacklist_warning_1", "warning"));
        CHECK_FALSE(plan.sendable);
        // without a blacklist the check is reported as skipped, not dropped
        PrintPlan bare = make_plan(obj, project, PrinterFacts{}, PlanOptions{}, no_overrides());
        bool      listed = false;
        for (const json& s : bare.checks_skipped)
            listed = listed || s["id"] == "filament_blacklist";
        CHECK(listed);
    }
    SECTION("more filaments than the printer takes")
    {
        PlanProject many = project_of({fil(0, "PLA", "#FF0000FF")}, 17);
        CHECK(has_check(make_plan(obj, many, PrinterFacts{}, PlanOptions{}, no_overrides()), "too_many_filaments", "error"));
    }
    SECTION("timelapse without a readable reason is a warning from the slice")
    {
        project.slice_warnings.push_back({NOT_GENERATE_TIMELAPSE, "10014002", ""});
        PrintPlan plan = make_plan(obj, project, PrinterFacts{}, PlanOptions{}, no_overrides());
        REQUIRE(has_check(plan, "timelapse_not_generated", "warning"));
        for (const PlanCheck& c : plan.checks)
            if (c.id == "timelapse_not_generated")
                CHECK_THAT(c.message, ContainsSubstring("By-Object"));
    }
}

TEST_CASE("Building a plan and its PrintParams sends nothing to the printer", "[McpPrintPlanner]")
{
    MachineObject obj(nullptr, nullptr, "test", "test_dev", "127.0.0.1");
    make_ready(obj);
    load_ams(obj, {{"0", 0x1, {{"FF0000FF", "PLA"}}}});

    ScopedPublishCapture capture;
    PlanProject          project = project_of({fil(0, "PLA", "#FF0000FF")}, 1);
    PrintPlan            plan    = make_plan(obj, project, PrinterFacts{}, PlanOptions{}, no_overrides());

    PrintSetup setup;
    setup.dev_id       = "test_dev";
    setup.access_code  = "secret-code";
    setup.ams_mapping  = plan.enc.ams_mapping.dump();
    setup.ams_mapping2 = plan.enc.ams_mapping2.dump();
    setup.file_3mf     = "plate_1.3mf";
    const json params  = print_params_json(setup);
    CHECK(params["ams_mapping"] == "[0]");
    CHECK(params["plate_index"] == 1);
    CHECK(params["print_type"] == "from_normal");

    // callers redact the secrets before returning the parameters
    const json redacted = redact(params);
    CHECK(redacted["password"] == "<redacted>");
    CHECK(redacted.dump().find("secret-code") == std::string::npos);

    CHECK(capture.messages().empty());
    // plan_to_json carries the encodings and the check lists
    const json j = plan_to_json(plan);
    CHECK(j["encodings"]["ams_mapping"] == json::array({0}));
    CHECK(j.contains("errors"));
    CHECK(j.contains("warnings"));
    CHECK(j.contains("checks_skipped"));
}

TEST_CASE("The dialog's filament list comes from the used slots", "[McpPrintPlanner]")
{
    std::vector<PlanSlot> slots = {{"Bambu PLA", "PLA", "GFA00", "Bambu Lab", "#FF0000FF"},
                                   {"Bambu PETG", "PETG", "GFG00", "Bambu Lab", "#00FF00FF"},
                                   {"Generic ABS", "ABS", "GFB99", "Generic", "#0000FFFF"}};
    // used slots are 1-based as PartPlate::get_used_filaments returns them; slot 9 does not exist
    auto filaments = filaments_from(slots, {1, 3, 9}, {1, 2, 2}, 2);
    REQUIRE(filaments.size() == 2);
    CHECK(filaments[0].id == 0);
    CHECK(filaments[0].type == "PLA");
    CHECK(filaments[0].nozzle == 1); // map 1 is the left nozzle
    CHECK(filaments[1].id == 2);
    CHECK(filaments[1].nozzle == 0);
    CHECK(filaments[1].brand == "Generic");
    // on a single nozzle project no filament belongs to a side
    CHECK(filaments_from(slots, {1}, {1, 1, 1}, 1)[0].filament_map == 0);
}
