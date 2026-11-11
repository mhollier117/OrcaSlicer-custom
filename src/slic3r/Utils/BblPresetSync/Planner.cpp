#include "Planner.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace Slic3r::BblPresetSync {

bool want_body(const Key& row_key, const std::string& row_setting_id, long long row_update_time, const LedgerView& ledger)
{
    for (const auto& kv : ledger.entries)
        if (kv.second.setting_id == row_setting_id) return !(kv.first == row_key) || kv.second.update_time < row_update_time;
    return true;
}

namespace {

class Planner
{
public:
    Planner(const std::vector<LocalPreset>& locals, const std::vector<RemoteRow>& remote, const LedgerView& ledger, long long now)
        : m_ledger(ledger), m_now(now), m_remote_empty(remote.empty())
    {
        for (const LocalPreset& l : locals) m_locals[l.key] = &l;
        reduce_duplicates(remote);
    }

    Plan run()
    {
        step_a();
        step_b();
        step_c();
        step_d();
        m_plan.dry_run = !m_ledger.first_run_confirmed;
        return std::move(m_plan);
    }

private:
    const LedgerView&                       m_ledger;
    long long                               m_now;
    bool                                    m_remote_empty;
    std::map<Key, const LocalPreset*>       m_locals;
    std::vector<RemoteRow>                  m_rows; // after duplicate reduction
    std::map<std::string, const RemoteRow*> m_row_by_id;
    std::map<Key, const RemoteRow*>         m_free_row_at_key; // a row whose id is in no ledger entry
    std::set<Key>                           m_row_keys;
    std::set<std::string>                   m_handled;  // ids consumed by step A
    std::set<std::string>                   m_released; // ids released to step B by A.3
    std::set<Key>                           m_relink;   // keys whose free row step B pairs despite the entry
    std::set<Key>                           m_used;     // keys that already have an action
    Plan                                    m_plan;

    void reduce_duplicates(const std::vector<RemoteRow>& remote)
    {
        std::set<std::string> ledger_ids;
        for (const auto& kv : m_ledger.entries) ledger_ids.insert(kv.second.setting_id);

        std::map<Key, std::vector<const RemoteRow*>> by_key;
        for (const RemoteRow& r : remote) by_key[r.key].push_back(&r);

        for (const auto& [key, rows] : by_key) {
            const RemoteRow* best = rows.front();
            for (const RemoteRow* r : rows)
                if (r->update_time > best->update_time || (r->update_time == best->update_time && r->setting_id > best->setting_id)) best = r;
            if (rows.size() > 1) m_plan.notes.push_back("duplicate remote rows for " + key.str());
            for (const RemoteRow* r : rows)
                if (r == best || ledger_ids.count(r->setting_id)) m_rows.push_back(*r);
        }
        for (const RemoteRow& r : m_rows) {
            m_row_by_id.emplace(r.setting_id, &r);
            m_row_keys.insert(r.key);
            if (!ledger_ids.count(r.setting_id)) m_free_row_at_key.emplace(r.key, &r);
        }
    }

    LocalState local_state(const Key& k) const
    {
        auto it = m_locals.find(k);
        return it == m_locals.end() ? LocalState::Missing : it->second->state;
    }
    const LocalPreset* local(const Key& k) const
    {
        auto it = m_locals.find(k);
        return it == m_locals.end() ? nullptr : it->second;
    }
    const RemoteRow* row(const std::string& id) const
    {
        auto it = m_row_by_id.find(id);
        return it == m_row_by_id.end() ? nullptr : it->second;
    }
    bool local_wins(const LocalPreset& l, const RemoteRow& r) const
    {
        return l.mtime + m_ledger.clock_offset + TIE_WINDOW_SECONDS >= r.update_time;
    }

    void emit(Action a)
    {
        if (m_used.count(a.key) || (a.rename_from_set && m_used.count(a.rename_from))) {
            m_plan.notes.push_back("skipped a second action for " + a.key.str());
            return;
        }
        m_used.insert(a.key);
        if (a.rename_from_set) m_used.insert(a.rename_from);
        m_plan.actions.push_back(std::move(a));
    }

