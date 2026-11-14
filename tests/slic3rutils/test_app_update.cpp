#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <ios>
#include <istream>
#include <optional>
#include <sstream>
#include <streambuf>
#include <string>
#include <vector>

#include <openssl/sha.h>

#include "slic3r/Utils/AppUpdate.hpp"

using namespace Slic3r::AppUpdate;

namespace {

const std::string check_url = "https://api.github.com/repos/example-owner/example-repo/releases";
const std::string prefix    = "https://github.com/example-owner/example-repo/releases/download/";
const std::string installer = "OrcaSlicer_Windows_Installer_V9.8.7_x64.exe";
const std::string hex_a     = std::string(64, 'a');
const std::string hex_b     = std::string(64, 'b');
constexpr long long mb      = 1LL << 20;

ReleaseAsset asset(const std::string& name, long long size, const std::string& digest = {})
{
    return {name, prefix + "v9.8.7%2Bcustom/" + name, digest, size};
}

std::vector<ReleaseAsset> release()
{
    return {asset(installer, 90 * mb, "sha256:" + std::string(64, 'C')), asset(installer + ".sha256", 110),
            asset("OrcaSlicer_Windows_Installer_V9.8.7_arm64.exe", 90 * mb)};
}

// Gives some bytes, then fails the way a broken disk read does: the stream turns the exception into badbit.
class FailingBuf : public std::streambuf
{
public:
    FailingBuf() { setg(m_data, m_data, m_data + sizeof(m_data)); }

protected:
    int_type underflow() override { throw std::ios_base::failure("read failed"); }

private:
    char m_data[100] = {};
};

std::string to_hex(const unsigned char* digest, std::size_t length)
{
    static const char digits[] = "0123456789abcdef";
    std::string       hex;
    for (std::size_t i = 0; i < length; ++i) {
        hex.push_back(digits[digest[i] >> 4]);
        hex.push_back(digits[digest[i] & 0x0F]);
    }
    return hex;
}

} // namespace

TEST_CASE("The download prefix follows from a GitHub releases URL", "[AppUpdate]")
{
    CHECK(download_prefix_for(check_url) == prefix);
    CHECK(download_prefix_for(check_url + "/") == prefix);
    CHECK(download_prefix_for(check_url + "?per_page=10") == prefix);
    CHECK(download_prefix_for(check_url + "/?per_page=10") == prefix);
}

TEST_CASE("Any other check URL has no download prefix", "[AppUpdate]")
{
    CHECK(download_prefix_for("https://api.example.com/repos/example-owner/example-repo/releases").empty());
    CHECK(download_prefix_for("http://api.github.com/repos/example-owner/example-repo/releases").empty());
    CHECK(download_prefix_for("https://api.github.com/repos/example-owner/example-repo/extra/releases").empty());
    CHECK(download_prefix_for("https://api.github.com/repos/example-owner/releases").empty());
    CHECK(download_prefix_for("https://api.github.com/repos/example owner/example-repo/releases").empty());
    CHECK(download_prefix_for("").empty());
}

TEST_CASE("The installer for this build and its checksum are offered", "[AppUpdate]")
{
    const auto offer = choose_installer(release(), "_x64.exe", prefix);
    REQUIRE(offer.has_value());
    CHECK(offer->name == installer);
    CHECK(offer->url == prefix + "v9.8.7%2Bcustom/" + installer);
    CHECK(offer->sha256_url == prefix + "v9.8.7%2Bcustom/" + installer + ".sha256");
    CHECK(offer->size == 90 * mb);
    CHECK(offer->digest_hex == std::string(64, 'c'));
}

TEST_CASE("An installer without its checksum is not offered", "[AppUpdate]")
{
    std::vector<ReleaseAsset> assets = release();
    assets.erase(assets.begin() + 1);
    CHECK_FALSE(choose_installer(assets, "_x64.exe", prefix).has_value());
}

TEST_CASE("Two installers for the same architecture are not offered", "[AppUpdate]")
{
    std::vector<ReleaseAsset> assets = release();
    assets.push_back(asset("OrcaSlicer_Windows_Installer_V9.8.6_x64.exe", 90 * mb));
    assets.push_back(asset("OrcaSlicer_Windows_Installer_V9.8.6_x64.exe.sha256", 110));
    CHECK_FALSE(choose_installer(assets, "_x64.exe", prefix).has_value());
}

