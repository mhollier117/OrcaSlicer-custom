#include "Mirror.hpp"

#include "BodyCheck.hpp"
#include "Hash.hpp"
#include "Merge.hpp"
#include "Planner.hpp"

#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace Slic3r::BblPresetSync {

namespace fs = boost::filesystem;

namespace {

const char* action_name(ActionType t)
{
    switch (t) {
    case ActionType::Create: return "create";
    case ActionType::Update: return "update";
    case ActionType::DeleteRemote: return "delete_remote";
    case ActionType::Pull: return "pull";
    case ActionType::DeleteLocal: return "delete_local";
    case ActionType::Link: return "link";
    case ActionType::DropEntry: return "drop_entry";
    case ActionType::MarkUnresolvable: return "mark_unresolvable";
    case ActionType::SetAbsent: return "set_absent";
    }
    return "";
}

const char* body_status_name(BodyStatus s)
{
    switch (s) {
    case BodyStatus::Valid: return "valid";
    case BodyStatus::WrongIdentity: return "wrong identity";
    case BodyStatus::MetadataOnly: return "metadata only";
    case BodyStatus::RootMissingType: return "root without filament_type";
    }
    return "";
}

std::string safe_name(const std::string& name)
{
    std::string out;
    for (unsigned char c : name) out += (std::isalnum(c) || c == '-' || c == '_' || c == '.') ? static_cast<char>(c) : '_';
    return out;
}

// yyyymmdd-hhmmss, UTC
std::string utc_stamp(long long t)
{
    std::string s = boost::posix_time::to_iso_string(boost::posix_time::from_time_t(static_cast<std::time_t>(t)));
    const size_t p = s.find('T');
    if (p != std::string::npos) s[p] = '-';
    return s;
}

bool body_is_root(const ValueMap& body)
{
    auto empty_or_absent = [&body](const char* key) {
        auto it = body.find(key);
        return it == body.end() || it->second.empty();
    };
    return empty_or_absent("base_id") && empty_or_absent("inherits");
}

bool same_summary(const FirstRunSummary& a, const FirstRunSummary& b)
{
    return a.uploads == b.uploads && a.downloads == b.downloads && a.links == b.links &&
           a.conflicts_local_wins == b.conflicts_local_wins && a.conflicts_remote_wins == b.conflicts_remote_wins &&
           a.remote_deletes == b.remote_deletes && a.candidates == b.candidates && a.notes == b.notes;
}

void save_ledger(const Ledger& ledger)
{
    std::string err;
    if (!ledger.save(&err)) BOOST_LOG_TRIVIAL(warning) << "bbl preset sync: ledger save failed: " << err;
}

} // namespace

Mirror::Mirror(IHost& host, ICloud& cloud, ILocalStore& store) : m_host(host), m_cloud(cloud), m_store(store) {}

LedgerView Mirror::view_of(const Ledger& ledger) const
{
    LedgerView v;
    v.entries             = ledger.entries();
    v.first_run_confirmed = ledger.first_run_confirmed();
    v.clock_offset        = ledger.clock_offset();
    return v;
}

void Mirror::end_pass(const std::string& result, long long now)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_status.last_result = result;
    m_status.last_pass   = now;
}

void Mirror::record_failure(const Key& key, long long now)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Backoff&                    b = m_backoff[key];
    ++b.fail_count;
    long long delay = BACKOFF_FIRST_SECONDS;
    for (int i = 1; i < b.fail_count && delay < BACKOFF_MAX_SECONDS; ++i) delay *= 2;
    b.retry_after = now + std::min(delay, BACKOFF_MAX_SECONDS);
}

