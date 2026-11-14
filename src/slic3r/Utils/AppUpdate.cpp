#include "AppUpdate.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <ios>
#include <istream>
#include <memory>
#include <regex>
#include <string>
#include <vector>

#include <boost/nowide/fstream.hpp>
#include <openssl/evp.h>

namespace Slic3r { namespace AppUpdate {

namespace {

constexpr long long min_installer_size = 1LL << 20; // 1 MB
constexpr long long max_installer_size = 1LL << 30; // 1 GB

bool ends_with(const std::string& text, const std::string& suffix)
{
    return text.size() >= suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_hex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

std::string lower(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return text;
}

// 64 hex digits, returned in lower case.
std::optional<std::string> sha256_text(const std::string& text)
{
    if (text.size() != 64 || !std::all_of(text.begin(), text.end(), is_hex))
        return std::nullopt;
    return lower(text);
}

// `url` starts with `prefix` and its remainder stays below it: no ".." segment, backslash, or an encoded dot,
// slash or backslash that a server could decode into one.
bool is_below(const std::string& url, const std::string& prefix)
{
    if (prefix.empty() || url.size() <= prefix.size() || url.compare(0, prefix.size(), prefix) != 0)
        return false;
    const std::string rest = lower(url.substr(prefix.size()));
    for (const char* bad : {"..", "\\", "%2e", "%2f", "%5c"})
        if (rest.find(bad) != std::string::npos)
            return false;
    return true;
}

// A name that can be used as a file name as it is: no path separators, drive colon or control characters.
bool is_plain_file_name(const std::string& name)
{
    if (name.empty() || name.front() == '.')
        return false;
    return std::none_of(name.begin(), name.end(), [](unsigned char c) {
        return c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
               c == '>' || c == '|';
    });
}

} // namespace

std::string download_prefix_for(const std::string& check_url)
{
    static const std::regex github_releases(R"(^https://api\.github\.com/repos/([A-Za-z0-9_.-]+)/([A-Za-z0-9_.-]+)/releases/?(\?.*)?$)");
    std::smatch match;
    if (!std::regex_match(check_url, match, github_releases))
        return {};
    return "https://github.com/" + match[1].str() + "/" + match[2].str() + "/releases/download/";
}

std::optional<InstallerOffer> choose_installer(const std::vector<ReleaseAsset>& assets, const std::string& arch_suffix,
                                               const std::string& prefix)
{
    if (prefix.empty() || arch_suffix.empty())
        return std::nullopt;

    const ReleaseAsset* installer = nullptr;
    for (const ReleaseAsset& asset : assets) {
        if (!ends_with(asset.name, arch_suffix) || asset.name.find("Installer") == std::string::npos)
            continue;
        if (installer != nullptr)
            return std::nullopt; // more than one: which one is meant is not clear
        installer = &asset;
    }
    if (installer == nullptr || installer->size < min_installer_size || installer->size > max_installer_size ||
        !is_plain_file_name(installer->name) || !is_below(installer->url, prefix))
        return std::nullopt;

    const std::string checksum_name = installer->name + ".sha256";
    const auto        checksum      = std::find_if(assets.begin(), assets.end(),
                                                   [&](const ReleaseAsset& asset) { return asset.name == checksum_name; });
    if (checksum == assets.end() || !is_below(checksum->url, prefix))
        return std::nullopt;

    InstallerOffer offer;
    offer.name       = installer->name;
    offer.url        = installer->url;
    offer.sha256_url = checksum->url;
    offer.size       = installer->size;
    const std::string digest_tag = "sha256:";
    if (installer->digest.compare(0, digest_tag.size(), digest_tag) == 0) {
        const auto hex = sha256_text(installer->digest.substr(digest_tag.size()));
        if (!hex)
            return std::nullopt; // a SHA-256 digest that cannot be checked is not the same as none
        offer.digest_hex = *hex;
    }
    return offer;
}

std::optional<std::string> parse_sha256_file(const std::string& text, const std::string& expected_name)
{
    const std::string line = text.substr(0, text.find('\n'));
    std::size_t       pos  = line.find_first_not_of(" \t");
    if (pos == std::string::npos)
        return std::nullopt;
    const std::size_t token_end = std::min(line.find_first_of(" \t\r", pos), line.size());
    const auto        hex       = sha256_text(line.substr(pos, token_end - pos));
    if (!hex)
        return std::nullopt;

    pos = line.find_first_not_of(" \t", token_end);
    if (pos != std::string::npos && line[pos] == '*')
        ++pos;
    std::string name = pos == std::string::npos ? std::string() : line.substr(pos);
    while (!name.empty() && (name.back() == '\r' || name.back() == ' ' || name.back() == '\t'))
        name.pop_back();
    if (!name.empty() && name != expected_name)
        return std::nullopt;
    return hex;
}

std::optional<std::string> sha256_hex(std::istream& in)
{
    if (!in)
        return std::nullopt;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!ctx || EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1)
        return std::nullopt;

    std::vector<char> chunk(1 << 16);
    while (in) {
        in.read(chunk.data(), std::streamsize(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got > 0 && EVP_DigestUpdate(ctx.get(), chunk.data(), std::size_t(got)) != 1)
            return std::nullopt;
    }
    if (in.bad())
        return std::nullopt;

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  length = 0;
    if (EVP_DigestFinal_ex(ctx.get(), digest, &length) != 1)
        return std::nullopt;
    static const char digits[] = "0123456789abcdef";
    std::string       hex;
    hex.reserve(length * 2);
    for (unsigned int i = 0; i < length; ++i) {
        hex.push_back(digits[digest[i] >> 4]);
        hex.push_back(digits[digest[i] & 0x0F]);
    }
    return hex;
}

std::optional<std::string> sha256_hex(const std::string& path_utf8)
{
    boost::nowide::ifstream file(path_utf8.c_str(), std::ios::binary);
    if (!file)
        return std::nullopt;
    return sha256_hex(file);
}

std::wstring helper_event_name(unsigned long pid)
{
    return L"Local\\OrcaSlicerMcpStdio." + std::to_wstring(pid);
}

Verdict judge(long long actual_size, const std::optional<std::string>& actual_hex, long long expected_size,
              const std::string& expected_hex, const std::string& asset_digest_hex)
{
    if (!actual_hex)
        return Verdict::Unreadable;
    if (actual_size != expected_size)
        return Verdict::WrongSize;
    const std::string actual = lower(*actual_hex);
    if (expected_hex.empty() || actual != lower(expected_hex))
        return Verdict::WrongDigest;
    if (!asset_digest_hex.empty() && actual != lower(asset_digest_hex))
        return Verdict::WrongDigest;
    return Verdict::Ok;
}

}} // namespace Slic3r::AppUpdate
