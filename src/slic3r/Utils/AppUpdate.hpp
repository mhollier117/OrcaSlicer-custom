#pragma once

#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

// The parts of the in-app update that need neither wx nor the network: which release asset is this build's
// installer, the published checksum, and whether a downloaded file matches it. See docs/HLSD/app-update.md.
namespace Slic3r { namespace AppUpdate {

// One entry of a GitHub release's "assets" array.
struct ReleaseAsset
{
    std::string name;
    std::string url;    // browser_download_url
    std::string digest; // "sha256:<hex>" or empty
    long long   size{0};
};

// The installer offered for this build, and where its checksum is published.
struct InstallerOffer
{
    std::string name;
    std::string url;
    std::string sha256_url;
    std::string digest_hex; // from the release JSON, lower case; empty when the release does not give one
    long long   size{0};
};

// "https://api.github.com/repos/<owner>/<repo>/releases" (an optional trailing slash or "?..." is accepted)
// -> "https://github.com/<owner>/<repo>/releases/download/"; empty for anything else. Owner and repo: [A-Za-z0-9_.-]+.
std::string download_prefix_for(const std::string& check_url);

// The installer for this build among a release's assets: exactly one asset whose name ends with `arch_suffix`
// ("_x64.exe" or "_arm64.exe") and contains "Installer", with a size from 1 MB to 1 GB, plus the asset named
// <that name>.sha256; both URLs must start with `prefix` and `prefix` must not be empty. Otherwise nullopt.
// A URL whose remainder could leave the prefix (a ".." segment, a backslash, an encoded dot, slash or backslash)
// does not count as starting with it, and the name must be a plain file name.
std::optional<InstallerOffer> choose_installer(const std::vector<ReleaseAsset>& assets, const std::string& arch_suffix,
                                               const std::string& prefix);

// The digest in a .sha256 file: the first token must be 64 hex digits (returned in lower case); when a file name
// follows (after spaces and an optional '*'), it must equal `expected_name`. Otherwise nullopt.
std::optional<std::string> parse_sha256_file(const std::string& text, const std::string& expected_name);

// SHA-256 of a stream / of a file, lower-case hex; nullopt when it cannot be read. Reads in chunks.
std::optional<std::string> sha256_hex(std::istream& in);
std::optional<std::string> sha256_hex(const std::string& path_utf8);

enum class Verdict { Ok, WrongSize, WrongDigest, Unreadable };

// `expected_hex` from the .sha256 file; `asset_digest_hex` from the release JSON, or empty.
// Hex digits are compared without regard to case.
Verdict judge(long long actual_size, const std::optional<std::string>& actual_hex, long long expected_size,
              const std::string& expected_hex, const std::string& asset_digest_hex);

}} // namespace Slic3r::AppUpdate
