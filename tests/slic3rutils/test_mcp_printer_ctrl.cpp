// Tests for the printer MCP control dispatchers (fork): printer_control, printer_ams, printer_options,
// printer_calibration. Each action is run against a MachineObject whose printer agent records the messages
// it is asked to publish.

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
#include <catch2/catch_approx.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <wx/timer.h>

#include "slic3r/GUI/DeviceCore/DevConfig.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevFan.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/Mcp/McpPrinterCtrl.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"
#include "libslic3r/AppConfig.hpp"
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

class RecordingAgent final : public OrcaPrinterAgent
{
public:
    RecordingAgent() : OrcaPrinterAgent(""), m_info{"ctrl-test-agent", "Control Test Agent", "1.0", "test agent"} {}
    AgentInfo get_agent_info() override { return m_info; }
    int send_message(std::string, std::string json_str, int, int) override
    {
        messages.push_back(json::parse(json_str));
        return 0;
    }
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override
    {
        return send_message(std::move(dev_id), std::move(json_str), qos, flag);
    }
    std::vector<json> messages;

private:
    AgentInfo m_info;
};

struct Rig
{
    AppConfig                        config;
    std::shared_ptr<RecordingAgent>  agent{std::make_shared<RecordingAgent>()};
    NetworkAgent                     network{nullptr, agent};
    DeviceManager                    manager{&network, false, &config};
    MachineObject                    obj{&manager, &network, "Test printer", "ctrl-device", "192.0.2.10"};

    Rig() { obj.dev_connection_type = "lan"; }

    const json& msg(size_t i = 0) const { return agent->messages.at(i); }
    const json& print(size_t i = 0) const { return msg(i).at("print"); }

    // extruder 0 .. count - 1 with the given current temperatures; `loaded` marks filament in the extruder
    void extruders(int count, int current, std::vector<int> temps, bool loaded = false, int snow = 0)
    {
        json info = json::array();
        for (int i = 0; i < count; ++i)
            info.push_back({{"id", i}, {"filam_bak", json::array()}, {"info", loaded ? 2 : 0}, {"temp", temps.at(i)}, {"spre", 0},
                            {"snow", snow}, {"star", 0}, {"stat", 0}, {"hnow", 0}});
        ExtderSystemParser::ParseV2_0({{"state", count | (current << 4)}, {"info", info}}, obj.GetExtderSystem());
    }

    // AMS 0 of the given info type with trays 0 and 1 present, tray 2 empty
    void ams(const std::string& info_hex = "00000001")
    {
        const json push = {{"ams",
                            {{"tray_exist_bits", "3"},
                             {"ams", json::array({{{"id", "0"},
                                                    {"info", info_hex},
                                                    {"tray", json::array({{{"id", "0"}, {"tray_color", "FF0000FF"}},
                                                                          {{"id", "1"}, {"tray_color", "00FF00FF"}},
                                                                          {{"id", "2"}}})}}})}}}};
        DevFilaSystemParser::ParseV1_0(push, &obj, obj.GetFilaSystem().get(), false);
    }
};

} // namespace

#define REFUSED(expr) CHECK_THROWS_AS((expr), ToolFailure)

TEST_CASE("printer_control set_temp sends the bed, nozzle and chamber commands", "[McpPrinterCtrl]")
{
    Rig r;
    r.obj.m_support_mqtt_bet_ctrl = true;
    r.obj.bed_temp_range          = {0, 110};
    CHECK(dispatch_control(r.obj, {{"action", "set_temp"}, {"target", "bed"}, {"value", 60}}) == 0);
    CHECK(r.print().at("command") == "set_bed_temp");
    CHECK(r.print().at("temp") == 60);
    REFUSED(dispatch_control(r.obj, {{"action", "set_temp"}, {"target", "bed"}, {"value", 500}}));

    // one nozzle: the plain command (a gcode line), several: the indexed one
    r.agent->messages.clear();
    r.obj.nozzle_temp_range = {20, 300};
    CHECK(dispatch_control(r.obj, {{"action", "set_temp"}, {"target", "nozzle"}, {"value", 200}}) == 0);
    CHECK(r.print().at("param").get<std::string>().find("M104 S200") == 0);
    REFUSED(dispatch_control(r.obj, {{"action", "set_temp"}, {"target", "nozzle"}, {"value", 400}}));
    REFUSED(dispatch_control(r.obj, {{"action", "set_temp"}, {"target", "nozzle"}, {"value", 200}, {"extruder", 1}}));

    r.extruders(2, 0, {25, 25});
    r.agent->messages.clear();
    CHECK(dispatch_control(r.obj, {{"action", "set_temp"}, {"target", "nozzle"}, {"value", 220}, {"extruder", 1}}) == 0);
    CHECK(r.print().at("command") == "set_nozzle_temp");
    CHECK(r.print().at("extruder_index") == 1);
    CHECK(r.print().at("target_temp") == 220);

    // chamber needs the printer to allow editing it
    REFUSED(dispatch_control(r.obj, {{"action", "set_temp"}, {"target", "chamber"}, {"value", 40}}));
    r.obj.GetConfig()->ParseConfig({{"support_chamber_temp_edit", true}, {"support_chamber_temp_edit_range", {0, 60}}});
    r.agent->messages.clear();
    CHECK(dispatch_control(r.obj, {{"action", "set_temp"}, {"target", "chamber"}, {"value", 45}}) == 0);
    CHECK(r.print().at("command") == "set_ctt");
    CHECK(r.print().at("ctt_val") == 45);
    REFUSED(dispatch_control(r.obj, {{"action", "set_temp"}, {"target", "chamber"}, {"value", 80}}));
    REFUSED(dispatch_control(r.obj, {{"action", "set_temp"}, {"target", "head"}, {"value", 80}}));
}

