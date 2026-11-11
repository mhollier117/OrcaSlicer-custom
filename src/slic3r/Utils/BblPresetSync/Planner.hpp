#pragma once

#include "Ledger.hpp"
#include "Types.hpp"

#include <map>
#include <string>
#include <vector>

namespace Slic3r::BblPresetSync {

// A local change within this many seconds of the remote one still wins.
constexpr long long TIE_WINDOW_SECONDS = 120;
// Consecutive lists a linked row must be absent from before its local preset is deleted.
constexpr int ABSENT_PASSES_TO_DELETE = 2;
// Fewest deletes (and at least half the linked entries) that trigger the mass-delete stop.
constexpr int MASS_DELETE_MIN = 3;
// A row must have been absent for at least this long (and ABSENT_PASSES_TO_DELETE lists) before a local delete.
constexpr long long ABSENT_MIN_SECONDS = 240;

enum class LocalState { Missing, Eligible, Ineligible };

// One per user preset in the snapshot (Eligible or Ineligible).
struct LocalPreset
{
    Key         key;
    LocalState  state{LocalState::Eligible};
    std::string hash;     // upload_hash of its upload map (Eligible only)
    long long   mtime{0}; // file modification time, unix seconds, local clock
};

struct RemoteRow
{
    Key         key;
    std::string setting_id;
    long long   update_time{0};
    bool        has_body{false}; // a body was drained and check_body() said Valid
    std::string body_hash;       // upload_hash(body) when has_body
};

// Snapshot of the ledger.
struct LedgerView
{
    std::map<Key, LedgerEntry> entries;
    bool                       first_run_confirmed{false};
    long long                  clock_offset{0};
};

enum class ActionType { Create, Update, DeleteRemote, Pull, DeleteLocal, Link, DropEntry, MarkUnresolvable, SetAbsent };

struct Action
{
    ActionType  type{ActionType::Create};
    Key         key;                    // the local/ledger key the action is about
    std::string setting_id;             // Bambu id involved ("" for Create)
    bool        backup_local{false};    // copy the local files to the backup folder first
    bool        backup_remote{false};   // save the remote body to the backup folder first
    bool        rename_from_set{false};
    Key         rename_from{};          // Pull with rename_from_set: afterwards the old local preset rename_from is deleted
    int         absent_passes{0};       // SetAbsent: the new count
    long long   absent_since{0};        // SetAbsent: the new absent_since (0 = cleared)
    std::string reason;                 // MarkUnresolvable, or a note
};

struct Plan
{
    std::vector<Action>      actions;
    std::vector<std::string> notes; // human-readable, e.g. duplicate rows, body unavailable
    bool                     dry_run{false}; // first_run_confirmed was false
    std::string              halt;           // "", "mass_delete" or "empty_list"
    std::vector<Key>         halted_local_deletes;
    std::vector<Key>         halted_remote_deletes;
};

// Called from the list's check function, row by row: should this row's body be downloaded?
bool want_body(const Key& row_key, const std::string& row_setting_id, long long row_update_time, const LedgerView& ledger);

// now: unix seconds, local clock.
Plan make_plan(const std::vector<LocalPreset>& locals, const std::vector<RemoteRow>& remote, const LedgerView& ledger, long long now);

} // namespace Slic3r::BblPresetSync
