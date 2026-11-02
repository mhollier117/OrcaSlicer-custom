// Tests for the printer MCP tools (fork): printer_list / printer_status / printer_job / printer_hms builders.
// Windows include preamble as in test_dev_mapping.cpp (Windows.h first, wx/timer.h before DeviceManager.hpp).
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
#include <catch2/catch_all.hpp>

#include <wx/timer.h>

#include "slic3r/GUI/DeviceCore/DevDefs.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/Mcp/McpPrinterStatus.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/OrcaPrinterAgent.hpp"
#include "slic3r/Utils/PublishCapture.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI::Mcp;
using json = nlohmann::json;

namespace {

const std::string k_secret = "SEKRET99";

// tray_info_idx / tray_type are left out of the pushes: parsing them reads the GUI preset bundle,
// which a headless test has no. The tray type is set directly afterwards (as test_dev_mapping.cpp does).
void set_tray(MachineObject& m, const char* ams, const char* tray, const char* type)
{
    DevAmsTray* t = m.GetFilaSystem()->GetAmsTray(ams, tray);
    REQUIRE(t != nullptr);
    t->m_fila_type = type;
}

void single_nozzle_one_ams(MachineObject& m)
{
    m.set_access_code(k_secret, false);
    m.dev_connection_type = "lan";
    m.print_status        = "RUNNING";
    m.mc_print_percent    = 42;
    m.mc_left_time        = 90 * 60;
    m.curr_layer          = 10;
    m.total_layers        = 100;
    m.subtask_name        = "benchy";
    const json print = json::parse(R"({
        "nozzle_temper": 215, "nozzle_target_temper": 220,
        "ams": {
            "tray_exist_bits": "7", "ams_exist_bits": "1", "tray_now": "1",
            "ams": [ { "id": "0", "info": "00000001", "humidity": "3", "temp": "24.5", "tray": [
                { "id": "0", "tray_color": "FF0000FF", "remain": 80, "k": 0.02 },
                { "id": "1", "tray_color": "00FF00FF", "remain": 50 },
                { "id": "2", "tray_color": "0000FFFF", "cols": ["0000FFFF", "FFFFFFFF"] },
                { "id": "3", "tray_color": "00000000" }
            ] } ]
        }
    })");
    ExtderSystemParser::ParseV1_0(print, m.GetExtderSystem());
    DevFilaSystemParser::ParseV1_0(print, &m, m.GetFilaSystem().get(), false);
    // the parse reads tray_exist_bits from the message through MachineObject::parse_json; set it as it would be
    m.tray_exist_bits = 7;
    m.ams_exist_bits  = 1;
    DevFilaSystemParser::ParseV1_0(print, &m, m.GetFilaSystem().get(), false);
    set_tray(m, "0", "0", "PLA");
    set_tray(m, "0", "1", "PETG");
    set_tray(m, "0", "2", "PLA");
    m.vt_slot.clear(); // the object starts with placeholder external slots
    DevAmsTray ext("255");
    ext.m_fila_type = "TPU";
    ext.color       = "FFFFFFFF";
    m.vt_slot.push_back(ext);
}

void dual_nozzle(MachineObject& m)
{
    m.printer_type = "O1D";
    const json extruders = json::parse(R"({
        "state": 2,
        "info": [
          { "id": 0, "filam_bak": [], "info": 8, "temp": 14418140, "spre": 0, "snow": 1, "star": 0, "stat": 0, "hnow": 0 },
          { "id": 1, "filam_bak": [], "info": 8, "temp": 0, "spre": 0, "snow": 258, "star": 0, "stat": 0, "hnow": 0 }
        ]
    })");
    ExtderSystemParser::ParseV2_0(extruders, m.GetExtderSystem());
    m.tray_exist_bits = 0x100F7; // AMS0 slot3 empty, AMS1 all four, AMS-HT 128 present
    m.ams_exist_bits  = 0x13;
    const json print = json::parse(R"({
        "ams": { "ams": [
            { "id": "0",   "info": "00000001", "tray": [ { "id": "0", "tray_color": "FF0000FF" }, { "id": "1", "tray_color": "00FF00FF" },
                                                       { "id": "2", "tray_color": "0000FFFF" }, { "id": "3", "tray_color": "00000000" } ] },
            { "id": "1",   "info": "00000101", "tray": [ { "id": "0", "tray_color": "111111FF" }, { "id": "1", "tray_color": "222222FF" },
                                                       { "id": "2", "tray_color": "333333FF" }, { "id": "3", "tray_color": "444444FF" } ] },
            { "id": "128", "info": "00000104", "tray": [ { "id": "0", "tray_color": "ABCDEFFF" } ] }
        ] }
    })");
    DevFilaSystemParser::ParseV1_0(print, &m, m.GetFilaSystem().get(), false);
    set_tray(m, "0", "0", "PLA");
    set_tray(m, "1", "2", "PETG");
    set_tray(m, "128", "0", "ABS");
    m.vt_slot.clear();
    for (const char* id : {"255", "254"}) {
        DevAmsTray ext(id);
        ext.m_fila_type = "TPU";
        ext.color       = "FFFFFFFF";
        m.vt_slot.push_back(ext);
    }
}

