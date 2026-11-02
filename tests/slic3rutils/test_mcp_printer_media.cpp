// Tests for the printer camera and file tools (fork): tunnel URLs, secret handling, file entries, refusals.
#include <catch2/catch_all.hpp>

#include "slic3r/GUI/Mcp/McpPrinterCommon.hpp"
#include "slic3r/GUI/Mcp/McpPrinterMedia.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"
#include "slic3r/Utils/PrinterNetworkTypes.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::GUI::Mcp;
using nlohmann::json;
using Catch::Matchers::ContainsSubstring;

namespace {

MediaEndpoint endpoint()
{
    MediaEndpoint e;
    e.ip          = "192.168.1.50";
    e.access_code = "SECRET42";
    e.dev_id      = "01P00A000000001";
    e.dev_ver     = "01.08.00.00";
    e.net_ver     = "02.00.00.50";
    e.cli_id      = "client-uuid";
    e.cli_ver     = "2.5.0";
    return e;
}

template<class F> std::string failure_text(F&& f)
{
    try {
        f();
    } catch (const ToolFailure& e) {
        return e.what();
    }
    return {};
}

std::vector<MediaFileEntry> entries()
{
    return {{"a.3mf", "/sdcard/model/a.3mf", 100, 1700000000},
            {"b.3mf", "/sdcard/model/b.3mf", 200, 1700000100},
            {"a.3mf", "/sdcard/cache/a.3mf", 300, 1700000200}};
}

} // namespace

TEST_CASE("Camera URL for an MJPEG printer uses the local tunnel on port 6000", "[McpPrinterMedia]")
{
    const std::string url = camera_lan_url(LVL_Local, endpoint());
    CHECK_THAT(url, ContainsSubstring("bambu:///local/192.168.1.50.?port=6000"));
    CHECK_THAT(url, ContainsSubstring("user=bblp"));
    CHECK_THAT(url, ContainsSubstring("device=01P00A000000001"));
    CHECK_THAT(url, ContainsSubstring("dev_ver=01.08.00.00"));
}

TEST_CASE("Camera URL for an RTSPS printer carries user, code and host in the authority", "[McpPrinterMedia]")
{
    const std::string url = camera_lan_url(LVL_Rtsps, endpoint());
    CHECK_THAT(url, ContainsSubstring("bambu:///rtsps___bblp:SECRET42@192.168.1.50/streaming/live/1?proto=rtsps"));
    const std::string plain = camera_lan_url(LVL_Rtsp, endpoint());
    CHECK_THAT(plain, ContainsSubstring("bambu:///rtsp___bblp:SECRET42@192.168.1.50/streaming/live/1?proto=rtsp"));
}

TEST_CASE("Camera URL is refused without LAN access or live view", "[McpPrinterMedia]")
{
    MediaEndpoint no_ip = endpoint();
    no_ip.ip.clear();
    CHECK_THROWS_AS(camera_lan_url(LVL_Local, no_ip), ToolFailure);
    CHECK_THROWS_AS(files_lan_url(no_ip), ToolFailure);

    MediaEndpoint no_code = endpoint();
    no_code.access_code.clear();
    CHECK_THROWS_AS(camera_lan_url(LVL_Rtsps, no_code), ToolFailure);
    CHECK_THROWS_AS(files_lan_url(no_code), ToolFailure);

    CHECK_THAT(failure_text([] { camera_lan_url(LVL_Disable, endpoint()); }), ContainsSubstring("Liveview is off"));
    CHECK_THROWS_AS(camera_lan_url(LVL_None, endpoint()), ToolFailure);
}

TEST_CASE("Files URL is the local tunnel URL", "[McpPrinterMedia]")
{
    const std::string url = files_lan_url(endpoint());
    CHECK_THAT(url, ContainsSubstring("bambu:///local/192.168.1.50.?port=6000&user=bblp&passwd=SECRET42"));
}

TEST_CASE("Secrets are removed from anything a tool returns", "[McpPrinterMedia]")
{
    const MediaEndpoint e = endpoint();
    const std::string   url = camera_lan_url(LVL_Rtsps, e);

    SECTION("scrub_secrets replaces the code wherever it appears")
    {
        const std::string text = scrub_secrets("failed " + url + " and again " + url, {e.access_code, ""});
        CHECK_THAT(text, !ContainsSubstring("SECRET42"));
        CHECK_THAT(text, ContainsSubstring("<redacted>"));
    }
    SECTION("redact drops url keys and passwd values from a reply")
    {
        json reply = {{"name", "x"}, {"url", url}, {"nested", {{"camera_url", url}, {"hint", "passwd=SECRET42"}}}};
        const std::string dumped = redact(reply).dump();
        CHECK_THAT(dumped, !ContainsSubstring("SECRET42"));
        CHECK_THAT(dumped, ContainsSubstring("\"name\":\"x\""));
    }
}

