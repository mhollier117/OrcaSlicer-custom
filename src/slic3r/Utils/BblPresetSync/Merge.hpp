#pragma once

#include "Types.hpp"

#include <functional>
#include <string>
#include <vector>

namespace Slic3r::BblPresetSync {

using KeyTest = std::function<bool(const std::string&)>;

struct MergeResult
{
    ValueMap merged;                // what the local preset should hold: Orca-known, non-metadata keys only
    int      taken_remote{0};       // keys whose value came from R
    int      kept_local{0};         // keys that stayed as in L although R differs (Studio could not hold it, or a conflict won locally)
    int      conflicts{0};          // keys changed on both sides to different values
    std::vector<std::string> kept_unknown_to_studio; // keys R dropped that Studio does not know (kept from L)
    // True when `merged` differs from R on a key Bambu can hold: some k with orca_knows(k), and (studio_knows(k) or k is in R),
    // and merged's value for k (or its absence) differs from R's. Then the merged preset must be uploaded.
    bool needs_upload{false};
};

// Three-way merge of the local upload map L, the Bambu body R and the base B (last body known to be on Bambu).
MergeResult merge_bodies(const ValueMap& local, const ValueMap& remote, const ValueMap& base, bool local_newer, const KeyTest& orca_knows,
                         const KeyTest& studio_knows);

// What to upload: the local map, plus every non-metadata key of `base` that OrcaSlicer does not know
// (settings only Studio understands are passed through unchanged). Local wins on a key in both.
ValueMap upload_with_foreign(const ValueMap& local, const ValueMap& base, const KeyTest& orca_knows);

// Fingerprint for change detection: upload_hash() of `values` after dropping every key that OrcaSlicer knows,
// Studio does not know, and `base` does not contain.
std::string fingerprint(const ValueMap& values, const ValueMap& base, const KeyTest& orca_knows, const KeyTest& studio_knows);

} // namespace Slic3r::BblPresetSync
