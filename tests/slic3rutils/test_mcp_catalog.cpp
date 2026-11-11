#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/Mcp/McpService.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <string>

using namespace Slic3r::GUI::Mcp;
using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

// resources/ of the source tree: PROFILES_DIR is <source>/resources/profiles.
fs::path seed_path() { return fs::path(std::string(PROFILES_DIR)).parent_path() / "mcp" / "catalog.json"; }

} // namespace

// Hidden: run it as slic3rutils_tests "[.McpCatalogRegen]" to rewrite the committed seed.
TEST_CASE("Regenerate the MCP catalog seed", "[.McpCatalogRegen]")
{
    const fs::path path = seed_path();
    fs::create_directories(path.parent_path());
    boost::nowide::ofstream out(path.string().c_str(), std::ios::binary | std::ios::trunc);
    REQUIRE(out);
    out << McpService::catalog_json().dump(2) << "\n";
}

TEST_CASE("the shipped MCP catalog seed matches the tools the build registers", "[Mcp][McpCatalog]")
{
    boost::nowide::ifstream in(seed_path().string().c_str(), std::ios::binary);
    REQUIRE(in);
    json seed = json::parse(in, nullptr, false);
    REQUIRE(seed.is_object());

    json current = McpService::catalog_json();
    // The version moves with every release; the tools are what the seed pins.
    seed.erase("version");
    current.erase("version");
    if (seed != current)
        FAIL("MCP catalog seed is stale: run slic3rutils_tests \"[.McpCatalogRegen]\"");
}

TEST_CASE("the catalog lists tools with schemas and none named for the bridge", "[Mcp][McpCatalog]")
{
    const json catalog = McpService::catalog_json();
    REQUIRE(catalog["tools"].is_array());
    CHECK(catalog["tools"].size() > 10);
    CHECK_FALSE(catalog["instructions"].get<std::string>().empty());
    for (const json& tool : catalog["tools"]) {
        const std::string name = tool["name"].get<std::string>();
        INFO(name);
        CHECK(name.rfind("orca_", 0) != 0); // orca_* is the bridge's own namespace
        CHECK(tool["inputSchema"].is_object());
    }
}

TEST_CASE("the preset_sync tool takes an action and a closed set of arguments", "[Mcp][McpCatalog]")
{
    const json catalog = McpService::catalog_json();
    const json* tool   = nullptr;
    for (const json& t : catalog.at("tools"))
        if (t.at("name") == "preset_sync") tool = &t;
    REQUIRE(tool != nullptr);

    const json& schema = tool->at("inputSchema");
    CHECK(schema.at("required") == json::array({"action"}));
    CHECK(schema.at("additionalProperties") == false);
    CHECK(schema.at("properties").at("action").at("enum") ==
          json::array({"status", "sync_now", "confirm_first_run", "convert", "copy_k", "forget", "resolve_halt"}));
    CHECK(schema.at("properties").at("accept").at("type") == "boolean");
    CHECK(schema.at("properties").at("convert").at("type") == "array");
    CHECK(schema.at("properties").at("decline").at("items").at("type") == "string");
    CHECK(schema.at("properties").at("type").at("enum") == json::array({"print", "filament", "printer"}));
    CHECK(schema.at("properties").at("choice").at("enum").size() == 5);
    CHECK(schema.at("properties").at("from_filament_id").at("type") == "string");
    CHECK(tool->at("annotations").at("readOnlyHint") == false);
}