const json* find_unit(const json& ams, int id)
{
    for (const json& u : ams["units"])
        if (u["ams_id"] == id)
            return &u;
    return nullptr;
}

} // namespace

TEST_CASE("printer_summary_json describes a LAN printer without its access code", "[McpPrinter]")
{
    MachineObject m(nullptr, nullptr, "Bench", "dev123", "192.168.1.50");
    single_nozzle_one_ams(m);
    const json s = printer_summary_json(m, true);
    CHECK(s["dev_id"] == "dev123");
    CHECK(s["name"] == "Bench");
    CHECK(s["connection"] == "lan");
    CHECK(s["selected"] == true);
    CHECK(s["has_access_code"] == true);
    CHECK(s["ip"] == "192.168.1.50");
    CHECK(s["printing"] == true);
    CHECK(s["print_status"] == "RUNNING");
    CHECK(s["progress"] == 42);
    CHECK(s.dump().find(k_secret) == std::string::npos);
}

TEST_CASE("printer_status_json reports a single-nozzle printer with one AMS", "[McpPrinter]")
{
    MachineObject m(nullptr, nullptr, "Bench", "dev123", "192.168.1.50");
    single_nozzle_one_ams(m);
    const json st = printer_status_json(m, {});

    CHECK(st["print"]["progress"] == 42);
    CHECK(st["print"]["remaining_min"] == 90);
    CHECK(st["print"]["layer"] == 10);
    CHECK(st["print"]["total_layers"] == 100);
    CHECK(st["print"]["can_pause"] == true);
    CHECK(st["print"]["can_resume"] == false);
    CHECK(st["print"]["can_abort"] == true);

    REQUIRE(st["temps"]["extruders"].size() == 1);
    CHECK(st["temps"]["extruders"][0]["side"] == "main");
    CHECK(st["temps"]["extruders"][0]["current"] == 215);
    CHECK(st["temps"]["extruders"][0]["target"] == 220);

    const json* unit = find_unit(st["ams"], 0);
    REQUIRE(unit != nullptr);
    CHECK((*unit)["type"] == "ams");
    CHECK((*unit)["extruder"] == "main");
    CHECK((*unit)["humidity_level"] == 3);
    REQUIRE((*unit)["trays"].size() == 4);
    const json& t0 = (*unit)["trays"][0];
    CHECK(t0["tray_index"] == 0);
    CHECK(t0["color"] == "#FF0000FF");
    CHECK(t0["type"] == "PLA");
    CHECK(t0["remain"] == 80);
    CHECK(t0["empty"] == false);
    CHECK((*unit)["trays"][2]["colors"].size() == 2);
    CHECK((*unit)["trays"][3]["empty"] == true);
    CHECK((*unit)["trays"][3]["tray_index"] == 3);

    REQUIRE(st["ams"]["external_spools"].size() == 1);
    CHECK(st["ams"]["external_spools"][0]["id"] == 255);
    CHECK(st["ams"]["external_spools"][0]["side"] == "main");
    CHECK(st["ams"]["external_spools"][0]["type"] == "TPU");

    CHECK(st["capabilities"]["is_multi_extruders"] == false);
    CHECK(st["nozzles"]["rack"]["supported"] == false);
    CHECK(st.dump().find(k_secret) == std::string::npos);
}