TEST_CASE("printer_control set_fan uses the printer's fan protocol", "[McpPrinterCtrl]")
{
    Rig r;
    r.obj.is_enable_np = true;
    // no aux fan reported: only the part fan
    REFUSED(dispatch_control(r.obj, {{"action", "set_fan"}, {"fan", "aux"}, {"percent", 50}}));
    r.obj.GetFan()->converse_to_duct(true, true, true);
    r.obj.GetFan()->ParseV2_0({{"support_aux_fan", true}});
    CHECK(dispatch_control(r.obj, {{"action", "set_fan"}, {"fan", "aux"}, {"percent", 50}}) == 0);
    CHECK(r.print().at("command") == "set_fan");
    CHECK(r.print().at("fan_index") == int(FAN_REMOTE_COOLING_0_IDX));
    CHECK(r.print().at("speed") == 50);

    r.agent->messages.clear();
    r.obj.is_enable_np = false; // old protocol: M106 with 0..255
    CHECK(dispatch_control(r.obj, {{"action", "set_fan"}, {"fan", "part"}, {"percent", 50}}) == 0);
    CHECK(r.print().at("command") == "gcode_line");
    CHECK(r.print().at("param").get<std::string>().find("M106 P1 S127") == 0);
    REFUSED(dispatch_control(r.obj, {{"action", "set_fan"}, {"fan", "part"}, {"percent", 101}}));
    REFUSED(dispatch_control(r.obj, {{"action", "set_fan"}, {"fan", "exhaust"}, {"percent", 10}}));
}

TEST_CASE("printer_control set_speed works only while printing", "[McpPrinterCtrl]")
{
    Rig r;
    REFUSED(dispatch_control(r.obj, {{"action", "set_speed"}, {"level", "sport"}}));
    r.obj.print_status = "RUNNING";
    CHECK(dispatch_control(r.obj, {{"action", "set_speed"}, {"level", "sport"}}) == 0);
    CHECK(r.print().at("command") == "print_speed");
    CHECK(r.print().at("param") == "3");
    r.agent->messages.clear();
    CHECK(dispatch_control(r.obj, {{"action", "set_speed"}, {"level", 4}}) == 0);
    CHECK(r.print().at("param") == "4");
    REFUSED(dispatch_control(r.obj, {{"action", "set_speed"}, {"level", 5}}));
    REFUSED(dispatch_control(r.obj, {{"action", "set_speed"}, {"level", "warp"}}));
}

TEST_CASE("printer_control light sets both chamber lights", "[McpPrinterCtrl]")
{
    Rig r;
    CHECK(dispatch_control(r.obj, {{"action", "light"}, {"on", true}}) == 0);
    REQUIRE(r.agent->messages.size() == 2);
    CHECK(r.msg(0).at("system").at("command") == "ledctrl");
    CHECK(r.msg(0).at("system").at("led_node") == "chamber_light");
    CHECK(r.msg(0).at("system").at("led_mode") == "on");
    CHECK(r.msg(1).at("system").at("led_node") == "chamber_light2");
    r.agent->messages.clear();
    CHECK(dispatch_control(r.obj, {{"action", "light"}, {"on", false}}) == 0);
    CHECK(r.msg(0).at("system").at("led_mode") == "off");
    REFUSED(dispatch_control(r.obj, {{"action", "light"}}));
}

TEST_CASE("printer_control home, jog, extrude and select_extruder are refused while printing", "[McpPrinterCtrl]")
{
    Rig r;
    r.obj.print_status = std::string(GENERATE("RUNNING", "PAUSE"));
    r.extruders(2, 0, {220, 220});
    REFUSED(dispatch_control(r.obj, {{"action", "home"}}));
    REFUSED(dispatch_control(r.obj, {{"action", "jog"}, {"axis", "X"}, {"distance", 10}}));
    REFUSED(dispatch_control(r.obj, {{"action", "extrude"}, {"length", 10}}));
    REFUSED(dispatch_control(r.obj, {{"action", "select_extruder"}, {"extruder", 1}}));
    CHECK(r.agent->messages.empty());
}