void Mirror::finish_pull(const Session& s, const std::shared_ptr<Ledger>& ledger, const Action& action, long long row_update_time,
                         const OpOutcome& outcome, const PullFollowUp& follow)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_session || !(*m_session == s)) return;
        m_pending.erase(action.key);
        if (action.rename_from_set) m_pending.erase(action.rename_from);
        if (outcome.result == OpResult::Applied) m_backoff.erase(action.key);
    }
    switch (outcome.result) {
    case OpResult::Applied: {
        LedgerEntry e = ledger->get(action.key).value_or(LedgerEntry{});
        e.setting_id    = action.setting_id;
        e.update_time   = row_update_time;
        if (follow.needs_upload) {
            // The merged preset differs from Bambu's: the next pass sees a local change and uploads it once.
            e.synced_hash = upload_hash(follow.remote_body);
        } else {
            // Exactly what the pass computes for the unchanged preset, so a plain download uploads nothing.
            const PresetType type = action.key.type;
            const KeyTest    orca = [type](const std::string& k) { return orca_knows(type, k); };
            e.synced_hash = fingerprint(upload_with_foreign(outcome.new_upload, follow.remote_body, orca), follow.remote_body, orca,
                                        [this](const std::string& k) { return m_studio.knows(k); });
        }
        e.state         = "linked";
        e.absent_passes = 0;
        e.absent_since  = 0;
        ledger->put(action.key, e);
        if (action.rename_from_set) {
            const auto old = ledger->get(action.rename_from);
            if (old && !old->setting_id.empty() && old->setting_id != action.setting_id && follow.base) follow.base->erase(old->setting_id);
            ledger->erase(action.rename_from);
        }
        if (follow.base) follow.base->put(action.setting_id, follow.remote_body);
        break;
    }
    case OpResult::Unresolvable: {
        LedgerEntry e = ledger->get(action.key).value_or(LedgerEntry{});
        e.setting_id  = action.setting_id;
        e.update_time = row_update_time;
        e.state       = "unresolvable";
        e.reason      = outcome.reason;
        ledger->put(action.key, e);
        break;
    }
    case OpResult::Postponed: {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.waiting.push_back(action.key.str() + ": " + outcome.reason);
        break;
    }
    case OpResult::Failed: record_failure(action.key, m_host.now()); break;
    }
    save_ledger(*ledger);
}

void Mirror::finish_delete(const Session& s, const std::shared_ptr<Ledger>& ledger, const Action& action, const OpOutcome& outcome,
                           const std::shared_ptr<BaseStore>& base)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_session || !(*m_session == s)) return;
        m_pending.erase(action.key);
        if (outcome.result == OpResult::Applied) m_backoff.erase(action.key);
    }
    switch (outcome.result) {
    case OpResult::Applied: {
        const auto e = ledger->get(action.key);
        if (e && !e->setting_id.empty() && base) base->erase(e->setting_id);
        ledger->erase(action.key);
        break;
    }
    case OpResult::Postponed: {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.waiting.push_back(action.key.str() + ": " + outcome.reason);
        break;
    }
    case OpResult::Unresolvable:
    case OpResult::Failed: record_failure(action.key, m_host.now()); break;
    }
    save_ledger(*ledger);
}

