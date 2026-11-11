# Bambu cloud preset sync

Keeps Bambu-based user presets in the user's Bambu account as well as in the Orca cloud, in both
directions. It exists so a custom filament can be chosen for an AMS slot on the printer, in Bambu
Handy and in Bambu Studio, and so OrcaSlicer's AMS sync maps that slot back to the custom preset.

It is a mirror beside the Orca-cloud sync, not part of it. The Orca-cloud sync is unchanged.

## Why it is separate

OrcaSlicer's sync keeps one cloud identity per preset in `<name>.info`, and its pull code owns that
identity: `PresetBundle::load_user_presets(AppConfig&, map)` deletes synced presets missing from the
map it is given, and `check_and_fix_syncinfo` resets a `setting_id` that is not an Orca id. Bambu
data therefore never goes through that code and never enters `.info`. The mirror has its own state
file and reaches the local presets only through `PresetCollection::save_current_preset` and
`delete_preset`, as a user action would, so every change it makes continues to the Orca cloud by
the normal path.

## What is mirrored

Decided from in-memory preset data (`Eligibility.cpp`):

- **Child**: a process, printer or filament whose direct parent is a BBL system preset with a
  `setting_id`; for a filament the parent's filament id must also have a Bambu id in
  `resources/printers/bambu_filament_ids.json` (`BBLPrinterAgent::bambu_filament_id`). Uploaded as
  the diff `get_differed_values_to_update` produces, with `inherits` and `base_id`.
- **Custom filament**: a root filament with a filament id whose `compatible_printers` names only
  BBL printers. Uploaded as the full config. This is the only shape a printer lists: it asks the
  cloud for the user's presets that have no `base_id`.
- Everything else stays local. A user preset that inherits another user preset is not mirrored.

A filament that targets only BBL printers but has a parent Bambu does not know (or is a root with
no filament id) is a **conversion candidate**. Converting it (`convert_filament`) makes it a
standalone root with a `P…` id, `"P" + md5(<name before " @"> + "@" + <bambu uid>)[0:7]`, the id
Bambu Studio gives a custom filament. It keeps its name and Orca-cloud identity. Conversion is
offered in the first-run dialog and, later, by a notification; it is never automatic.

## State

`data_dir()/bbl_sync/ledger_<orca user id>_<bambu uid>.json` (`Ledger.cpp`), written atomically.
Per preset key (`<type>/<name>`): the Bambu `setting_id`, the server time and the upload hash at
the last sync, a state (`linked`, `declined`, `unresolvable`, `too_large`), and the absence
counters used before a delete. It also records conversions (old and new filament id, printers whose
K profiles were copied). Losing the ledger is safe: nothing is linked, so nothing can be deleted,
and the first-run dialog pairs presets again by name.

Backups of anything overwritten, deleted or converted go to `data_dir()/bbl_sync/backup/`.

## The pass

`BblPresetSyncController::tick` runs on the preset sync thread after each Orca push pass and calls
`Mirror::run_pass`. It needs both sign-ins, `sync_user_preset` and `sync_bbl_user_preset`.

1. **Snapshot** of every user preset, built on the UI thread (`build_snapshot`): eligibility, the
   upload map, its hash, the file time.
2. **List** the Bambu presets whenever something local changed, every 5 minutes, and on request.
   No write ever uses a list older than its own pass, because Bambu's `PATCH` has no conflict
   check. A downloaded body is used only if its type and id match its row and it holds settings:
   the plugin reports success even when a body download failed, and its cache is keyed by name.
3. **Plan** (`Planner.cpp`, a pure function of snapshot, list and ledger). Per preset: upload a
   local change, pull a remote one, pair presets of the same name, create or delete on either
   side. When both sides changed, the newer wins (file time corrected by the clock offset learned
   from write responses, against the server time; within 120 s the local one wins) and the loser
   is backed up first.
4. **Execute.** Network calls run on the sync thread. Local changes are queued to the UI thread
   and not waited for; each rechecks the account session and the preset's hash before it acts.

Local changes wait while that preset list has unsaved edits, because replacing or deleting a preset
reselects, which would discard them. A delete also waits while the preset is selected, used by the
project or has presets based on it.

## Deletes

A preset deleted on Bambu is deleted locally (and so from the Orca cloud) only after it was absent
from two successful lists at least 4 minutes apart. Two stops ask the user instead of deleting:
an empty list, and a pass that would delete 3 or more presets and at least half of the linked ones,
in either direction. A preset that merely stops being eligible is never treated as deleted.

## The first run

Until the user confirms, the pass only plans. `BblPresetSyncFirstRunDialog` shows what would be
uploaded, downloaded and paired, and a checklist of conversion candidates. Declined candidates are
remembered in the ledger and not offered again.

## K profiles

A printer keeps flow-dynamics profiles in a table keyed by filament id, so a converted filament
would lose its profiles. `k_copy_step` copies them: for the selected, connected printer it reads
the table and adds a copy of each entry of the old id under the new id (`plan_k_copy`). It never
deletes or changes an entry, and respects the table limit. Which profile a slot uses is not part
of the table and is chosen when the slot is set.

## Loader seam

`PresetCollection::load_user_preset` has a `materialize_only` parameter. When set, the function
returns the merged config a cloud body describes and changes nothing. It is the only way to apply
a per-variant diff (multi-extruder printers) without duplicating that logic, and the only change
this feature makes to upstream preset code.

## Driving and inspecting it

The MCP tool `preset_sync` reports what is linked, planned and waiting, and can confirm the first
run, convert a filament, resolve a stop and request a K copy. It never returns cloud ids or tokens.

## Limits

- A delete on Bambu reaches OrcaSlicer after 5 to 10 minutes.
- Two edits of one preset on both sides within the same few seconds can lose one.
- Two Bambu presets of different types with the same name can be uploaded but not pulled.
- The sync runs only while signed in to the Orca cloud as well.