TEST_CASE("printer_control home and jog send the axis commands", "[McpPrinterCtrl]")
{
    Rig r;
    CHECK(dispatch_control(r.obj, {{"action", "home"}}) == 0);
    CHECK(r.print().at("param") == "G28 \n");
    r.agent->messages.clear();
    r.obj.m_support_mqtt_homing = true;
    CHECK(dispatch_control(r.obj, {{"action", "home"}}) == 0);
    CHECK(r.print().at("command") == "back_to_center");

    r.agent->messages.clear();
    CHECK(dispatch_control(r.obj, {{"action", "jog"}, {"axis", "X"}, {"distance", 25}}) == 0);
    CHECK(r.print().at("param").get<std::string>().find("G1 X25.0 F3000") != std::string::npos);
    r.agent->messages.clear();
    CHECK(dispatch_control(r.obj, {{"action", "jog"}, {"axis", "Z"}, {"distance", 5}}) == 0);
    CHECK(r.print().at("param").get<std::string>().find("F900") != std::string::npos);

    // an axis that is not at home must be homed first (bit 0 = X)
    r.obj.m_home_flag = 2;
    REFUSED(dispatch_control(r.obj, {{"action", "jog"}, {"axis", "X"}, {"distance", 10}}));
    r.agent->messages.clear();
    CHECK(dispatch_control(r.obj, {{"action", "jog"}, {"axis", "Y"}, {"distance", 10}}) == 0);
    r.obj.m_home_flag = 0;

    REFUSED(dispatch_control(r.obj, {{"action", "jog"}, {"axis", "W"}, {"distance", 10}}));
    REFUSED(dispatch_control(r.obj, {{"action", "jog"}, {"axis", "X"}, {"distance", 0}}));
    REFUSED(dispatch_control(r.obj, {{"action", "jog"}, {"axis", "X"}, {"distance", 101}}));
    REFUSED(dispatch_control(r.obj, {{"action", "jog"}, {"axis", "X"}}));
}

TEST_CASE("printer_control jog on MQTT axis control moves in exact steps", "[McpPrinterCtrl]")
{
    Rig r;
    r.obj.m_support_mqtt_axis_control = true;
    REFUSED(dispatch_control(r.obj, {{"action", "jog"}, {"axis", "X"}, {"distance", 5}}));
    CHECK(dispatch_control(r.obj, {{"action", "jog"}, {"axis", "X"}, {"distance", 10}}) == 0);
    // the generic printer agent ignores the flag (BBLPrinterAgent turns it into xyz_ctrl); the step rule is ours
    CHECK(r.print().at("command") == "gcode_line");
    CHECK(r.print().at("param").get<std::string>().find("G1 X10.0") != std::string::npos);
    r.agent->messages.clear();
    CHECK(dispatch_control(r.obj, {{"action", "jog"}, {"axis", "X"}, {"distance", -1}}) == 0);
    CHECK(r.print().at("param").get<std::string>().find("G1 X-1.0") != std::string::npos);
}

TEST_CASE("printer_control extrude needs a hot nozzle", "[McpPrinterCtrl]")
{
    Rig r;
    r.extruders(1, 0, {100});
    REFUSED(dispatch_control(r.obj, {{"action", "extrude"}, {"length", 10}}));
    CHECK(r.agent->messages.empty());

    r.extruders(1, 0, {200});
    CHECK(dispatch_control(r.obj, {{"action", "extrude"}, {"length", 10}}) == 0);
    CHECK(r.print().at("param").get<std::string>().find("G0 E10.0 F900") != std::string::npos);
    r.agent->messages.clear();
    CHECK(dispatch_control(r.obj, {{"action", "extrude"}, {"length", -10}}) == 0);
    CHECK(r.print().at("param").get<std::string>().find("E-10.0") != std::string::npos);

    r.agent->messages.clear();
    r.obj.is_enable_np = true;
    CHECK(dispatch_control(r.obj, {{"action", "extrude"}, {"length", 10}}) == 0);
    CHECK(r.print().at("command") == "set_extrusion_length");
    CHECK(r.print().at("extruder_index") == 0);
    CHECK(r.print().at("length") == 10);
    REFUSED(dispatch_control(r.obj, {{"action", "extrude"}, {"length", 10}, {"extruder", 1}}));
    REFUSED(dispatch_control(r.obj, {{"action", "extrude"}, {"length", 2.5}}));
}

TEST_CASE("printer_control select_extruder is for multi-extruder printers", "[McpPrinterCtrl]")
{
    Rig r;
    REFUSED(dispatch_control(r.obj, {{"action", "select_extruder"}, {"extruder", 1}}));
    r.extruders(2, 0, {25, 25});
    CHECK(dispatch_control(r.obj, {{"action", "select_extruder"}, {"extruder", 1}}) == 0);
    CHECK(r.print().at("command") == "select_extruder");
    CHECK(r.print().at("extruder_index") == 1);
    REFUSED(dispatch_control(r.obj, {{"action", "select_extruder"}, {"extruder", 2}}));
}

TEST_CASE("printer_control refuses unknown actions", "[McpPrinterCtrl]")
{
    Rig r;
    REFUSED(dispatch_control(r.obj, {{"action", "explode"}}));
    REFUSED(dispatch_control(r.obj, json::object()));
    REFUSED(dispatch_ams(r.obj, {{"action", "explode"}}));
    REFUSED(dispatch_options(r.obj, {{"action", "explode"}}));
    REFUSED(dispatch_calibration(r.obj, {{"action", "explode"}}));
}

TEST_CASE("A dispatcher inside a publish capture records its messages and sends none", "[McpPrinterCtrl]")
{
    // The light goes through MachineObject::publish_json -> NetworkAgent, which the capture intercepts (the
    // gcode-style commands are intercepted one level lower, in BBLPrinterAgent, which this test agent bypasses).
    Rig r;
    {
        ScopedPublishCapture capture;
        CHECK(dispatch_control(r.obj, {{"action", "light"}, {"on", true}}) == 0);
        REQUIRE(capture.messages().size() == 2);
        CHECK(json::parse(capture.messages()[0].json).at("system").at("command") == "ledctrl");
    }
    CHECK(r.agent->messages.empty());
    CHECK(dispatch_control(r.obj, {{"action", "light"}, {"on", true}}) == 0);
    CHECK(r.agent->messages.size() == 2);
}

