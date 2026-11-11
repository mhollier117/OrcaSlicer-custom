#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/Utils/BblFilament/PrinterTap.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace fil = Slic3r::BblFilament;
using nlohmann::json;

namespace {

// The status messages below are built from what the parsers read:
//  - DevFilaSystemParser::ParseV1_0 (DevFilaSystem.cpp:381-840): `ams.ams[]` units with `id` and the hex `info`
//    (type = low 4 bits, :477), `tray[]` slots with their keys (:656-785), and the hex bit strings
//    `ams.tray_exist_bits` (:398) and `ams.tray_reading_bits` (:420);
//  - the external spools: `vir_slot` (an array of trays with `id` 255 / 254) or the older `vt_tray`
//    (DeviceManager.cpp:4135-4181, parse_vt_tray :5167);
//  - `home_flag` bit 16 (parse_home_flag, DeviceManager.cpp:1206) and `nozzle_diameter`.

json slot(const std::string& id, const std::string& type, const std::string& uid)
{
    json t = {{"id", id}, {"tag_uid", uid}};
    if (!type.empty()) {
        t["tray_type"]     = type;
        t["tray_info_idx"] = "GFL99";
    }
    return t;
}

// Unit 0: a plain AMS (info "1") with four slots: 0 an official RFID spool, 1 a third-party filament, 2 empty,
// 3 being read. Unit 1: an AMS 2 Pro (info "3"). Unit 128: an AMS HT (info "4") on the second nozzle (extruder bits 1).
// Exist bits: unit 0 slots 0,1,3 = bits 0,1,3; unit 1 slot 0 = bit 4; unit 128 = bit 16. Reading bits: unit 0 slot 3.
json ams_units()
{
    json s0 = {{"id", "0"},
               {"tag_uid", "A1B2C3D4E5F601AB"},
               {"tray_uuid", "0123456789ABCDEF0123456789ABCDEF"},
               {"tray_info_idx", "GFA00"},
               {"tray_type", "PLA"},
               {"tray_sub_brands", "PLA Basic"},
               {"tray_id_name", "A00-W1"},
               {"tray_weight", "1000"},
               {"tray_color", "ff00aa80"},
               {"cols", json::array({"ff00aa80", "00ff00"})},
               {"ctype", 1},
               {"remain", 80}};
    json s1 = {{"id", "1"},
               {"tag_uid", "0000000000000000"},
               {"tray_uuid", "00000000000000000000000000000000"},
               {"tray_info_idx", "GFL99"},
               {"tray_type", "PETG"},
               {"tray_color", "FF0000"},
               {"tray_weight", "750"}};
    json s2 = {{"id", "2"}};
    json s3 = slot("3", "ASA", "A1B2C3D4E5F601AC");
    s3["remain"] = 100;
    json pro = slot("0", "ABS", "0000000000000000");
    json ht  = slot("0", "TPU", "0000000000000000");
    return json::array({json{{"id", "0"}, {"info", "1"}, {"tray", json::array({s0, s1, s2, s3})}},
                        json{{"id", "1"}, {"info", "3"}, {"tray", json::array({pro})}},
                        json{{"id", "128"}, {"info", "104"}, {"tray", json::array({ht})}}});
}

json full_print()
{
    return {{"command", "push_status"},
            {"msg", 0},
            {"nozzle_diameter", "0.4"},
            {"home_flag", 65536 + 8},
            {"ams", {{"ams", ams_units()}, {"tray_exist_bits", "1001b"}, {"tray_reading_bits", "8"}}},
            {"vir_slot", json::array({json{{"id", "255"}, {"tray_type", "PA"}, {"tray_info_idx", "GFN99"}, {"tray_color", "00ff00ff"}, {"remain", 40}},
                                      json{{"id", "254"}}})}};
}

std::string message(const json& print) { return json{{"print", print}}.dump(); }

std::string full_message() { return message(full_print()); }

std::string diff_message(json print)
{
    print["command"] = "push_status";
    print["msg"]     = 1;
    return message(print);
}

const fil::TrayFacts* find_tray(const fil::PrinterFacts* f, int ams_id, const std::string& slot_id)
{
    if (!f) return nullptr;
    for (const auto& t : f->trays)
        if (t.ams_id == ams_id && t.slot_id == slot_id) return &t;
    return nullptr;
}

} // namespace

