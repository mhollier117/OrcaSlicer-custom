#include "Merge.hpp"

#include "Forms.hpp"
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

        // A setting whose type differs between the two slicers: the remote value is in Studio's form.
        const FormRule rule = in.forms != nullptr ? in.forms->rule(k) : FormRule::Same;
        if (rule == FormRule::Never) {
            if (l) out.merged[k] = *l; // not exchanged
            continue;
        }
        const bool translated = rule != FormRule::Same;
        // The local value as Studio would hold it (nullopt: Studio cannot hold it, which counts as different).
        const std::optional<std::string> ls = translated && l ? in.forms->to_studio(k, *l) : l;
        const bool                       same_as_remote = l ? (ls && r && *ls == *r) : !r;

        bool local_changed, remote_changed;
        if (in.pairing) {
            remote_changed = r.has_value();
            local_changed  = l && r && !same_as_remote;
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
        } else if (same_as_remote) {
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
        if (take_remote && translated && l && same_as_remote) {
            result = l; // the local value already is the remote one in Studio's view: translating back could lose list items
        } else if (take_remote) {
            const std::optional<std::string> taken = r && translated ? in.forms->to_orca(k, *r, l.value_or(std::string())) : r;
            if (taken) {
                result = taken;
                ++out.taken_remote;
            } else if (r) {
                result = l; // OrcaSlicer cannot take the remote value
                ++out.kept_local;
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

ValueMap apply_local_edits(const ValueMap& remote_now, const ValueMap& local, const ValueMap& local_base, const KeyTest& orca_knows,
                           const Forms* forms)
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
        const FormRule rule = forms != nullptr ? forms->rule(k) : FormRule::Same;
        if (rule == FormRule::Never) continue; // the body keeps what remote_now has
        if (!l) {
            out.erase(k);
        } else if (rule == FormRule::Same) {
            out[k] = *l;
        } else if (const auto studio_value = forms->to_studio(k, *l)) {
            out[k] = *studio_value;
        } // else Studio cannot hold the value: the body keeps what remote_now has
    }
    if (const auto t = find_value(local, "type")) out["type"] = *t;
    if (const auto v = find_value(remote_now, "version")) out["version"] = *v;
    else if (const auto lv = find_value(local, "version")) out["version"] = *lv;
    return out;
}

ValueMap upload_with_foreign(const ValueMap& local, const ValueMap& base, const KeyTest& orca_knows, const Forms* forms)
{
    ValueMap    out      = forms != nullptr ? forms->studio_body(local) : local;
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