TEST_CASE("printer_ams set_tray writes the slot's filament settings", "[McpPrinterCtrl]")
{
    Rig r;
    r.ams();
    const json args = {{"action", "set_tray"}, {"ams_id", 0}, {"slot_id", 1}, {"tray_info_idx", "GFL99"}, {"setting_id", "SET1"},
                       {"tray_type", "PLA"}, {"color", "#ff8000"}, {"nozzle_temp_min", 190}, {"nozzle_temp_max", 230}};
    json info = json::object();
    CHECK(dispatch_ams(r.obj, args, &info) == 0);
    CHECK(r.print().at("command") == "ams_filament_setting");
    CHECK(r.print().at("ams_id") == 0);
    CHECK(r.print().at("slot_id") == 1);
    CHECK(r.print().at("tray_id") == 1);
    CHECK(r.print().at("tray_info_idx") == "GFL99");
    CHECK(r.print().at("setting_id") == "SET1");
    CHECK(r.print().at("tray_color") == "FF8000FF");
    CHECK(r.print().at("tray_type") == "PLA");
    CHECK(r.print().at("nozzle_temp_min") == 190);
    CHECK(r.print().at("nozzle_temp_max") == 230);
    CHECK(r.obj.GetFilaSystem()->GetAmsTray("0", "1")->color == "FF8000FF");

    auto with = [&](const char* key, json value) {
        json a = args;
        if (value.is_null()) a.erase(key); else a[key] = value;
        return a;
    };
    REFUSED(dispatch_ams(r.obj, with("color", "orange")));
    REFUSED(dispatch_ams(r.obj, with("tray_type", nullptr)));
    REFUSED(dispatch_ams(r.obj, with("nozzle_temp_min", 300)));
    REFUSED(dispatch_ams(r.obj, with("ams_id", 3)));
    REFUSED(dispatch_ams(r.obj, with("slot_id", 3)));
    REFUSED(dispatch_ams(r.obj, with("colors", json::array({"#112233"})))); // firmware without multi-colour
    // a Bambu RFID spool is read only
    r.obj.GetFilaSystem()->GetAmsTray("0", "1")->tag_uid = "0123456789ABCDEF";
    REFUSED(dispatch_ams(r.obj, args));
    r.obj.GetFilaSystem()->GetAmsTray("0", "1")->tag_uid = "";
    // no edits during a print unless the printer advertises it or the app config allows it
    r.obj.print_status = "RUNNING";
    r.config.set_bool("allow_ams_edit_while_printing", false);
    CHECK_FALSE(r.obj.can_edit_filament_while_printing());
    REFUSED(dispatch_ams(r.obj, args));
    // override_print_lock sends it anyway, for testing what the firmware does
    r.agent->messages.clear();
    CHECK(dispatch_ams(r.obj, with("override_print_lock", true)) == 0);
    CHECK(r.print().at("command") == "ams_filament_setting");
    // the fork default: allowed by the app config although the printer does not advertise it
    r.config.set_bool("allow_ams_edit_while_printing", true);
    CHECK(r.obj.can_edit_filament_while_printing());
    r.agent->messages.clear();
    CHECK(dispatch_ams(r.obj, args) == 0);
    r.config.set_bool("allow_ams_edit_while_printing", false);
    r.obj.is_support_filament_setting_inprinting = true;
    r.agent->messages.clear();
    CHECK(dispatch_ams(r.obj, args) == 0);
}

TEST_CASE("printer_ams set_tray sends multi-colour lists to firmware that takes them", "[McpPrinterCtrl]")
{
    Rig r;
    r.ams();
    r.obj.is_support_filament_manual_multi_color = true;
    CHECK(dispatch_ams(r.obj, {{"action", "set_tray"}, {"ams_id", 0}, {"slot_id", 0}, {"tray_info_idx", "GFL99"}, {"tray_type", "PLA"},
                               {"color", "#FF0000"}, {"colors", {"#FF0000", "#0000FF80"}}, {"color_type", 1}, {"nozzle_temp_min", 190},
                               {"nozzle_temp_max", 230}}) == 0);
    CHECK(r.print().at("cols") == json::array({"FF0000FF", "0000FF80"}));
    CHECK(r.print().at("ctype") == 1);
}

TEST_CASE("printer_ams reset_tray clears the slot and its K", "[McpPrinterCtrl]")
{
    Rig r;
    r.ams();
    r.obj.cali_version = -1;
    CHECK(dispatch_ams(r.obj, {{"action", "reset_tray"}, {"ams_id", 0}, {"slot_id", 1}}) == 0);
    REQUIRE(r.agent->messages.size() == 2);
    CHECK(r.print(0).at("command") == "ams_filament_setting");
    CHECK(r.print(0).at("tray_info_idx") == "");
    CHECK(r.print(0).at("nozzle_temp_max") == 0);
    CHECK(r.print(1).at("command") == "extrusion_cali_set");
    CHECK(r.print(1).at("tray_id") == 1);
    CHECK(r.print(1).at("k_value") == Catch::Approx(0.0));

    r.agent->messages.clear();
    r.obj.cali_version = 0;
    CHECK(dispatch_ams(r.obj, {{"action", "reset_tray"}, {"ams_id", 0}, {"slot_id", 1}}) == 0);
    REQUIRE(r.agent->messages.size() == 2);
    CHECK(r.print(1).at("command") == "extrusion_cali_sel");
    CHECK(r.print(1).at("cali_idx") == -1);
}

