#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/Utils/BblFilament/Spool.hpp"
#include "slic3r/Utils/BblFilament/SpoolCloud.hpp"
#include "slic3r/Utils/BblFilament/SpoolService.hpp"
#include "slic3r/Utils/BblFilament/SpoolWorker.hpp"

#include "bbl_filament_test_utils.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <limits>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fil = Slic3r::BblFilament;
using fil::json;
using namespace BblFilamentTest;

namespace {

fil::Spool parsed(const json& row)
{
    auto s = fil::spool_from_cloud(row);
    REQUIRE(s.has_value());
    return *s;
}

fil::Spool current_spool()
{
    fil::Spool s = parsed(row_with(1000001));
    return s;
}

fil::FilamentLookup full_lookup()
{
    fil::FilamentLookup l;
    l.is_support    = [](const std::string& id) -> std::optional<bool> { return id == "GFS00"; };
    l.filament_type = [](const std::string& id) { return id == "GFS00" ? std::string("PLA-S") : std::string("PETG"); };
    l.tray_id_name  = [](const std::string& id, const std::vector<std::string>& colors, int color_type) {
        return "T:" + id + ":" + std::to_string(colors.size()) + ":" + std::to_string(color_type);
    };
    return l;
}

const std::vector<std::string>& forbidden_update_keys()
{
    static const std::vector<std::string> keys{"createType", "RFID",    "rolls",      "inPrinter", "devId",       "amsSn",   "amsId",
                                               "amsType",    "slotId",  "deviceName", "status",    "tray_id_name", "remain_percent",
                                               "spool_weight", "diameter", "color_name", "favorite", "entry_method", "bound_dev_id"};
    return keys;
}


} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// spool_from_cloud

TEST_CASE("A cloud list row becomes a spool with every mapped field", "[BblFilament]")
{
    json row                = example_row();
    row["id"]               = 1000001;
    row["RFID"]             = "ABCDEF0123";
    row["slotId"]           = 3;
    row["amsId"]            = 2;
    row["amsType"]          = 1;
    row["inPrinter"]        = true;
    row["devId"]            = "DEV1";
    row["amsSn"]            = "SN1";
    row["deviceName"]       = "X1C";
    row["note"]             = "hello";
    row["trayIdName"]       = "A00-W1";
    row["createType"]       = "ams";
    const fil::Spool s = parsed(row);
    CHECK(s.id == "1000001");
    CHECK(s.filament_id == "GFA00");
    CHECK(s.rfid == "ABCDEF0123");
    CHECK(s.tray_id_name == "A00-W1");
    CHECK(s.vendor == "Bambu Lab");
    CHECK(s.type == "PLA");
    CHECK(s.name == "PLA Basic");
    CHECK(s.color == "#FFFFFFFF");
    CHECK(s.colors.empty());
    CHECK(s.color_type == 2);
    CHECK(s.total_net_weight == 1000.0);
    CHECK(s.net_weight == 750.0);
    CHECK(s.status == 0);
    CHECK(s.create_type == "ams");
    CHECK(s.created_at == "1700000000");
    CHECK(s.updated_at == "1700000000");
    CHECK(s.note == "hello");
    CHECK(s.in_printer);
    CHECK(s.dev_id == "DEV1");
    CHECK(s.ams_sn == "SN1");
    CHECK(s.slot_id == "3");
    CHECK(s.device_name == "X1C");
    CHECK(s.ams_id == 2);
    CHECK(s.ams_type == 1);
}

TEST_CASE("A cloud row with a string id is accepted", "[BblFilament]")
{
    json row  = example_row();
    row["id"] = "1000005";
    CHECK(parsed(row).id == "1000005");
}

TEST_CASE("A cloud row without a usable id is dropped", "[BblFilament]")
{
    json row = example_row();
    SECTION("missing") { row.erase("id"); }
    SECTION("zero") { row["id"] = 0; }
    SECTION("negative") { row["id"] = -4; }
    SECTION("non numeric text") { row["id"] = "abc"; }
    SECTION("empty text") { row["id"] = ""; }
    SECTION("too long") { row["id"] = "1234567890123456789"; }
    SECTION("a float") { row["id"] = 1.5; }
    SECTION("null") { row["id"] = nullptr; }
    CHECK_FALSE(fil::spool_from_cloud(row).has_value());
}

TEST_CASE("A cloud row that is not an object is dropped", "[BblFilament]")
{
    CHECK_FALSE(fil::spool_from_cloud(json::array()).has_value());
    CHECK_FALSE(fil::spool_from_cloud(json(5)).has_value());
    CHECK_FALSE(fil::spool_from_cloud(json(nullptr)).has_value());
}

TEST_CASE("An RFID of only zeros is treated as no tag", "[BblFilament]")
{
    json row    = example_row();
    row["RFID"] = "0000000000";
    CHECK(parsed(row).rfid.empty());
    CHECK_FALSE(fil::valid_rfid(""));
    CHECK_FALSE(fil::valid_rfid("000"));
    CHECK(fil::valid_rfid("00A"));
}

TEST_CASE("A cloud row with colors null has no colours", "[BblFilament]")
{
    json row      = example_row();
    row["colors"] = nullptr;
    CHECK(parsed(row).colors.empty());
}

TEST_CASE("The first entry of colors wins over color", "[BblFilament]")
{
    json row       = example_row();
    row["color"]   = "#111111FF";
    row["colors"]  = json::array({"#222222FF", "#333333FF"});
    row["colorType"] = 1;
    const fil::Spool s = parsed(row);
    CHECK(s.color == "#222222FF");
    CHECK(s.colors.size() == 2);
    CHECK(s.color_type == 1);
}

TEST_CASE("A missing colour type is derived from the colour count", "[BblFilament]")
{
    json row = example_row();
    row.erase("colorType");
    CHECK(parsed(row).color_type == 2);
    row["colors"] = json::array({"#222222FF", "#333333FF"});
    CHECK(parsed(row).color_type == 1);
    CHECK(fil::normalize_color_type(7, 1) == 2);
    CHECK(fil::normalize_color_type(0, 5) == 0);
}

TEST_CASE("A cloud status of 1 means information needed and anything else is active", "[BblFilament]")
{
    json row     = example_row();
    row["status"] = 1;
    CHECK(parsed(row).status == 1);
    row["status"] = 7;
    CHECK(parsed(row).status == 0);
}

TEST_CASE("Cloud row fields of the wrong type are ignored", "[BblFilament]")
{
    json row           = example_row();
    row["filamentName"] = 12;
    row["netWeight"]    = "750";
    row["inPrinter"]    = "yes";
    row["colors"]       = "red";
    row["amsId"]        = "x";
    row["status"]       = "1";
    row["note"]         = json::array();
    const fil::Spool s = parsed(row);
    CHECK(s.name.empty());
    CHECK(s.net_weight == 0.0);
    CHECK_FALSE(s.in_printer);
    CHECK(s.colors.empty());
    CHECK(s.ams_id == -1);
    CHECK(s.status == 0);
    CHECK(s.note.empty());
}

TEST_CASE("The spool id parser accepts one to eighteen digits above zero", "[BblFilament]")
{
    CHECK(fil::parse_spool_id("1000001") == 1000001LL);
    CHECK(fil::parse_spool_id("123456789012345678").has_value());
    CHECK_FALSE(fil::parse_spool_id("1234567890123456789").has_value());
    CHECK_FALSE(fil::parse_spool_id("0").has_value());
    CHECK_FALSE(fil::parse_spool_id("").has_value());
    CHECK_FALSE(fil::parse_spool_id("-1").has_value());
    CHECK_FALSE(fil::parse_spool_id("12a").has_value());
}

// ---------------------------------------------------------------------------------------------------------------------
// spool_to_page

TEST_CASE("A spool shown to the page carries exactly the page's keys", "[BblFilament]")
{
    const json page = fil::spool_to_page(parsed(row_with(1000001)));
    const std::set<std::string> expected{"spool_id",   "setting_id",   "tag_uid",       "tray_id_name", "brand",        "material_type",
                                         "series",     "color_name",   "color_code",    "colors",       "color_type",   "diameter",
                                         "initial_weight", "spool_weight", "net_weight", "remain_percent", "status",     "entry_method",
                                         "created_at", "updated_at",   "bound_dev_id",  "bound_ams_id", "note",         "favorite",
                                         "cloud_synced", "in_printer", "dev_id",        "ams_sn",       "ams_id",       "ams_type",
                                         "slot_id",    "device_name",  "tray_label"};
    std::set<std::string> actual;
    for (auto it = page.begin(); it != page.end(); ++it) actual.insert(it.key());
    CHECK(actual == expected);
    CHECK(page["spool_id"] == "1000001");
    CHECK(page["setting_id"] == "GFA00");
    CHECK(page["brand"] == "Bambu Lab");
    CHECK(page["material_type"] == "PLA");
    CHECK(page["series"] == "PLA Basic");
    CHECK(page["color_code"] == "#FFFFFFFF");
    CHECK(page["initial_weight"] == 1000);
    CHECK(page["net_weight"] == 750);
    CHECK(page["diameter"] == 1.75);
    CHECK(page["cloud_synced"] == true);
    CHECK(page["favorite"] == false);
    CHECK(page["spool_weight"] == 0);
}

