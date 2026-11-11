#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/BblFilament/Spool.hpp"
#include "slic3r/Utils/BblFilament/SpoolCloud.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace fil = Slic3r::BblFilament;
using fil::json;

namespace {

json example_row(long long id = 1000001, const std::string& name = "PLA Basic")
{
    return json::parse(R"({"id":1,"createType":"manual","filamentVendor":"Bambu Lab","filamentType":"PLA","filamentName":"PLA Basic",
 "filamentId":"GFA00","RFID":"","color":"#FFFFFFFF","colorType":2,"colors":null,"netWeight":750,"totalNetWeight":1000,
 "note":"","createdAt":1700000000,"updatedAt":1700000000,"status":0,"isSupport":false,"trayIdName":"","category":"PLA",
 "inPrinter":false,"devId":"","amsSn":"","slotId":"","amsId":0,"amsType":0,"deviceName":""})");
}

json row_with(long long id, const std::string& name = "PLA Basic")
{
    json r         = example_row();
    r["id"]        = id;
    r["filamentName"] = name;
    return r;
}

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

// A cloud that holds rows in memory and logs every call.
class FakeCloud : public fil::ISpoolCloud
{
public:
    std::vector<json>        rows;
    std::vector<std::string> log;
    std::vector<json>        bodies; // create and update bodies, in order
    json                     config_body = json{{"filamentColors", json::array()}};
    long long                next_id     = 2000001;

    bool ignore_offset = false;
    bool null_hits     = false;
    bool omit_hits     = false;
    bool with_total    = false;

    // Forced replies for the next calls of a kind; nullopt lets that call run normally.
    std::map<std::string, std::deque<std::optional<fil::CloudReply>>> forced;

    void fill(int count, long long first_id = 1000001)
    {
        for (int i = 0; i < count; ++i) rows.push_back(row_with(first_id + i, "Spool " + std::to_string(first_id + i)));
    }

    fil::CloudReply list_page(int offset, int limit) override
    {
        log.push_back("list " + std::to_string(offset));
        if (auto f = take("list")) return *f;
        if (ignore_offset) offset = 0;
        json hits = json::array();
        for (int i = offset; i < static_cast<int>(rows.size()) && i < offset + limit; ++i) hits.push_back(rows[i]);
        json body = json::object();
        if (hits.empty() && null_hits) body["hits"] = nullptr;
        else if (hits.empty() && omit_hits) {
        } else body["hits"] = hits;
        if (with_total) body["total"] = static_cast<int>(rows.size());
        return ok(body);
    }

    fil::CloudReply create(const json& body) override
    {
        log.push_back("create");
        bodies.push_back(body);
        if (auto f = take("create")) return *f;
        json row = body;
        row["id"] = next_id++;
        rows.push_back(row);
        return ok(json::object());
    }

    fil::CloudReply update(const std::string& id, const json& body) override
    {
        log.push_back("update " + id);
        bodies.push_back(body);
        if (auto f = take("update")) return *f;
        for (json& row : rows) {
            if (row["id"].dump() == id) {
                for (auto it = body.begin(); it != body.end(); ++it)
                    if (it.key() != "id") row[it.key()] = it.value();
                return ok(json{{"filamentV2", row}});
            }
        }
        return fil::classify_reply(-29, R"({"code":404,"error":"not found"})");
    }

    fil::CloudReply remove(const std::vector<std::string>& ids) override
    {
        std::string joined;
        for (const auto& id : ids) joined += (joined.empty() ? "" : ",") + id;
        log.push_back("remove " + joined);
        if (auto f = take("remove")) return *f;
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const json& row) { return std::find(ids.begin(), ids.end(), row["id"].dump()) != ids.end(); }),
                   rows.end());
        return ok(json::object());
    }

    fil::CloudReply config() override
    {
        log.push_back("config");
        if (auto f = take("config")) return *f;
        return ok(config_body);
    }

    static fil::CloudReply ok(json body)
    {
        fil::CloudReply r;
        r.body = std::move(body);
        return r;
    }

    int count(const std::string& prefix) const
    {
        return static_cast<int>(std::count_if(log.begin(), log.end(), [&](const std::string& l) { return l.rfind(prefix, 0) == 0; }));
    }

private:
    std::optional<fil::CloudReply> take(const std::string& kind)
    {
        auto it = forced.find(kind);
        if (it == forced.end() || it->second.empty()) return std::nullopt;
        auto f = it->second.front();
        it->second.pop_front();
        return f;
    }
};

fil::CloudReply forced_error(fil::CloudError e, const std::string& message = "")
{
    fil::CloudReply r;
    r.error   = e;
    r.rc      = -28;
    r.message = message;
    return r;
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
    const auto b = fil::update_body(current_spool(), json{{"color_code", "#00FF00FF"}}, full_lookup());
    REQUIRE(b.has_value());
    CHECK((*b)["trayIdName"] == "T:GFA00:1:2");
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
    const auto e = fil::classify_reply(0, "");
    CHECK(e.ok());
    CHECK(e.body.is_object());
    const auto h = fil::classify_reply(0, R"({"hits":[]})");
    CHECK(h.body["hits"].is_array());
    const auto bad = fil::classify_reply(0, "<html>");
    CHECK(bad.ok());
    CHECK(bad.body.is_object());
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