TEST_CASE("printer_ams set_k selects a profile or writes K", "[McpPrinterCtrl]")
{
    Rig r;
    r.ams();
    r.obj.cali_version = 0;
    CHECK(dispatch_ams(r.obj, {{"action", "set_k"}, {"ams_id", 0}, {"slot_id", 1}, {"cali_idx", 3}, {"filament_id", "GFA00"}, {"nozzle_diameter", 0.4}}) == 0);
    CHECK(r.print().at("command") == "extrusion_cali_sel");
    CHECK(r.print().at("tray_id") == 1);
    CHECK(r.print().at("ams_id") == 0);
    CHECK(r.print().at("slot_id") == 1);
    CHECK(r.print().at("cali_idx") == 3);
    CHECK(r.print().at("filament_id") == "GFA00");
    CHECK(r.print().at("nozzle_diameter") == "0.4");
    REFUSED(dispatch_ams(r.obj, {{"action", "set_k"}, {"ams_id", 0}, {"slot_id", 1}, {"k", 0.04}}));

    r.agent->messages.clear();
    r.obj.cali_version = -1;
    CHECK(dispatch_ams(r.obj, {{"action", "set_k"}, {"ams_id", 0}, {"slot_id", 1}, {"k", 0.04}}) == 0);
    CHECK(r.print().at("command") == "extrusion_cali_set");
    CHECK(r.print().at("tray_id") == 1);
    CHECK(r.print().at("k_value").get<double>() == Catch::Approx(0.04));
    REFUSED(dispatch_ams(r.obj, {{"action", "set_k"}, {"ams_id", 0}, {"slot_id", 1}, {"cali_idx", 3}}));
    REFUSED(dispatch_ams(r.obj, {{"action", "set_k"}, {"ams_id", 0}, {"slot_id", 1}, {"k", 3.0}}));
}

TEST_CASE("printer_ams settings update only what is given", "[McpPrinterCtrl]")
{
    Rig r;
    r.ams();
    REFUSED(dispatch_ams(r.obj, {{"action", "settings"}}));
    REFUSED(dispatch_ams(r.obj, {{"action", "settings"}, {"remain_detect", true}})); // unsupported
    r.obj.GetFilaSystem()->GetAmsSystemSetting().SetDetectOnPowerupEnabled(true);
    CHECK(dispatch_ams(r.obj, {{"action", "settings"}, {"insert_read", true}}) == 0);
    CHECK(r.print().at("command") == "ams_user_setting");
    CHECK(r.print().at("tray_read_option") == true);
    CHECK(r.print().at("startup_read_option") == true); // kept from the current state
    CHECK(r.print().at("calibrate_remain_flag") == false);

    r.agent->messages.clear();
    r.obj.is_support_filament_backup = true;
    CHECK(dispatch_ams(r.obj, {{"action", "settings"}, {"auto_refill", true}}) == 0);
    CHECK(r.print().at("auto_switch_filament") == true);
    REFUSED(dispatch_ams(r.obj, {{"action", "settings"}, {"air_print_detect", true}}));
}

TEST_CASE("printer_ams load and unload address the slot and the extruder", "[McpPrinterCtrl]")
{
    Rig r;
    r.ams();
    CHECK(dispatch_ams(r.obj, {{"action", "load"}, {"ams_id", 0}, {"slot_id", 1}}) == 0);
    CHECK(r.print().at("command") == "ams_change_filament");
    CHECK(r.print().at("ams_id") == 0);
    CHECK(r.print().at("slot_id") == 1);
    CHECK(r.print().at("target") == 1);
    r.agent->messages.clear();
    CHECK(dispatch_ams(r.obj, {{"action", "load"}, {"tray_index", 0}}) == 0);
    CHECK(r.print().at("slot_id") == 0);

    REFUSED(dispatch_ams(r.obj, {{"action", "load"}, {"ams_id", 0}, {"slot_id", 2}}));                 // empty slot
    REFUSED(dispatch_ams(r.obj, {{"action", "load"}, {"ams_id", 0}, {"slot_id", 1}, {"extruder", 1}})); // no Filament Track Switch

    // nothing loaded yet
    REFUSED(dispatch_ams(r.obj, {{"action", "unload"}}));
    r.extruders(1, 0, {200}, true, 1); // ams 0 slot 1 in the extruder
    r.agent->messages.clear();
    CHECK(dispatch_ams(r.obj, {{"action", "unload"}}) == 0);
    CHECK(r.print().at("command") == "ams_change_filament");
    CHECK(r.print().at("ams_id") == 0);
    CHECK(r.print().at("target") == 255);
    CHECK(r.print().at("slot_id") == 255);

    r.obj.print_status = "RUNNING";
    r.agent->messages.clear();
    REFUSED(dispatch_ams(r.obj, {{"action", "load"}, {"ams_id", 0}, {"slot_id", 1}}));
    REFUSED(dispatch_ams(r.obj, {{"action", "unload"}}));
    REFUSED(dispatch_ams(r.obj, {{"action", "calibrate"}, {"ams_id", 0}}));
    REFUSED(dispatch_ams(r.obj, {{"action", "refresh_rfid"}, {"ams_id", 0}, {"slot_id", 0}}));
    CHECK(r.agent->messages.empty());
}