TEST_CASE("The remaining percentage follows the weights", "[BblFilament]")
{
    fil::Spool s = current_spool();
    int        expected = 0;
    SECTION("three quarters") { s.net_weight = 750; s.total_net_weight = 1000; expected = 75; }
    SECTION("empty") { s.net_weight = 0; s.total_net_weight = 1000; expected = 0; }
    SECTION("over full is clamped") { s.net_weight = 1200; s.total_net_weight = 1000; expected = 100; }
    SECTION("no total") { s.net_weight = 500; s.total_net_weight = 0; expected = 100; }
    SECTION("negative remaining") { s.net_weight = -5; s.total_net_weight = 1000; expected = 100; }
    CHECK(fil::spool_to_page(s)["remain_percent"] == expected);
}

TEST_CASE("The page sees ams spools as ams_sync and manual spools as manual", "[BblFilament]")
{
    fil::Spool s = current_spool();
    s.create_type = "ams";
    CHECK(fil::spool_to_page(s)["entry_method"] == "ams_sync");
    s.create_type = "manual";
    CHECK(fil::spool_to_page(s)["entry_method"] == "manual");
}

TEST_CASE("The page sees status 1 as info_needed", "[BblFilament]")
{
    fil::Spool s = current_spool();
    CHECK(fil::spool_to_page(s)["status"] == "active");
    s.status = 1;
    CHECK(fil::spool_to_page(s)["status"] == "info_needed");
}

// ---------------------------------------------------------------------------------------------------------------------
// apply_page_patch

TEST_CASE("A page patch changes each mapped field of the copy", "[BblFilament]")
{
    fil::Spool s = current_spool();
    fil::apply_page_patch(s, json{{"setting_id", "GFB00"},
                                  {"brand", "Other"},
                                  {"material_type", "PETG"},
                                  {"series", "PETG HF"},
                                  {"color_code", "#00FF00FF"},
                                  {"color_type", 0},
                                  {"initial_weight", 500},
                                  {"net_weight", 100},
                                  {"note", "n"}});
    CHECK(s.filament_id == "GFB00");
    CHECK(s.vendor == "Other");
    CHECK(s.type == "PETG");
    CHECK(s.name == "PETG HF");
    CHECK(s.color == "#00FF00FF");
    CHECK(s.color_type == 0);
    CHECK(s.total_net_weight == 500.0);
    CHECK(s.net_weight == 100.0);
    CHECK(s.note == "n");
}

TEST_CASE("A page patch ignores null, unknown and wrongly typed keys", "[BblFilament]")
{
    fil::Spool       s      = current_spool();
    const fil::Spool before = s;
    fil::apply_page_patch(s, json{{"brand", nullptr}, {"spool_id", "5"}, {"remain_percent", 3}, {"series", 7}, {"net_weight", "x"}, {"colors", "red"}});
    CHECK(s.vendor == before.vendor);
    CHECK(s.id == before.id);
    CHECK(s.name == before.name);
    CHECK(s.net_weight == before.net_weight);
    CHECK(s.colors == before.colors);
}

TEST_CASE("A page patch with colors makes the first one the colour", "[BblFilament]")
{
    fil::Spool s = current_spool();
    fil::apply_page_patch(s, json{{"color_code", "#111111FF"}, {"colors", json::array({"#222222FF", "#333333FF"})}});
    CHECK(s.color == "#222222FF");
    CHECK(s.colors.size() == 2);
}

// ---------------------------------------------------------------------------------------------------------------------
// create_body

TEST_CASE("A manual spool becomes a manual create body", "[BblFilament]")
{
    const json page = json{{"brand", "Bambu Lab"}, {"material_type", "PLA"}, {"series", "PLA Basic"}, {"setting_id", "GFA00"},
                           {"color_code", "#FFFFFFFF"}, {"color_type", 2}, {"initial_weight", 1000}, {"net_weight", 800},
                           {"note", "shelf 2"}};
    const json b = fil::create_body(page, {});
    CHECK(b["createType"] == "manual");
    CHECK(b["filamentVendor"] == "Bambu Lab");
    CHECK(b["filamentType"] == "PLA");
    CHECK(b["filamentName"] == "PLA Basic");
    CHECK(b["filamentId"] == "GFA00");
    CHECK(b["isSupport"] == false);
    CHECK_FALSE(b.contains("RFID"));
    CHECK(b["color"] == "#FFFFFFFF");
    CHECK(b["colorType"] == 2);
    CHECK_FALSE(b.contains("colors"));
    CHECK(b["trayIdName"] == "");
    CHECK(b["rolls"] == 1);
    CHECK(b["totalNetWeight"] == 1000);
    CHECK(b["netWeight"] == 800);
    CHECK(b["note"] == "shelf 2");
}

TEST_CASE("A spool with a valid tag becomes an ams create body with the RFID", "[BblFilament]")
{
    const json b = fil::create_body(json{{"tag_uid", "A1B2C3"}, {"material_type", "PLA"}}, {});
    CHECK(b["createType"] == "ams");
    CHECK(b["RFID"] == "A1B2C3");
    const json z = fil::create_body(json{{"tag_uid", "0000"}, {"material_type", "PLA"}}, {});
    CHECK(z["createType"] == "manual");
    CHECK_FALSE(z.contains("RFID"));
}

TEST_CASE("A remaining weight of zero is sent as zero", "[BblFilament]")
{
    const json b = fil::create_body(json{{"initial_weight", 1000}, {"net_weight", 0}}, {});
    CHECK(b["netWeight"] == 0);
    CHECK(b["totalNetWeight"] == 1000);
}

TEST_CASE("A missing remaining weight defaults to the total", "[BblFilament]")
{
    const json b = fil::create_body(json{{"initial_weight", 1000}}, {});
    CHECK(b["netWeight"] == 1000);
    const json n = fil::create_body(json{{"material_type", "PLA"}}, {});
    CHECK_FALSE(n.contains("netWeight"));
    CHECK_FALSE(n.contains("totalNetWeight"));
}

TEST_CASE("An empty series falls back to the material type in a create body", "[BblFilament]")
{
    const json b = fil::create_body(json{{"material_type", "PETG"}, {"series", ""}}, {});
    CHECK(b["filamentName"] == "PETG");
}

TEST_CASE("A multicolour spool keeps its colors and takes the first as colour", "[BblFilament]")
{
    const json b = fil::create_body(json{{"color_code", "#000000FF"}, {"colors", json::array({"#111111FF", "#222222FF"})}, {"color_type", 1}}, {});
    CHECK(b["color"] == "#111111FF");
    CHECK(b["colorType"] == 1);
    CHECK(b["colors"].size() == 2);
}

TEST_CASE("A create body never carries printer link keys", "[BblFilament]")
{
    const json b = fil::create_body(json{{"material_type", "PLA"}, {"in_printer", true}, {"dev_id", "D"}, {"ams_id", 1}, {"ams_sn", "S"},
                                         {"ams_type", 1}, {"slot_id", "2"}, {"device_name", "N"}},
                                    {});
    for (const char* key : {"inPrinter", "devId", "amsSn", "amsId", "amsType", "slotId", "deviceName"}) CHECK_FALSE(b.contains(key));
}

TEST_CASE("A create body uses the lookups when they are set", "[BblFilament]")
{
    const json b = fil::create_body(json{{"setting_id", "GFS00"}, {"color_code", "#123456FF"}, {"material_type", "PLA"}}, full_lookup());
    CHECK(b["isSupport"] == true);
    CHECK(b["trayIdName"] == "T:GFS00:1:2");

    const json given = fil::create_body(json{{"setting_id", "GFS00"}, {"tray_id_name", "X1"}}, full_lookup());
    CHECK(given["trayIdName"] == "X1");

    const json multi = fil::create_body(json{{"setting_id", "G"}, {"colors", json::array({"#1", "#2"})}}, full_lookup());
    CHECK(multi["trayIdName"] == "T:G:2:1");
}