    static Action make(ActionType type, const Key& key, const std::string& setting_id = "")
    {
        Action a;
        a.type       = type;
        a.key        = key;
        a.setting_id = setting_id;
        return a;
    }

    void body_unavailable(const Key& key) { m_plan.notes.push_back("body unavailable for " + key.str()); }

    void release(const std::string& id)
    {
        m_handled.erase(id);
        m_released.insert(id);
    }

    void pull(const Key& key, const std::string& id, const RemoteRow& r, bool backup_local)
    {
        if (!r.has_body) {
            body_unavailable(r.key);
            return;
        }
        Action a       = make(ActionType::Pull, key, id);
        a.backup_local = backup_local;
        emit(std::move(a));
    }

    // Both sides changed: the mirror merges setting by setting; the newer side wins a setting changed on both.
    void merge_pull(const Key& key, const std::string& id, const RemoteRow& r, const LocalPreset& l)
    {
        if (!r.has_body) {
            body_unavailable(r.key);
            return;
        }
        Action a         = make(ActionType::Pull, key, id);
        a.backup_local   = true;
        a.backup_remote  = true;
        a.merge_conflict = true;
        a.local_newer    = local_wins(l, r);
        emit(std::move(a));
    }

    void step_a()
    {
        for (const auto& [key, e] : m_ledger.entries) {
            const RemoteRow* r = row(e.setting_id);
            m_handled.insert(e.setting_id);

            if (e.state == "declined") continue;

            const LocalPreset* l  = local(key);
            const LocalState   ls = l ? l->state : LocalState::Missing;

            if (e.state == "too_large") {
                if (ls == LocalState::Eligible && l->hash != e.synced_hash)
                    emit(r ? make(ActionType::Update, key, e.setting_id) : make(ActionType::Create, key));
                continue;
            }
            if (e.state == "unresolvable") {
                if (r && !(r->key == key)) {
                    emit(make(ActionType::DropEntry, key, e.setting_id));
                    release(e.setting_id);
                } else if (r && r->update_time > e.update_time) {
                    release(e.setting_id);
                }
                continue;
            }
            if (e.state != "linked") continue;

            if (ls == LocalState::Ineligible) continue;

            if (r && !(r->key == key)) { // renamed remotely
                if (ls == LocalState::Missing) {
                    emit(make(ActionType::DropEntry, key, e.setting_id));
                    release(e.setting_id);
                } else if (local_state(r->key) == LocalState::Missing) {
                    if (!r->has_body) {
                        body_unavailable(r->key);
                        continue;
                    }
                    Action a          = make(ActionType::Pull, r->key, r->setting_id);
                    a.rename_from_set = true;
                    a.rename_from     = key;
                    a.backup_local    = true;
                    emit(std::move(a));
                } else {
                    Action a = make(ActionType::MarkUnresolvable, key, "");
                    a.reason = "remote renamed onto an existing name";
                    emit(std::move(a));
                    release(e.setting_id);
                }
                continue;
            }

            if (!r && m_free_row_at_key.count(key)) { // the row was re-created under another id
                m_relink.insert(key);
                continue;
            }

            const bool lc = ls == LocalState::Eligible && l->hash != e.synced_hash;
            const bool rc = r && r->update_time > e.update_time;

            if (ls == LocalState::Eligible && r) {
                if (!lc && !rc) {
                    if (e.absent_passes != 0 || e.absent_since != 0) {
                        Action a        = make(ActionType::SetAbsent, key, e.setting_id);
                        a.absent_passes = 0;
                        a.absent_since  = 0;
                        emit(std::move(a));
                    }
                } else if (lc && !rc) {
                    emit(make(ActionType::Update, key, e.setting_id));
                } else if (!lc && rc) {
                    pull(key, e.setting_id, *r, false);
                } else { // conflict
                    if (!r->has_body) {
                        body_unavailable(key);
                    } else {
                        merge_pull(key, e.setting_id, *r, *l);
                    }
                }
            } else if (ls == LocalState::Missing && r) {
                if (!rc) emit(make(ActionType::DeleteRemote, key, e.setting_id));
                else pull(key, e.setting_id, *r, false);
            } else if (ls == LocalState::Eligible) { // row absent
                const int       n     = e.absent_passes + 1;
                const long long since = e.absent_since != 0 ? e.absent_since : m_now;
                if (n >= ABSENT_PASSES_TO_DELETE && m_now - since >= ABSENT_MIN_SECONDS) {
                    if (!lc) {
                        Action a       = make(ActionType::DeleteLocal, key, e.setting_id);
                        a.backup_local = true;
                        emit(std::move(a));
                    } else {
                        emit(make(ActionType::Create, key));
                    }
                } else {
                    Action a        = make(ActionType::SetAbsent, key, e.setting_id);
                    a.absent_passes = n;
                    a.absent_since  = since;
                    emit(std::move(a));
                }
            } else { // local missing, row absent
                emit(make(ActionType::DropEntry, key, e.setting_id));
            }
        }
    }