void Mirror::run_pass(Trigger trigger)
{
    const long long now   = m_host.now();
    const auto      s_opt = m_host.session();

    // 1. Session and ledger.
    if (!s_opt) {
        end_pass("skipped: preconditions", now);
        return;
    }
    const Session s = *s_opt;
    {
        bool changed;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            changed = !m_session || !(*m_session == s);
        }
        if (changed) {
            auto        fresh = std::make_shared<Ledger>(Ledger::file_path(m_host.data_dir(), s.orca_user_id, s.bambu_uid));
            std::string err;
            if (!fresh->load(&err)) BOOST_LOG_TRIVIAL(warning) << "bbl preset sync: ledger load failed, starting empty: " << err;
            // The halted key lists do not survive a restart: the next plan recomputes and announces the halt.
            if (!fresh->halted().empty()) {
                fresh->set_halted("");
                save_ledger(*fresh);
            }
            std::lock_guard<std::mutex> lock(m_mutex);
            const bool                  uid_changed = !m_session || m_session->bambu_uid != s.bambu_uid;
            m_session                                = s;
            m_ledger                                 = fresh;
            m_base                                   = std::make_shared<BaseStore>(BaseStore::dir(m_host.data_dir(), s.orca_user_id, s.bambu_uid));
            m_body_cache.clear();
            m_pending.clear();
            m_backoff.clear();
            m_creates_blocked      = false;
            m_list_failed_notified = false;
            m_have_list            = false;
            m_last_list            = 0;
            m_last_summary.reset();
            m_summary_declined = false;
            m_last_candidates.clear();
            m_proceed_local = m_proceed_remote = false;
            m_halted_local.clear();
            m_halted_remote.clear();
            if (uid_changed) m_auth_blocked = false;
        }
    }
    std::shared_ptr<Ledger> ledger;
    std::shared_ptr<BaseStore> base;
    bool                    have_list;
    long long               last_list;
    std::set<Key>           pending;
    std::map<Key, Backoff>  backoff;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ledger    = m_ledger;
        base      = m_base;
        have_list = m_have_list;
        last_list = m_last_list;
        pending   = m_pending;
        backoff   = m_backoff;
        if (m_auth_blocked) {
            m_status.last_result = "skipped: auth";
            m_status.last_pass   = now;
            return;
        }
    }

    if (!m_studio_loaded) {
        m_studio        = StudioKeys::load(m_host.resources_dir() / "printers" / "bambu_studio_option_keys.json");
        m_studio_loaded = true;
    }
    const KeyTest studio = [this](const std::string& k) { return m_studio.knows(k); };

    // 2. Halted.
    if (const std::string halted = ledger->halted(); !halted.empty()) {
        end_pass("halted: " + halted, now);
        return;
    }

    // 3. Snapshot.
    const std::optional<LocalSnapshot> snap_opt = m_store.snapshot(s);
    if (!snap_opt) {
        end_pass("skipped: no snapshot", now);
        return;
    }
    const LocalSnapshot& snap = *snap_opt;
    std::map<Key, const LocalItem*> items;
    for (const LocalItem& item : snap.items) items[item.key] = &item;

    // Filaments that became candidates after the first run are offered once per change of that list.
    if (ledger->first_run_confirmed()) {
        std::vector<Key> candidates;
        for (const LocalItem& item : snap.items)
            if (item.cls.eligibility == Eligibility::Candidate && !ledger->get(item.key)) candidates.push_back(item.key);
        std::sort(candidates.begin(), candidates.end());
        bool announce = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (candidates.empty()) {
                m_last_candidates.clear();
            } else if (candidates != m_last_candidates) {
                m_last_candidates = candidates;
                announce          = true;
            }
        }
        if (announce) m_host.on_candidates(candidates);
    }

    // The planner's view of the local presets. `remote_bodies`: valid bodies by key, for presets that are not linked yet.
    std::map<Key, std::string> fp_hash; // key -> hash compared with the ledger's synced_hash
    auto build_locals = [&](const LedgerView& lv, const std::map<Key, const ValueMap*>* remote_bodies) {
        std::vector<LocalPreset> out;
        fp_hash.clear();
        static const ValueMap none;
        for (const LocalItem& item : snap.items) {
            LocalPreset l;
            l.key   = item.key;
            l.state = LocalState::Ineligible;
            if (item.upload.empty()) {
                out.push_back(l);
                continue;
            }
            const auto e      = lv.entries.find(item.key);
            const bool linked = e != lv.entries.end() && (e->second.state == "linked" || e->second.state == "too_large");
            ValueMap   ref;
            if (linked) {
                if (auto b = base->get(e->second.setting_id)) ref = std::move(*b);
            } else if (remote_bodies != nullptr) {
                auto r = remote_bodies->find(item.key);
                if (r != remote_bodies->end()) ref = *r->second;
            }
            const PresetType type = item.key.type;
            const KeyTest    orca = [type](const std::string& k) { return orca_knows(type, k); };
            l.state = (item.cls.eligibility == Eligibility::Eligible || linked) ? LocalState::Eligible : LocalState::Ineligible;
            l.hash  = fingerprint(upload_with_foreign(item.upload, ref, orca), ref, orca, studio);
            l.mtime = item.mtime;
            fp_hash[item.key] = l.hash;
            out.push_back(std::move(l));
        }
        return out;
    };

    // 4. Is a list needed?
    const LedgerView view = view_of(*ledger);
    {
        auto skip = [&](const Key& k) {
            if (pending.count(k)) return true;
            auto b = backoff.find(k);
            return b != backoff.end() && b->second.retry_after > now;
        };
        bool need = !have_list || trigger == Trigger::Manual || now - last_list >= LIST_INTERVAL_SECONDS;
        if (!need) {
            for (const LocalPreset& l : build_locals(view, nullptr)) {
                if (l.state != LocalState::Eligible || skip(l.key)) continue;
                auto e = view.entries.find(l.key);
                if (e == view.entries.end() ||
                    ((e->second.state == "linked" || e->second.state == "too_large") && e->second.synced_hash != l.hash)) {
                    need = true;
                    break;
                }
            }
        }
        if (!need) {
            for (const auto& [key, e] : view.entries)
                if (e.state == "linked" && !items.count(key) && !skip(key)) {
                    need = true;
                    break;
                }
        }
        if (!need) {
            end_pass("ok", now);
            return;
        }
    }

    // 5. List.
    auto cache_hit = [this](const std::string& setting_id, long long update_time) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto                        it = m_body_cache.find(setting_id);
        return it != m_body_cache.end() && it->second.update_time == update_time;
    };
    const ListResult r = m_cloud.list(m_host.bundle_version(), [&view, &cache_hit](const CloudRow& row) {
        return want_body(Key{row.type, row.name}, row.setting_id, row.update_time, view) && !cache_hit(row.setting_id, row.update_time);
    });
    if (!r.ok) {
        bool first;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            first                  = !m_list_failed_notified;
            m_list_failed_notified = true;
        }
        if (first) m_host.notify("list_failed", r.error);
        end_pass("list failed: " + r.error, now);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_last_list            = now;
        m_have_list            = true;
        m_list_failed_notified = false;
    }

    // 6. Remote rows.
    std::vector<RemoteRow>   rows;
    std::vector<ValueMap>    row_bodies; // parallel to rows; empty unless has_body
    std::vector<std::string> notes;
    {
        std::map<std::string, std::set<PresetType>> types_by_name;
        for (const CloudRow& row : r.rows) types_by_name[row.name].insert(row.type);
        std::set<std::string> collision_noted;
        for (const CloudRow& row : r.rows) {
            RemoteRow rr;
            rr.key         = Key{row.type, row.name};
            rr.setting_id  = row.setting_id;
            rr.update_time = row.update_time;
            ValueMap       body;
            const ValueMap* found = nullptr;
            if (types_by_name[row.name].size() > 1) {
                if (collision_noted.insert(row.name).second) notes.push_back("name used by several preset types: " + row.name);
            } else {
                auto it = r.bodies_by_name.find(row.name);
                if (it != r.bodies_by_name.end()) found = &it->second;
            }
            const bool collided = types_by_name[row.name].size() > 1;
            const bool cached   = !collided && cache_hit(row.setting_id, row.update_time);
            const bool wanted   = want_body(rr.key, row.setting_id, row.update_time, view) && !cached;
            if (found == nullptr && cached) {
                std::lock_guard<std::mutex> lock(m_mutex);
                body         = m_body_cache[row.setting_id].body;
                rr.has_body  = true;
                rr.body_hash = upload_hash(body);
            } else if (found != nullptr) {
                const BodyStatus st = check_body(*found, row.type, row.setting_id, body_is_root(*found));
                if (st == BodyStatus::Valid) {
                    rr.has_body  = true;
                    rr.body_hash = upload_hash(*found);
                    body         = *found;
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_body_cache[row.setting_id] = CachedBody{row.update_time, *found};
                } else if (wanted) {
                    notes.push_back("body unavailable for " + rr.key.str() + ": " + body_status_name(st));
                }
            } else if (wanted) {
                notes.push_back("body unavailable for " + rr.key.str() + ": missing");
            }
            rows.push_back(std::move(rr));
            row_bodies.push_back(std::move(body));
        }
    }
    auto find_row = [&](const Key& key, const std::string& setting_id) -> int {
        int any = -1;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (!(rows[i].key == key)) continue;
            if (rows[i].setting_id == setting_id) return static_cast<int>(i);
            if (any < 0) any = static_cast<int>(i);
        }
        return any;
    };

    // 7. Plan.
    std::map<Key, const ValueMap*> remote_bodies;
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].has_body) remote_bodies.emplace(rows[i].key, &row_bodies[i]);
    const std::vector<LocalPreset> locals = build_locals(view, &remote_bodies);
    Plan                           plan   = make_plan(locals, rows, view, now);
    notes.insert(notes.end(), plan.notes.begin(), plan.notes.end());

    std::vector<Action>      actions;
    std::vector<std::string> waiting;
    for (const Action& a : plan.actions) {
        auto in_backoff = [&](const Key& k) {
            auto b = backoff.find(k);
            return b != backoff.end() && b->second.retry_after > now;
        };
        if (pending.count(a.key) || (a.rename_from_set && pending.count(a.rename_from)))
            waiting.push_back(a.key.str() + ": pending");
        else if (in_backoff(a.key))
            waiting.push_back(a.key.str() + ": backoff");
        else
            actions.push_back(a);
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.notes   = notes;
        m_status.waiting = waiting;
        m_status.planned.clear();
        for (const Action& a : actions) m_status.planned.push_back(std::string(action_name(a.type)) + " " + a.key.str());
    }

    // 8. Dry run.
    if (plan.dry_run) {
        FirstRunSummary sum;
        for (const Action& a : plan.actions) {
            switch (a.type) {
            case ActionType::Create: sum.uploads.push_back(a.key); break;
            case ActionType::Pull:
                if (a.merge_conflict) (a.local_newer ? sum.conflicts_local_wins : sum.conflicts_remote_wins).push_back(a.key);
                else (a.backup_local ? sum.conflicts_remote_wins : sum.downloads).push_back(a.key);
                break;
            case ActionType::Link: sum.links.push_back(a.key); break;
            case ActionType::Update: sum.uploads.push_back(a.key); break;
            case ActionType::DeleteRemote: sum.remote_deletes.push_back(a.key); break;
            default: break;
            }
        }
        for (const LocalItem& item : snap.items)
            if (item.cls.eligibility == Eligibility::Candidate && !view.entries.count(item.key)) sum.candidates.push_back(item.key);
        sum.notes = notes;

        bool show;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            show = !m_summary_declined && (!m_last_summary || !same_summary(*m_last_summary, sum));
            if (show) m_last_summary = sum;
        }
        if (show) m_host.on_first_run(sum);
        end_pass("dry run", now);
        return;
    }

    // 9. Halt.
    {
        bool proceed_local, proceed_remote;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            proceed_local    = m_proceed_local;
            proceed_remote   = m_proceed_remote;
            m_proceed_local  = false;
            m_proceed_remote = false;
        }
        if (!plan.halt.empty()) {
            auto readd = [&](std::vector<Key>& keys, bool wanted, ActionType type) {
                if (!wanted) return;
                for (const Key& k : keys) {
                    Action a;
                    a.type = type;
                    a.key  = k;
                    auto e = view.entries.find(k);
                    if (e != view.entries.end()) a.setting_id = e->second.setting_id;
                    a.backup_local = type == ActionType::DeleteLocal;
                    actions.push_back(std::move(a));
                }
                keys.clear();
            };
            readd(plan.halted_local_deletes, proceed_local, ActionType::DeleteLocal);
            readd(plan.halted_remote_deletes, proceed_remote, ActionType::DeleteRemote);
            if (plan.halted_local_deletes.empty() && plan.halted_remote_deletes.empty()) plan.halt.clear();
        }
        if (!plan.halt.empty()) {
            ledger->set_halted(plan.halt);
            save_ledger(*ledger);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_halted_local  = plan.halted_local_deletes;
                m_halted_remote = plan.halted_remote_deletes;
            }
            m_host.on_halt(plan.halt, plan.halted_local_deletes, plan.halted_remote_deletes);
        }
    }

    // 10. Execute.
    bool        changed = false;
    bool        wrote   = false;
    std::string stopped;
    auto link = [&](const Key& key, const std::string& id, long long update_time, const std::string& hash) {
        LedgerEntry e = ledger->get(key).value_or(LedgerEntry{});
        e.setting_id    = id;
        e.update_time   = update_time;
        e.synced_hash   = hash;
        e.state         = "linked";
        e.absent_passes = 0;
        e.absent_since  = 0;
        ledger->put(key, e);
        changed = true;
    };
    auto clear_backoff = [&](const Key& key) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_backoff.erase(key);
    };
    auto add_waiting = [&](const std::string& text) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.waiting.push_back(text);
    };
    // Returns true when the pass must stop.
    auto fail = [&](const Key& key, const WriteResult& w, const LocalItem* item) -> bool {
        if (w.http_code == 401) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_auth_blocked = true;
            }
            m_host.notify("auth", "");
            return true;
        }
        if (w.server_code == "14") {
            bool first;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                first             = !m_creates_blocked;
                m_creates_blocked = true;
            }
            if (first) m_host.notify("preset_limit", "");
            return false;
        }
        if (w.http_code == 413) {
            LedgerEntry e = ledger->get(key).value_or(LedgerEntry{});
            e.state       = "too_large";
            const auto h  = fp_hash.find(key);
            e.synced_hash = item != nullptr && h != fp_hash.end() ? h->second : std::string();
            ledger->put(key, e);
            changed = true;
            return false;
        }
        record_failure(key, m_host.now());
        return false;
    };

    m_store.begin_batch();
    for (const Action& a : actions) {
        if (m_host.cancelled()) {
            stopped = "stopped: cancelled";
            break;
        }
        if (const auto cur = m_host.session(); !cur || !(*cur == s)) {
            stopped = "stopped: session changed";
            break;
        }
        const bool network = a.type == ActionType::Create || a.type == ActionType::Update || a.type == ActionType::DeleteRemote;
        auto       it      = items.find(a.key);
        const LocalItem* item = it == items.end() ? nullptr : it->second;
        const int        ri   = find_row(a.key, a.setting_id);
        const RemoteRow* row  = ri < 0 ? nullptr : &rows[ri];

        if (network && wrote) m_host.sleep_ms(WRITE_GAP_MS);

        switch (a.type) {
        case ActionType::Create: {
            if (item == nullptr) break;
            bool blocked;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                blocked = m_creates_blocked;
            }
            if (blocked) {
                add_waiting(a.key.str() + ": preset limit");
                break;
            }
            wrote                 = true;
            const ValueMap values = upload_with_foreign(item->upload, ValueMap{}, [t = a.key.type](const std::string& k) { return orca_knows(t, k); });
            WriteResult    w      = m_cloud.create(a.key.name, values);
            if (w.ok) {
                base->put(w.setting_id, values);
                link(a.key, w.setting_id, w.update_time, upload_hash(values));
                if (w.update_time > 0) ledger->set_clock_offset(w.update_time - now);
                clear_backoff(a.key);
            } else if (fail(a.key, w, item)) {
                stopped = "stopped: auth";
            }
            break;
        }
        case ActionType::Update: {
            if (item == nullptr) break;
            wrote                 = true;
            const ValueMap values = upload_with_foreign(item->upload, base->get(a.setting_id).value_or(ValueMap{}),
                                                        [t = a.key.type](const std::string& k) { return orca_knows(t, k); });
            WriteResult    w      = m_cloud.update(a.setting_id, a.key.name, values);
            if (w.ok) {
                base->put(a.setting_id, values);
                link(a.key, a.setting_id, w.update_time, upload_hash(values));
                if (w.update_time > 0) ledger->set_clock_offset(w.update_time - now);
                clear_backoff(a.key);
            } else if (w.http_code == 422) {
                notes.push_back("remote copy missing for " + a.key.str());
                std::lock_guard<std::mutex> lock(m_mutex);
                m_last_list = 0;
            } else if (fail(a.key, w, item)) {
                stopped = "stopped: auth";
            }
            break;
        }
        case ActionType::DeleteRemote: {
            wrote = true;
            if (m_cloud.remove(a.setting_id)) {
                if (!a.setting_id.empty()) base->erase(a.setting_id);
                ledger->erase(a.key);
                changed = true;
                clear_backoff(a.key);
            } else if (fail(a.key, WriteResult{}, item)) {
                stopped = "stopped: auth";
            }
            break;
        }
        case ActionType::Link:
            if (item != nullptr && row != nullptr && row->has_body) {
                base->put(a.setting_id, row_bodies[ri]);
                link(a.key, a.setting_id, row->update_time, upload_hash(row_bodies[ri]));
            }
            break;
        case ActionType::DropEntry:
            if (const auto e = ledger->get(a.key); e && !e->setting_id.empty()) base->erase(e->setting_id);
            ledger->erase(a.key);
            changed = true;
            break;
        case ActionType::SetAbsent: {
            if (auto e = ledger->get(a.key)) {
                e->absent_passes = a.absent_passes;
                e->absent_since  = a.absent_since;
                ledger->put(a.key, *e);
                changed = true;
            }
            break;
        }
        case ActionType::MarkUnresolvable: {
            LedgerEntry e = ledger->get(a.key).value_or(LedgerEntry{});
            e.state       = "unresolvable";
            e.reason      = a.reason;
            e.setting_id  = a.setting_id;
            ledger->put(a.key, e);
            changed = true;
            break;
        }
        case ActionType::Pull: {
            if (row == nullptr || !row->has_body) break;
            const ValueMap& remote_body = row_bodies[ri];
            if (a.backup_remote) {
                const fs::path dir  = Ledger::backup_dir(m_host.data_dir());
                const fs::path file = dir / (utc_stamp(now) + "_" + type_string(a.key.type) + "_" + safe_name(a.key.name) + "_remote.json");
                bool           ok   = false;
                try {
                    boost::system::error_code ec;
                    fs::create_directories(dir, ec);
                    boost::nowide::ofstream out(file.string(), std::ios::binary | std::ios::trunc);
                    if (out) {
                        out << nlohmann::json(remote_body).dump(2);
                        out.close();
                        ok = !out.fail();
                    }
                } catch (...) {
                    ok = false;
                }
                if (!ok) {
                    notes.push_back("remote backup failed, pull skipped for " + a.key.str());
                    break;
                }
            }
            // A preset that is gone locally is downloaded whole: its base says nothing about what it holds.
            const ValueMap  base_body = item != nullptr ? base->get(a.setting_id).value_or(ValueMap{}) : ValueMap{};
            const PresetType type     = a.key.type;
            const MergeResult m       = merge_bodies(item != nullptr ? item->upload : ValueMap{}, remote_body, base_body, a.local_newer,
                                                     [type](const std::string& k) { return orca_knows(type, k); }, studio);
            PullOp op;
            op.session             = s;
            op.key                 = a.key;
            op.body                = m.merged;
            for (const char* k : {"name", "type", "version", "setting_id", "user_id"}) {
                auto it = remote_body.find(k);
                if (it != remote_body.end()) op.body[k] = it->second;
            }
            op.backup_local        = a.backup_local;
            op.rename_from_set     = a.rename_from_set;
            op.rename_from         = a.rename_from;
            op.expected_local_hash = item != nullptr ? item->hash : std::string();
            if (m.conflicts > 0) notes.push_back("merged " + a.key.str() + ": " + std::to_string(m.conflicts) + " settings differed on both sides");
            if (!m.kept_unknown_to_studio.empty())
                notes.push_back("kept " + std::to_string(m.kept_unknown_to_studio.size()) + " OrcaSlicer-only settings for " + a.key.str());
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_pending.insert(a.key);
                if (a.rename_from_set) m_pending.insert(a.rename_from);
            }
            const long long row_time = row->update_time;
            PullFollowUp    follow{remote_body, m.needs_upload, base};
            m_store.apply_pull(std::move(op), [this, s, ledger, a, row_time, follow](OpOutcome outcome) { finish_pull(s, ledger, a, row_time, outcome, follow); });
            break;
        }
        case ActionType::DeleteLocal: {
            DeleteOp op;
            op.session             = s;
            op.key                 = a.key;
            op.backup_local        = a.backup_local;
            op.expected_local_hash = item != nullptr ? item->hash : std::string();
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_pending.insert(a.key);
            }
            m_store.delete_local(std::move(op), [this, s, ledger, a, base](OpOutcome outcome) { finish_delete(s, ledger, a, outcome, base); });
            break;
        }
        }
        if (!stopped.empty()) break;
    }

    m_store.end_batch();

    // 11. Save and finish.
    if (changed) save_ledger(*ledger);
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status.notes = notes;
    }
    if (!stopped.empty())
        end_pass(stopped, now);
    else
        end_pass(plan.halt.empty() ? "ok" : "halted: " + plan.halt, now);
}