TEST_CASE("A message that is not a status is ignored by the printer tap", "[BblFilament]")
{
    fil::PrinterTap tap;
    tap.on_message("d1", R"({"info":{"command":"get_version"}})", 5);
    tap.on_message("d1", R"({"print":{"command":"gcode_line","note":"push_status"}})", 5);
    tap.on_message("d1", R"({"print":"push_status"})", 5);
    tap.on_message("d1", R"(["push_status"])", 5);
    tap.on_message("d1", R"({"print":{"command":"push_status","msg":2}})", 5);
    tap.on_message("d1", R"({"print":{"command":"push_status","msg":"0"}})", 5);
    CHECK(tap.facts("d1") == nullptr);
    CHECK(tap.printers().empty());
}

TEST_CASE("Garbage does not throw in the printer tap", "[BblFilament]")
{
    fil::PrinterTap tap;
    CHECK_NOTHROW(tap.on_message("d1", "push_status {{{", 1));
    CHECK_NOTHROW(tap.on_message("d1", "", 1));
    CHECK_NOTHROW(tap.on_message("d1", std::string("push_status\0\xff\xfe", 14), 1));
    CHECK_NOTHROW(tap.on_message("d1", R"({"print":{"command":"push_status","ams":5,"vir_slot":"x","home_flag":"y"}})", 1));
    const auto* f = tap.facts("d1");
    REQUIRE(f != nullptr);
    CHECK(f->trays.empty());
    CHECK_FALSE(f->pa_supported.has_value());
}

TEST_CASE("A stray byte in a status message does not lose it", "[BblFilament]")
{
    fil::PrinterTap tap;
    std::string     text = full_message();
    text.insert(text.find("push_status") - 3, "\xff"); // inside the quotes of a key
    tap.on_message("d1", text, 1);
    // the key "command" is damaged, so this one is not a status; the same stray byte in a value is repaired
    json p = full_print();
    p["ams"]["ams"][0]["display_name"] = "xx";
    std::string text2                  = message(p);
    text2.replace(text2.find("xx"), 2, "x\xff");
    tap.on_message("d2", text2, 1);
    const auto* f = tap.facts("d2");
    REQUIRE(f != nullptr);
    CHECK(f->trays.size() == 8);
}

TEST_CASE("A full state sets has_full and fills the trays in unit, slot and external order", "[BblFilament]")
{
    fil::PrinterTap tap;
    tap.on_message("d1", full_message(), 100);
    const auto* f = tap.facts("d1");
    REQUIRE(f != nullptr);
    CHECK(f->has_full);
    CHECK(f->last_message_at == 100);
    CHECK(f->observations == 0);
    REQUIRE(f->trays.size() == 8);
    const std::vector<std::pair<int, std::string>> order = {{0, "0"}, {0, "1"}, {0, "2"}, {0, "3"}, {1, "0"}, {128, "0"}, {255, "0"}, {254, "0"}};
    for (std::size_t i = 0; i < order.size(); ++i) {
        CHECK(f->trays[i].ams_id == order[i].first);
        CHECK(f->trays[i].slot_id == order[i].second);
    }
    CHECK(tap.printers() == std::vector<std::string>{"d1"});
}

