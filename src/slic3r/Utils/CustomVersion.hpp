#pragma once

#include <string>

namespace Slic3r {

// This fork's releases carry a revision (custom_version.inc) on top of the upstream version, as a fourth
// number: "2.5.0.2", tagged "v2.5.0.2+custom". The version parser folds a fourth number into the patch
// (patch * 100 + revision), so a three-part and a four-part version do not compare correctly; the update
// check therefore compares four-part forms on both sides.

// A version text with exactly three leading numbers gets ".0" after the third ("2.5.1" -> "2.5.1.0",
// "2.6.0-rc" -> "2.6.0.0-rc", "2.5.1+custom" -> "2.5.1.0+custom"). Two or four numbers, or anything that does
// not start with numbers and dots, is returned unchanged.
std::string four_part_version(const std::string& version);

// The full version of this fork's build: `base` with `revision` as its fourth number ("2.5.0", "2" -> "2.5.0.2";
// "2.6.0-rc", "3" -> "2.6.0.3-rc"). An empty or non-numeric revision, or a base that does not have exactly three
// leading numbers, gives four_part_version(base).
std::string compose_custom_version(const std::string& base, const std::string& revision);

// compose_custom_version(SoftFever_VERSION, ORCA_CUSTOM_REVISION).
std::string custom_build_version();

// For showing a release tag: without a leading 'v' and without the "+..." part ("v2.5.0.2+custom" -> "2.5.0.2").
std::string display_version_of_tag(const std::string& tag);

} // namespace Slic3r