// ---------------------------------------------------------------------------------------------------------------------
// update_body

TEST_CASE("An update body for one changed field has the id, the name and that field", "[BblFilament]")
{
    const auto b = fil::update_body(current_spool(), json{{"note", "new"}}, {});
    REQUIRE(b.has_value());
    CHECK((*b)["id"].is_number_integer());
    CHECK((*b)["id"] == 1000001);
    CHECK((*b)["filamentName"] == "PLA Basic");
    CHECK((*b)["note"] == "new");
    CHECK(b->size() == 3);
}

TEST_CASE("An empty note in an edit is sent", "[BblFilament]")
{
    const auto b = fil::update_body(current_spool(), json{{"note", ""}}, {});
    REQUIRE(b.has_value());
    CHECK((*b)["note"] == "");
}

TEST_CASE("An edit of the series is sent as the filament name", "[BblFilament]")
{
    const auto b = fil::update_body(current_spool(), json{{"series", "Renamed"}}, {});
    REQUIRE(b.has_value());
    CHECK((*b)["filamentName"] == "Renamed");
}

TEST_CASE("An empty current name falls back to the type", "[BblFilament]")
{
    fil::Spool s = current_spool();
    s.name.clear();
    const auto b = fil::update_body(s, json{{"note", "x"}}, {});
    REQUIRE(b.has_value());
    CHECK((*b)["filamentName"] == "PLA");
}

TEST_CASE("The initial weight of an edit maps to the total net weight", "[BblFilament]")
{
    const auto b = fil::update_body(current_spool(), json{{"initial_weight", 850.4}}, {});
    REQUIRE(b.has_value());
    CHECK((*b)["totalNetWeight"] == 850);
    CHECK_FALSE(b->contains("initial_weight"));
}

TEST_CASE("A remaining weight of zero in an edit is sent as zero", "[BblFilament]")
{
    const auto b = fil::update_body(current_spool(), json{{"net_weight", 0}}, {});
    REQUIRE(b.has_value());
    CHECK((*b)["netWeight"] == 0);
    const auto neg = fil::update_body(current_spool(), json{{"net_weight", -5}}, {});
    REQUIRE(neg.has_value());
    CHECK((*neg)["netWeight"] == 0);
}

TEST_CASE("A colour edit with empty colors sends colors with that colour", "[BblFilament]")
{
    const auto b = fil::update_body(current_spool(), json{{"color_code", "#00FF00FF"}, {"colors", json::array()}, {"color_type", 2}}, {});
    REQUIRE(b.has_value());
    CHECK((*b)["color"] == "#00FF00FF");
    CHECK((*b)["colors"] == json::array({"#00FF00FF"}));
    CHECK((*b)["colorType"] == 2);
}

TEST_CASE("A colour edit replaces a single stale colors entry but not a longer list", "[BblFilament]")
{
    const auto one = fil::update_body(current_spool(), json{{"color_code", "#AA"}, {"colors", json::array({"#BB"})}}, {});
    REQUIRE(one.has_value());
    CHECK((*one)["colors"] == json::array({"#AA"}));
    const auto many = fil::update_body(current_spool(), json{{"color_code", "#AA"}, {"colors", json::array({"#BB", "#CC"})}, {"color_type", 9}}, {});
    REQUIRE(many.has_value());
    CHECK((*many)["colors"] == json::array({"#BB", "#CC"}));
    CHECK((*many)["colorType"] == 1);
}

TEST_CASE("A patch of only fields the cloud does not store gives an empty update body", "[BblFilament]")
{
    const auto b = fil::update_body(current_spool(), json{{"color_name", "Red"}, {"remain_percent", 40}, {"spool_weight", 200}}, full_lookup());
    REQUIRE(b.has_value());
    CHECK(b->is_object());
    CHECK(b->empty());
}

TEST_CASE("A filament id change with lookups sends isSupport, the type and the tray name", "[BblFilament]")
{
    const auto b = fil::update_body(current_spool(), json{{"setting_id", "GFS00"}}, full_lookup());
    REQUIRE(b.has_value());
    CHECK((*b)["filamentId"] == "GFS00");
    CHECK((*b)["isSupport"] == true);
    CHECK((*b)["filamentType"] == "PLA-S");
    CHECK((*b)["trayIdName"] == "T:GFS00:1:2");
}

TEST_CASE("A filament id change without lookups sends none of the derived keys", "[BblFilament]")
{
    const auto b = fil::update_body(current_spool(), json{{"setting_id", "GFS00"}}, {});
    REQUIRE(b.has_value());
    CHECK_FALSE(b->contains("isSupport"));
    CHECK_FALSE(b->contains("filamentType"));
    CHECK_FALSE(b->contains("trayIdName"));
}

TEST_CASE("A colour change recomputes the tray name from the colours after the patch", "[BblFilament]")
{
    std::vector<std::string> seen_colors;
    int                      seen_type = -1;
    fil::FilamentLookup      lookup    = full_lookup();
    lookup.tray_id_name                = [&](const std::string&, const std::vector<std::string>& colors, int color_type) {
        seen_colors = colors;
        seen_type   = color_type;
        return std::string("tray");
    };
    const auto b = fil::update_body(current_spool(), json{{"color_code", "#00FF00FF"}}, lookup);
    REQUIRE(b.has_value());
    CHECK(seen_colors == std::vector<std::string>{"#00FF00FF"});
    CHECK(seen_type == 2);
    CHECK((*b)["trayIdName"] == "tray");
    CHECK_FALSE(b->contains("isSupport"));
    CHECK((*b)["filamentType"] == "PETG");
}

TEST_CASE("An update body for a spool with an invalid id is nullopt", "[BblFilament]")
{
    fil::Spool s = current_spool();
    s.id         = "abc";
    CHECK_FALSE(fil::update_body(s, json{{"note", "x"}}, {}).has_value());
}

TEST_CASE("An update body never carries keys the cloud edit must not send", "[BblFilament]")
{
    json patch = json::object();
    for (const auto& key : forbidden_update_keys()) patch[key] = "x";
    patch["note"] = "kept";
    const auto b = fil::update_body(current_spool(), patch, full_lookup());
    REQUIRE(b.has_value());
    for (const auto& key : forbidden_update_keys()) CHECK_FALSE(b->contains(key));
    CHECK((*b)["note"] == "kept");
}

// ---------------------------------------------------------------------------------------------------------------------
// classify_reply

TEST_CASE("A zero return code is a success with the parsed body", "[BblFilament]")
{
    const auto r = fil::classify_reply(0, "{}");
    CHECK(r.ok());
    CHECK(r.body.is_object());
    const auto h = fil::classify_reply(0, R"({"hits":[]})");
    CHECK(h.body["hits"].is_array());
}

TEST_CASE("A zero return code with an empty or unparsable body leaves the body null", "[BblFilament]")
{
    const auto e = fil::classify_reply(0, "");
    CHECK(e.ok());
    CHECK(e.body.is_null());
    const auto bad = fil::classify_reply(0, "<html>");
    CHECK(bad.ok());
    CHECK(bad.body.is_null());
}

TEST_CASE("A failed call with a 404 body is NotFound", "[BblFilament]")
{
    const auto r = fil::classify_reply(-29, R"({"code":404,"error":"not found"})");
    CHECK(r.error == fil::CloudError::NotFound);
    CHECK(r.message == "not found");
    CHECK(r.rc == -29);
    CHECK(r.body.is_null());
}

TEST_CASE("A failed call with another client error is Rejected", "[BblFilament]")
{
    const auto r = fil::classify_reply(-28, R"({"code":400,"error":"bad"})");
    CHECK(r.error == fil::CloudError::Rejected);
    CHECK(r.message == "bad");
    const auto s = fil::classify_reply(-28, R"({"code":"422","message":"nope"})");
    CHECK(s.error == fil::CloudError::Rejected);
    CHECK(s.message == "nope");
}

TEST_CASE("A failed call without a usable client error body is Unknown", "[BblFilament]")
{
    for (const std::string body : {std::string(""), std::string("<html>"), std::string(R"({"code":500})"), std::string(R"({"error":"x"})"),
                                   std::string(R"({"code":"abc"})"), std::string("[1]")}) {
        const auto r = fil::classify_reply(-27, body);
        CHECK(r.error == fil::CloudError::Unknown);
        CHECK(r.message.empty());
    }
}