TEST_CASE("The slots of one AMS carry what the printer reports", "[BblFilament]")
{
    fil::PrinterTap tap;
    tap.on_message("d1", full_message(), 1);
    const auto* f = tap.facts("d1");
    REQUIRE(f != nullptr);

    const auto* official = find_tray(f, 0, "0");
    REQUIRE(official != nullptr);
    CHECK(official->ams_type == 1);
    CHECK_FALSE(official->external);
    CHECK(official->occupied);
    CHECK_FALSE(official->reading);
    CHECK(official->official);
    CHECK(official->tag_uid == "A1B2C3D4E5F601AB");
    CHECK(official->uuid == "0123456789ABCDEF0123456789ABCDEF");
    CHECK(official->filament_id == "GFA00");
    CHECK(official->type == "PLA");
    CHECK(official->sub_brands == "PLA Basic");
    CHECK(official->tray_id_name == "A00-W1");
    CHECK(official->color == "#FF00AA80");
    REQUIRE(official->colors.size() == 2);
    CHECK(official->colors[0] == "#FF00AA80");
    CHECK(official->colors[1] == "#00FF00FF");
    CHECK(official->color_type == 1);
    CHECK(official->total_g == 1000);
    CHECK(official->remain == 80);

    const auto* third = find_tray(f, 0, "1");
    REQUIRE(third != nullptr);
    CHECK(third->occupied);
    CHECK_FALSE(third->official);
    CHECK(third->uuid == "00000000000000000000000000000000");
    CHECK(third->color == "#FF0000FF"); // six digits get an opaque alpha
    CHECK(third->colors.empty());
    CHECK(third->color_type == 2);
    CHECK(third->total_g == 750);
    CHECK(third->remain == -1);
    CHECK(third->tray_id_name.empty());

    const auto* empty = find_tray(f, 0, "2");
    REQUIRE(empty != nullptr);
    CHECK_FALSE(empty->occupied);
    CHECK_FALSE(empty->reading);
    CHECK(empty->type.empty());
    CHECK(empty->color.empty());
    CHECK(empty->total_g == 0);

    const auto* reading = find_tray(f, 0, "3");
    REQUIRE(reading != nullptr);
    CHECK(reading->occupied);
    CHECK(reading->reading);
    CHECK(reading->official);
    CHECK(reading->remain == 100);
}

TEST_CASE("The unit type comes from the info string", "[BblFilament]")
{
    fil::PrinterTap tap;
    tap.on_message("d1", full_message(), 1);
    const auto* f = tap.facts("d1");
    REQUIRE(f != nullptr);
    CHECK(find_tray(f, 0, "0")->ams_type == 1);   // info "1"
    CHECK(find_tray(f, 1, "0")->ams_type == 3);   // info "3": AMS 2 Pro
    CHECK(find_tray(f, 128, "0")->ams_type == 4); // info "104": only the low four bits are the type
    CHECK(find_tray(f, 255, "0")->ams_type == 0);

    json p = full_print();
    p["ams"]["ams"][0].erase("info"); // no info: a plain AMS
    p["ams"]["ams"][1]["info"] = "2";
    p["ams"]["ams"][2]["info"] = 4; // not a string: absent
    tap.on_message("d2", message(p), 1);
    f = tap.facts("d2");
    REQUIRE(f != nullptr);
    CHECK(find_tray(f, 0, "0")->ams_type == 1);
    CHECK(find_tray(f, 1, "0")->ams_type == 2);
    CHECK(find_tray(f, 128, "0")->ams_type == 1);
}

TEST_CASE("The slots of the three kinds of unit read their own bits", "[BblFilament]")
{
    fil::PrinterTap tap;
    tap.on_message("d1", full_message(), 1);
    const auto* f = tap.facts("d1");
    REQUIRE(f != nullptr);
    CHECK(find_tray(f, 1, "0")->occupied);   // a normal unit: bit ams*4+slot = 4
    CHECK(find_tray(f, 128, "0")->occupied); // a single-slot unit: bit 16+(id-128)

    // A mixed AMS Lite (type 5): exist and reading bits 24+slot.
    json p = {{"command", "push_status"},
              {"msg", 0},
              {"ams", {{"ams", json::array({json{{"id", "0"},
                                                  {"info", "5"},
                                                  {"tray", json::array({slot("0", "PLA", "0"), slot("1", "PLA", "0"), slot("2", "PLA", "0"), slot("3", "PLA", "0")})}}})},
                       {"tray_exist_bits", "2000001"},  // bit 25 (slot 1) and bit 0 (which a mixed unit must not read)
                       {"tray_reading_bits", "8000000"}}}}; // bit 27 (slot 3)
    tap.on_message("d2", message(p), 1);
    f = tap.facts("d2");
    REQUIRE(f != nullptr);
    CHECK_FALSE(find_tray(f, 0, "0")->occupied);
    CHECK(find_tray(f, 0, "1")->occupied);
    CHECK_FALSE(find_tray(f, 0, "2")->occupied);
    CHECK_FALSE(find_tray(f, 0, "3")->occupied);
    CHECK_FALSE(find_tray(f, 0, "1")->reading);
    CHECK(find_tray(f, 0, "3")->reading);
    CHECK(find_tray(f, 0, "0")->ams_type == 5);

    // A second single-slot unit uses the next bit.
    json q = {{"command", "push_status"},
              {"msg", 0},
              {"ams", {{"ams", json::array({json{{"id", "129"}, {"info", "4"}, {"tray", json::array({slot("0", "PLA", "0")})}}})},
                       {"tray_exist_bits", "20000"},
                       {"tray_reading_bits", "20000"}}}};
    tap.on_message("d3", message(q), 1);
    f = tap.facts("d3");
    REQUIRE(f != nullptr);
    CHECK(find_tray(f, 129, "0")->occupied);
    CHECK(find_tray(f, 129, "0")->reading);
}

