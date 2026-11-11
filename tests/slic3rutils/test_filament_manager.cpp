#include <catch2/catch_test_macros.hpp>

#include "bbl_filament_test_utils.hpp"

#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/FilamentManager/FmColorCodes.hpp"
#include "slic3r/GUI/FilamentManager/FmDevices.hpp"
#include "slic3r/GUI/FilamentManager/FmPresets.hpp"
#include "slic3r/GUI/FilamentManager/FmPrinters.hpp"
#include "slic3r/GUI/FilamentManager/FmProtocol.hpp"
#include "slic3r/GUI/FilamentManager/FmRuntime.hpp"
#include "slic3r/GUI/FilamentManager/FmSpoolPicker.hpp"
#include "slic3r/GUI/FilamentManager/FmViewModel.hpp"
#include "slic3r/Utils/BblFilament/LinkDriver.hpp"
#include "slic3r/Utils/BblFilament/Linker.hpp"
#include "slic3r/Utils/BblFilament/SpoolCloud.hpp"

#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
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

TEST_CASE("Only the Filament Manager page counts as the page URL", "[FilamentManager]")
{
    CHECK(fm::is_page_url("file:///C:/x/resources/web/filament_manager/index.html?lang=en_US&theme=dark&gen=1#/filament_manager"));
    CHECK(fm::is_page_url("FILE:///C:/X/Resources/Web/Filament_Manager/Index.html?gen=1"));
    CHECK(fm::is_page_url("file:///C:\\x\\resources\\web\\filament_manager\\index.html#/a"));
    CHECK_FALSE(fm::is_page_url("file:///C:/x/resources/web/homepage/index.html?gen=1"));
    CHECK_FALSE(fm::is_page_url("https://example.com/web/filament_manager/index.html"));
    CHECK_FALSE(fm::is_page_url("file:///x/index.html?u=/web/filament_manager/index.html"));
    CHECK_FALSE(fm::is_page_url(""));
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
    CHECK(t.find("GFA00", {"#FF6A13FF"}, 1) != nullptr);
}