TEST_CASE("An asset outside the release downloads is not offered", "[AppUpdate]")
{
    SECTION("the installer elsewhere")
    {
        std::vector<ReleaseAsset> assets = release();
        assets[0].url                    = "https://downloads.example.com/" + installer;
        CHECK_FALSE(choose_installer(assets, "_x64.exe", prefix).has_value());
    }
    SECTION("the checksum elsewhere")
    {
        std::vector<ReleaseAsset> assets = release();
        assets[1].url                    = "https://github.com/other-owner/example-repo/releases/download/v1/" + installer + ".sha256";
        CHECK_FALSE(choose_installer(assets, "_x64.exe", prefix).has_value());
    }
    SECTION("a path that climbs out of the prefix")
    {
        std::vector<ReleaseAsset> assets = release();
        assets[0].url = prefix + "../../../../other-owner/other-repo/releases/download/v1/" + installer;
        CHECK_FALSE(choose_installer(assets, "_x64.exe", prefix).has_value());
        assets[0].url = prefix + "%2E%2E/other/" + installer;
        CHECK_FALSE(choose_installer(assets, "_x64.exe", prefix).has_value());
    }
}

TEST_CASE("Nothing is offered without a download prefix", "[AppUpdate]")
{
    CHECK_FALSE(choose_installer(release(), "_x64.exe", "").has_value());
}

TEST_CASE("The other architecture's installer is not offered", "[AppUpdate]")
{
    std::vector<ReleaseAsset> assets = release();
    assets.erase(assets.begin() + 2); // only the x64 installer and its checksum are left
    CHECK_FALSE(choose_installer(assets, "_arm64.exe", prefix).has_value());
}

TEST_CASE("An installer of implausible size is not offered", "[AppUpdate]")
{
    std::vector<ReleaseAsset> assets = release();
    assets[0].size                   = 10;
    CHECK_FALSE(choose_installer(assets, "_x64.exe", prefix).has_value());
    assets[0].size = 2 * 1024 * mb;
    CHECK_FALSE(choose_installer(assets, "_x64.exe", prefix).has_value());
}

TEST_CASE("A release digest in another form leaves the digest empty", "[AppUpdate]")
{
    std::vector<ReleaseAsset> assets = release();
    assets[0].digest                 = "md5:" + std::string(32, 'a');
    const auto offer                 = choose_installer(assets, "_x64.exe", prefix);
    REQUIRE(offer.has_value());
    CHECK(offer->digest_hex.empty());
}

TEST_CASE("A malformed SHA-256 release digest withdraws the offer", "[AppUpdate]")
{
    std::vector<ReleaseAsset> assets = release();
    assets[0].digest                 = "sha256:" + std::string(63, 'a');
    CHECK_FALSE(choose_installer(assets, "_x64.exe", prefix).has_value());
    assets[0].digest = "sha256:" + std::string(63, 'a') + "z";
    CHECK_FALSE(choose_installer(assets, "_x64.exe", prefix).has_value());
    assets[0].digest = "sha256:";
    CHECK_FALSE(choose_installer(assets, "_x64.exe", prefix).has_value());
    assets[0].digest = "";
    CHECK(choose_installer(assets, "_x64.exe", prefix).has_value());
}

TEST_CASE("An installer name or URL that could leave its folder withdraws the offer", "[AppUpdate]")
{
    SECTION("a name with a path separator")
    {
        for (const std::string name : {"sub/" + installer, "sub\\" + installer}) {
            std::vector<ReleaseAsset> assets = {asset(name, 90 * mb), asset(name + ".sha256", 110)};
            CHECK_FALSE(choose_installer(assets, "_x64.exe", prefix).has_value());
        }
    }
    SECTION("a URL with a dot-dot segment or an encoded separator")
    {
        for (const std::string tail : {"v1/../" + installer, "v1%2F" + installer, "v1%5c" + installer}) {
            std::vector<ReleaseAsset> assets = release();
            assets[0].url                    = prefix + tail;
            CHECK_FALSE(choose_installer(assets, "_x64.exe", prefix).has_value());
        }
    }
}