TEST_CASE("Without tray_exist_bits a slot that reports a type is occupied", "[BblFilament]")
{
    fil::PrinterTap tap;
    json            p = full_print();
    p["ams"].erase("tray_exist_bits");
    p["ams"].erase("tray_reading_bits");
    tap.on_message("d1", message(p), 1);
    const auto* f = tap.facts("d1");
    REQUIRE(f != nullptr);
    CHECK(find_tray(f, 0, "0")->occupied);
    CHECK(find_tray(f, 0, "1")->occupied);
    CHECK_FALSE(find_tray(f, 0, "2")->occupied);
    CHECK_FALSE(find_tray(f, 0, "3")->reading);

    p["ams"]["tray_exist_bits"] = "zz"; // not hex: absent
    tap.on_message("d2", message(p), 1);
    CHECK(find_tray(tap.facts("d2"), 0, "1")->occupied);
}

TEST_CASE("The external spools are read from vir_slot and from the older vt_tray", "[BblFilament]")
{
    fil::PrinterTap tap;
    tap.on_message("d1", full_message(), 1);
    const auto* f = tap.facts("d1");
    REQUIRE(f != nullptr);
    const auto* main = find_tray(f, 255, "0");
    REQUIRE(main != nullptr);
    CHECK(main->external);
    CHECK(main->ams_type == 0);
    CHECK(main->occupied);
    CHECK_FALSE(main->reading);
    CHECK(main->type == "PA");
    CHECK(main->color == "#00FF00FF");
    REQUIRE(main->colors.size() == 1); // no cols: the colour itself, as parse_vt_tray does
    CHECK(main->colors[0] == "#00FF00FF");
    CHECK(main->remain == 40);
    const auto* deputy = find_tray(f, 254, "0");
    REQUIRE(deputy != nullptr);
    CHECK_FALSE(deputy->occupied);

    // The older form: one vt_tray, whatever its id says, is the main spool.
    json p = {{"command", "push_status"}, {"msg", 0}, {"vt_tray", json{{"id", "254"}, {"tray_type", "PLA"}, {"tag_uid", "A1B2C3D4E5F601AB"}}}};
    tap.on_message("d2", message(p), 1);
    f = tap.facts("d2");
    REQUIRE(f != nullptr);
    REQUIRE(f->trays.size() == 1);
    CHECK(f->trays[0].ams_id == 255);
    CHECK(f->trays[0].official);

    // vir_slot wins when both are present; other ids are not external spools.
    p["vir_slot"] = json::array({json{{"id", "7"}, {"tray_type", "PLA"}}, json{{"id", "254"}, {"tray_type", "PETG"}}});
    tap.on_message("d3", message(p), 1);
    f = tap.facts("d3");
    REQUIRE(f != nullptr);
    REQUIRE(f->trays.size() == 1);
    CHECK(f->trays[0].ams_id == 254);
    CHECK(f->trays[0].type == "PETG");
}

TEST_CASE("A diff that brings the whole ams array again updates one tray", "[BblFilament]")
{
    fil::PrinterTap tap;
    tap.on_message("d1", full_message(), 100);
    json units                         = ams_units();
    units[0]["tray"][0]["remain"]      = 55;
    tap.on_message("d1", diff_message(json{{"ams", {{"ams", units}}}}), 160);
    const auto* f = tap.facts("d1");
    REQUIRE(f != nullptr);
    CHECK(f->has_full);
    CHECK(f->observations == 1);
    CHECK(f->last_message_at == 160);
    CHECK(find_tray(f, 0, "0")->remain == 55);
    CHECK(find_tray(f, 0, "1")->remain == -1);
    CHECK(f->trays.size() == 8);
    // the bit strings were not in the diff: they stay
    CHECK(find_tray(f, 0, "0")->occupied);
    CHECK(find_tray(f, 0, "3")->reading);
}

