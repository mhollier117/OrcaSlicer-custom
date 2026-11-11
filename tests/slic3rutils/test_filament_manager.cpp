#include <catch2/catch_test_macros.hpp>

#include "bbl_filament_test_utils.hpp"

#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/FilamentManager/FmColorCodes.hpp"
#include "slic3r/GUI/FilamentManager/FmDevices.hpp"
#include "slic3r/GUI/FilamentManager/FmPresets.hpp"
#include "slic3r/GUI/FilamentManager/FmProtocol.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fm  = Slic3r::GUI::Fm;
namespace fil = Slic3r::BblFilament;
using fm::json;
using namespace BblFilamentTest;

namespace {

std::string request_text(const std::string& head_extra = "", const std::string& body = R"({"module":"filament","submod":"spool","action":"list","payload":{"a":1}})")
{
    return R"({"head":{"version":"1.0","type":"request","seq":5)" + head_extra + R"(},"body":)" + body + "}";
}

const char* const k_color_table = R"({"data":[
 {"fila_id":"GFA00","fila_type":"PLA Basic","fila_color_code":"10300","color_code":"A0","fila_color_type":"\u5355\u8272","fila_color":["#FF6A13FF"],
  "fila_color_name":{"en":"Orange","de":"Orange-de","fr":"Orange-fr"}},
 {"fila_id":"GFA00","fila_type":"PLA Basic","fila_color_code":"10101","color_code":"K0","fila_color_type":"\u5355\u8272","fila_color":["#000000FF"],
  "fila_color_name":{"en":"Black"}},
 {"fila_id":"GFA00","fila_type":"PLA Basic","fila_color_code":"10900","color_code":"M0","fila_color_type":"\u591a\u62fc\u8272","fila_color":["#FF0000FF","#00FF00FF"],
  "fila_color_name":{"en":"Red Green"}},
 {"fila_id":"GFB00","fila_type":"ABS","fila_color_code":"20000","color_code":"G1","fila_color_type":"\u6e10\u53d8\u8272","fila_color":["#111111FF","#222222FF"],
  "fila_color_name":{"zh":"Verlauf"}},
 {"fila_id":"GFC00","fila_type":"PETG","fila_color_code":"30000","color_code":"C0","fila_color_type":"\u5355\u8272","fila_color":[]},
 {"fila_color_code":"40000","color_code":"X0","fila_color_type":"\u5355\u8272","fila_color":["#123456FF"]}
]})";

const fm::ColorCodes& fixture_colors()
{
    static const fm::ColorCodes table = fm::ColorCodes::parse(k_color_table);
    return table;
}

std::unique_ptr<Slic3r::PresetCollection> make_filament_collection()
{
    const auto& defaults = static_cast<const Slic3r::PrintRegionConfig&>(Slic3r::FullPrintConfig::defaults());
    return std::make_unique<Slic3r::PresetCollection>(Slic3r::Preset::TYPE_FILAMENT, Slic3r::Preset::filament_options(), defaults);
}

struct FilamentSpec
{
    std::string name;
    std::string filament_id;
    std::string vendor;
    std::string type;
    bool        is_system{true};
    std::string inherits;
    bool        is_support{false};
};

void add_filament(Slic3r::PresetCollection& c, const FilamentSpec& spec)
{
    Slic3r::DynamicPrintConfig config(c.default_preset().config);
    config.option<Slic3r::ConfigOptionString>("inherits", true)->value        = spec.inherits;
    config.option<Slic3r::ConfigOptionStrings>("filament_vendor", true)->values = {spec.vendor};
    config.option<Slic3r::ConfigOptionStrings>("filament_type", true)->values   = {spec.type};
    config.option<Slic3r::ConfigOptionBools>("filament_is_support", true)->values = {spec.is_support};
    Slic3r::Preset& p = c.load_preset(std::string(), spec.name, config, false);
    p.is_system       = spec.is_system;
    p.filament_id     = spec.filament_id;
    p.alias           = spec.name;
}

fm::ToBambuId test_to_bambu()
{
    return [](const std::string& id) {
        static const std::map<std::string, std::string> map{{"OFAAA", "GFA00"}, {"OFSUP", "GFS00"}};
        auto it = map.find(id);
        return it == map.end() ? id : it->second;
    };
}

const json* find_vendor(const json& tree, const std::string& name)
{
    for (const json& v : tree["vendors"])
        if (v["name"] == name)
            return &v;
    return nullptr;
}