TEST_CASE("Installer sizes from 1 MB to 1 GB are accepted", "[AppUpdate]")
{
    std::vector<ReleaseAsset> assets = release();
    assets[0].size                   = mb;
    CHECK(choose_installer(assets, "_x64.exe", prefix).has_value());
    assets[0].size = mb - 1;
    CHECK_FALSE(choose_installer(assets, "_x64.exe", prefix).has_value());
    assets[0].size = 1024 * mb;
    CHECK(choose_installer(assets, "_x64.exe", prefix).has_value());
    assets[0].size = 1024 * mb + 1;
    CHECK_FALSE(choose_installer(assets, "_x64.exe", prefix).has_value());
}

TEST_CASE("The helper event name carries the process id", "[AppUpdate]")
{
    CHECK(helper_event_name(4242) == L"Local\\OrcaSlicerMcpStdio.4242");
    CHECK(helper_event_name(7) == L"Local\\OrcaSlicerMcpStdio.7");
}

TEST_CASE("A checksum file gives its digest", "[AppUpdate]")
{
    CHECK(parse_sha256_file(hex_a + "  " + installer + "\n", installer) == hex_a);
    CHECK(parse_sha256_file(std::string(64, 'A') + "  " + installer + "\r\n", installer) == hex_a);
    CHECK(parse_sha256_file(hex_a + " *" + installer, installer) == hex_a);
    CHECK(parse_sha256_file(hex_a, installer) == hex_a);
    CHECK(parse_sha256_file(hex_a + "\n", installer) == hex_a);
}

TEST_CASE("A checksum file that does not fit is refused", "[AppUpdate]")
{
    CHECK_FALSE(parse_sha256_file(hex_a + "  other.exe\n", installer).has_value());
    CHECK_FALSE(parse_sha256_file(std::string(63, 'a') + "  " + installer, installer).has_value());
    CHECK_FALSE(parse_sha256_file(std::string(65, 'a') + "  " + installer, installer).has_value());
    CHECK_FALSE(parse_sha256_file(std::string(63, 'a') + "g  " + installer, installer).has_value());
    CHECK_FALSE(parse_sha256_file("", installer).has_value());
}

TEST_CASE("The SHA-256 of a stream is its lower-case hex digest", "[AppUpdate]")
{
    std::istringstream empty("");
    CHECK(sha256_hex(empty) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    std::istringstream abc("abc");
    CHECK(sha256_hex(abc) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    // Longer than one read chunk.
    std::string bytes(200000, '\0');
    for (std::size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = char((i * 31 + 7) & 0xFF);
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), digest);
    std::istringstream stream(bytes);
    CHECK(sha256_hex(stream) == to_hex(digest, SHA256_DIGEST_LENGTH));
}

TEST_CASE("A stream that fails while being read has no SHA-256", "[AppUpdate]")
{
    FailingBuf   buffer;
    std::istream in(&buffer);
    CHECK_FALSE(sha256_hex(in).has_value());
}

TEST_CASE("A download is judged by its size and digests", "[AppUpdate]")
{
    CHECK(judge(90 * mb, hex_a, 90 * mb, hex_a, hex_a) == Verdict::Ok);
    CHECK(judge(90 * mb, hex_a, 90 * mb, hex_a, "") == Verdict::Ok);
    CHECK(judge(90 * mb - 1, hex_a, 90 * mb, hex_a, "") == Verdict::WrongSize);
    CHECK(judge(90 * mb, hex_a, 90 * mb, hex_b, "") == Verdict::WrongDigest);
    CHECK(judge(90 * mb, hex_a, 90 * mb, hex_a, hex_b) == Verdict::WrongDigest);
    CHECK(judge(90 * mb, std::nullopt, 90 * mb, hex_a, "") == Verdict::Unreadable);
    CHECK(judge(90 * mb, std::string(64, 'A'), 90 * mb, hex_a, std::string(64, 'A')) == Verdict::Ok);
    CHECK(judge(90 * mb, hex_a, 90 * mb, "", "") == Verdict::WrongDigest);
}