TEST_CASE("A diff without ams leaves the trays alone", "[BblFilament]")
{
    fil::PrinterTap tap;
    tap.on_message("d1", full_message(), 100);
    tap.on_message("d1", diff_message(json{{"mc_percent", 12}}), 110);
    tap.on_message("d1", diff_message(json{{"mc_percent", 13}}), 120);
    const auto* f = tap.facts("d1");
    REQUIRE(f != nullptr);
    CHECK(f->trays.size() == 8);
    CHECK(find_tray(f, 0, "0")->remain == 80);
    CHECK(f->observations == 2);
    CHECK(f->last_message_at == 120);
}

TEST_CASE("A diff of only the exist bits flips occupied", "[BblFilament]")
{
    fil::PrinterTap tap;
    tap.on_message("d1", full_message(), 100);
    REQUIRE_FALSE(find_tray(tap.facts("d1"), 0, "2")->occupied);
    tap.on_message("d1", diff_message(json{{"ams", {{"tray_exist_bits", "1001f"}}}}), 101); // slot 2 now present
    const auto* f = tap.facts("d1");
    CHECK(find_tray(f, 0, "2")->occupied);
    CHECK(find_tray(f, 0, "0")->occupied);
    CHECK(find_tray(f, 0, "0")->official); // the units were not in the diff: they stay
    tap.on_message("d1", diff_message(json{{"ams", {{"tray_exist_bits", "10019"}}}}), 102); // slot 1 gone
    f = tap.facts("d1");
    CHECK_FALSE(find_tray(f, 0, "1")->occupied);
    CHECK(find_tray(f, 0, "3")->occupied);
}

TEST_CASE("A diff replaces an array and a scalar whole but merges objects key by key", "[BblFilament]")
{
    fil::PrinterTap tap;
    tap.on_message("d1", full_message(), 100);
    // vir_slot is an array: replaced, so the deputy spool is gone
    tap.on_message("d1", diff_message(json{{"vir_slot", json::array({json{{"id", "255"}, {"tray_type", "PLA"}}})}}), 101);
    const auto* f = tap.facts("d1");
    CHECK(find_tray(f, 254, "0") == nullptr);
    CHECK(find_tray(f, 255, "0")->type == "PLA");
    CHECK(find_tray(f, 0, "0") != nullptr);
    // a scalar is replaced
    tap.on_message("d1", diff_message(json{{"nozzle_diameter", "0.6"}}), 102);
    CHECK(tap.facts("d1")->nozzle_diameter > 0.55);
    // a part that changed type is replaced
    tap.on_message("d1", diff_message(json{{"ams", "gone"}}), 103);
    f = tap.facts("d1");
    CHECK(find_tray(f, 0, "0") == nullptr);
    CHECK(find_tray(f, 255, "0") != nullptr);
}

TEST_CASE("A second full state replaces everything", "[BblFilament]")
{
    fil::PrinterTap tap;
    tap.on_message("d1", full_message(), 100);
    tap.on_message("d1", diff_message(json{{"mc_percent", 1}}), 101);
    REQUIRE(tap.facts("d1")->observations == 1);
    json p = full_print();
    p["ams"]["ams"].erase(p["ams"]["ams"].begin() + 1, p["ams"]["ams"].end()); // only unit 0 is left
    p.erase("vir_slot");
    p.erase("home_flag");
    tap.on_message("d1", message(p), 200);
    const auto* f = tap.facts("d1");
    REQUIRE(f != nullptr);
    CHECK(f->has_full);
    CHECK(f->observations == 0);
    CHECK(f->trays.size() == 4);
    CHECK(find_tray(f, 1, "0") == nullptr);
    CHECK(find_tray(f, 128, "0") == nullptr);
    CHECK(find_tray(f, 255, "0") == nullptr);
    CHECK_FALSE(f->pa_supported.has_value());
}