TEST_CASE("printer_status_json maps AMS units to the sides of a dual-nozzle printer", "[McpPrinter]")
{
    MachineObject m(nullptr, nullptr, "H2D", "devH2D", "192.168.1.60");
    dual_nozzle(m);
    const json st = printer_status_json(m, {"temps", "ams", "nozzles", "capabilities"});

    CHECK(st["capabilities"]["is_multi_extruders"] == true);
    REQUIRE(st["temps"]["extruders"].size() == 2);
    CHECK(st["temps"]["extruders"][0]["side"] == "right");
    CHECK(st["temps"]["extruders"][1]["side"] == "left");
    CHECK(st["temps"]["extruders"][0]["current"] == 220);
    CHECK(st["temps"]["extruders"][0]["target"] == 220);
    CHECK(st["nozzles"]["extruders"].size() == 2);

    const json* a0 = find_unit(st["ams"], 0);
    const json* a1 = find_unit(st["ams"], 1);
    const json* ht = find_unit(st["ams"], 128);
    REQUIRE(a0 != nullptr);
    REQUIRE(a1 != nullptr);
    REQUIRE(ht != nullptr);
    CHECK((*a0)["extruder"] == "right");
    CHECK((*a0)["bound_extruders"] == json::array({"right"}));
    CHECK((*a1)["extruder"] == "left");
    CHECK((*a1)["bound_extruders"] == json::array({"left"}));
    CHECK((*ht)["type"] == "ams_ht");
    CHECK((*ht)["extruder"] == "left");

    // tray_index formula of DevMapping.cpp: ams*4+slot, AMS-HT ams+slot
    CHECK((*a0)["trays"][3]["tray_index"] == 3);
    CHECK((*a0)["trays"][3]["empty"] == true);
    CHECK((*a1)["trays"][2]["tray_index"] == 6);
    CHECK((*a1)["trays"][2]["type"] == "PETG");
    CHECK((*ht)["trays"][0]["tray_index"] == 128);
    CHECK((*ht)["trays"][0]["exists"] == true);

    REQUIRE(st["ams"]["external_spools"].size() == 2);
    CHECK(st["ams"]["external_spools"][0]["id"] == 255);
    CHECK(st["ams"]["external_spools"][0]["side"] == "right");
    CHECK(st["ams"]["external_spools"][1]["id"] == 254);
    CHECK(st["ams"]["external_spools"][1]["side"] == "left");

    REQUIRE(st["ams"]["current_tray"].size() == 2);
    CHECK(st["ams"]["current_tray"][0]["side"] == "right");
    CHECK(st["ams"]["current_tray"][0]["slot_id"] == "1");
    CHECK(st["ams"]["current_tray"][1]["ams_id"] == "1");
    CHECK(st["ams"]["current_tray"][1]["slot_id"] == "2");
}

TEST_CASE("printer_tray_index follows the mapping formulas", "[McpPrinter]")
{
    CHECK(printer_tray_index(false, false, 2, 3) == 11);
    CHECK(printer_tray_index(false, true, 128, 0) == 128);
    CHECK(printer_tray_index(true, false, 0, 2) == 26);
}

TEST_CASE("printer_status_json rejects unknown sections and survives an empty printer", "[McpPrinter]")
{
    MachineObject m(nullptr, nullptr, "Bare", "bare", "10.0.0.1");
    CHECK_THROWS_AS(printer_status_json(m, {"print", "bogus"}), ToolFailure);
    try {
        printer_status_json(m, {"bogus"});
    } catch (const ToolFailure& e) {
        CHECK(std::string(e.what()).find("capabilities") != std::string::npos);
    }
    const json st = printer_status_json(m, {});
    for (const std::string& name : printer_status_section_names())
        CHECK(st.contains(name));
    CHECK(st["ams"]["units"].empty());
    CHECK(st["hms"].is_array());
}