const json* find_type(const json& vendor, const std::string& name)
{
    for (const json& t : vendor["types"])
        if (t["name"] == name)
            return &t;
    return nullptr;
}

std::vector<std::string> item_ids(const json& type)
{
    std::vector<std::string> out;
    for (const json& i : type["items"])
        out.push_back(i["filament_id"].get<std::string>());
    return out;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// FmProtocol

TEST_CASE("A valid request packet is parsed", "[FilamentManager]")
{
    auto r = fm::parse_request(request_text());
    REQUIRE(r.has_value());
    CHECK(r->seq == 5);
    CHECK(r->module == "filament");
    CHECK(r->submod == "spool");
    CHECK(r->action == "list");
    CHECK(r->payload == json{{"a", 1}});
}

TEST_CASE("A request without a payload object gets an empty payload", "[FilamentManager]")
{
    auto missing = fm::parse_request(request_text("", R"({"module":"filament","submod":"a","action":"b"})"));
    REQUIRE(missing.has_value());
    CHECK(missing->payload == json::object());
    auto text = fm::parse_request(request_text("", R"({"module":"filament","submod":"a","action":"b","payload":"x"})"));
    REQUIRE(text.has_value());
    CHECK(text->payload == json::object());
}

TEST_CASE("Body fields that are not strings read as empty", "[FilamentManager]")
{
    auto r = fm::parse_request(request_text("", R"({"module":3,"submod":null,"action":"b"})"));
    REQUIRE(r.has_value());
    CHECK(r->module.empty());
    CHECK(r->submod.empty());
    CHECK(r->action == "b");
}

TEST_CASE("A packet with a wrong version, type or seq is refused", "[FilamentManager]")
{
    CHECK_FALSE(fm::parse_request(R"({"head":{"version":"2.0","type":"request","seq":1},"body":{}})").has_value());
    CHECK_FALSE(fm::parse_request(R"({"head":{"version":"1.0","type":"report","seq":1},"body":{}})").has_value());
    CHECK_FALSE(fm::parse_request(R"({"head":{"version":"1.0","type":"request","seq":-1},"body":{}})").has_value());
    CHECK_FALSE(fm::parse_request(R"({"head":{"version":"1.0","type":"request","seq":"1"},"body":{}})").has_value());
    CHECK_FALSE(fm::parse_request(R"({"head":{"version":"1.0","type":"request"},"body":{}})").has_value());
}

TEST_CASE("A packet without a body or that is not an object is refused", "[FilamentManager]")
{
    CHECK_FALSE(fm::parse_request(R"({"head":{"version":"1.0","type":"request","seq":1}})").has_value());
    CHECK_FALSE(fm::parse_request(R"({"head":{"version":"1.0","type":"request","seq":1},"body":[]})").has_value());
    CHECK_FALSE(fm::parse_request("not json").has_value());
    CHECK_FALSE(fm::parse_request("[1,2]").has_value());
    CHECK_FALSE(fm::parse_request("").has_value());
}

TEST_CASE("A response body has the six keys and a null payload becomes an object", "[FilamentManager]")
{
    json b = fm::make_body("filament", "spool", "list", 3, "bad", nullptr);
    CHECK(b["module"] == "filament");
    CHECK(b["submod"] == "spool");
    CHECK(b["action"] == "list");
    CHECK(b["error_code"] == 3);
    CHECK(b["message"] == "bad");
    CHECK(b["payload"] == json::object());
    CHECK(b.size() == 6);
}

TEST_CASE("The push script wraps the packet for one generation", "[FilamentManager]")
{
    const std::string s = fm::push_script("report", 9, 1234, json{{"k", "v"}}, "7");
    CHECK(s.rfind("(function(p,g){", 0) == 0);
    CHECK(s.size() > 8);
    CHECK(s.substr(s.size() - 6) == ",\"7\");");
    CHECK(s.find("\"event\":\"device\"") != std::string::npos);
    CHECK(s.find("\"type\":\"report\"") != std::string::npos);
    CHECK(s.find("\"seq\":9") != std::string::npos);
    CHECK(s.find("\"ts\":1234") != std::string::npos);
}

TEST_CASE("The push script writes non ASCII text as escapes", "[FilamentManager]")
{
    const std::string s = fm::push_script("response", 1, 2, json{{"t", "\xC3\xBC\xE2\x80\xA8"}}, "7");
    CHECK(s.find("\\u00fc") != std::string::npos);
    CHECK(s.find("\\u2028") != std::string::npos);
    CHECK(std::all_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x80; }));
}