TEST_CASE("printer_ams drying starts and stops within the AMS limits", "[McpPrinterCtrl]")
{
    Rig r;
    r.ams("00000003"); // type 3: AMS 2 Pro
    REFUSED(dispatch_ams(r.obj, {{"action", "dry_start"}, {"ams_id", 0}, {"hours", 4}, {"temperature", 55}, {"filament_type", "PLA"}})); // no remote drying
    r.obj.is_support_remote_dry = true;
    r.obj.GetFilaSystem()->GetAmsTray("0", "0")->m_fila_type = "PETG";
    json info = json::object();
    CHECK(dispatch_ams(r.obj, {{"action", "dry_start"}, {"ams_id", 0}, {"hours", 4}, {"temperature", 55}}, &info) == 0);
    CHECK(info.at("filament_type") == "PETG");
    CHECK(r.print().at("command") == "ams_filament_drying");
    CHECK(r.print().at("ams_id") == 0);
    CHECK(r.print().at("mode") == 1);
    CHECK(r.print().at("temp") == 55);
    CHECK(r.print().at("duration") == 4);
    CHECK(r.print().at("filament") == "PETG");
    CHECK(r.print().at("cooling_temp") == 50);
    REFUSED(dispatch_ams(r.obj, {{"action", "dry_start"}, {"ams_id", 0}, {"hours", 4}, {"temperature", 90}}));
    REFUSED(dispatch_ams(r.obj, {{"action", "dry_start"}, {"ams_id", 0}, {"hours", 30}, {"temperature", 55}}));

    r.agent->messages.clear();
    CHECK(dispatch_ams(r.obj, {{"action", "dry_stop"}, {"ams_id", 0}}) == 0);
    CHECK(r.print().at("command") == "ams_filament_drying");
    CHECK(r.print().at("mode") == 0);
    REFUSED(dispatch_ams(r.obj, {{"action", "dry_stop"}, {"ams_id", 5}}));
}

TEST_CASE("printer_ams control sends the AMS command and reset needs support and confirmation", "[McpPrinterCtrl]")
{
    Rig r;
    r.ams();
    CHECK(dispatch_ams(r.obj, {{"action", "control"}, {"cmd", "abort"}}) == 0);
    CHECK(r.print().at("command") == "ams_control");
    CHECK(r.print().at("param") == "abort");
    REFUSED(dispatch_ams(r.obj, {{"action", "control"}, {"cmd", "explode"}}));
    REFUSED(dispatch_ams(r.obj, {{"action", "reset"}, {"confirm", true}})); // this printer offers no AMS id reset
}

TEST_CASE("printer_options set sends the xcam and camera commands for supported options only", "[McpPrinterCtrl]")
{
    Rig r;
    REFUSED(dispatch_options(r.obj, {{"action", "set"}, {"option", "ai_monitoring"}, {"value", true}}));
    REFUSED(dispatch_options(r.obj, {{"action", "set"}, {"option", "nonsense"}, {"value", true}}));

    r.obj.GetConfig()->ParseConfig({{"support_ai_monitoring", true}});
    CHECK(dispatch_options(r.obj, {{"action", "set"}, {"option", "ai_monitoring"}, {"value", true}, {"sensitivity", "high"}}) == 0);
    CHECK(r.msg().at("xcam").at("command") == "xcam_control_set");
    CHECK(r.msg().at("xcam").at("module_name") == "printing_monitor");
    CHECK(r.msg().at("xcam").at("control") == true);
    CHECK(r.msg().at("xcam").at("halt_print_sensitivity") == "high");
    REFUSED(dispatch_options(r.obj, {{"action", "set"}, {"option", "ai_monitoring"}, {"value", true}, {"sensitivity", "extreme"}}));
    REFUSED(dispatch_options(r.obj, {{"action", "set"}, {"option", "ai_monitoring"}, {"value", "yes"}}));

    r.agent->messages.clear();
    r.obj.is_support_spaghetti_detection = true;
    CHECK(dispatch_options(r.obj, {{"action", "set"}, {"option", "spaghetti_detection"}, {"value", false}}) == 0);
    CHECK(r.msg().at("xcam").at("module_name") == "spaghetti_detector");
    CHECK(r.msg().at("xcam").at("halt_print_sensitivity") == "medium");

    r.agent->messages.clear();
    r.obj.has_ipcam                   = true;
    r.obj.is_support_1080dpi          = true;
    r.obj.camera_resolution_supported = {"720p", "1080p"};
    CHECK(dispatch_options(r.obj, {{"action", "set"}, {"option", "camera_resolution"}, {"value", "1080p"}}) == 0);
    CHECK(r.msg().at("camera").at("command") == "ipcam_resolution_set");
    CHECK(r.msg().at("camera").at("resolution") == "1080p");
    REFUSED(dispatch_options(r.obj, {{"action", "set"}, {"option", "camera_resolution"}, {"value", "4k"}}));
    REFUSED(dispatch_options(r.obj, {{"action", "set"}, {"option", "camera_recording"}, {"value", true}})); // no storage card
    REFUSED(dispatch_options(r.obj, {{"action", "set"}, {"option", "snapshot"}, {"value", true}}));         // unsupported

    // get sends nothing and lists every option with its support
    r.agent->messages.clear();
    CHECK(dispatch_options(r.obj, {{"action", "get"}}) == 0);
    CHECK(r.agent->messages.empty());
    const json state = options_state(r.obj);
    bool       found = false;
    for (const json& e : state)
        if (e.at("option") == "ai_monitoring") {
            found = true;
            CHECK(e.at("supported") == true);
        } else if (e.at("option") == "fod_check") {
            CHECK(e.at("supported") == false);
        }
    CHECK(found);
}