TEST_CASE("A long server error text is cut to 200 bytes", "[BblFilament]")
{
    const std::string text(300, 'x');
    const auto        r = fil::classify_reply(-28, json{{"code", 400}, {"error", text}}.dump());
    CHECK(r.error == fil::CloudError::Rejected);
    CHECK(r.message.size() == 200);
}

// ---------------------------------------------------------------------------------------------------------------------
// list_all

TEST_CASE("Listing an empty cloud gives no spools whatever shape the empty page has", "[BblFilament]")
{
    FakeCloud cloud;
    SECTION("empty array") {}
    SECTION("null hits") { cloud.null_hits = true; }
    SECTION("no hits key") { cloud.omit_hits = true; }
    const auto r = fil::list_all(cloud, {});
    CHECK(r.ok);
    CHECK(r.spools.empty());
    CHECK_FALSE(r.paging_suspect);
    CHECK(cloud.log == std::vector<std::string>{"list 0"});
}

TEST_CASE("Listing pages through the whole cloud in order", "[BblFilament]")
{
    FakeCloud cloud;
    int       count = 0;
    std::vector<std::string> calls;
    SECTION("exactly one page") { count = 20; calls = {"list 0", "list 20"}; }
    SECTION("one over a page") { count = 21; calls = {"list 0", "list 20"}; }
    SECTION("several pages") { count = 45; calls = {"list 0", "list 20", "list 40"}; }
    SECTION("under a page") { count = 5; calls = {"list 0"}; }
    cloud.fill(count);
    const auto r = fil::list_all(cloud, {});
    REQUIRE(r.ok);
    REQUIRE(static_cast<int>(r.spools.size()) == count);
    for (int i = 0; i < count; ++i) CHECK(r.spools[i].id == std::to_string(1000001 + i));
    CHECK(cloud.log == calls);
    CHECK_FALSE(r.paging_suspect);
}

TEST_CASE("A failing second page fails the whole list and returns no spools", "[BblFilament]")
{
    FakeCloud cloud;
    cloud.fill(45);
    cloud.forced["list"] = {std::nullopt, forced_error(fil::CloudError::Unknown, "boom")};
    const auto r         = fil::list_all(cloud, {});
    CHECK_FALSE(r.ok);
    CHECK(r.spools.empty());
    CHECK(r.failure.error == fil::CloudError::Unknown);
    CHECK(r.failure.message == "boom");
}

TEST_CASE("A server that ignores the offset stops after the second page and is flagged", "[BblFilament]")
{
    FakeCloud cloud;
    cloud.fill(45);
    cloud.ignore_offset = true;
    const auto r        = fil::list_all(cloud, {});
    CHECK(r.ok);
    CHECK(r.spools.size() == 20);
    CHECK(r.paging_suspect);
    CHECK(cloud.log == std::vector<std::string>{"list 0", "list 20"});
}

TEST_CASE("A total in the reply ends the listing without the extra page", "[BblFilament]")
{
    FakeCloud cloud;
    cloud.fill(20);
    cloud.with_total = true;
    const auto r     = fil::list_all(cloud, {});
    CHECK(r.ok);
    CHECK(r.spools.size() == 20);
    CHECK(cloud.log == std::vector<std::string>{"list 0"});
}

TEST_CASE("Rows without a usable id are counted as skipped", "[BblFilament]")
{
    FakeCloud cloud;
    cloud.fill(3);
    json bad = example_row();
    bad.erase("id");
    cloud.rows.insert(cloud.rows.begin() + 1, bad);
    cloud.rows.push_back(json(5));
    const auto r = fil::list_all(cloud, {});
    CHECK(r.ok);
    CHECK(r.spools.size() == 3);
    CHECK(r.skipped_rows == 2);
}

TEST_CASE("A hits value of the wrong type fails the list", "[BblFilament]")
{
    FakeCloud cloud;
    fil::CloudReply reply;
    reply.body           = json{{"hits", "nope"}};
    cloud.forced["list"] = {reply};
    const auto r         = fil::list_all(cloud, {});
    CHECK_FALSE(r.ok);
    CHECK(r.failure.error == fil::CloudError::Unknown);
    CHECK(r.failure.message == "unexpected response");
}

TEST_CASE("A body that is not an object fails the list", "[BblFilament]")
{
    FakeCloud cloud;
    fil::CloudReply reply;
    reply.body           = json::array();
    cloud.forced["list"] = {reply};
    CHECK_FALSE(fil::list_all(cloud, {}).ok);
}

TEST_CASE("A cancelled listing fails before the call", "[BblFilament]")
{
    FakeCloud cloud;
    cloud.fill(5);
    const auto r = fil::list_all(cloud, [] { return true; });
    CHECK_FALSE(r.ok);
    CHECK(r.failure.message == "cancelled");
    CHECK(cloud.log.empty());
}

TEST_CASE("Duplicate ids across pages are kept once", "[BblFilament]")
{
    FakeCloud cloud;
    cloud.fill(25);
    cloud.rows[22] = cloud.rows[3]; // a repeat of an id seen on page one
    const auto r   = fil::list_all(cloud, {});
    CHECK(r.ok);
    CHECK(r.spools.size() == 24);
}

// ---------------------------------------------------------------------------------------------------------------------
// PluginSpoolCloud

TEST_CASE("The plugin spool cloud is unavailable when no plugin is loaded", "[BblFilament]")
{
    fil::PluginSpoolCloud cloud;
    CHECK(cloud.list_page(0, 20).error == fil::CloudError::Unavailable);
    CHECK(cloud.create(json{{"a", 1}}).error == fil::CloudError::Unavailable);
    CHECK(cloud.update("1", json{{"a", 1}}).error == fil::CloudError::Unavailable);
    CHECK(cloud.remove({"1"}).error == fil::CloudError::Unavailable);
    CHECK(cloud.config().error == fil::CloudError::Unavailable);
}

TEST_CASE("The plugin spool cloud rejects a delete without ids", "[BblFilament]")
{
    fil::PluginSpoolCloud cloud;
    CHECK(cloud.remove({}).error == fil::CloudError::Rejected);
}

// ---------------------------------------------------------------------------------------------------------------------
// SpoolService, driven by a manual runner: the "worker" and the "ui" are queues the test steps by hand.


TEST_CASE("The first pull fills the view and a second pull reports kept spools", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(3);
    rig.svc->request_pull();
    rig.settle();
    CHECK(rig.svc->view().size() == 3);
    CHECK(rig.has_event("pull_done 3 0"));
    CHECK(rig.has_event("spools_changed"));
    CHECK(rig.svc->state().last_synced_at > 0);
    CHECK_FALSE(rig.svc->state().is_syncing);
    rig.events.clear();
    rig.svc->request_pull();
    rig.settle();
    CHECK(rig.has_event("pull_done 0 3"));
}

TEST_CASE("A failed pull keeps the list and sets the error until the next good pull", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(3);
    rig.svc->request_pull();
    rig.settle();
    rig.cloud->forced["list"] = {forced_error(fil::CloudError::Unknown, "slow")};
    rig.svc->request_pull();
    rig.settle();
    CHECK(rig.svc->view().size() == 3);
    CHECK(rig.svc->state().has_error);
    CHECK(rig.svc->state().error_kind == fil::FailKind::Unknown);
    CHECK(rig.svc->state().error_message == "slow");
    rig.svc->request_pull();
    rig.settle();
    CHECK_FALSE(rig.svc->state().has_error);
}

TEST_CASE("Repeated pull requests cause one list", "[BblFilament]")
{
    Rig rig;
    rig.svc->request_pull();
    rig.svc->request_pull();
    rig.svc->request_pull();
    rig.settle();
    CHECK(rig.cloud->count("list") == 1);
}

TEST_CASE("A pull request during a running pull causes no second pull", "[BblFilament]")
{
    Rig rig;
    rig.svc->request_pull();
    rig.step_worker(); // the list ran, its completion has not been delivered yet
    rig.svc->request_pull();
    rig.settle();
    CHECK(rig.cloud->count("list") == 1);
}

TEST_CASE("An added spool is created once and followed by one pull", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    rig.svc->request_pull();
    rig.settle();
    rig.cloud->log.clear();
    const auto accepted = rig.svc->add(page_spool("Fresh"));
    CHECK(accepted.ok);
    rig.settle();
    CHECK(rig.cloud->log == std::vector<std::string>{"create", "list 0"});
    CHECK(rig.has_event("push_done create "));
    CHECK(rig.svc->view().size() == 3);
}