TEST_CASE("Frame size is scaled to a multiple of 16 wide and keeps the aspect ratio", "[McpPrinterMedia]")
{
    const FrameSize same = fit_frame_width(1280, 720, 1280);
    CHECK(same.width == 1280);
    CHECK(same.height == 720);

    const FrameSize half = fit_frame_width(1920, 1080, 960);
    CHECK(half.width == 960);
    CHECK(half.height == 540);

    const FrameSize odd = fit_frame_width(1920, 1080, 1000);
    CHECK(odd.width % 16 == 0);
    CHECK(odd.width <= 1000);
    CHECK(odd.height % 2 == 0);

    const FrameSize padded = fit_frame_width(1000, 500, 4096);
    CHECK(padded.width % 16 == 0);

    const FrameSize none = fit_frame_width(0, 0, 1280);
    CHECK(none.width == 0);
}

TEST_CASE("A file is found by printer path, or by a name only one file has", "[McpPrinterMedia]")
{
    const auto list = entries();
    CHECK(find_media_file(list, "/sdcard/model/b.3mf") == 1);
    CHECK(find_media_file(list, "b.3mf") == 1);
    CHECK(find_media_file(list, "/sdcard/cache/a.3mf") == 2);
    CHECK_THAT(failure_text([&] { find_media_file(list, "a.3mf"); }), ContainsSubstring("More than one file"));
}

TEST_CASE("Deleting or fetching an unknown path is refused", "[McpPrinterMedia]")
{
    const auto list = entries();
    CHECK_THROWS_AS(find_media_file(list, "/sdcard/model/missing.3mf"), ToolFailure);
    CHECK_THROWS_AS(find_media_file(list, ""), ToolFailure);
    CHECK_THROWS_AS(find_media_file({}, "a.3mf"), ToolFailure);
}

TEST_CASE("A file entry reports its path, size and time without secrets", "[McpPrinterMedia]")
{
    const json j = media_file_json(entries()[0], "model", "sd", true);
    CHECK(j["name"] == "a.3mf");
    CHECK(j["path"] == "/sdcard/model/a.3mf");
    CHECK(j["size"] == 100);
    CHECK(j["time"] == 1700000000);
    CHECK(j["time_utc"] == "2023-11-14T22:13:20Z");
    CHECK(j["storage"] == "sd");
    CHECK(j["has_thumbnail"] == true);

    // old firmware has no path: the name is the id
    const json old = media_file_json({"clip.mp4", "", 5, 0}, "video", "sd", true);
    CHECK(old["path"] == "clip.mp4");
}

TEST_CASE("Storage names map to the tunnel's argument", "[McpPrinterMedia]")
{
    CHECK(media_storage_arg("").empty());
    CHECK(media_storage_arg("sd").empty());
    CHECK(media_storage_arg("external").empty());
    CHECK(media_storage_arg("internal") == "internal");
    CHECK_THROWS_AS(media_storage_arg("../etc"), ToolFailure);
}

TEST_CASE("Printer file names with separators are not used as local names", "[McpPrinterMedia]")
{
    CHECK(is_plain_file_name("benchy.gcode.3mf"));
    CHECK_FALSE(is_plain_file_name(""));
    CHECK_FALSE(is_plain_file_name(".."));
    CHECK_FALSE(is_plain_file_name("a/b.3mf"));
    CHECK_FALSE(is_plain_file_name("a\\b.3mf"));
}

TEST_CASE("Tunnel result codes have readable texts", "[McpPrinterMedia]")
{
    CHECK_THAT(media_error_text(17), ContainsSubstring("SD card"));
    CHECK_THAT(media_error_text(5), ContainsSubstring("busy"));
    CHECK_THAT(media_error_text(9999), ContainsSubstring("9999"));
}

TEST_CASE("Storage is chosen from the names the printer lists", "[McpPrinterMedia]")
{
    std::string label;

    SECTION("no SD card: the first non-SD storage the printer lists")
    {
        CHECK(choose_media_storage({"emmc"}, false, "", label) == "emmc");
        CHECK(label == "emmc");
    }
    SECTION("SD card inserted and listed")
    {
        CHECK(choose_media_storage({"sdcard", "emmc"}, true, "", label) == "sdcard");
        CHECK(label == "sd");
    }
    SECTION("explicit names map onto the list")
    {
        CHECK(choose_media_storage({"sdcard", "emmc"}, true, "sd", label) == "sdcard");
        CHECK(choose_media_storage({"sdcard", "emmc"}, true, "internal", label) == "emmc");
        CHECK(choose_media_storage({"sdcard", "internal"}, false, "internal", label) == "internal");
    }
    SECTION("no answer from the printer keeps the old guess")
    {
        CHECK(choose_media_storage({}, true, "", label).empty());
        CHECK(label == "sd");
        CHECK(choose_media_storage({}, false, "", label) == "internal");
        CHECK(label == "internal");
    }
}