TEST_CASE("A generation that is not digits becomes zero in the push script", "[FilamentManager]")
{
    const std::string s = fm::push_script("response", 1, 2, json::object(), "x\"y");
    CHECK(s.substr(s.size() - 6) == ",\"0\");");
    const std::string empty = fm::push_script("response", 1, 2, json::object(), "");
    CHECK(empty.substr(empty.size() - 6) == ",\"0\");");
    const std::string too_long = fm::push_script("response", 1, 2, json::object(), std::string(21, '1'));
    CHECK(too_long.substr(too_long.size() - 6) == ",\"0\");");
}

TEST_CASE("The generation is read from the page URL", "[FilamentManager]")
{
    auto g = fm::generation_of_url("file:///x/index.html?lang=de_DE&theme=dark&gen=12#/filament_manager");
    REQUIRE(g.has_value());
    CHECK(*g == "12");
    CHECK_FALSE(fm::generation_of_url("file:///x/index.html?lang=de&theme=dark#/filament_manager").has_value());
    CHECK_FALSE(fm::generation_of_url("file:///x/index.html?gen=abc#/f").has_value());
    CHECK_FALSE(fm::generation_of_url("file:///x/index.html#/f?gen=5").has_value());
}

TEST_CASE("The page URL carries language, theme and generation", "[FilamentManager]")
{
    CHECK(fm::page_url("file:///x/index.html", "de_DE", true, "3") == "file:///x/index.html?lang=de_DE&theme=dark&gen=3#/filament_manager");
    CHECK(fm::page_url("file:///x/index.html", "", false, "3") == "file:///x/index.html?theme=light&gen=3#/filament_manager");
}

// ---------------------------------------------------------------------------------------------------------------------
// FmColorCodes

TEST_CASE("The colour table keeps usable entries only", "[FilamentManager]")
{
    const fm::ColorCodes& t = fixture_colors();
    CHECK_FALSE(t.empty());
    REQUIRE(t.entries("GFA00") != nullptr);
    CHECK(t.entries("GFA00")->size() == 3);
    REQUIRE(t.entries("GFB00") != nullptr);
    CHECK(t.entries("GFB00")->size() == 1);
    CHECK(t.entries("GFC00") == nullptr);
    CHECK(t.entries("") == nullptr);
    CHECK(t.entries("GFZ99") == nullptr);
}

TEST_CASE("The colour table reads the colour type of an entry", "[FilamentManager]")
{
    const fm::ColorCodes& t = fixture_colors();
    CHECK((*t.entries("GFA00"))[0].color_type == 2);
    CHECK((*t.entries("GFA00"))[2].color_type == 1);
    CHECK((*t.entries("GFB00"))[0].color_type == 0);
    CHECK((*t.entries("GFA00"))[0].colors == std::vector<std::string>{"#FF6A13FF"});
    CHECK((*t.entries("GFA00"))[0].color_code == "A0");
    CHECK((*t.entries("GFA00"))[0].fila_color_code == "10300");
}

TEST_CASE("The first entry of a filament sets its type", "[FilamentManager]")
{
    CHECK(fixture_colors().fila_type("GFA00") == "PLA Basic");
    CHECK(fixture_colors().fila_type("GFB00") == "ABS");
    CHECK(fixture_colors().fila_type("GFZ99").empty());
}

TEST_CASE("A colour is found whatever its spelling", "[FilamentManager]")
{
    const fm::ColorCodes& t = fixture_colors();
    for (const std::string& spelling : {"#ff6a13", "FF6A13FF", "#FF6A13FF"}) {
        const fm::ColorEntry* e = t.find("GFA00", {spelling}, 2);
        REQUIRE(e != nullptr);
        CHECK(e->color_code == "A0");
    }
}

TEST_CASE("Two colours are found in either order", "[FilamentManager]")
{
    const fm::ColorCodes& t = fixture_colors();
    const fm::ColorEntry* a = t.find("GFA00", {"#FF0000FF", "#00FF00FF"}, 1);
    const fm::ColorEntry* b = t.find("GFA00", {"#00ff00", "#ff0000"}, 1);
    REQUIRE(a != nullptr);
    REQUIRE(b != nullptr);
    CHECK(a == b);
    CHECK(a->color_code == "M0");
}