void Mirror::confirm_first_run(bool accepted)
{
    std::shared_ptr<Ledger> ledger;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!accepted) m_summary_declined = true;
        else m_have_list = false;
        ledger = m_ledger;
    }
    if (accepted && ledger) {
        ledger->set_first_run_confirmed(true);
        save_ledger(*ledger);
    }
}

void Mirror::reset_candidates_prompt()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_last_candidates.clear();
}

void Mirror::reset_first_run_prompt()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_last_summary.reset();
    m_summary_declined = false;
}

void Mirror::set_declined(const Key& key)
{
    const auto ledger = this->ledger();
    if (!ledger) return;
    LedgerEntry e;
    e.state = "declined";
    ledger->put(key, e);
    save_ledger(*ledger);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_have_list = false;
}

void Mirror::forget(const Key& key)
{
    std::shared_ptr<Ledger>    ledger;
    std::shared_ptr<BaseStore> base;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_backoff.erase(key);
        ledger = m_ledger;
        base   = m_base;
    }
    if (ledger) {
        if (const auto e = ledger->get(key); e && !e->setting_id.empty() && base) base->erase(e->setting_id);
        ledger->erase(key);
        save_ledger(*ledger);
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    m_have_list = false;
}

void Mirror::resolve_halt(HaltChoice choice)
{
    if (choice == HaltChoice::Later) return;
    std::shared_ptr<Ledger> ledger;
    std::vector<Key>        local, remote;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ledger = m_ledger;
        local  = m_halted_local;
        remote = m_halted_remote;
        m_have_list = false;
        if (choice == HaltChoice::ProceedLocalDeletes) m_proceed_local = true;
        if (choice == HaltChoice::ProceedRemoteDeletes) m_proceed_remote = true;
        if (choice == HaltChoice::KeepLocal) m_halted_local.clear();
        if (choice == HaltChoice::RestoreFromBambu) m_halted_remote.clear();
    }
    if (!ledger) return;
    ledger->set_halted("");
    if (choice == HaltChoice::KeepLocal) {
        for (const Key& k : local) {
            LedgerEntry e;
            e.state = "declined";
            ledger->put(k, e);
        }
    } else if (choice == HaltChoice::RestoreFromBambu) {
        for (const Key& k : remote) ledger->erase(k);
    }
    save_ledger(*ledger);
}

Status Mirror::status() const
{
    Status                  st;
    std::shared_ptr<Ledger> ledger;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        st     = m_status;
        ledger = m_ledger;
    }
    if (ledger) {
        st.halted              = ledger->halted();
        st.first_run_confirmed = ledger->first_run_confirmed();
        st.linked              = 0;
        for (const auto& kv : ledger->entries())
            if (kv.second.state == "linked") ++st.linked;
    }
    return st;
}

std::shared_ptr<Ledger> Mirror::ledger() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_ledger;
}

} // namespace Slic3r::BblPresetSync
