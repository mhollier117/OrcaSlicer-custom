#include "CustomVersion.hpp"

#include <cctype>
#include <cstddef>
#include <string>

#include "libslic3r_version.h"

namespace Slic3r {

namespace {

bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

// The number of leading dot-separated numbers in `version`, and in `end` the position just after the last one.
int leading_numbers(const std::string& version, std::size_t& end)
{
    int         count = 0;
    std::size_t pos   = 0;
    end               = 0;
    while (pos < version.size() && is_digit(version[pos])) {
        while (pos < version.size() && is_digit(version[pos]))
            ++pos;
        ++count;
        end = pos;
        if (pos + 1 < version.size() && version[pos] == '.' && is_digit(version[pos + 1]))
            ++pos;
        else
            break;
    }
    return count;
}

} // namespace

std::string four_part_version(const std::string& version)
{
    std::size_t end = 0;
    if (leading_numbers(version, end) != 3)
        return version;
    std::string result = version;
    result.insert(end, ".0");
    return result;
}

std::string compose_custom_version(const std::string& base, const std::string& revision)
{
    bool numeric = !revision.empty();
    for (char c : revision)
        if (!is_digit(c))
            numeric = false;
    std::size_t end = 0;
    if (!numeric || leading_numbers(base, end) != 3)
        return four_part_version(base);
    std::string result = base;
    result.insert(end, "." + revision);
    return result;
}

std::string custom_build_version() { return compose_custom_version(SoftFever_VERSION, ORCA_CUSTOM_REVISION); }

std::string display_version_of_tag(const std::string& tag)
{
    std::string result = tag;
    if (!result.empty() && result.front() == 'v')
        result.erase(0, 1);
    const std::size_t plus = result.find('+');
    if (plus != std::string::npos)
        result.erase(plus);
    return result;
}

} // namespace Slic3r