TEST_CASE("A colour does not match under another colour type", "[FilamentManager]")
{
    const fm::ColorCodes& t = fixture_colors();
    CHECK(t.find("GFB00", {"#111111FF", "#222222FF"}, 0) != nullptr);
    CHECK(t.find("GFB00", {"#111111FF", "#222222FF"}, 1) == nullptr);
    CHECK(t.find("GFA00", {"#FF6A13FF"}, 0) == nullptr);
}

TEST_CASE("An unknown filament or colour finds nothing", "[FilamentManager]")
{
    const fm::ColorCodes& t = fixture_colors();
    CHECK(t.find("GFZ99", {"#FF6A13FF"}, 2) == nullptr);
    CHECK(t.find("GFA00", {"#010203FF"}, 2) == nullptr);
    CHECK(t.find("GFA00", {"nonsense"}, 2) == nullptr);
    CHECK(t.find("GFA00", {}, 2) == nullptr);
}

TEST_CASE("A colour name follows the language with a fallback to English", "[FilamentManager]")
{
    const fm::ColorEntry& e = (*fixture_colors().entries("GFA00"))[0];
    CHECK(fm::ColorCodes::name_for(e, "de_DE") == "Orange-de");
    CHECK(fm::ColorCodes::name_for(e, "de") == "Orange-de");
    CHECK(fm::ColorCodes::name_for(e, "xx") == "Orange");
    CHECK(fm::ColorCodes::name_for(e, "") == "Orange");
    const fm::ColorEntry& only_zh = (*fixture_colors().entries("GFB00"))[0];
    CHECK(fm::ColorCodes::name_for(only_zh, "en") == "Verlauf");
    CHECK(fm::ColorCodes::name_for(fm::ColorEntry{}, "en").empty());
}

TEST_CASE("Hex colours are normalised to eight upper case digits", "[FilamentManager]")
{
    CHECK(fm::ColorCodes::normalize_hex("ff6a13") == "#FF6A13FF");
    CHECK(fm::ColorCodes::normalize_hex("#ff6a1380") == "#FF6A1380");
    CHECK(fm::ColorCodes::normalize_hex("#FF6A13FF") == "#FF6A13FF");
    CHECK(fm::ColorCodes::normalize_hex("#FFF").empty());
    CHECK(fm::ColorCodes::normalize_hex("#GG0000").empty());
    CHECK(fm::ColorCodes::normalize_hex("").empty());
    CHECK(fm::ColorCodes::normalize_hex("#FF6A13FF00").empty());
}

TEST_CASE("Garbage text gives an empty colour table", "[FilamentManager]")
{
    CHECK(fm::ColorCodes::parse("garbage").empty());
    CHECK(fm::ColorCodes::parse("[]").empty());
    CHECK(fm::ColorCodes::parse(R"({"data":5})").empty());
    CHECK(fm::ColorCodes::load_file("C:/this/does/not/exist.json").empty());
}

TEST_CASE("The tray colour name is the filament number and the colour code", "[FilamentManager]")
{
    const fm::ColorCodes& t = fixture_colors();
    CHECK(fm::tray_id_name_for(t, "GFA00", {"#000000FF"}, 2) == "A00-K0");
    CHECK(fm::tray_id_name_for(t, "GFA00", {"#010203FF"}, 2).empty());
    CHECK(fm::tray_id_name_for(t, "GF", {"#000000FF"}, 2).empty());
    CHECK(fm::tray_id_name_for(t, "P1234567", {"#000000FF"}, 2).empty());
}

// ---------------------------------------------------------------------------------------------------------------------
// FmPresets

TEST_CASE("A mapped system preset appears with its Bambu id", "[FilamentManager]")
{
    auto c = make_filament_collection();
    add_filament(*c, {"Bambu PLA Basic", "OFAAA", "Bambu Lab", "PLA"});
    json tree = fm::preset_tree(*c, test_to_bambu());
    const json* v = find_vendor(tree, "Bambu Lab");
    REQUIRE(v != nullptr);
    const json* t = find_type(*v, "PLA");
    REQUIRE(t != nullptr);
    REQUIRE(t->at("items").size() == 1);
    const json& item = t->at("items")[0];
    CHECK(item["name"] == "Bambu PLA Basic");
    CHECK(item["series"] == "Bambu PLA Basic");
    CHECK(item["filament_id"] == "GFA00");
    CHECK_FALSE(item.contains("is_user"));
    CHECK_FALSE(item.contains("setting_id"));
    CHECK(t->at("series") == json::array({"Bambu PLA Basic"}));
}

