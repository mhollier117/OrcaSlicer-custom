#include "Merge.hpp"

#include "Hash.hpp"

#include <optional>
#include <set>

namespace Slic3r::BblPresetSync {

namespace {

std::optional<std::string> find_value(const ValueMap& m, const std::string& key)
{
    auto it = m.find(key);
    if (it == m.end()) return std::nullopt;
    return it->second;
}

} // namespace

MergeResult merge_bodies(const ValueMap& local, const ValueMap& remote, const ValueMap& base, bool local_newer, const KeyTest& orca_knows,
                         const KeyTest& studio_knows)
{
    MergeResult           out;
    const auto&           metadata = hash_ignored_keys();
    std::set<std::string> keys;
    for (const ValueMap* m : {&local, &remote, &base})
        for (const auto& kv : *m)
            if (!metadata.count(kv.first)) keys.insert(kv.first);

    for (const std::string& k : keys) {
        if (!orca_knows(k)) continue;
        const auto l = find_value(local, k), r = find_value(remote, k), b = find_value(base, k);

        std::optional<std::string> result;
        const bool                 remote_changed = r != b;
        const bool                 local_changed  = l != b;
        bool                       take_remote    = false;
        if (!remote_changed) {
            result = l;
        } else if (!local_changed) {
            take_remote = true;
        } else if (l == r) {
            result = l;
        } else {
            ++out.conflicts;
            if (local_newer) {
                result = l;
                ++out.kept_local;
            } else {
                take_remote = true;
            }
        }
        if (take_remote) {
            if (r) {
                result = r;
                ++out.taken_remote;
            } else if (studio_knows(k)) {
                result.reset(); // the preset inherits it
                ++out.taken_remote;
            } else {
                result = l;
                ++out.kept_local;
                if (l) out.kept_unknown_to_studio.push_back(k);
            }
        }
        if (result) out.merged[k] = *result;

        if ((studio_knows(k) || r) && result != r) out.needs_upload = true;
    }
    return out;
}

ValueMap upload_with_foreign(const ValueMap& local, const ValueMap& base, const KeyTest& orca_knows)
{
    ValueMap    out      = local;
    const auto& metadata = hash_ignored_keys();
    for (const auto& [k, v] : base)
        if (!metadata.count(k) && !orca_knows(k)) out.emplace(k, v);
    return out;
}

std::string fingerprint(const ValueMap& values, const ValueMap& base, const KeyTest& orca_knows, const KeyTest& studio_knows)
{
    ValueMap kept;
    for (const auto& [k, v] : values)
        if (!(orca_knows(k) && !studio_knows(k) && !base.count(k))) kept.emplace(k, v);
    return upload_hash(kept);
}

} // namespace Slic3r::BblPresetSync