TEST_CASE("A full state without msg counts as full", "[BblFilament]")
{
    fil::PrinterTap tap;
    json            p = full_print();
    p.erase("msg");
    tap.on_message("d1", message(p), 1);
    CHECK(tap.facts("d1")->has_full);
}

TEST_CASE("A diff before any full state is merged but has_full stays false", "[BblFilament]")
{
    fil::PrinterTap tap;
    json            units = ams_units();
    tap.on_message("d1", diff_message(json{{"ams", {{"ams", units}, {"tray_exist_bits", "1001b"}}}}), 50);
    const auto* f = tap.facts("d1");
    REQUIRE(f != nullptr);
    CHECK_FALSE(f->has_full);
    CHECK(f->observations == 0);
    CHECK(f->last_message_at == 50);
    CHECK(f->trays.size() == 6);
    CHECK(find_tray(f, 0, "0")->occupied);
    tap.on_message("d1", full_message(), 60);
    CHECK(tap.facts("d1")->has_full);
}

TEST_CASE("The printer tap forgets one printer or all of them", "[BblFilament]")
{
    fil::PrinterTap tap;
    tap.on_message("d1", full_message(), 1);
    tap.on_message("d2", full_message(), 1);
    CHECK(tap.printers() == std::vector<std::string>{"d1", "d2"});
    tap.forget("d1");
    CHECK(tap.facts("d1") == nullptr);
    REQUIRE(tap.facts("d2") != nullptr);
    tap.forget("nobody");
    // a diff after forgetting does not bring the old state back
    tap.on_message("d2", diff_message(json{{"mc_percent", 1}}), 2);
    tap.forget("d2");
    tap.on_message("d2", diff_message(json{{"mc_percent", 1}}), 3);
    CHECK_FALSE(tap.facts("d2")->has_full);
    CHECK(tap.facts("d2")->trays.empty());
    tap.clear();
    CHECK(tap.printers().empty());
    CHECK(tap.facts("d2") == nullptr);
}

TEST_CASE("The printer tap keeps two printers apart", "[BblFilament]")
{
    fil::PrinterTap tap;
    tap.on_message("d1", full_message(), 10);
    json p                     = full_print();
    p["ams"]["ams"][0]["tray"][0]["tray_type"] = "PC";
    tap.on_message("d2", message(p), 20);
    tap.on_message("d1", diff_message(json{{"mc_percent", 1}}), 30);
    CHECK(find_tray(tap.facts("d1"), 0, "0")->type == "PLA");
    CHECK(find_tray(tap.facts("d2"), 0, "0")->type == "PC");
    CHECK(tap.facts("d1")->observations == 1);
    CHECK(tap.facts("d2")->observations == 0);
    CHECK(tap.facts("d1")->last_message_at == 30);
    CHECK(tap.facts("d2")->last_message_at == 20);
}

TEST_CASE("The nozzle diameter and the PA support come from the printer when it reports them", "[BblFilament]")
{
    fil::PrinterTap tap;
    tap.on_message("d1", full_message(), 1);
    const auto* f = tap.facts("d1");
    CHECK_THAT(f->nozzle_diameter, Catch::Matchers::WithinAbs(0.4, 1e-9));
    REQUIRE(f->pa_supported.has_value());
    CHECK(*f->pa_supported);

    json p = full_print();
    p["nozzle_diameter"] = 0.6; // a number is read too
    p["home_flag"]       = 8;   // bit 16 clear
    tap.on_message("d2", message(p), 1);
    f = tap.facts("d2");
    CHECK_THAT(f->nozzle_diameter, Catch::Matchers::WithinAbs(0.6, 1e-9));
    REQUIRE(f->pa_supported.has_value());
    CHECK_FALSE(*f->pa_supported);

    p.erase("nozzle_diameter");
    p.erase("home_flag");
    tap.on_message("d3", message(p), 1);
    f = tap.facts("d3");
    CHECK(f->nozzle_diameter == 0);
    CHECK_FALSE(f->pa_supported.has_value());

    p["nozzle_diameter"] = "wide";
    p["home_flag"]       = "65536";
    tap.on_message("d4", message(p), 1);
    f = tap.facts("d4");
    CHECK(f->nozzle_diameter == 0);
    CHECK_FALSE(f->pa_supported.has_value());
}

