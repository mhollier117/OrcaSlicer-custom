#pragma once

#include "Types.hpp"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace Slic3r::BblPresetSync {

class Forms;

using KeyTest = std::function<bool(const std::string&)>;

struct MergeInput
{
    const ValueMap& local;       // OrcaSlicer's upload map now (empty when the preset does not exist locally)
    const ValueMap& local_base;  // OrcaSlicer's upload map at the last sync
    const ValueMap& remote;      // the body on Bambu now
    const ValueMap& remote_base; // the body on Bambu at the last sync
    bool            pairing;     // first meeting of two same-named presets: there are no bases
    bool            local_newer; // which side wins a key changed on both
    KeyTest         orca_knows, studio_knows;
    // Settings whose type differs between the two slicers: a value crossing over is translated, one the other side cannot
    // hold stays where it is. Null: every value crosses unchanged.
    const Forms* forms{nullptr};
};

struct MergeResult
{
    ValueMap              merged;        // what the local preset should hold: Orca-known, non-metadata keys
    std::set<std::string> pending_local; // keys whose local edit must still be uploaded
    int                   taken_remote{0}, kept_local{0}, conflicts{0};
    std::vector<std::string> kept_unknown_to_studio; // keys R dropped that Studio does not know (kept from L)
};

// Exchanges edits: a key changed locally when local != local_base, remotely when remote != remote_base.
MergeResult merge_bodies(const MergeInput& in);

// The body to upload: `remote_now` (every key of the Bambu copy, including the ones OrcaSlicer does not know, but none of the
// envelope fields in hash_ignored_keys()) with the local edits applied: for every non-metadata key k that OrcaSlicer knows and where local[k] != local_base[k],
// set it to local[k], or remove it when local has no such key. Then `type` is set from `local` (always), and `version`
// from `remote_now` when it has one, else from `local`. With `forms`, an edited key with a rule is written in Studio's form;
// when Studio cannot hold the value (or the rule is Never) the body keeps what `remote_now` has.
ValueMap apply_local_edits(const ValueMap& remote_now, const ValueMap& local, const ValueMap& local_base, const KeyTest& orca_knows,
                           const Forms* forms = nullptr);

// What to upload for a preset paired by fingerprint: the local map (in Studio's form with `forms`), plus every non-metadata
// key of `base` that OrcaSlicer does not know. Local wins on a key in both.
ValueMap upload_with_foreign(const ValueMap& local, const ValueMap& base, const KeyTest& orca_knows, const Forms* forms = nullptr);

// Fingerprint for pairing: upload_hash() of `values` after dropping every key that OrcaSlicer knows,
// Studio does not know, and `base` does not contain.
std::string fingerprint(const ValueMap& values, const ValueMap& base, const KeyTest& orca_knows, const KeyTest& studio_knows);

} // namespace Slic3r::BblPresetSync