    void step_b()
    {
        std::vector<const RemoteRow*> rows;
        for (const RemoteRow& r : m_rows)
            if (!m_handled.count(r.setting_id)) rows.push_back(&r);
        std::sort(rows.begin(), rows.end(), [](const RemoteRow* a, const RemoteRow* b) {
            if (!(a->key == b->key)) return a->key < b->key;
            return a->setting_id < b->setting_id;
        });

        for (const RemoteRow* r : rows) {
            const Key& key = r->key;
            auto       it  = m_ledger.entries.find(key);
            if (it != m_ledger.entries.end() && !m_relink.count(key) &&
                !(it->second.setting_id == r->setting_id && m_released.count(r->setting_id))) {
                m_plan.notes.push_back("unlinked remote row for " + key.str());
                continue;
            }
            const LocalPreset* l  = local(key);
            const LocalState   ls = l ? l->state : LocalState::Missing;
            if (ls == LocalState::Ineligible) continue;
            if (ls == LocalState::Missing) {
                pull(key, r->setting_id, *r, false);
                continue;
            }
            if (!r->has_body) {
                body_unavailable(key);
                continue;
            }
            if (r->body_hash == l->hash) {
                emit(make(ActionType::Link, key, r->setting_id));
            } else {
                merge_pull(key, r->setting_id, *r, *l);
            }
        }
    }

    void step_c()
    {
        for (const auto& [key, l] : m_locals) {
            if (l->state != LocalState::Eligible) continue;
            if (m_ledger.entries.count(key) || m_row_keys.count(key)) continue;
            emit(make(ActionType::Create, key));
        }
    }

    void step_d()
    {
        size_t linked = 0;
        for (const auto& kv : m_ledger.entries)
            if (kv.second.state == "linked") ++linked;

        auto take = [&](ActionType type, std::vector<Key>& halted_keys) {
            for (const Action& a : m_plan.actions)
                if (a.type == type) halted_keys.push_back(a.key);
            m_plan.actions.erase(std::remove_if(m_plan.actions.begin(), m_plan.actions.end(),
                                                [type](const Action& a) { return a.type == type; }),
                                 m_plan.actions.end());
        };
        auto count = [&](ActionType type) {
            size_t n = 0;
            for (const Action& a : m_plan.actions)
                if (a.type == type) ++n;
            return n;
        };

        // An empty list is never believed as "everything was deleted".
        if (m_remote_empty && count(ActionType::DeleteLocal) > 0) {
            take(ActionType::DeleteLocal, m_plan.halted_local_deletes);
            m_plan.halt = "empty_list";
        }

        auto stop = [&](ActionType type, std::vector<Key>& halted_keys) {
            const size_t n = count(type);
            if (n < static_cast<size_t>(MASS_DELETE_MIN) || n * 2 < linked) return;
            take(type, halted_keys);
            if (m_plan.halt.empty()) m_plan.halt = "mass_delete";
        };
        stop(ActionType::DeleteLocal, m_plan.halted_local_deletes);
        stop(ActionType::DeleteRemote, m_plan.halted_remote_deletes);
    }
};

} // namespace

Plan make_plan(const std::vector<LocalPreset>& locals, const std::vector<RemoteRow>& remote, const LedgerView& ledger, long long now)
{
    return Planner(locals, remote, ledger, now).run();
}

} // namespace Slic3r::BblPresetSync