TEST_CASE("An add whose reply is unknown is never sent again", "[BblFilament]")
{
    Rig rig;
    rig.cloud->forced["create"] = {forced_error(fil::CloudError::Unknown)};
    rig.svc->add(page_spool());
    rig.settle();
    CHECK(rig.cloud->count("create") == 1);
    CHECK(rig.cloud->count("list") == 1);
    CHECK(rig.has_event("push_failed create  Unknown"));
}

TEST_CASE("An add with a quantity creates that many spools and then pulls once", "[BblFilament]")
{
    Rig rig;
    int quantity = 3;
    int expected = 3;
    SECTION("three") {}
    SECTION("zero is clamped to one") { quantity = 0; expected = 1; }
    SECTION("negative is clamped to one") { quantity = -4; expected = 1; }
    SECTION("500 is clamped to 99") { quantity = 500; expected = 99; }
    rig.svc->add(page_spool(), quantity);
    rig.settle();
    CHECK(rig.cloud->count("create") == expected);
    CHECK(rig.cloud->count("list 0") == 1); // one pull; 99 spools take several pages
    CHECK(rig.cloud->log.back().rfind("list", 0) == 0);
    CHECK(rig.cloud->log.front() == "create");
}

TEST_CASE("A batch runs each entry on its own and pulls once at the end", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    rig.svc->request_pull();
    rig.settle();
    rig.cloud->log.clear();
    rig.events.clear();
    rig.cloud->forced["create"] = {std::nullopt, forced_error(fil::CloudError::Rejected, "no")};

    const auto accepted = rig.svc->batch(json::array({page_spool("A"), page_spool("B")}),
                                         json::array({json{{"spool_id", "1000001"}, {"note", "x"}}, json{{"spool_id", "999"}, {"note", "y"}}}));
    CHECK(accepted.ok);
    CHECK(rig.has_event("push_failed update 999 BadRequest"));
    rig.settle();
    CHECK(rig.cloud->log == std::vector<std::string>{"create", "create", "update 1000001", "list 0"});
    CHECK(rig.has_event("push_done create "));
    CHECK(rig.has_event("push_failed create  Rejected"));
    CHECK(rig.has_event("push_done update 1000001"));
}

TEST_CASE("A batch needs two arrays", "[BblFilament]")
{
    Rig rig;
    CHECK_FALSE(rig.svc->batch(json::object(), json::array()).ok);
    CHECK(rig.svc->batch(json::array(), json::object()).kind == fil::FailKind::BadRequest);
    CHECK(rig.cloud->log.empty());
}

TEST_CASE("An edit shows in the view at once and the cloud gets the update body", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    rig.svc->request_pull();
    rig.settle();
    rig.cloud->log.clear();
    rig.cloud->bodies.clear();
    const fil::Spool before = rig.svc->view().front();

    const json patch{{"spool_id", "1000001"}, {"note", "hello"}};
    CHECK(rig.svc->update(patch).ok);
    CHECK(rig.note_of("1000001") == "hello");
    rig.settle();

    const auto expected = fil::update_body(before, patch, {});
    REQUIRE(expected.has_value());
    REQUIRE(rig.cloud->bodies.size() == 1);
    CHECK(rig.cloud->bodies.front() == *expected);
    CHECK(rig.cloud->log == std::vector<std::string>{"update 1000001", "list 0"});
    CHECK(rig.note_of("1000001") == "hello");
}

TEST_CASE("A rejected edit brings the old values back", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    rig.svc->request_pull();
    rig.settle();
    rig.cloud->log.clear();
    rig.events.clear();
    rig.cloud->forced["update"] = {forced_error(fil::CloudError::Rejected, "bad")};
    rig.svc->update(json{{"spool_id", "1000001"}, {"note", "hello"}});
    CHECK(rig.note_of("1000001") == "hello");
    rig.settle();
    CHECK(rig.note_of("1000001").empty());
    CHECK(rig.has_event("push_failed update 1000001 Rejected"));
    CHECK(rig.cloud->count("list") == 0);
    CHECK(rig.svc->state().has_error);
}

TEST_CASE("An edit of a spool the cloud no longer has is not turned into a create", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    rig.svc->request_pull();
    rig.settle();
    rig.cloud->log.clear();
    rig.events.clear();
    rig.cloud->rows.erase(rig.cloud->rows.begin()); // gone on the server; the update answers 404
    rig.svc->update(json{{"spool_id", "1000001"}, {"note", "hello"}});
    rig.settle();
    CHECK(rig.cloud->count("create") == 0);
    CHECK(rig.cloud->count("update") == 1);
    CHECK(rig.cloud->count("list") == 1);
    CHECK(rig.has_event("push_failed update 1000001 NotFound"));
    CHECK(rig.svc->view().size() == 1);
}

TEST_CASE("An edit that changes nothing the cloud stores is accepted without any work", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(1);
    rig.svc->request_pull();
    rig.settle();
    rig.cloud->log.clear();
    rig.events.clear();
    const auto accepted = rig.svc->update(json{{"spool_id", "1000001"}, {"color_name", "Red"}, {"remain_percent", 3}});
    CHECK(accepted.ok);
    CHECK(rig.events.empty());
    CHECK(rig.cloud->log.empty());
    CHECK(rig.worker.empty());
}

TEST_CASE("An edit of an unknown spool or without an id is a bad request", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(1);
    rig.svc->request_pull();
    rig.settle();
    CHECK(rig.svc->update(json{{"spool_id", "42"}, {"note", "x"}}).kind == fil::FailKind::BadRequest);
    CHECK_FALSE(rig.svc->update(json{{"note", "x"}}).ok);
    CHECK_FALSE(rig.svc->update(json::array()).ok);
}

TEST_CASE("The second of two queued edits is built from the result of the first", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(1);
    rig.svc->request_pull();
    rig.settle();
    rig.cloud->log.clear();
    rig.cloud->bodies.clear();
    rig.svc->update(json{{"spool_id", "1000001"}, {"series", "Renamed"}});
    rig.svc->update(json{{"spool_id", "1000001"}, {"note", "n"}});
    rig.settle();
    REQUIRE(rig.cloud->bodies.size() == 2);
    CHECK(rig.cloud->bodies[0]["filamentName"] == "Renamed");
    CHECK(rig.cloud->bodies[1]["filamentName"] == "Renamed");
    CHECK(rig.cloud->bodies[1]["note"] == "n");
    CHECK(rig.cloud->log == std::vector<std::string>{"update 1000001", "update 1000001", "list 0"});
}

TEST_CASE("The second of two queued edits does not carry a first edit the cloud refused", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(1);
    rig.svc->request_pull();
    rig.settle();
    rig.cloud->bodies.clear();
    rig.cloud->forced["update"] = {forced_error(fil::CloudError::Rejected), std::nullopt};
    rig.svc->update(json{{"spool_id", "1000001"}, {"series", "Renamed"}});
    rig.svc->update(json{{"spool_id", "1000001"}, {"note", "n"}});
    rig.settle();
    REQUIRE(rig.cloud->bodies.size() == 2);
    CHECK(rig.cloud->bodies[1]["filamentName"] == "Spool 1000001");
    CHECK(rig.cloud->bodies[1]["note"] == "n");
}

TEST_CASE("An edit queued behind a pull for a spool the pull dropped ends as not found without an update call", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(3);
    rig.svc->request_pull();
    rig.settle();
    rig.cloud->log.clear();
    rig.events.clear();
    rig.svc->request_pull(); // its job is queued but has not run
    rig.svc->update(json{{"spool_id", "1000002"}, {"note", "x"}});
    rig.cloud->rows.erase(rig.cloud->rows.begin() + 1);
    rig.settle();
    CHECK(rig.cloud->count("update") == 0);
    CHECK(rig.has_event("push_failed update 1000002 NotFound"));
    CHECK(rig.svc->view().size() == 2);
}

TEST_CASE("A removal hides the rows at once and the pull confirms it", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(3);
    rig.svc->request_pull();
    rig.settle();
    rig.cloud->log.clear();
    CHECK(rig.svc->remove({"1000001", "1000002", "1000002", "abc", "777"}).ok);
    CHECK(rig.svc->view().size() == 1);
    rig.settle();
    CHECK(rig.cloud->log == std::vector<std::string>{"remove 1000001,1000002", "list 0"});
    CHECK(rig.svc->view().size() == 1);
    CHECK(rig.cloud->rows.size() == 1);
    CHECK(rig.has_event("push_done delete 1000001"));
}