TEST_CASE("printer_calibration pa_list requests the profile table and reads it back", "[McpPrinterCtrl]")
{
    Rig r;
    CHECK(dispatch_calibration(r.obj, {{"action", "pa_list"}, {"nozzle_diameter", 0.4}, {"filament_id", "GFA00"}}) == 0);
    CHECK(r.print().at("command") == "extrusion_cali_get");
    CHECK(r.print().at("filament_id") == "GFA00");
    CHECK(r.print().at("nozzle_diameter") == "0.4");
    CHECK(r.obj.request_tab_from_bbs);
    CHECK(calibration_read_state(r.obj, "pa_list").at("ready") == false);

    PACalibResult entry;
    entry.cali_idx = 7;
    entry.name = "fast";
    entry.filament_id = "GFA00";
    entry.k_value = 0.03f;
    entry.nozzle_diameter = 0.4f;
    r.obj.pa_calib_tab.push_back(entry);
    r.obj.has_get_pa_calib_tab = true;
    r.obj.request_tab_from_bbs = false;
    const json state = calibration_read_state(r.obj, "pa_list");
    CHECK(state.at("ready") == true);
    CHECK(state.at("failed") == false);
    REQUIRE(state.at("entries").size() == 1);
    CHECK(state.at("entries")[0].at("cali_idx") == 7);
    CHECK(state.at("entries")[0].at("name") == "fast");
    CHECK(state.at("entries")[0].at("k_value").get<double>() == Catch::Approx(0.03));

    r.obj.has_get_pa_calib_tab = false; // answered with a failure
    CHECK(calibration_read_state(r.obj, "pa_list").at("failed") == true);
}

TEST_CASE("printer_calibration pa_set, pa_select and pa_delete send the profile edits", "[McpPrinterCtrl]")
{
    Rig r;
    r.ams();
    r.obj.cali_version = 0;
    const json set = {{"action", "pa_set"}, {"entries", {{{"name", "fast"}, {"filament_id", "GFA00"}, {"k", 0.03}, {"nozzle_diameter", 0.4}}}}};
    CHECK(dispatch_calibration(r.obj, set) == 0);
    CHECK(r.print().at("command") == "extrusion_cali_set");
    CHECK(r.print().at("filaments")[0].at("name") == "fast");
    CHECK(r.print().at("filaments")[0].at("filament_id") == "GFA00");
    CHECK(r.print().at("filaments")[0].at("tray_id") == -1);
    CHECK_FALSE(r.print().at("filaments")[0].contains("cali_idx"));

    // a same-named profile is replaced only on request
    PACalibResult old;
    old.name = "fast";
    old.filament_id = "GFA00";
    old.cali_idx = 4;
    old.nozzle_volume_type = nvtStandard; // PACalibResult leaves it uninitialized; the reply parser always sets it
    r.obj.pa_calib_tab.push_back(old);
    r.obj.has_get_pa_calib_tab = true;
    REFUSED(dispatch_calibration(r.obj, set));
    json overwrite = set;
    overwrite["overwrite"] = true;
    CHECK(dispatch_calibration(r.obj, overwrite) == 0);
    REFUSED(dispatch_calibration(r.obj, {{"action", "pa_set"}, {"entries", json::array()}}));
    REFUSED(dispatch_calibration(r.obj, {{"action", "pa_set"}, {"entries", {{{"name", "x"}, {"filament_id", "GFA00"}, {"k", 5.0}, {"nozzle_diameter", 0.4}}}}}));

    r.agent->messages.clear();
    CHECK(dispatch_calibration(r.obj, {{"action", "pa_select"}, {"ams_id", 0}, {"slot_id", 1}, {"cali_idx", 2}, {"nozzle_diameter", 0.4}}) == 0);
    CHECK(r.print().at("command") == "extrusion_cali_sel");
    CHECK(r.print().at("cali_idx") == 2);
    REFUSED(dispatch_calibration(r.obj, {{"action", "pa_select"}, {"ams_id", 0}, {"slot_id", 1}}));

    r.agent->messages.clear();
    CHECK(dispatch_calibration(r.obj, {{"action", "pa_delete"}, {"cali_idx", 5}, {"filament_id", "GFA00"}, {"nozzle_diameter", 0.4}}) == 0);
    CHECK(r.print().at("command") == "extrusion_cali_del");
    CHECK(r.print().at("cali_idx") == 5);
    CHECK(r.print().at("filament_id") == "GFA00");
    CHECK(r.print().contains("nozzle_id"));
    REFUSED(dispatch_calibration(r.obj, {{"action", "pa_delete"}, {"cali_idx", 5}, {"nozzle_diameter", 0.4}}));
}