TEST_CASE("A system preset whose id Bambu does not know is left out", "[FilamentManager]")
{
    auto c = make_filament_collection();
    add_filament(*c, {"Other PLA", "OFZZZ", "Acme", "PLA"});
    CHECK(fm::preset_tree(*c, test_to_bambu())["vendors"].empty());
}

TEST_CASE("A system child of a preset is left out", "[FilamentManager]")
{
    auto c = make_filament_collection();
    add_filament(*c, {"Bambu PLA Basic", "OFAAA", "Bambu Lab", "PLA"});
    add_filament(*c, {"Bambu PLA Basic 0.2", "OFAAA", "Bambu Lab", "PLA", true, "Bambu PLA Basic"});
    json tree = fm::preset_tree(*c, test_to_bambu());
    const json* t = find_type(*find_vendor(tree, "Bambu Lab"), "PLA");
    REQUIRE(t != nullptr);
    CHECK(t->at("items").size() == 1);
    CHECK(t->at("items")[0]["name"] == "Bambu PLA Basic");
}

TEST_CASE("A user custom filament appears as a user item", "[FilamentManager]")
{
    auto c = make_filament_collection();
    add_filament(*c, {"My Custom", "P1a2b3c4", "Mine", "PETG", false});
    json tree = fm::preset_tree(*c, test_to_bambu());
    const json* t = find_type(*find_vendor(tree, "Mine"), "PETG");
    REQUIRE(t != nullptr);
    REQUIRE(t->at("items").size() == 1);
    CHECK(t->at("items")[0]["filament_id"] == "P1a2b3c4");
    CHECK(t->at("items")[0]["is_user"] == true);
}

TEST_CASE("A user copy of a mapped system preset appears with the Bambu id", "[FilamentManager]")
{
    auto c = make_filament_collection();
    add_filament(*c, {"Bambu PLA Basic", "OFAAA", "Bambu Lab", "PLA"});
    add_filament(*c, {"Bambu PLA Basic copy", "OFAAA", "Bambu Lab", "PLA", false, "Bambu PLA Basic"});
    json tree = fm::preset_tree(*c, test_to_bambu());
    const json* t = find_type(*find_vendor(tree, "Bambu Lab"), "PLA");
    REQUIRE(t != nullptr);
    CHECK(t->at("items").size() == 2);
    CHECK(item_ids(*t) == std::vector<std::string>{"GFA00", "GFA00"});
    int users = 0;
    for (const json& i : t->at("items"))
        users += i.contains("is_user") ? 1 : 0;
    CHECK(users == 1);
}

TEST_CASE("A preset without a vendor is left out", "[FilamentManager]")
{
    auto c = make_filament_collection();
    add_filament(*c, {"No Vendor", "OFAAA", "", "PLA"});
    CHECK(fm::preset_tree(*c, test_to_bambu())["vendors"].empty());
}

TEST_CASE("A preset with an empty type lands under Other", "[FilamentManager]")
{
    auto c = make_filament_collection();
    add_filament(*c, {"Typeless", "OFAAA", "Bambu Lab", ""});
    json tree = fm::preset_tree(*c, test_to_bambu());
    REQUIRE(find_vendor(tree, "Bambu Lab") != nullptr);
    CHECK(find_type(*find_vendor(tree, "Bambu Lab"), "Other") != nullptr);
}

TEST_CASE("Two system presets with one Bambu id give one item", "[FilamentManager]")
{
    auto c = make_filament_collection();
    add_filament(*c, {"Bambu PLA Basic", "OFAAA", "Bambu Lab", "PLA"});
    add_filament(*c, {"Bambu PLA Basic B", "GFA00", "Bambu Lab", "PLA"});
    json tree = fm::preset_tree(*c, test_to_bambu());
    CHECK(find_type(*find_vendor(tree, "Bambu Lab"), "PLA")->at("items").size() == 1);
}