TEST_CASE("A rejected delete brings the rows back", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(3);
    rig.svc->request_pull();
    rig.settle();
    rig.cloud->forced["remove"] = {forced_error(fil::CloudError::Rejected)};
    rig.svc->remove({"1000001"});
    CHECK(rig.svc->view().size() == 2);
    rig.settle();
    CHECK(rig.svc->view().size() == 3);
    CHECK(rig.has_event("push_failed delete 1000001 Rejected"));
}

TEST_CASE("A removal of nothing known is a bad request", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(1);
    rig.svc->request_pull();
    rig.settle();
    CHECK(rig.svc->remove({"555"}).kind == fil::FailKind::BadRequest);
    CHECK(rig.svc->remove({}).kind == fil::FailKind::BadRequest);
}

TEST_CASE("A write queued while a pull runs goes after it and is followed by a new pull", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    rig.svc->request_pull(); // job queued, not run
    rig.svc->add(page_spool());
    rig.settle();
    CHECK(rig.cloud->log == std::vector<std::string>{"list 0", "create", "list 0"});
}

TEST_CASE("A pull never starts while a write is queued", "[BblFilament]")
{
    Rig rig;
    rig.svc->add(page_spool()); // running
    rig.svc->request_pull();
    rig.svc->add(page_spool()); // queued
    rig.settle();
    CHECK(rig.cloud->log == std::vector<std::string>{"create", "create", "list 0"});
}

TEST_CASE("A settled update keeps showing its edit when the next pull fails", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    rig.svc->request_pull();
    rig.settle();
    rig.cloud->forced["list"] = {forced_error(fil::CloudError::Unknown)};
    rig.svc->update(json{{"spool_id", "1000001"}, {"note", "mine"}});
    rig.settle();
    CHECK(rig.svc->state().has_error);
    CHECK(rig.note_of("1000001") == "mine");
    rig.cloud->rows[0]["note"] = "server";
    rig.svc->request_pull();
    rig.settle();
    CHECK(rig.note_of("1000001") == "server");
    CHECK_FALSE(rig.svc->state().has_error);
}

TEST_CASE("Signed out, every write is refused and nothing reaches the cloud", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    rig.svc->request_pull();
    rig.settle();
    rig.cloud->log.clear();
    SECTION("signed out") { rig.session.signed_in = false; }
    SECTION("closing") { rig.session.closing = true; }
    CHECK(rig.svc->add(page_spool()).kind == fil::FailKind::NotSignedIn);
    CHECK(rig.svc->batch(json::array(), json::array()).kind == fil::FailKind::NotSignedIn);
    CHECK(rig.svc->update(json{{"spool_id", "1000001"}, {"note", "x"}}).kind == fil::FailKind::NotSignedIn);
    CHECK(rig.svc->remove({"1000001"}).kind == fil::FailKind::NotSignedIn);
    CHECK_FALSE(rig.svc->add(page_spool()).ok);
    CHECK(rig.svc->view().empty());
    rig.svc->request_pull();
    rig.svc->request_config(true);
    CHECK(rig.worker.empty());
    CHECK(rig.cloud->log.empty());
    CHECK_FALSE(rig.svc->state().signed_in);
}

TEST_CASE("A reset while a call is in flight makes its completion do nothing", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    rig.svc->request_pull();
    rig.step_worker();
    rig.svc->reset();
    rig.events.clear();
    rig.run_ui();
    CHECK(rig.events.empty());
    CHECK(rig.svc->view().empty());
    CHECK_FALSE(rig.svc->status().have_list);
}

TEST_CASE("A changed account while a call is in flight drops the completion and clears the state", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(3);
    rig.svc->request_pull();
    rig.settle();
    rig.svc->request_pull();
    rig.step_worker();
    rig.events.clear();
    SECTION("another user") { rig.session.uid = "u2"; }
    SECTION("another plugin generation") { rig.session.generation = 2; }
    rig.run_ui();
    CHECK(rig.count_events("pull_done 0 3") == 0);
    CHECK(rig.svc->view().empty());
    CHECK_FALSE(rig.svc->status().have_list);
    CHECK(rig.worker.empty());
}

TEST_CASE("A completion after the service is gone does nothing", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    SECTION("job ran, completion queued")
    {
        rig.svc->request_pull();
        rig.step_worker();
        rig.svc.reset();
        rig.run_ui();
    }
    SECTION("job still queued")
    {
        rig.svc->request_pull();
        rig.svc.reset();
        rig.settle();
    }
    CHECK(rig.ui.empty());
    CHECK(rig.worker.empty());
}

TEST_CASE("A worker that refuses jobs fails writes and pulls as unavailable", "[BblFilament]")
{
    Rig rig;
    rig.accept_async = false;
    rig.svc->add(page_spool());
    CHECK(rig.has_event("push_failed create  Unavailable"));
    CHECK(rig.cloud->log.empty());
    rig.svc->request_pull();
    CHECK(rig.svc->state().has_error);
    CHECK(rig.svc->state().error_kind == fil::FailKind::Unavailable);
    CHECK_FALSE(rig.svc->state().is_syncing);
}

TEST_CASE("The config is fetched once, again on force, kept on failure and cleared by a reset", "[BblFilament]")
{
    Rig rig;
    rig.svc->request_config(false);
    rig.settle();
    CHECK(rig.has_event("config_fetched"));
    REQUIRE(rig.svc->cached_config() != nullptr);
    CHECK(rig.svc->cached_config()->contains("filamentColors"));
    CHECK(rig.cloud->count("config") == 1);

    rig.svc->request_config(false);
    rig.settle();
    CHECK(rig.cloud->count("config") == 1);

    rig.svc->request_config(true);
    rig.settle();
    CHECK(rig.cloud->count("config") == 2);

    rig.cloud->forced["config"] = {forced_error(fil::CloudError::Unknown)};
    rig.svc->request_config(true);
    rig.settle();
    CHECK(rig.cloud->count("config") == 3);
    CHECK(rig.svc->cached_config() != nullptr);

    rig.svc->reset();
    CHECK(rig.svc->cached_config() == nullptr);
}

TEST_CASE("The service status reports the queue and the list", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    rig.svc->request_pull();
    rig.settle();
    rig.svc->add(page_spool());
    const auto st = rig.svc->status();
    CHECK(st.have_list);
    CHECK(st.spool_count == 2);
    CHECK(st.pending_writes == 1);
    CHECK(st.state.is_syncing);
    rig.settle();
    CHECK(rig.svc->status().pending_writes == 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// SpoolWorker, on a real thread

namespace {

bool ready(std::future<void>& f) { return f.wait_for(std::chrono::seconds(10)) == std::future_status::ready; }

} // namespace

TEST_CASE("The spool worker runs jobs in the order they were posted", "[BblFilament]")
{
    fil::SpoolWorker   worker;
    std::mutex         mutex;
    std::vector<int>   order;
    std::promise<void> done;
    auto               finished  = done.get_future();
    bool               all_taken = true;
    for (int i = 0; i < 20; ++i) {
        const bool taken = worker.post([&, i] {
            std::lock_guard<std::mutex> lock(mutex);
            order.push_back(i);
            if (i == 19) done.set_value();
        });
        all_taken = all_taken && taken;
    }
    REQUIRE(ready(finished));
    CHECK(all_taken);
    std::vector<int> expected(20);
    for (int i = 0; i < 20; ++i) expected[i] = i;
    std::lock_guard<std::mutex> lock(mutex);
    CHECK(order == expected);
}

TEST_CASE("Draining the spool worker waits for the running job and forgets the queued ones", "[BblFilament]")
{
    fil::SpoolWorker   worker;
    std::promise<void> started, release, after_resume;
    auto               started_f = started.get_future();
    auto               release_f = release.get_future().share();
    auto               resumed_f = after_resume.get_future();
    std::atomic<bool>  released{false};
    std::atomic<bool>  first_done{false};
    std::atomic<bool>  second_ran{false};

    worker.post([&] {
        started.set_value();
        release_f.wait();
        first_done = true;
    });
    worker.post([&] { second_ran = true; });
    REQUIRE(ready(started_f));

    std::thread helper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        released = true;
        release.set_value();
    });
    worker.drain();
    const bool released_at_return   = released.load();
    const bool first_done_at_return = first_done.load();
    helper.join();

    CHECK(released_at_return);
    CHECK(first_done_at_return);
    CHECK_FALSE(second_ran.load());
    CHECK_FALSE(worker.accepting());
    CHECK_FALSE(worker.busy());
    CHECK_FALSE(worker.post([] {}));
    worker.drain(); // idempotent

    worker.resume();
    CHECK(worker.accepting());
    CHECK(worker.post([&] { after_resume.set_value(); }));
    CHECK(ready(resumed_f));
    CHECK_FALSE(second_ran.load());
}

