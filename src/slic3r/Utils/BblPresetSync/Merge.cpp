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

MergeResult merge_bodies(const MergeInput& in)
{
    MergeResult           out;
    const auto&           metadata = hash_ignored_keys();
    std::set<std::string> keys;
    for (const ValueMap* m : {&in.local, &in.local_base, &in.remote, &in.remote_base})
        for (const auto& kv : *m)
            if (!metadata.count(kv.first)) keys.insert(kv.first);

    for (const std::string& k : keys) {
        if (!in.orca_knows(k)) continue;
        const auto l = find_value(in.local, k), lb = find_value(in.local_base, k), r = find_value(in.remote, k),
                   rb = find_value(in.remote_base, k);

        bool local_changed, remote_changed;
        if (in.pairing) {
            remote_changed = r.has_value();
            local_changed  = l && r && *l != *r;
        } else {
            local_changed  = l != lb;
            remote_changed = r != rb;
        }

        std::optional<std::string> result;
        bool                       take_remote = false;
        if (!local_changed && !remote_changed) {
            result = l;
        } else if (!local_changed) {
            take_remote = true;
        } else if (!remote_changed) {
            result = l;
            out.pending_local.insert(k);
        } else if (l == r) {
            result = l;
        } else {
            ++out.conflicts;
            if (in.local_newer) {
                result = l;
                out.pending_local.insert(k);
                ++out.kept_local;
            } else {
                take_remote = true;
            }
        }
        if (take_remote) {
            if (r) {
                result = r;
                ++out.taken_remote;
            } else if (in.studio_knows(k)) {
                result.reset(); // the preset inherits it
                ++out.taken_remote;
            } else {
                result = l;
                ++out.kept_local;
                if (l) out.kept_unknown_to_studio.push_back(k);
            }
        }
        if (result) out.merged[k] = *result;
    }
    return out;
}

ValueMap apply_local_edits(const ValueMap& remote_now, const ValueMap& local, const ValueMap& local_base, const KeyTest& orca_knows)
{
    // The envelope fields of the downloaded body (ids, times, name...) never go back up.
    const auto& metadata = hash_ignored_keys();
    ValueMap    out;
    for (const auto& kv : remote_now)
        if (!metadata.count(kv.first)) out.insert(kv);
    std::set<std::string> keys;
    for (const ValueMap* m : {&local, &local_base})
        for (const auto& kv : *m)
            if (!metadata.count(kv.first)) keys.insert(kv.first);
    for (const std::string& k : keys) {
        if (!orca_knows(k)) continue;
        const auto l = find_value(local, k);
        if (l == find_value(local_base, k)) continue;
        if (l) out[k] = *l;
        else out.erase(k);
    }
    if (const auto t = find_value(local, "type")) out["type"] = *t;
    if (const auto v = find_value(remote_now, "version")) out["version"] = *v;
    else if (const auto lv = find_value(local, "version")) out["version"] = *lv;
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