TEST_CASE("A query with one colour is looked up as a single colour", "[FilamentManager]")
{
    const fm::ColorCodes& t = fixture_colors();
    const fm::ColorEntry* e = t.find("GFA00", {"#FF6A13FF"}, 0);
    REQUIRE(e != nullptr);
    CHECK(e->fila_color_code == "10300");
    CHECK(t.find("GFB00", {"#111111FF", "#222222FF"}, 0) != nullptr);
    CHECK(t.find("GFB00", {"#111111FF", "#222222FF"}, 2) == nullptr);
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

TEST_CASE("The page spool of a mapped system preset carries its Bambu id, names and colour", "[FilamentManager]")
{
    auto c = make_filament_collection();
    add_filament(*c, {"Bambu PLA Basic", "OFAAA", "Bambu Lab", "PLA"});
    const json s = fm::page_spool_for_filament(*c, "OFAAA", test_to_bambu(), "ff6a13", {}, 0);
    REQUIRE(s.is_object());
    CHECK(s["brand"] == "Bambu Lab");
    CHECK(s["material_type"] == "PLA");
    CHECK(s["series"] == "Bambu PLA Basic");
    CHECK(s["setting_id"] == "GFA00");
    CHECK(s["color_code"] == "#FF6A13FF");
    CHECK_FALSE(s.contains("colors"));
    CHECK(s["color_type"] == 2);
    CHECK(s["initial_weight"] == 1000);
    CHECK(s["net_weight"] == 1000);
    CHECK(s["spool_weight"] == 0);
    CHECK(s["entry_method"] == "manual");
    CHECK(s["note"] == "");
}

TEST_CASE("The page spool keeps several colours and their colour type, and one colour stays single", "[FilamentManager]")
{
    auto c = make_filament_collection();
    add_filament(*c, {"Bambu PLA Basic", "OFAAA", "Bambu Lab", "PLA"});
    const json multi = fm::page_spool_for_filament(*c, "OFAAA", test_to_bambu(), "FF0000FF", {"FF0000FF", "#00ff00"}, 0);
    REQUIRE(multi.is_object());
    CHECK(multi["colors"] == json::array({"#FF0000FF", "#00FF00FF"}));
    CHECK(multi["color_type"] == 0);
    CHECK(multi["color_code"] == "#FF0000FF");
    const json one = fm::page_spool_for_filament(*c, "OFAAA", test_to_bambu(), "FF0000FF", {"FF0000FF"}, 0);
    REQUIRE(one.is_object());
    CHECK_FALSE(one.contains("colors"));
    CHECK(one["color_type"] == 2);
}

TEST_CASE("The page spool of a user custom filament uses its own id", "[FilamentManager]")
{
    auto c = make_filament_collection();
    add_filament(*c, {"My Custom", "P1a2b3c4", "Mine", "PETG", false});
    const json s = fm::page_spool_for_filament(*c, "P1a2b3c4", test_to_bambu(), "#000000FF", {}, 2);
    REQUIRE(s.is_object());
    CHECK(s["setting_id"] == "P1a2b3c4");
    CHECK(s["brand"] == "Mine");
    CHECK(s["material_type"] == "PETG");
    CHECK(s["series"] == "My Custom");
}

TEST_CASE("The page spool is null when Bambu does not know the filament or the colour is not valid", "[FilamentManager]")
{
    auto c = make_filament_collection();
    add_filament(*c, {"Other PLA", "OFZZZ", "Acme", "PLA"});
    add_filament(*c, {"No Vendor", "OFAAA", "", "PLA"});
    CHECK(fm::page_spool_for_filament(*c, "OFZZZ", test_to_bambu(), "#FFFFFFFF", {}, 2).is_null());
    CHECK(fm::page_spool_for_filament(*c, "OFAAA", test_to_bambu(), "#FFFFFFFF", {}, 2).is_null());
    CHECK(fm::page_spool_for_filament(*c, "", test_to_bambu(), "#FFFFFFFF", {}, 2).is_null());
    CHECK(fm::page_spool_for_filament(*c, "OFNONE", test_to_bambu(), "#FFFFFFFF", {}, 2).is_null());
    add_filament(*c, {"Bambu PLA Basic", "OFAAA", "Bambu Lab", "PLA"});
    CHECK(fm::page_spool_for_filament(*c, "OFAAA", test_to_bambu(), "#FFFFFFFF", {}, 2).is_object());
    CHECK(fm::page_spool_for_filament(*c, "OFAAA", test_to_bambu(), "", {}, 2).is_null());
}

TEST_CASE("The page spool skips a system child and finds the root preset", "[FilamentManager]")
{
    auto c = make_filament_collection();
    add_filament(*c, {"Bambu PLA Basic 0.2", "OFAAA", "Bambu Lab", "PLA", true, "Bambu PLA Basic"});
    add_filament(*c, {"Bambu PLA Basic", "OFAAA", "Bambu Lab", "PLA"});
    const json s = fm::page_spool_for_filament(*c, "OFAAA", test_to_bambu(), "#FFFFFFFF", {}, 2);
    REQUIRE(s.is_object());
    CHECK(s["series"] == "Bambu PLA Basic");
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

TEST_CASE("The filament lookup knows nothing of an empty id", "[FilamentManager]")
{
    auto c = make_filament_collection();
    add_filament(*c, {"No id preset", "", "Bambu Lab", "PLA-S", false, "", true});
    add_filament(*c, {"Bambu PLA Basic", "OFAAA", "Bambu Lab", "PLA"});
    fil::FilamentLookup l = fm::filament_lookup(c.get(), test_to_bambu(), &fixture_colors());
    CHECK_FALSE(l.is_support("").has_value());
    CHECK(l.filament_type("").empty());
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

TEST_CASE("A tray with one colour and colour type zero is a single colour tray", "[FilamentManager]")
{
    Slic3r::DevAmsTray t = official_tray();
    t.ctype              = 0;
    t.cols.clear();
    json j = fm::tray_json(t, "1", "A2", true, tray_context());
    CHECK(j["color_type"] == 2);
    CHECK(j["color_name"] == "Orange-de");
    CHECK(j["fila_color_code"] == "10300");
    t.cols = {"FF0000FF", "00FF00FF"};
    CHECK(fm::tray_json(t, "1", "A2", true, tray_context())["color_type"] == 0);
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

// ---------------------------------------------------------------------------------------------------------------------
// FmViewModel

namespace {

struct VmRig
{
    Rig                              rig;
    fm::Host                         host;
    std::vector<json>                reports;
    std::vector<std::string>         calls;
    bool                             throw_presets{false};
    std::unique_ptr<fm::ViewModel>   vm;

    std::vector<std::string> assigned; // "dev ams slot series" per create_and_assign call

    explicit VmRig(bool with_host = true, bool with_create_and_assign = false)
    {
        if (with_create_and_assign)
            host.create_and_assign = [this](const json& s, const std::string& dev, int ams, const std::string& slot) {
                assigned.push_back(dev + " " + std::to_string(ams) + " " + slot + " " + s.value("series", std::string()));
                return fil::Accept{};
            };
        if (with_host) {
            host.dark    = [] { return true; };
            host.presets = [this] {
                calls.push_back("presets");
                if (throw_presets)
                    throw std::runtime_error("boom");
                return json{{"vendors", json::array({json{{"name", "V"}}})}};
            };
            host.machines = [this] {
                calls.push_back("machines");
                return json{{"machines", json::array()}, {"selected_dev_id", "dev1"}};
            };
            host.ams = [this](const std::string& dev, bool sw) {
                calls.push_back(std::string("ams ") + dev + (sw ? " switch" : ""));
                return json{{"selected_dev_id", "dev1"}, {"ams_units", json::array()}, {"ext_slots", json::array()}, {"is_rack", false}};
            };
            host.request_pushall = [this](const std::string& dev, std::string& used) {
                calls.push_back("pushall " + dev);
                if (dev == "none")
                    return false;
                used = dev.empty() ? "dev1" : dev;
                return true;
            };
            host.create_custom = [this](const std::string& vendor, const std::string& id) { calls.push_back("custom " + vendor + " " + id); };
            host.decorate_spool = [this](json& row) {
                calls.push_back("decorate");
                row["color_name"] = "decorated";
            };
        }
        vm = std::make_unique<fm::ViewModel>(*rig.svc, host, [this](const json& body) { reports.push_back(body); });
        rig.svc->events = vm->event_handlers();
    }

    ~VmRig() { rig.svc->events = {}; }

    json call(const std::string& submod, const std::string& action, json payload = json::object(), const std::string& module = "filament")
    {
        fm::Request r;
        r.module  = module;
        r.submod  = submod;
        r.action  = action;
        r.payload = std::move(payload);
        return vm->handle(r);
    }

    void pull()
    {
        rig.svc->request_pull();
        rig.settle();
    }

    std::vector<json> reports_of(const std::string& submod, const std::string& action) const
    {
        std::vector<json> out;
        for (const json& r : reports)
            if (r["submod"] == submod && r["action"] == action)
                out.push_back(r);
        return out;
    }

    int count_calls(const std::string& text) const { return static_cast<int>(std::count(calls.begin(), calls.end(), text)); }
};

} // namespace

TEST_CASE("The init answer carries theme, spools, presets, sync state and no debug", "[FilamentManager]")
{
    VmRig t;
    t.rig.cloud->fill(2);
    t.pull();
    json a = t.call("init", "init");
    CHECK(a["error_code"] == 0);
    CHECK(a["payload"]["theme"] == "dark");
    CHECK(a["payload"]["spools"].size() == 2);
    CHECK(a["payload"]["presets"]["vendors"].size() == 1);
    CHECK(a["payload"]["cloud_sync"]["logged_in"] == true);
    CHECK(a["payload"]["debug_enabled"] == false);
}

TEST_CASE("The init answer when signed out has no spools and is not logged in", "[FilamentManager]")
{
    VmRig t(false);
    t.rig.session.signed_in = false;
    json a                  = t.call("init", "init");
    CHECK(a["payload"]["theme"] == "light");
    CHECK(a["payload"]["spools"].empty());
    CHECK(a["payload"]["presets"] == json{{"vendors", json::array()}});
    CHECK(a["payload"]["cloud_sync"]["logged_in"] == false);
}

TEST_CASE("A spool list runs the host decorator on every row", "[FilamentManager]")
{
    VmRig t;
    t.rig.cloud->fill(3);
    t.pull();
    t.calls.clear();
    json a = t.call("spool", "list");
    CHECK(a["error_code"] == 0);
    REQUIRE(a["payload"].size() == 3);
    CHECK(t.count_calls("decorate") == 3);
    CHECK(a["payload"][0]["color_name"] == "decorated");
}

TEST_CASE("An added spool is created in the cloud", "[FilamentManager]")
{
    VmRig t;
    json  a = t.call("spool", "add", page_spool());
    CHECK(a["error_code"] == 0);
    t.rig.settle();
    CHECK(t.rig.cloud->count("create") == 1);
}

TEST_CASE("A batch add with a quantity of three creates three spools", "[FilamentManager]")
{
    VmRig t;
    json  a = t.call("spool", "batch_add", json{{"spool", page_spool()}, {"quantity", 3}});
    CHECK(a["error_code"] == 0);
    t.rig.settle();
    CHECK(t.rig.cloud->count("create") == 3);
}

TEST_CASE("A batch add with a bad quantity is refused and queues nothing", "[FilamentManager]")
{
    const json bad[] = {json("3"), json(1.5), json(0), json(-1), json(100), json(5000000000LL)};
    for (const json& q : bad) {
        VmRig t;
        json  a = t.call("spool", "batch_add", json{{"spool", page_spool()}, {"quantity", q}});
        CHECK(a["error_code"] == 3);
        t.rig.settle();
        CHECK(t.rig.cloud->count("create") == 0);
    }
}

TEST_CASE("A batch add without a quantity creates one spool", "[FilamentManager]")
{
    VmRig t;
    t.call("spool", "batch_add", json{{"spool", page_spool()}});
    t.rig.settle();
    CHECK(t.rig.cloud->count("create") == 1);
}

TEST_CASE("A batch create runs its creates and updates", "[FilamentManager]")
{
    VmRig t;
    t.rig.cloud->fill(1);
    t.pull();
    json a = t.call("spool", "batch_create", json{{"creates", json::array({page_spool("A")})}, {"updates", json::array({json{{"spool_id", "1000001"}, {"note", "n"}}})}});
    CHECK(a["error_code"] == 0);
    t.rig.settle();
    CHECK(t.rig.cloud->count("create") == 1);
    CHECK(t.rig.cloud->count("update 1000001") == 1);
}

namespace {

json read_from_slot(const std::string& name, json ams_id = "1", const std::string& slot = "2")
{
    json s             = page_spool(name);
    s["bound_dev_id"]  = "dev1";
    s["bound_ams_id"]  = std::move(ams_id);
    s["bound_slot_id"] = slot;
    return s;
}

} // namespace

TEST_CASE("A spool read from an AMS slot without RFID is created through the host to be assigned", "[FilamentManager]")
{
    VmRig t(true, true);
    json  a = t.call("spool", "add", read_from_slot("A"));
    CHECK(a["error_code"] == 0);
    t.rig.settle();
    CHECK(t.rig.cloud->count("create") == 0);
    REQUIRE(t.assigned.size() == 1);
    CHECK(t.assigned[0] == "dev1 1 2 A");

    json numeric = read_from_slot("B", 128);
    t.call("spool", "add", numeric);
    REQUIRE(t.assigned.size() == 2);
    CHECK(t.assigned[1] == "dev1 128 2 B");
}

TEST_CASE("A spool with an RFID, an external spool, a missing slot or no host function is added as before", "[FilamentManager]")
{
    const auto run = [](const json& spool, bool with_assign) {
        VmRig t(true, with_assign);
        json  a = t.call("spool", "add", spool);
        CHECK(a["error_code"] == 0);
        t.rig.settle();
        CHECK(t.assigned.empty());
        CHECK(t.rig.cloud->count("create") == 1);
    };
    json rfid       = read_from_slot("A");
    rfid["tag_uid"] = "0123456789ABCDEF";
    run(rfid, true);
    run(read_from_slot("A", "255"), true);
    run(read_from_slot("A", 255), true);
    run(read_from_slot("A", "254"), true);
    run(read_from_slot("A", "x1"), true);
    json no_slot = read_from_slot("A");
    no_slot.erase("bound_slot_id");
    run(no_slot, true);
    json empty_slot             = read_from_slot("A");
    empty_slot["bound_slot_id"] = "";
    run(empty_slot, true);
    json no_dev = read_from_slot("A");
    no_dev.erase("bound_dev_id");
    run(no_dev, true);
    run(read_from_slot("A"), false);

    // An RFID of zeros is no RFID.
    VmRig t(true, true);
    json  zeros    = read_from_slot("A");
    zeros["tag_uid"] = "0000000000000000";
    t.call("spool", "add", zeros);
    CHECK(t.assigned.size() == 1);
}

TEST_CASE("A batch create routes spools to be assigned apart and answers the spool list", "[FilamentManager]")
{
    VmRig t(true, true);
    t.rig.cloud->fill(1);
    t.pull();
    json rfid       = read_from_slot("R");
    rfid["tag_uid"] = "0123456789ABCDEF";
    json creates    = json::array({read_from_slot("S", "0", "3"), rfid, page_spool("P")});
    json a = t.call("spool", "batch_create", json{{"creates", creates}, {"updates", json::array({json{{"spool_id", "1000001"}, {"note", "n"}}})}});
    CHECK(a["error_code"] == 0);
    CHECK(a["payload"].is_array());
    t.rig.settle();
    REQUIRE(t.assigned.size() == 1);
    CHECK(t.assigned[0] == "dev1 0 3 S");
    CHECK(t.rig.cloud->count("create") == 2);
    CHECK(t.rig.cloud->count("update 1000001") == 1);
}

TEST_CASE("A batch create with a malformed part creates and assigns nothing", "[FilamentManager]")
{
    const json bad[] = {json{{"creates", json::array({read_from_slot("S", "0", "3")})}, {"updates", json::object()}},
                        json{{"creates", json::array({read_from_slot("S", "0", "3")})}, {"updates", json::array({json("x")})}},
                        json{{"creates", json::array({read_from_slot("S", "0", "3"), json(5)})}},
                        json{{"creates", json::array({page_spool("P")})}, {"updates", 5}},
                        json::array({read_from_slot("S", "0", "3")})};
    for (const json& payload : bad) {
        VmRig t(true, true);
        t.rig.cloud->fill(1);
        t.pull();
        json a = t.call("spool", "batch_create", payload);
        INFO(payload.dump());
        CHECK(a["error_code"] == 3);
        t.rig.settle();
        CHECK(t.assigned.empty());
        CHECK(t.rig.cloud->count("create") == 0);
        CHECK(t.rig.cloud->count("update") == 0);
    }
}

TEST_CASE("An update shows in the answer at once", "[FilamentManager]")
{
    VmRig t(false);
    t.rig.cloud->fill(1);
    t.pull();
    json a = t.call("spool", "update", json{{"spool_id", "1000001"}, {"note", "hello"}});
    CHECK(a["error_code"] == 0);
    REQUIRE(a["payload"].size() == 1);
    CHECK(a["payload"][0]["note"] == "hello");
    t.rig.settle();
    CHECK(t.rig.cloud->count("update 1000001") == 1);
}

TEST_CASE("A removed spool is deleted in the cloud", "[FilamentManager]")
{
    VmRig t;
    t.rig.cloud->fill(2);
    t.pull();
    json a = t.call("spool", "remove", json{{"spool_id", "1000001"}});
    CHECK(a["error_code"] == 0);
    CHECK(a["payload"].size() == 1);
    t.rig.settle();
    CHECK(t.rig.cloud->count("remove 1000001") == 1);
}

TEST_CASE("A batch remove deletes the listed spools", "[FilamentManager]")
{
    VmRig t;
    t.rig.cloud->fill(3);
    t.pull();
    json a = t.call("spool", "batch_remove", json{{"spool_ids", json::array({"1000001", "1000002"})}});
    CHECK(a["error_code"] == 0);
    CHECK(a["payload"].size() == 1);
    t.rig.settle();
    CHECK(t.rig.cloud->count("remove") >= 1);
    CHECK(t.rig.cloud->rows.size() == 1);
}

TEST_CASE("Every spool write is refused with minus two when signed out", "[FilamentManager]")
{
    VmRig t;
    t.rig.session.signed_in = false;
    const std::vector<std::pair<std::string, json>> writes{
        {"add", page_spool()},
        {"batch_add", json{{"spool", page_spool()}, {"quantity", 2}}},
        {"batch_create", json{{"creates", json::array({page_spool()})}, {"updates", json::array()}}},
        {"update", json{{"spool_id", "1000001"}, {"note", "x"}}},
        {"remove", json{{"spool_id", "1000001"}}},
        {"batch_remove", json{{"spool_ids", json::array({"1000001"})}}}};
    for (const auto& [action, payload] : writes) {
        json a = t.call("spool", action, payload);
        INFO(action);
        CHECK(a["error_code"] == -2);
        CHECK(a["message"] == "cloud sync requires sign-in and network");
        CHECK(a["payload"].is_array());
    }
}

TEST_CASE("A write with a payload of the wrong shape answers three", "[FilamentManager]")
{
    VmRig t;
    t.rig.cloud->fill(1);
    t.pull();
    const std::vector<std::pair<std::string, json>> writes{
        {"batch_add", json{{"spool", "x"}, {"quantity", 2}}},
        {"batch_create", json{{"creates", 5}, {"updates", json::array()}}},
        {"update", json::object()},
        {"remove", json{{"spool_id", 7}}},
        {"batch_remove", json{{"spool_ids", "x"}}}};
    for (const auto& [action, payload] : writes) {
        json a = t.call("spool", action, payload);
        INFO(action);
        CHECK(a["error_code"] == 3);
        CHECK(a["message"] == "bad request");
        CHECK(a["payload"].is_array());
    }
}

TEST_CASE("Actions the page may send but this app does not support answer minus one", "[FilamentManager]")
{
    VmRig t;
    for (const std::string& action : {"mark_empty", "toggle_favorite", "archive"}) {
        json a = t.call("spool", action, json{{"spool_id", "1"}});
        INFO(action);
        CHECK(a["error_code"] == -1);
        CHECK(a["message"] == "not supported");
        CHECK(a["payload"].is_array());
    }
    json push = t.call("sync", "push_all_now");
    CHECK(push["error_code"] == -1);
    CHECK(push["message"] == "not supported");
    CHECK(push["payload"].contains("logged_in"));
}

TEST_CASE("An unknown action or submod answers minus one and another module answers one", "[FilamentManager]")
{
    VmRig t;
    CHECK(t.call("spool", "frobnicate")["error_code"] == -1);
    CHECK(t.call("spool", "frobnicate")["message"] == "unknown action");
    CHECK(t.call("nothing", "list")["error_code"] == -1);
    json other = t.call("spool", "list", json::object(), "device");
    CHECK(other["error_code"] == 1);
    CHECK(other["message"] == "unknown module");
    CHECK(other["module"] == "device");
}

TEST_CASE("A sync pull causes one pull and answers a syncing state", "[FilamentManager]")
{
    VmRig t;
    t.rig.cloud->fill(1);
    json a = t.call("sync", "pull");
    CHECK(a["error_code"] == 0);
    CHECK(a["payload"]["is_syncing"] == true);
    t.rig.settle();
    CHECK(t.rig.cloud->count("list 0") == 1);
}

TEST_CASE("The sync status formats the time of the last sync", "[FilamentManager]")
{
    VmRig t;
    t.rig.clock      = 1700000000;
    t.rig.clock_step = 0;
    t.pull();
    json a = t.call("sync", "status");
    CHECK(a["payload"]["last_synced_at"] == "2023-11-14T22:13:20Z");
    CHECK(a["payload"]["last_error"]["code"] == 0);
    CHECK(a["payload"]["last_error"]["message"] == "");
}

TEST_CASE("The sync status of a never synced service has an empty time", "[FilamentManager]")
{
    VmRig t;
    CHECK(t.call("sync", "status")["payload"]["last_synced_at"] == "");
}

TEST_CASE("A rejected write shows its code and the server text in the sync status", "[FilamentManager]")
{
    VmRig t(false);
    t.rig.cloud->fill(1);
    t.pull();
    t.rig.cloud->forced["update"] = {forced_error(fil::CloudError::Rejected, "bad")};
    t.call("spool", "update", json{{"spool_id", "1000001"}, {"note", "x"}});
    t.rig.settle();
    json        a    = t.call("sync", "status");
    const std::string text = a["payload"]["last_error"]["message"].get<std::string>();
    CHECK(a["payload"]["last_error"]["code"] == 400);
    CHECK(text.size() > 6);
    CHECK(text.substr(text.size() - 6) == " (bad)");
}

TEST_CASE("The host decides the text of an error when it has a say", "[FilamentManager]")
{
    VmRig t;
    t.host.fail_text = [](fil::FailKind, const std::string& m) { return "host:" + m; };
    t.vm.reset();
    t.vm = std::make_unique<fm::ViewModel>(*t.rig.svc, t.host, [&t](const json& b) { t.reports.push_back(b); });
    t.rig.svc->events = t.vm->event_handlers();
    t.rig.cloud->fill(1);
    t.pull();
    t.rig.cloud->forced["update"] = {forced_error(fil::CloudError::Rejected, "bad")};
    t.call("spool", "update", json{{"spool_id", "1000001"}, {"note", "x"}});
    t.rig.settle();
    CHECK(t.call("sync", "status")["payload"]["last_error"]["message"] == "host:bad");
}

TEST_CASE("A config fetch answers pending and then reports the config", "[FilamentManager]")
{
    VmRig t;
    json  a = t.call("config", "fetch");
    CHECK(a["message"] == "pending");
    CHECK(a["payload"] == json::object());
    t.rig.settle();
    auto fetched = t.reports_of("config", "fetched");
    REQUIRE(fetched.size() == 1);
    CHECK(fetched[0]["payload"] == t.rig.cloud->config_body);
}

TEST_CASE("A later config fetch answers from the cache unless forced", "[FilamentManager]")
{
    VmRig t;
    t.call("config", "fetch");
    t.rig.settle();
    json cached = t.call("config", "fetch");
    CHECK(cached["message"] == "cached");
    CHECK(cached["payload"] == t.rig.cloud->config_body);
    json forced = t.call("config", "fetch", json{{"force", true}});
    CHECK(forced["message"] == "pending");
    t.rig.settle();
    CHECK(t.rig.cloud->count("config") == 2);
}

TEST_CASE("The service events become reports", "[FilamentManager]")
{
    VmRig t;
    t.rig.cloud->fill(2);
    t.pull();
    CHECK_FALSE(t.reports_of("spool", "list").empty());
    CHECK_FALSE(t.reports_of("sync", "state").empty());
    auto done = t.reports_of("sync", "pull_done");
    REQUIRE(done.size() == 1);
    CHECK(done[0]["module"] == "filament");
    CHECK(done[0]["payload"]["spools"].size() == 2);
    CHECK(done[0]["payload"]["state"]["logged_in"] == true);
    CHECK(t.reports_of("spool", "list")[0]["payload"].is_array());
}

TEST_CASE("The first pull reports no added spools", "[FilamentManager]")
{
    VmRig t;
    t.rig.cloud->fill(2);
    t.pull();
    auto done = t.reports_of("sync", "pull_done");
    REQUIRE(done.size() == 1);
    CHECK(done[0]["payload"]["added"] == 0);
    CHECK(done[0]["payload"]["updated"] == 2);
}

TEST_CASE("A pull after an add reports one added spool", "[FilamentManager]")
{
    VmRig t;
    t.rig.cloud->fill(2);
    t.pull();
    t.call("spool", "add", page_spool());
    t.rig.settle();
    auto done = t.reports_of("sync", "pull_done");
    REQUIRE(done.size() == 2);
    CHECK(done[1]["payload"]["added"] == 1);
    CHECK(done[1]["payload"]["updated"] == 2);
}

TEST_CASE("A pull after a reset again reports no added spools", "[FilamentManager]")
{
    VmRig t;
    t.rig.cloud->fill(2);
    t.pull();
    t.rig.session.generation += 1;
    t.rig.svc->reset();
    t.pull();
    auto done = t.reports_of("sync", "pull_done");
    REQUIRE(done.size() == 2);
    CHECK(done[1]["payload"]["added"] == 0);
    CHECK(done[1]["payload"]["updated"] == 2);
}

TEST_CASE("A finished write reports its operation and spool", "[FilamentManager]")
{
    VmRig t;
    t.rig.cloud->fill(1);
    t.pull();
    t.call("spool", "update", json{{"spool_id", "1000001"}, {"note", "x"}});
    t.rig.settle();
    auto done = t.reports_of("sync", "push_done");
    REQUIRE(done.size() == 1);
    CHECK(done[0]["payload"]["op"] == "update");
    CHECK(done[0]["payload"]["spool_id"] == "1000001");
    CHECK(done[0]["payload"].contains("state"));
}

TEST_CASE("A failed write reports its code and text", "[FilamentManager]")
{
    VmRig t(false);
    t.rig.cloud->fill(1);
    t.pull();
    t.rig.cloud->forced["update"] = {forced_error(fil::CloudError::Rejected, "bad")};
    t.call("spool", "update", json{{"spool_id", "1000001"}, {"note", "x"}});
    t.rig.settle();
    auto failed = t.reports_of("sync", "push_failed");
    REQUIRE(failed.size() == 1);
    CHECK(failed[0]["payload"]["op"] == "update");
    CHECK(failed[0]["payload"]["spool_id"] == "1000001");
    CHECK(failed[0]["payload"]["code"] == 400);
    CHECK(failed[0]["payload"]["message"].get<std::string>().find("(bad)") != std::string::npos);
}

TEST_CASE("A custom filament request calls the host and answers pending", "[FilamentManager]")
{
    VmRig t;
    json  a = t.call("preset", "create_custom", json{{"vendor", "Acme"}, {"client_request_id", "c1"}});
    CHECK(a["error_code"] == 0);
    CHECK(a["message"] == "pending");
    CHECK(t.count_calls("custom Acme c1") == 1);
}

TEST_CASE("A custom filament request without a host answers minus one", "[FilamentManager]")
{
    VmRig t(false);
    CHECK(t.call("preset", "create_custom", json{{"vendor", "Acme"}})["error_code"] == -1);
}

TEST_CASE("A created custom filament sends the result and then the preset list", "[FilamentManager]")
{
    VmRig t;
    t.vm->custom_created("c1", true, json{{"vendor", "Acme"}, {"filament_id", "P1"}});
    auto done = t.reports_of("preset", "create_custom_done");
    REQUIRE(done.size() == 1);
    CHECK(done[0]["payload"]["ok"] == true);
    CHECK(done[0]["payload"]["client_request_id"] == "c1");
    CHECK(done[0]["payload"]["created"]["filament_id"] == "P1");
    CHECK(t.reports_of("preset", "list").size() == 1);
}

TEST_CASE("A failed custom filament sends only the result", "[FilamentManager]")
{
    VmRig t;
    t.vm->custom_created("c2", false, json::object());
    auto done = t.reports_of("preset", "create_custom_done");
    REQUIRE(done.size() == 1);
    CHECK(done[0]["payload"]["ok"] == false);
    CHECK_FALSE(done[0]["payload"].contains("created"));
    CHECK(t.reports_of("preset", "list").empty());
}

TEST_CASE("A change of the selected machine sends machines and AMS data", "[FilamentManager]")
{
    VmRig t;
    t.vm->machine_selected_changed();
    auto sent = t.reports_of("machine", "selected_changed");
    REQUIRE(sent.size() == 1);
    CHECK(sent[0]["payload"]["selected_dev_id"] == "dev1");
    CHECK(sent[0]["payload"]["machines"]["selected_dev_id"] == "dev1");
    CHECK(sent[0]["payload"]["ams"]["selected_dev_id"] == "dev1");
}

TEST_CASE("Host members that are unset give the default payloads", "[FilamentManager]")
{
    VmRig t(false);
    t.vm->machine_selected_changed();
    auto sent = t.reports_of("machine", "selected_changed");
    REQUIRE(sent.size() == 1);
    CHECK(sent[0]["payload"]["selected_dev_id"] == "");
    CHECK(sent[0]["payload"]["machines"]["machines"].empty());
    CHECK(t.call("machine", "list")["payload"]["machines"].empty());
    CHECK(t.call("ams", "list")["payload"]["ams_units"].empty());
    CHECK(t.call("preset", "list")["payload"] == json{{"vendors", json::array()}});
    CHECK(t.call("colors", "query_for_id", json{{"fila_id", "GFA00"}})["payload"]["candidates"].empty());
    CHECK(t.call("machine", "request_pushall")["error_code"] == -2);
}

TEST_CASE("The machine and AMS requests pass their arguments to the host", "[FilamentManager]")
{
    VmRig t;
    json  ams = t.call("ams", "list", json{{"dev_id", "d9"}, {"switch_selected", true}});
    CHECK(ams["error_code"] == 0);
    CHECK(t.count_calls("ams d9 switch") == 1);
    json push = t.call("machine", "request_pushall", json{{"dev_id", "d9"}});
    CHECK(push["payload"]["dev_id"] == "d9");
    json gone = t.call("machine", "request_pushall", json{{"dev_id", "none"}});
    CHECK(gone["error_code"] == -2);
    CHECK(gone["message"] == "machine not found");
}

TEST_CASE("Service events send nothing once the view-model is gone", "[FilamentManager]")
{
    VmRig t;
    t.rig.cloud->fill(1);
    t.rig.svc->events = {}; // the owner of the events (Runtime::detach) clears them before the view-model goes
    t.vm.reset();
    t.pull();
    CHECK(t.reports.empty());
}

TEST_CASE("A view-model created on a service that already has a list reports the next pull's added count as it is", "[FilamentManager]")
{
    VmRig t;
    t.rig.cloud->fill(1);
    t.pull(); // the list is pulled before the panel exists
    t.rig.svc->events = {};
    t.rig.cloud->fill(2, 1000002);
    VmRig& same = t;
    same.vm.reset();
    same.vm = std::make_unique<fm::ViewModel>(*same.rig.svc, same.host, [&same](const json& b) { same.reports.push_back(b); });
    same.rig.svc->events = same.vm->event_handlers();
    same.reports.clear();
    same.pull();
    std::vector<json> done = same.reports_of("sync", "pull_done");
    REQUIRE(done.size() == 1);
    CHECK(done[0]["payload"]["added"] == 2);
}

TEST_CASE("A host function that throws makes the request answer an internal error", "[FilamentManager]")
{
    VmRig t;
    t.throw_presets = true;
    json a          = t.call("init", "init");
    CHECK(a["error_code"] == 2);
    CHECK(a["message"] == "internal error");
}

TEST_CASE("The colour response lists the candidates of a filament", "[FilamentManager]")
{
    json r = fm::colors_response(fixture_colors(), "GFA00", "de");
    CHECK(r["fila_id"] == "GFA00");
    CHECK(r["fila_type"] == "PLA Basic");
    REQUIRE(r["candidates"].size() == 3);
    CHECK(r["candidates"][0]["color_code"] == "10300");
    CHECK(r["candidates"][0]["name"] == "Orange-de");
    CHECK(r["candidates"][0]["color_type"] == 2);
    CHECK(r["candidates"][0]["colors"] == json::array({"#FF6A13FF"}));
    json none = fm::colors_response(fixture_colors(), "GFZ99", "de");
    CHECK(none["fila_type"] == "");
    CHECK(none["candidates"].empty());
}

TEST_CASE("An empty colour name of a spool is filled from the table", "[FilamentManager]")
{
    json spool = {{"setting_id", "GFA00"}, {"color_name", ""}, {"color_code", "#FF6A13FF"}, {"colors", json::array()}, {"color_type", 2}};
    fm::decorate_color_name(spool, fixture_colors(), "en");
    CHECK(spool["color_name"] == "Orange");
    json two = {{"setting_id", "GFA00"}, {"color_name", ""}, {"color_code", "#FF0000FF"}, {"colors", json::array({"#FF0000FF", "#00FF00FF"})}, {"color_type", 1}};
    fm::decorate_color_name(two, fixture_colors(), "en");
    CHECK(two["color_name"] == "Red Green");
}

TEST_CASE("A colour name that is already set is kept", "[FilamentManager]")
{
    json spool = {{"setting_id", "GFA00"}, {"color_name", "Mine"}, {"color_code", "#FF6A13FF"}, {"colors", json::array()}, {"color_type", 2}};
    fm::decorate_color_name(spool, fixture_colors(), "en");
    CHECK(spool["color_name"] == "Mine");
    json unknown = {{"setting_id", "GFZ99"}, {"color_name", ""}, {"color_code", "#FF6A13FF"}, {"colors", json::array()}, {"color_type", 2}};
    fm::decorate_color_name(unknown, fixture_colors(), "en");
    CHECK(unknown["color_name"] == "");
}

TEST_CASE("The tray label of a spool follows its place in the printer", "[FilamentManager]")
{
    json slot = {{"in_printer", true}, {"ams_type", 1}, {"ams_id", 1}, {"slot_id", "2"}, {"tray_label", ""}};
    fm::decorate_tray_label(slot);
    CHECK(slot["tray_label"] == "B3");
    json ext = {{"in_printer", true}, {"ams_type", 0}, {"ams_id", 0}, {"slot_id", "0"}, {"tray_label", ""}};
    fm::decorate_tray_label(ext);
    CHECK(ext["tray_label"] == "Ext");
    json shelf = {{"in_printer", false}, {"ams_type", 1}, {"ams_id", 1}, {"slot_id", "2"}, {"tray_label", ""}};
    fm::decorate_tray_label(shelf);
    CHECK(shelf["tray_label"] == "");
}

TEST_CASE("An automatic weight push is reported in the shape of Bambu Studio", "[FilamentManager]")
{
    VmRig t;
    fil::AutoPushSummary busy;
    busy.pushed           = 2;
    busy.skipped_cooldown = 1;
    busy.busy             = true;
    t.vm->auto_push_summary(busy);
    fil::AutoPushSummary idle;
    idle.pushed = 1;
    t.vm->auto_push_summary(idle);

    auto sent = t.reports_of("sync", "auto_push_summary");
    REQUIRE(sent.size() == 2);
    CHECK(sent[0]["module"] == "filament");
    const json& p = sent[0]["payload"];
    CHECK(p["trigger"] == "auto");
    CHECK(p["device_state"] == "busy");
    CHECK(p["pushed"] == 2);
    CHECK(p["skipped_cooldown"] == 1);
    CHECK(p["skipped_no_diff"] == 0);
    CHECK(p["skipped_no_rfid"] == 0);
    CHECK(sent[1]["payload"]["device_state"] == "idle");
    CHECK(sent[1]["payload"]["pushed"] == 1);
}

TEST_CASE("Without a session override or the environment the Preferences check box decides the link mode", "[FilamentManager]")
{
    CHECK(fm::link_config(false, "", std::nullopt, {}).mode == fil::LinkMode::Off);
    CHECK(fm::link_config(true, "", std::nullopt, {}).mode == fil::LinkMode::On);
    CHECK(fm::link_config(true, "", std::nullopt, {}).only_slots.empty());
    CHECK(std::string(fm::link_mode_source("", std::nullopt)) == "config");
}

TEST_CASE("The environment decides the link mode in place of the check box", "[FilamentManager]")
{
    for (bool enabled : {false, true}) {
        CHECK(fm::link_config(enabled, "off", std::nullopt, {}).mode == fil::LinkMode::Off);
        CHECK(fm::link_config(enabled, "dry_run", std::nullopt, {}).mode == fil::LinkMode::DryRun);
        CHECK(fm::link_config(enabled, "on", std::nullopt, {}).mode == fil::LinkMode::On);
    }
    CHECK(std::string(fm::link_mode_source("dry_run", std::nullopt)) == "env");
    // An unknown or empty value is ignored.
    for (const char* ignored : {"", "ON", "dryrun", "1", " on", "default"}) {
        CHECK(fm::link_config(false, ignored, std::nullopt, {}).mode == fil::LinkMode::Off);
        CHECK(fm::link_config(true, ignored, std::nullopt, {}).mode == fil::LinkMode::On);
        CHECK_FALSE(fm::parse_link_mode(ignored).has_value());
        CHECK(std::string(fm::link_mode_source(ignored, std::nullopt)) == "config");
    }
}

TEST_CASE("The session override decides the link mode before the environment and the check box", "[FilamentManager]")
{
    for (bool enabled : {false, true})
        for (const char* env : {"", "off", "dry_run", "on"}) {
            CHECK(fm::link_config(enabled, env, fil::LinkMode::Off, {}).mode == fil::LinkMode::Off);
            CHECK(fm::link_config(enabled, env, fil::LinkMode::DryRun, {}).mode == fil::LinkMode::DryRun);
            CHECK(fm::link_config(enabled, env, fil::LinkMode::On, {}).mode == fil::LinkMode::On);
            CHECK(std::string(fm::link_mode_source(env, fil::LinkMode::Off)) == "session");
        }
}

TEST_CASE("The allow-list comes from the session only", "[FilamentManager]")
{
    const std::set<std::string> only{"dev1:0:1", "dev2:128:0"};
    CHECK(fm::link_config(true, "", std::nullopt, only).only_slots == only);
    CHECK(fm::link_config(false, "on", fil::LinkMode::DryRun, only).only_slots == only);
    CHECK(fm::link_config(true, "on", std::nullopt, {}).only_slots.empty());
}

TEST_CASE("Link mode names read back as the mode they name", "[FilamentManager]")
{
    for (fil::LinkMode m : {fil::LinkMode::Off, fil::LinkMode::DryRun, fil::LinkMode::On}) {
        const std::optional<fil::LinkMode> back = fm::parse_link_mode(fm::link_mode_text(m));
        REQUIRE(back.has_value());
        CHECK(*back == m);
    }
}

TEST_CASE("The soft-match queue shows the spool ids and facts of each entry and its candidates, never an RFID or a serial", "[FilamentManager]")
{
    fil::Spool hit;
    hit.id         = "11";
    hit.vendor     = "Bambu Lab";
    hit.name       = "PLA Basic";
    hit.color      = "#FF0000FF";
    hit.net_weight = 812.4;
    hit.rfid       = "0123456789ABCDEF0123456789ABCDEF";
    hit.dev_id     = "01P00A391800123";
    hit.ams_sn     = "19C06A4A2900456";
    fil::Spool candidate = hit;
    candidate.id         = "12";
    candidate.name       = "PLA Matte";
    candidate.net_weight = 400;
    fil::SoftMatchEntry entry;
    entry.hit        = hit;
    entry.candidates = {candidate};
    fil::SoftMatchQueue queue;
    queue.entries = {entry};

    const json items = fm::soft_match_items(queue);
    REQUIRE(items.size() == 1);
    CHECK(items[0]["spool_id"] == "11");
    CHECK(items[0]["brand"] == "Bambu Lab");
    CHECK(items[0]["name"] == "PLA Basic");
    CHECK(items[0]["color"] == "#FF0000FF");
    CHECK(items[0]["remaining_g"] == 812);
    REQUIRE(items[0]["candidates"].size() == 1);
    CHECK(items[0]["candidates"][0]["spool_id"] == "12");
    CHECK(items[0]["candidates"][0]["name"] == "PLA Matte");
    CHECK(items[0]["candidates"][0]["remaining_g"] == 400);
    const std::string text = items.dump();
    CHECK(text.find(hit.rfid) == std::string::npos);
    CHECK(text.find(hit.dev_id) == std::string::npos);
    CHECK(text.find(hit.ams_sn) == std::string::npos);
    CHECK(fm::soft_match_items(fil::SoftMatchQueue{}).empty());
}

TEST_CASE("A slot shows a spool with several colours only where the printer takes several", "[FilamentManager]")
{
    fil::Spool one;
    one.color = "#FF0000FF";
    fil::Spool two = one;
    two.colors     = {"#FF0000FF", "#00FF00FF"};
    fil::Spool five = one;
    five.colors     = {"#FF0000FF", "#00FF00FF", "#0000FFFF", "#FFFFFFFF", "#000000FF"};
    fil::Spool unreadable = one;
    unreadable.colors     = {"#FF0000FF", "nope"};

    CHECK(fm::slot_shows_colours(one, false, 4));
    CHECK(fm::slot_shows_colours(one, true, 4));
    CHECK_FALSE(fm::slot_shows_colours(two, false, 4));
    CHECK(fm::slot_shows_colours(two, true, 4));
    CHECK_FALSE(fm::slot_shows_colours(five, true, 4));
    CHECK(fm::slot_shows_colours(unreadable, false, 4)); // the colour that cannot be read is left out
}