TEST_CASE("Destroying a spool worker with a queued job does not hang", "[BblFilament]")
{
    std::promise<void> started, release;
    auto               started_f = started.get_future();
    auto               release_f = release.get_future().share();
    std::atomic<bool>  queued_ran{false};
    std::thread        helper;
    {
        fil::SpoolWorker worker;
        worker.post([&] {
            started.set_value();
            release_f.wait();
        });
        worker.post([&] { queued_ran = true; });
        REQUIRE(ready(started_f));
        helper = std::thread([&] {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            release.set_value();
        });
    }
    helper.join();
    CHECK_FALSE(queued_ran.load());
}

TEST_CASE("A throwing job does not stop the spool worker", "[BblFilament]")
{
    fil::SpoolWorker   worker;
    std::promise<void> done;
    auto               done_f = done.get_future();
    worker.post([] { throw std::runtime_error("boom"); });
    worker.post([] { throw 5; });
    worker.post([&] { done.set_value(); });
    CHECK(ready(done_f));
}

TEST_CASE("A spool worker that was never used drains and is destroyed cleanly", "[BblFilament]")
{
    fil::SpoolWorker worker;
    CHECK(worker.accepting());
    CHECK_FALSE(worker.busy());
    worker.drain();
}

// ---------------------------------------------------------------------------------------------------------------------
// Review fixes

TEST_CASE("A page patch of only a colour code makes the copy show that colour", "[BblFilament]")
{
    fil::Spool s = current_spool();
    s.color      = "#111111FF";
    s.colors     = {"#111111FF"};
    fil::apply_page_patch(s, json{{"color_code", "#222222FF"}});
    CHECK(s.color == "#222222FF");
    CHECK(s.colors == std::vector<std::string>{"#222222FF"});
}

TEST_CASE("A page patch with a colour code and empty colors makes the copy show that colour", "[BblFilament]")
{
    fil::Spool s = current_spool();
    s.color      = "#111111FF";
    s.colors     = {"#111111FF"};
    fil::apply_page_patch(s, json{{"color_code", "#222222FF"}, {"colors", json::array()}});
    CHECK(s.color == "#222222FF");
    CHECK(s.colors == std::vector<std::string>{"#222222FF"});
}

TEST_CASE("A page patch of only a colour code collapses a multicolour spool to that colour", "[BblFilament]")
{
    fil::Spool s = current_spool();
    s.color      = "#111111FF";
    s.colors     = {"#111111FF", "#333333FF"};
    s.color_type = 1;
    fil::apply_page_patch(s, json{{"color_code", "#222222FF"}});
    CHECK(s.colors == std::vector<std::string>{"#222222FF"});
    CHECK(s.color == "#222222FF");
}

TEST_CASE("A page patch with two colors and a colour code keeps both colors", "[BblFilament]")
{
    fil::Spool s = current_spool();
    fil::apply_page_patch(s, json{{"colors", json::array({"#AA", "#BB"})}, {"color_code", "#AA"}});
    CHECK(s.colors == std::vector<std::string>{"#AA", "#BB"});
    CHECK(s.color == "#AA");
}

TEST_CASE("A page patch normalises the colour type against the colour count", "[BblFilament]")
{
    fil::Spool s = current_spool();
    fil::apply_page_patch(s, json{{"colors", json::array({"#AA", "#BB"})}, {"color_type", 7}});
    CHECK(s.color_type == 1);
    fil::Spool one = current_spool();
    fil::apply_page_patch(one, json{{"color_code", "#AA"}, {"color_type", 7}});
    CHECK(one.color_type == 2);
}

TEST_CASE("The view after a colour edit equals the view after the cloud applied it and a pull replaced the list", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    rig.cloud->rows[0]["color"]     = "#111111FF";
    rig.cloud->rows[0]["colors"]    = json::array({"#111111FF"});
    rig.cloud->rows[0]["colorType"] = 2;
    rig.svc->request_pull();
    rig.settle();

    REQUIRE(rig.svc->update(json{{"spool_id", "1000001"}, {"color_code", "#222222FF"}, {"color_type", 7}}).ok);
    const std::vector<fil::Spool> optimistic = rig.svc->view();
    rig.settle();
    const std::vector<fil::Spool> confirmed = rig.svc->view();
    REQUIRE(optimistic.size() == 2);
    REQUIRE(confirmed.size() == 2);
    for (std::size_t i = 0; i < optimistic.size(); ++i) CHECK(fil::spool_to_page(optimistic[i]) == fil::spool_to_page(confirmed[i]));
    CHECK(confirmed.front().color == "#222222FF");
}

TEST_CASE("A huge weight is bounded in create and update bodies", "[BblFilament]")
{
    const json created = fil::create_body(json{{"initial_weight", 1e30}, {"net_weight", 1e30}}, {});
    CHECK(created["totalNetWeight"] == 999999999);
    CHECK(created["netWeight"] == 999999999);
    const auto edited = fil::update_body(current_spool(), json{{"initial_weight", 1e30}, {"net_weight", 1e30}}, {});
    REQUIRE(edited.has_value());
    CHECK((*edited)["totalNetWeight"] == 999999999);
    CHECK((*edited)["netWeight"] == 999999999);
}

TEST_CASE("A negative weight in an edit is sent as zero", "[BblFilament]")
{
    const auto edited = fil::update_body(current_spool(), json{{"initial_weight", -50}, {"net_weight", -1e30}}, {});
    REQUIRE(edited.has_value());
    CHECK((*edited)["totalNetWeight"] == 0);
    CHECK((*edited)["netWeight"] == 0);
}

TEST_CASE("A non-finite weight counts as absent", "[BblFilament]")
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    const json   created = fil::create_body(json{{"initial_weight", nan}, {"net_weight", inf}}, {});
    CHECK_FALSE(created.contains("totalNetWeight"));
    CHECK_FALSE(created.contains("netWeight"));
    const auto edited = fil::update_body(current_spool(), json{{"net_weight", nan}, {"note", "x"}}, {});
    REQUIRE(edited.has_value());
    CHECK_FALSE(edited->contains("netWeight"));
    json row           = example_row();
    row["netWeight"]   = nan;
    CHECK(parsed(row).net_weight == 0.0);
}

TEST_CASE("A cloud row with a huge remaining weight shows a full spool", "[BblFilament]")
{
    json row             = example_row();
    row["netWeight"]     = 1e30;
    row["totalNetWeight"] = 1;
    CHECK(fil::spool_to_page(parsed(row))["remain_percent"] == 100);
}

TEST_CASE("A config fetch queued behind a pull does not leave the page syncing", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    std::vector<bool> syncing_at_state_changed;
    rig.svc->events.state_changed = [&] { syncing_at_state_changed.push_back(rig.svc->state().is_syncing); };
    rig.svc->request_pull();
    rig.svc->request_config(true);
    rig.step_worker();
    rig.run_ui(); // the pull is done; only the config job is pending now
    CHECK(rig.worker.size() == 1);
    CHECK_FALSE(rig.svc->state().is_syncing);
    rig.settle();
    REQUIRE_FALSE(syncing_at_state_changed.empty());
    CHECK_FALSE(syncing_at_state_changed.back());
    CHECK(rig.cloud->count("config") == 1);
}

TEST_CASE("A reset forgets the sync time and the paging flags of the previous account", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(45);
    rig.cloud->ignore_offset = true;
    rig.svc->request_pull();
    rig.settle();
    REQUIRE(rig.svc->state().last_synced_at > 0);
    REQUIRE(rig.svc->status().paging_suspect);
    rig.svc->reset();
    CHECK(rig.svc->state().last_synced_at == 0);
    CHECK_FALSE(rig.svc->status().paging_suspect);
    CHECK(rig.svc->status().skipped_rows == 0);
}

TEST_CASE("A skipped-row count is cleared by a reset", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    rig.cloud->rows.push_back(json(5));
    rig.svc->request_pull();
    rig.settle();
    REQUIRE(rig.svc->status().skipped_rows == 1);
    rig.svc->reset();
    CHECK(rig.svc->status().skipped_rows == 0);
}