TEST_CASE("printer_calibration requests results and refuses to start a calibration", "[McpPrinterCtrl]")
{
    Rig r;
    CHECK(dispatch_calibration(r.obj, {{"action", "auto_pa_result"}, {"nozzle_diameter", 0.4}}) == 0);
    CHECK(r.print().at("command") == "extrusion_cali_get_result");
    CHECK(calibration_read_state(r.obj, "auto_pa_result").at("ready") == false);
    r.obj.get_pa_calib_result = true;
    CHECK(calibration_read_state(r.obj, "auto_pa_result").at("ready") == true);

    r.agent->messages.clear();
    CHECK(dispatch_calibration(r.obj, {{"action", "flow_result"}, {"nozzle_diameter", 0.4}}) == 0);
    CHECK(r.print().at("command") == "flowrate_get_result");
    FlowRatioCalibResult flow;
    flow.tray_id = 1;
    flow.nozzle_diameter = 0.4f;
    flow.filament_id = "GFA00";
    flow.flow_ratio = 0.97f;
    flow.confidence = 0;
    r.obj.flow_ratio_results.push_back(flow);
    r.obj.get_flow_calib_result = true;
    const json state = calibration_read_state(r.obj, "flow_result");
    CHECK(state.at("ready") == true);
    CHECK(state.at("entries")[0].at("flow_ratio").get<double>() == Catch::Approx(0.97));

    r.agent->messages.clear();
    REFUSED(dispatch_calibration(r.obj, {{"action", "auto_pa_start"}}));
    REFUSED(dispatch_calibration(r.obj, {{"action", "flow_start"}}));
    CHECK(r.agent->messages.empty());
}

// ---- Dry runs on the real Bambu agent path (orchestrator) ------------------------------------------
// The tests above use an OrcaPrinterAgent subclass, which records messages itself. These use the real
// BBLPrinterAgent with no network plugin loaded: a message that escaped the capture would reach
// BBLPrinterAgent's plugin path and fail, so a zero result plus a captured message proves the dry run
// stopped it, including the gcode-style commands BBLPrinterAgent builds itself.
#include "slic3r/Utils/BBLPrinterAgent.hpp"

namespace {
struct BblRig
{
    AppConfig                        config;
    std::shared_ptr<BBLPrinterAgent> agent{std::make_shared<BBLPrinterAgent>()};
    NetworkAgent                     network{nullptr, agent};
    DeviceManager                    manager{&network, false, &config};
    MachineObject                    obj{&manager, &network, "Bambu test printer", "bbl-device", "192.0.2.20"};
};
} // namespace

TEST_CASE("Dry runs capture Bambu commands instead of sending them", "[McpPrinterCtrl][DryRun]")
{
    BblRig rig;
    rig.obj.dev_connection_type = GENERATE("lan", "cloud");

    const std::vector<json> calls = {
        {{"action", "light"}, {"on", true}},
        {{"action", "set_temp"}, {"target", "bed"}, {"value", 0}},
        {{"action", "home"}},
    };
    for (const json& args : calls) {
        DYNAMIC_SECTION(args.at("action").get<std::string>() << " over " << rig.obj.dev_connection_type)
        {
            ScopedPublishCapture capture;
            int                  rc = -99;
            try {
                rc = dispatch_control(rig.obj, args);
            } catch (const ToolFailure& e) {
                // A precondition refusal is fine for this printer state; it must not have sent anything.
                INFO("refused: " << e.what());
                CHECK(capture.messages().empty());
                continue;
            }
            CHECK(rc == 0);
            REQUIRE_FALSE(capture.messages().empty());
            for (const PublishCapture::Message& m : capture.messages())
                CHECK(m.dev_id == "bbl-device");
        }
    }
}

TEST_CASE("Printing cannot start while a dry run captures", "[McpPrinterCtrl][DryRun]")
{
    BblRig            rig;
    ScopedPublishCapture capture;
    PrintParams       params;
    params.dev_id = "bbl-device";
    CHECK(rig.network.start_local_print(params, nullptr, nullptr) == -1);
    CHECK(rig.network.start_print(params, nullptr, nullptr, nullptr) == -1);
    CHECK(rig.network.start_sdcard_print(params, nullptr, nullptr) == -1);
    CHECK(capture.messages().empty());
}

TEST_CASE("Without a capture the Bambu agent tries to send", "[McpPrinterCtrl][DryRun]")
{
    // Control: with no plugin loaded and no capture, the same command does not report success, which
    // is what makes the zero result inside a capture meaningful.
    BblRig rig;
    rig.obj.dev_connection_type = "lan";
    REQUIRE(PublishCapture::current() == nullptr);
    // (light is not usable here: DevLamp::CtrlSetChamberLight returns void, so it always reports 0.)
    CHECK(dispatch_control(rig.obj, {{"action", "set_temp"}, {"target", "bed"}, {"value", 0}}) != 0);
}
