#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <regex>
#include <string>

#include "libslic3r/Semver.hpp"
#include "libslic3r_version.h"
#include "slic3r/Utils/CustomVersion.hpp"

using Slic3r::compose_custom_version;
using Slic3r::custom_build_version;
using Slic3r::display_version_of_tag;
using Slic3r::four_part_version;
using Slic3r::Semver;

namespace {

Semver parsed(const std::string& text)
{
    const auto version = Semver::parse(text);
    REQUIRE(version.has_value());
    return *version;
}

} // namespace

TEST_CASE("A version with three numbers gets a fourth", "[CustomVersion]")
{
    CHECK(four_part_version("2.5.1") == "2.5.1.0");
    CHECK(four_part_version("2.6.0-rc") == "2.6.0.0-rc");
    CHECK(four_part_version("2.5.1+custom") == "2.5.1.0+custom");
}

TEST_CASE("A version without exactly three numbers is left as it is", "[CustomVersion]")
{
    CHECK(four_part_version("2.5.0.2") == "2.5.0.2");
    CHECK(four_part_version("2.5") == "2.5");
    CHECK(four_part_version("obn-bundle") == "obn-bundle");
    CHECK(four_part_version("") == "");
}

TEST_CASE("The custom version puts the revision fourth", "[CustomVersion]")
{
    CHECK(compose_custom_version("2.5.0", "2") == "2.5.0.2");
    CHECK(compose_custom_version("2.6.0-rc", "3") == "2.6.0.3-rc");
}

TEST_CASE("A missing or non-numeric revision gives the four-part base", "[CustomVersion]")
{
    CHECK(compose_custom_version("2.5.0", "") == "2.5.0.0");
    CHECK(compose_custom_version("2.5.0", "x") == "2.5.0.0");
    CHECK(compose_custom_version("2.5", "2") == "2.5");
}

TEST_CASE("A release tag is shown without its v and build metadata", "[CustomVersion]")
{
    CHECK(display_version_of_tag("v2.5.0.2+custom") == "2.5.0.2");
    CHECK(display_version_of_tag("2.5.1") == "2.5.1");
    CHECK(display_version_of_tag("v2.6.0-rc+custom") == "2.6.0-rc");
}

TEST_CASE("Four-part versions order releases by upstream version, then revision", "[CustomVersion]")
{
    CHECK(parsed("2.5.0.2") > parsed(four_part_version("2.5.0")));
    CHECK(parsed(four_part_version("2.5.1")) > parsed("2.5.0.2"));
    CHECK(parsed("2.5.0.3") > parsed("2.5.0.2"));
    CHECK_FALSE(parsed("2.5.0.2") > parsed("2.5.0.2"));
    CHECK(parsed(four_part_version("2.5.1+custom")) > parsed("2.5.0.99"));
}

TEST_CASE("An installed build without the revision still sees a revised release as newer", "[CustomVersion]")
{
    // Such a build compares the unpadded tag with its unpadded version.
    CHECK(parsed("2.5.0.2") > parsed("2.5.0"));
}

TEST_CASE("The build version is the upstream version with this build's revision", "[CustomVersion]")
{
    const std::string upstream = SoftFever_VERSION;
    std::smatch       numbers;
    REQUIRE(std::regex_search(upstream, numbers, std::regex("^[0-9]+\\.[0-9]+\\.[0-9]+")));

    const std::string version = custom_build_version();
    CHECK_THAT(version, Catch::Matchers::StartsWith(numbers.str()));
    CHECK_THAT(version, Catch::Matchers::EndsWith("." ORCA_CUSTOM_REVISION));
}