TEST_CASE("A write waiting in the worker when the account changes never reaches the cloud", "[BblFilament]")
{
    Rig rig;
    rig.svc->add(page_spool());
    REQUIRE(rig.worker.size() == 1);
    rig.session.uid = "u2";
    rig.svc->reset();
    rig.events.clear();
    rig.settle();
    CHECK(rig.cloud->log.empty());
    CHECK(rig.events.empty());
}

TEST_CASE("A write waiting in the worker when the service is destroyed never reaches the cloud", "[BblFilament]")
{
    Rig rig;
    rig.svc->add(page_spool());
    REQUIRE(rig.worker.size() == 1);
    rig.events.clear();
    rig.svc.reset();
    rig.settle();
    CHECK(rig.cloud->log.empty());
    CHECK(rig.events.empty());
}

TEST_CASE("A write the worker threw away fails as unavailable and the service goes on", "[BblFilament]")
{
    Rig rig;
    rig.svc->add(page_spool());
    rig.worker.clear(); // the worker forgot the job
    rig.run_ui();
    CHECK(rig.has_event("push_failed create  Unavailable"));
    CHECK(rig.cloud->log.empty());
    CHECK(rig.svc->status().pending_writes == 0);
    rig.events.clear();
    CHECK(rig.svc->add(page_spool()).ok);
    rig.settle();
    CHECK(rig.cloud->count("create") == 1);
    CHECK(rig.has_event("push_done create "));
}

TEST_CASE("A pull the worker threw away sets the unavailable error", "[BblFilament]")
{
    Rig rig;
    rig.svc->request_pull();
    rig.worker.clear();
    rig.run_ui();
    CHECK(rig.svc->state().has_error);
    CHECK(rig.svc->state().error_kind == fil::FailKind::Unavailable);
    CHECK_FALSE(rig.svc->state().is_syncing);
}

TEST_CASE("A completion delivered twice is applied once", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    rig.svc->request_pull();
    rig.step_worker();
    REQUIRE(rig.ui.size() == 1);
    auto copy = rig.ui.front();
    rig.run_ui();
    copy();
    CHECK(rig.count_events("pull_done 2 0") == 1);
    CHECK(rig.cloud->count("list") == 1);
}

TEST_CASE("A service on a real worker is not stuck after the worker drops its queued job", "[BblFilament]")
{
    fil::SpoolWorker worker;
    auto             cloud = std::make_shared<FakeCloud>();
    std::mutex       ui_mutex;
    std::deque<std::function<void()>> ui;

    fil::ServiceDeps deps;
    deps.cloud      = cloud;
    deps.run_async  = [&](std::function<void()> job) { return worker.post(std::move(job)); };
    deps.post_to_ui = [&](std::function<void()> f) {
        std::lock_guard<std::mutex> lock(ui_mutex);
        ui.push_back(std::move(f));
    };
    deps.session = [] { return fil::Session{true, "u1", 1, false}; };
    deps.now     = [] { return 1000LL; };
    fil::SpoolService svc(std::move(deps));

    std::promise<void> started, release;
    auto               started_f = started.get_future();
    auto               release_f = release.get_future().share();
    worker.post([&] {
        started.set_value();
        release_f.wait();
    });
    REQUIRE(ready(started_f));
    CHECK(svc.add(page_spool()).ok); // queued behind the blocked job
    CHECK(svc.status().pending_writes == 1);

    std::thread helper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        release.set_value();
    });
    worker.drain();
    helper.join();

    std::deque<std::function<void()>> closures;
    {
        std::lock_guard<std::mutex> lock(ui_mutex);
        closures.swap(ui);
    }
    for (auto& f : closures) f();
    CHECK(svc.status().pending_writes == 0);
    CHECK_FALSE(svc.state().is_syncing);
    CHECK(cloud->log.empty());
}

TEST_CASE("A resume during a drain waits for the drain and takes effect after it", "[BblFilament]")
{
    fil::SpoolWorker   worker;
    std::promise<void> started, release, ran;
    auto               started_f = started.get_future();
    auto               release_f = release.get_future().share();
    auto               ran_f     = ran.get_future();
    worker.post([&] {
        started.set_value();
        release_f.wait();
    });
    REQUIRE(ready(started_f));

    std::thread drainer([&] { worker.drain(); });
    for (int i = 0; i < 400 && worker.accepting(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    std::thread resumer([&] { worker.resume(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    const bool accepting_during = worker.accepting();
    const bool posted_during    = worker.post([] {});
    release.set_value();
    drainer.join();
    resumer.join();

    CHECK_FALSE(accepting_during);
    CHECK_FALSE(posted_during);
    CHECK(worker.accepting());
    CHECK(worker.post([&] { ran.set_value(); }));
    CHECK(ready(ran_f));
}

TEST_CASE("After an edit ends unknown the later queued edits of that spool are dropped", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    rig.svc->request_pull();
    rig.settle();
    rig.cloud->log.clear();
    rig.events.clear();
    rig.cloud->forced["update"] = {forced_error(fil::CloudError::Unknown)};
    rig.svc->update(json{{"spool_id", "1000001"}, {"note", "a"}});
    rig.svc->update(json{{"spool_id", "1000001"}, {"note", "b"}});
    rig.svc->update(json{{"spool_id", "1000002"}, {"note", "c"}});
    rig.settle();
    CHECK(rig.cloud->log == std::vector<std::string>{"update 1000001", "update 1000002", "list 0"});
    CHECK(rig.count_events("push_failed update 1000001 Unknown") == 2);
    CHECK(rig.has_event("push_done update 1000002"));
}

TEST_CASE("A list reply that is not JSON fails the list and the service keeps its spools", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(3);
    rig.svc->request_pull();
    rig.settle();
    const std::string raw = GENERATE(std::string(""), std::string("<html>"));
    rig.cloud->forced["list"] = {fil::classify_reply(0, raw)};
    rig.svc->request_pull();
    rig.settle();
    CHECK(rig.svc->view().size() == 3);
    CHECK(rig.svc->state().has_error);
    CHECK(rig.svc->state().error_message == "unexpected response");

    FakeCloud cloud;
    cloud.forced["list"] = {fil::classify_reply(0, raw)};
    const auto r         = fil::list_all(cloud, {});
    CHECK_FALSE(r.ok);
    CHECK(r.failure.error == fil::CloudError::Unknown);
}

TEST_CASE("A config reply that is not JSON keeps the cache and fires nothing", "[BblFilament]")
{
    Rig rig;
    rig.cloud->forced["config"] = {fil::classify_reply(0, "")};
    rig.svc->request_config(false);
    rig.settle();
    CHECK(rig.svc->cached_config() == nullptr);
    CHECK_FALSE(rig.has_event("config_fetched"));

    rig.svc->request_config(true);
    rig.settle();
    REQUIRE(rig.svc->cached_config() != nullptr);
    rig.cloud->forced["config"] = {fil::classify_reply(0, "<html>")};
    rig.svc->request_config(true);
    rig.settle();
    REQUIRE(rig.svc->cached_config() != nullptr);
    CHECK(rig.svc->cached_config()->contains("filamentColors"));
    CHECK(rig.count_events("config_fetched") == 1);
}

TEST_CASE("An event handler that destroys the service does not crash and no later event fires", "[BblFilament]")
{
    Rig rig;
    rig.svc->events.push_done = [&](const std::string&, const std::string&) {
        rig.events.push_back("destroying");
        rig.svc.reset();
    };
    rig.svc->add(page_spool());
    rig.step_worker();
    rig.run_ui();
    CHECK(rig.svc.get() == nullptr);
    CHECK(rig.events.back() == "destroying");
    rig.settle();
    CHECK(rig.events.back() == "destroying");
    CHECK(rig.cloud->count("list") == 0);
}

TEST_CASE("An event handler that replaces the events does not crash", "[BblFilament]")
{
    Rig rig;
    rig.svc->events.push_done = [&](const std::string&, const std::string&) {
        rig.events.push_back("replacing");
        rig.svc->events = {};
    };
    rig.svc->add(page_spool());
    rig.settle();
    CHECK(rig.has_event("replacing"));
    CHECK(rig.events.back() == "replacing");
    CHECK(rig.cloud->count("list") == 1);
}

TEST_CASE("A pull_done handler may request another pull", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(3);
    int calls                  = 0;
    rig.svc->events.pull_done = [&](int, int) {
        if (++calls == 1) rig.svc->request_pull();
    };
    rig.svc->request_pull();
    rig.settle();
    CHECK(calls == 2);
    CHECK(rig.cloud->count("list") == 2);
    CHECK(rig.svc->view().size() == 3);
}