TEST_CASE("Vendors and types are sorted by name", "[FilamentManager]")
{
    auto c = make_filament_collection();
    add_filament(*c, {"P1", "P0000001", "Zeta", "TPU", false});
    add_filament(*c, {"P2", "P0000002", "Alpha", "PLA", false});
    add_filament(*c, {"P3", "P0000003", "Alpha", "ABS", false});
    json tree = fm::preset_tree(*c, test_to_bambu());
    REQUIRE(tree["vendors"].size() == 2);
    CHECK(tree["vendors"][0]["name"] == "Alpha");
    CHECK(tree["vendors"][1]["name"] == "Zeta");
    CHECK(tree["vendors"][0]["types"][0]["name"] == "ABS");
    CHECK(tree["vendors"][0]["types"][1]["name"] == "PLA");
}

TEST_CASE("The filament lookup answers by Bambu id", "[FilamentManager]")
{
    auto c = make_filament_collection();
    add_filament(*c, {"Bambu Support", "OFSUP", "Bambu Lab", "PLA-S", true, "", true});
    add_filament(*c, {"Bambu PLA Basic", "OFAAA", "Bambu Lab", "PLA"});
    fil::FilamentLookup l = fm::filament_lookup(c.get(), test_to_bambu(), &fixture_colors());
    REQUIRE(l.is_support);
    REQUIRE(l.filament_type);
    REQUIRE(l.tray_id_name);
    CHECK(l.is_support("GFS00") == std::optional<bool>(true));
    CHECK(l.is_support("GFA00") == std::optional<bool>(false));
    CHECK(l.filament_type("GFS00") == "PLA-S");
    CHECK(l.filament_type("GFA00") == "PLA");
    CHECK(l.tray_id_name("GFA00", {"#000000FF"}, 2) == "A00-K0");
}

TEST_CASE("The filament lookup knows nothing of an unknown id", "[FilamentManager]")
{
    auto c = make_filament_collection();
    add_filament(*c, {"Bambu PLA Basic", "OFAAA", "Bambu Lab", "PLA"});
    fil::FilamentLookup l = fm::filament_lookup(c.get(), test_to_bambu(), &fixture_colors());
    CHECK_FALSE(l.is_support("GFZ99").has_value());
    CHECK(l.filament_type("GFZ99").empty());
}

TEST_CASE("The filament lookup leaves functions unset for null inputs", "[FilamentManager]")
{
    auto c = make_filament_collection();
    fil::FilamentLookup no_presets = fm::filament_lookup(nullptr, test_to_bambu(), &fixture_colors());
    CHECK_FALSE(static_cast<bool>(no_presets.is_support));
    CHECK_FALSE(static_cast<bool>(no_presets.filament_type));
    CHECK(static_cast<bool>(no_presets.tray_id_name));
    fil::FilamentLookup no_colors = fm::filament_lookup(c.get(), test_to_bambu(), nullptr);
    CHECK(static_cast<bool>(no_colors.is_support));
    CHECK(static_cast<bool>(no_colors.filament_type));
    CHECK_FALSE(static_cast<bool>(no_colors.tray_id_name));
}

// ---------------------------------------------------------------------------------------------------------------------
// FmDevices

TEST_CASE("Tray labels name the unit and the slot", "[FilamentManager]")
{
    CHECK(fm::tray_label(0, 0) == "A1");
    CHECK(fm::tray_label(1, 3) == "B4");
    CHECK(fm::tray_label(128, 0) == "HT-A");
    CHECK(fm::tray_label(153, 0) == "HT-Z");
    CHECK(fm::tray_label(-1, 0).empty());
    CHECK(fm::tray_label(0, -1).empty());
    CHECK(fm::tray_label(200, 0).empty());
}

namespace {

fm::TrayContext tray_context()
{
    fm::TrayContext ctx;
    ctx.colors    = &fixture_colors();
    ctx.lang      = "de_DE";
    ctx.to_bambu  = test_to_bambu();
    return ctx;
}

Slic3r::DevAmsTray official_tray()
{
    Slic3r::DevAmsTray t("1");
    t.tag_uid    = "0123456789AB0100";
    t.uuid       = "UUID-1";
    t.setting_id = "OFAAA";
    t.m_fila_type = "PLA";
    t.sub_brands = "PLA Basic";
    t.color      = "FF6A13FF";
    t.weight     = "1000";
    t.remain     = 40;
    t.diameter   = "1.75";
    t.is_bbl     = true;
    t.ctype      = 2;
    t.is_exists  = true;
    return t;
}

} // namespace