TEST_CASE("printer_job and printer_hms gate on the printer state", "[McpPrinter]")
{
    MachineObject m(nullptr, nullptr, "Bench", "dev123", "192.168.1.50");
    m.print_status = "IDLE";
    CHECK_THROWS_AS(check_printer_job_allowed(m, "pause", {}), ToolFailure);
    CHECK_THROWS_AS(check_printer_job_allowed(m, "stop", {}), ToolFailure);
    CHECK_THROWS_AS(check_printer_job_allowed(m, "bogus", {}), ToolFailure);
    m.print_status = "RUNNING";
    CHECK_NOTHROW(check_printer_job_allowed(m, "pause", {}));
    CHECK_THROWS_AS(check_printer_job_allowed(m, "resume", {}), ToolFailure);
    CHECK_NOTHROW(check_printer_job_allowed(m, "stop", {}));
    CHECK_THROWS_AS(check_printer_job_allowed(m, "skip_objects", {1}), ToolFailure); // printer lacks support
    m.is_support_partskip = true;
    CHECK_THROWS_AS(check_printer_job_allowed(m, "skip_objects", {}), ToolFailure);
    CHECK_NOTHROW(check_printer_job_allowed(m, "skip_objects", {1, 2}));
    m.print_status = "PAUSE";
    CHECK_NOTHROW(check_printer_job_allowed(m, "resume", {}));
    m.jobState_ = 2;
    CHECK_THROWS_AS(check_printer_job_allowed(m, "resume", {}), ToolFailure);

    m.print_error = 0;
    CHECK_THROWS_AS(check_printer_hms_allowed(m, "resume"), ToolFailure);
    CHECK_NOTHROW(check_printer_hms_allowed(m, "stop_buzzer"));
    m.print_error = 0x0300400C;
    CHECK_NOTHROW(check_printer_hms_allowed(m, "stop"));
}

namespace {

class RecordingAgent final : public OrcaPrinterAgent
{
public:
    RecordingAgent() : OrcaPrinterAgent("") {}
    AgentInfo get_agent_info() override { return {"mcp-test", "MCP test agent", "1.0", "test"}; }
    int       send_message(std::string, std::string json_str, int, int) override { messages.push_back(json::parse(json_str)); return 0; }
    int       send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override
    {
        return send_message(std::move(dev_id), std::move(json_str), qos, flag);
    }
    std::vector<json> messages;
};

} // namespace

TEST_CASE("printer_job and printer_hms commands carry the expected payloads", "[McpPrinter]")
{
    auto          agent = std::make_shared<RecordingAgent>();
    NetworkAgent  network(nullptr, agent);
    MachineObject m(nullptr, &network, "Bench", "dev123", "192.168.1.50");
    m.dev_connection_type = "lan";
    m.job_id_             = "job-7";
    m.print_error         = 0x0300400C;

    SECTION("pause / resume / stop / skip")
    {
        CHECK(send_printer_job_command(m, "pause", {}) == 0);
        CHECK(send_printer_job_command(m, "resume", {}) == 0);
        CHECK(send_printer_job_command(m, "stop", {}) == 0);
        CHECK(send_printer_job_command(m, "skip_objects", {3, 5}) == 0);
        REQUIRE(agent->messages.size() == 4);
        CHECK(agent->messages[0]["print"]["command"] == "pause");
        CHECK(agent->messages[1]["print"]["command"] == "resume");
        CHECK(agent->messages[2]["print"]["command"] == "stop");
        CHECK(agent->messages[3]["print"]["command"] == "skip_objects");
        CHECK(agent->messages[3]["print"]["obj_list"] == json::array({3, 5}));
    }
    SECTION("hms actions use the print error and job id")
    {
        CHECK(send_printer_hms_command(m, "resume", false) == 0);
        CHECK(send_printer_hms_command(m, "stop", false) == 0);
        CHECK(send_printer_hms_command(m, "ignore", true) == 0);
        CHECK(send_printer_hms_command(m, "stop_buzzer", false) == 0);
        REQUIRE(agent->messages.size() == 4);
        CHECK(agent->messages[0]["print"]["command"] == "resume");
        CHECK(agent->messages[0]["print"]["err"] == std::to_string(0x0300400C));
        CHECK(agent->messages[0]["print"]["job_id"] == "job-7");
        CHECK(agent->messages[1]["print"]["command"] == "stop");
        CHECK(agent->messages[2]["print"]["command"] == "idle_ignore");
        CHECK(agent->messages[3]["print"]["command"] == "buzzer_ctrl");
    }
    SECTION("clear sends the print-error clean commands")
    {
        CHECK(send_printer_hms_command(m, "clear", false) == 0);
        REQUIRE(agent->messages.size() == 2);
        CHECK(agent->messages[0]["print"]["command"] == "clean_print_error");
        CHECK(agent->messages[1]["system"]["command"] == "uiop");
    }
    SECTION("a dry run records the message and sends nothing")
    {
        {
            ScopedPublishCapture capture;
            CHECK(send_printer_job_command(m, "pause", {}) == 0);
            REQUIRE(capture.messages().size() == 1);
            CHECK(json::parse(capture.messages()[0].json)["print"]["command"] == "pause");
            CHECK(capture.messages()[0].lan == true);
        }
        CHECK(agent->messages.empty());
    }
}