TEST_CASE("Values of the wrong JSON type are treated as absent", "[BblFilament]")
{
    fil::PrinterTap tap;
    json            p = full_print();
    json&           t = p["ams"]["ams"][0]["tray"][0];
    t["remain"]        = "50";
    t["tray_weight"]   = json::object();
    t["tray_color"]    = 5;
    t["cols"]          = "red";
    t["ctype"]         = "1";
    t["tag_uid"]       = 7;
    t["tray_uuid"]     = nullptr;
    t["tray_type"]     = 3;
    t["tray_sub_brands"] = json::array();
    p["ams"]["ams"][0]["tray"][1]["id"] = 1; // a slot whose id is not a string is skipped
    p["ams"]["ams"].push_back(json{{"id", 3}, {"tray", json::array({slot("0", "PLA", "0")})}}); // so is a unit
    p["ams"]["ams"].push_back("junk");
    p["ams"]["ams"][0]["tray"].push_back(42);
    tap.on_message("d1", message(p), 1);
    const auto* f = tap.facts("d1");
    REQUIRE(f != nullptr);
    const auto* tray = find_tray(f, 0, "0");
    REQUIRE(tray != nullptr);
    CHECK(tray->remain == -1);
    CHECK(tray->total_g == 0);
    CHECK(tray->color.empty());
    CHECK(tray->colors.empty());
    CHECK(tray->color_type == 2);
    CHECK(tray->tag_uid.empty());
    CHECK_FALSE(tray->official);
    CHECK(tray->uuid.empty());
    CHECK(tray->type.empty());
    CHECK(tray->sub_brands.empty());
    CHECK(find_tray(f, 0, "1") == nullptr);
    CHECK(f->trays.size() == 7);
}

TEST_CASE("Colours, weights and the official flag follow their rules", "[BblFilament]")
{
    fil::PrinterTap tap;
    json            p = full_print();
    auto&           trays = p["ams"]["ams"][0]["tray"];
    trays[0]["tray_color"]  = "abcdef12";
    trays[0]["cols"]        = json::array({"abc", "#123456", "00112233", "zzzzzz", 5, "aabbcc"});
    trays[0]["tag_uid"]     = "A1B2C3D4E5F601A"; // 15 characters
    trays[1]["tray_color"]  = "#FF0000";         // a leading # is not a hex value
    trays[1]["tag_uid"]     = "A1B2C3D4E5F602AB"; // not "01" at offset 12
    trays[3]["tag_uid"]     = "A1B2C3D4E5F601ABC"; // 17 characters
    trays[3]["tray_weight"] = "1000.4";
    trays[3]["remain"]      = 101;
    trays[3]["tray_color"]  = "";
    tap.on_message("d1", message(p), 1);
    const auto* f = tap.facts("d1");
    REQUIRE(f != nullptr);
    CHECK(find_tray(f, 0, "0")->color == "#ABCDEF12");
    CHECK(find_tray(f, 0, "0")->colors == std::vector<std::string>{"#00112233", "#AABBCCFF"});
    CHECK_FALSE(find_tray(f, 0, "0")->official);
    CHECK(find_tray(f, 0, "1")->color.empty());
    CHECK_FALSE(find_tray(f, 0, "1")->official);
    CHECK_FALSE(find_tray(f, 0, "3")->official);
    CHECK(find_tray(f, 0, "3")->total_g == 1000);
    CHECK(find_tray(f, 0, "3")->remain == -1);
    CHECK(find_tray(f, 0, "3")->color.empty());
}

TEST_CASE("The facts of a printer are the same until the next message", "[BblFilament]")
{
    fil::PrinterTap tap;
    tap.on_message("d1", full_message(), 1);
    const auto* a = tap.facts("d1");
    const auto* b = tap.facts("d1");
    REQUIRE(a != nullptr);
    CHECK(a == b);
    CHECK(a->trays.size() == 8);
    CHECK(b->trays.size() == 8);
    tap.on_message("d1", diff_message(json{{"ams", {{"tray_exist_bits", "1001f"}}}}), 2);
    CHECK(find_tray(tap.facts("d1"), 0, "2")->occupied);
}