TEST_CASE("An official tray carries its tag, translated id and colour name", "[FilamentManager]")
{
    json j = fm::tray_json(official_tray(), "1", "A2", true, tray_context());
    CHECK(j["slot_id"] == "1");
    CHECK(j["tray_label"] == "A2");
    CHECK(j["is_exists"] == true);
    CHECK(j["is_unknown"] == false);
    CHECK(j["tag_uid"] == "UUID-1");
    CHECK(j["tray_id_name"] == "");
    CHECK(j["setting_id"] == "GFA00");
    CHECK(j["fila_type"] == "PLA");
    CHECK(j["sub_brands"] == "PLA Basic");
    CHECK(j["color"] == "#FF6A13FF");
    CHECK(j["colors"] == json::array({"#FF6A13FF"}));
    CHECK(j["color_type"] == 2);
    CHECK(j["color_name"] == "Orange-de");
    CHECK(j["fila_color_code"] == "10300");
    CHECK(j["remain_g"] == -1);
    CHECK(j["is_bbl"] == true);
    CHECK(j["diameter"] == "1.75");
}

TEST_CASE("A third party tray has no tag uid", "[FilamentManager]")
{
    Slic3r::DevAmsTray t = official_tray();
    t.tag_uid            = "0000000000000000";
    CHECK(fm::tray_json(t, "1", "A2", true, tray_context())["tag_uid"] == "");
    t.tag_uid = "0123456789AB01";
    CHECK(fm::tray_json(t, "1", "A2", true, tray_context())["tag_uid"] == "");
}

TEST_CASE("The remaining weight follows the weight and the percentage", "[FilamentManager]")
{
    Slic3r::DevAmsTray t = official_tray();
    CHECK(fm::tray_json(t, "1", "A2", true, tray_context())["remain_weight"] == 400);
    t.weight = "";
    CHECK(fm::tray_json(t, "1", "A2", true, tray_context())["remain_weight"].is_null());
    t.weight = "1000";
    t.remain = -1;
    CHECK(fm::tray_json(t, "1", "A2", true, tray_context())["remain_weight"].is_null());
    t.remain = 101;
    CHECK(fm::tray_json(t, "1", "A2", true, tray_context())["remain_weight"].is_null());
    t.remain = 40;
    t.weight = "abc";
    CHECK(fm::tray_json(t, "1", "A2", true, tray_context())["remain_weight"].is_null());
}

TEST_CASE("A tray with two colours reports both and its colour type", "[FilamentManager]")
{
    Slic3r::DevAmsTray t = official_tray();
    t.cols               = {"FF0000FF", "", "00FF00FF"};
    t.ctype              = 1;
    json j               = fm::tray_json(t, "1", "A2", true, tray_context());
    CHECK(j["colors"] == json::array({"#FF0000FF", "#00FF00FF"}));
    CHECK(j["color_type"] == 1);
    CHECK(j["color_name"] == "Red Green");
    t.ctype = 7;
    CHECK(fm::tray_json(t, "1", "A2", true, tray_context())["color_type"] == 1);
}

TEST_CASE("A tray without a colour reports no colours", "[FilamentManager]")
{
    Slic3r::DevAmsTray t = official_tray();
    t.color.clear();
    json j = fm::tray_json(t, "1", "A2", true, tray_context());
    CHECK(j["color"] == "");
    CHECK(j["colors"] == json::array());
    CHECK(j["is_unknown"] == true);
    CHECK_FALSE(j.contains("color_name"));
}

TEST_CASE("An absent tray has only the three base keys", "[FilamentManager]")
{
    json j = fm::tray_json(official_tray(), "2", "A3", false, tray_context());
    CHECK(j.size() == 3);
    CHECK(j["slot_id"] == "2");
    CHECK(j["tray_label"] == "A3");
    CHECK(j["is_exists"] == false);
}

TEST_CASE("A device manager that is missing gives empty machine and AMS data", "[FilamentManager]")
{
    CHECK(fm::machine_list(nullptr) == json{{"machines", json::array()}, {"selected_dev_id", ""}});
    CHECK(fm::ams_data(nullptr, tray_context())["ams_units"].empty());
    std::string used;
    CHECK_FALSE(fm::request_pushall(nullptr, "", used));
    fm::select_machine(nullptr, "x");
}
