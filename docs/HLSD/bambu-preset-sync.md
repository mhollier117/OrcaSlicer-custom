# Bambu cloud preset sync

Keeps user presets in the user's Bambu account as well as in the Orca cloud, in both directions, so a
user can move between OrcaSlicer and Bambu Studio and find the same presets, and so a custom filament
can be chosen for an AMS slot on the printer, in Bambu Handy and in Bambu Studio.

It is a mirror beside the Orca-cloud sync, not part of it. The Orca-cloud sync is unchanged.

## Why it is separate

OrcaSlicer's sync keeps one cloud identity per preset in `<name>.info`, and its pull code owns that
identity: `PresetBundle::load_user_presets(AppConfig&, map)` deletes synced presets missing from the
map it is given, and `check_and_fix_syncinfo` resets a `setting_id` that is not an Orca id. Bambu
data therefore never goes through that code and never enters `.info`. The mirror has its own state
and reaches the local presets only through `PresetCollection::save_current_preset` and
`delete_preset`, as a user action would, so every change it makes continues to the Orca cloud by
the normal path.

## What is synced

- **Down**: every preset on Bambu that OrcaSlicer can load, whatever the printer. A preset whose
  parent profile OrcaSlicer lacks is skipped and stays on Bambu.
- **Up**: new local presets that Bambu Studio can load (`Eligibility.cpp`):
  - a **child** whose direct parent is a BBL system preset with a `setting_id` (for a filament the
    parent's filament id must also have a Bambu id in `resources/printers/bambu_filament_ids.json`);
  - a **custom filament**: a root filament with a filament id whose `compatible_printers` names only
    BBL printers. This is the only shape a printer lists: it asks the cloud for the user's presets
    that have no `base_id`.
- **Linked means synced**: a preset that has a link in the ledger stays synced in both directions
  whatever the rules above say. They only decide which new local presets are uploaded.

BBL profiles are identical in both slicers by name and id. Bambu Studio ships eleven other vendors,
all present in OrcaSlicer, whose profiles match only partly; a preset loads wherever its parent
exists under the same name.

A filament that targets only BBL printers but has a parent Bambu does not know (or is a root with
no filament id) is a **conversion candidate**. Converting it (`convert_filament`) makes it a
standalone root with a `P…` id, `"P" + md5(<name before " @"> + "@" + <bambu uid>)[0:7]`, the id
Bambu Studio gives a custom filament. It keeps its name and Orca-cloud identity. Conversion is
offered in the first-run dialog and, later, by a notification; it is never automatic.

## Settings are merged, never replaced

The two slicers do not hold the same settings: OrcaSlicer defines about a thousand keys, Bambu
Studio about 750, with some 370 only in OrcaSlicer and some 120 only in Studio. Studio ignores a
key it does not know when it loads a preset and leaves it out when it saves. The two also write the
same preset differently: OrcaSlicer always emits some keys Studio omits, and substitutes a default
for an enum value it does not know. Exchanging whole presets would therefore lose settings on every
save, and comparing the two representations would mistake those differences for edits.

So the mirror exchanges **edits**, against two bases kept per linked preset (`BaseStore`,
`data_dir()/bbl_sync/base_<orca>_<bambu>/<setting_id>.json`):

- `remote`: the body on Bambu at the last sync;
- `local`: OrcaSlicer's upload map of the preset at the last sync.

A key changed locally when it differs from the local base, and remotely when it differs from the
remote base (`merge_bodies`).

- **Pull** applies the remote edits to the local preset. A key changed on both sides goes to the
  newer side. A key that disappeared from the Bambu body means "inherit the parent's value" only if
  Bambu Studio knows the key; otherwise Studio dropped it and OrcaSlicer keeps its value.
- **Push** (`apply_local_edits`) sends the Bambu copy with only the local edits applied. Keys only
  Studio understands pass through untouched; OrcaSlicer's representation never overwrites Studio's.
- Two same-named presets that meet for the first time are paired: equal ones are linked, different
  ones merged, with a disagreement going to the newer side and nothing uploaded unless the local
  side won one.

Which keys Studio knows is a list of the keys Studio's `PrintConfig.cpp` defines (`StudioKeys`).
OrcaSlicer keeps it current itself: about once a week `refresh_studio_keys_step` fetches that file
from Bambu Studio's public repository at its latest release tag, extracts the keys
(`parse_studio_print_config`) and caches them in `data_dir()/bbl_sync/studio_keys.json`. A download
is accepted only if it is plausible (enough keys, and some that must be there); otherwise the list
in use stays. Until a download succeeds the bundled `resources/printers/bambu_studio_option_keys.json`
is used. The requests carry nothing about the user and are not made in stealth mode or while the
sync is off. A key missing from the list is treated as unknown to Studio, which keeps OrcaSlicer's
value: a stale list loses nothing, it only fails to carry a "reset to default" made in Studio for a
setting newer than the list. The opposite error is possible only for a setting the latest Studio
release has, the user's older Studio lacks, and OrcaSlicer also has.

One limit remains: a value Studio cannot represent for a shared key is replaced by Studio's default
when the preset is saved in Studio, and that is indistinguishable from a deliberate change.

## State

`data_dir()/bbl_sync/ledger_<orca user id>_<bambu uid>.json` (`Ledger.cpp`), written atomically.
Per preset key (`<type>/<name>`): the Bambu `setting_id`, the server time, the hash of the local
base, a state (`linked`, `declined`, `unresolvable`, `too_large`), and the absence counters used
before a delete. It also records conversions (old and new filament id, printers whose K profiles
were copied). Losing the ledger or a base is safe: nothing is linked, so nothing can be deleted,
and presets pair again by name.

Backups of anything overwritten, deleted or converted go to `data_dir()/bbl_sync/backup/`.

## The pass

`BblPresetSyncController::tick` runs on the preset sync thread after each Orca push pass and calls
`Mirror::run_pass`. It needs both sign-ins, `sync_user_preset` and `sync_bbl_user_preset`.

1. **Snapshot** of every user preset, built on the UI thread (`build_snapshot`).
2. **List** the Bambu presets whenever something local changed, every 5 minutes, and on request.
   No write ever uses a list older than its own pass, because Bambu's `PATCH` has no conflict
   check. A body is downloaded once per version and cached. It is used only if its type and id
   match its row and it holds settings: the plugin reports success even when a body download
   failed, and its cache is keyed by name.
3. **Plan** (`Planner.cpp`, a pure function of snapshot, list and ledger): upload a local change,
   pull a remote one, pair presets of the same name, create or delete on either side.
4. **Execute.** Network calls run on the sync thread. A pass's local changes are queued and
   applied on the UI thread as one batch with one refresh; each rechecks the account session and
   the preset's state before it acts.

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
would lose its profiles. `k_copy_step` copies them, on the same printer: for each connected printer
(the selected one and the other cloud printers of the account, `k_copy_printers`) it reads the table
and adds a copy of each entry of the old id under the new id (`plan_k_copy`). A printer that is not
selected may lack its PA flag in the app, and the app refreshes nozzle diameters only for the
selected printer; the Filament Manager's printer tap, which is fed while this sync is on even with
the Filament Manager tab off, supplies the flag when it is missing and, for a printer that is not
selected, the nozzle diameter it heard in place of the app's. A printer that fails or goes away does not stop the others. A table
read that fails ends that printer's part of the pass, so its other copies are not asked for one by
one; they are tried again ten minutes later. A printer that answers but refuses to list its table
(seen with custom firmware) is not asked again by the automatic copy while the app runs; a manual
request still asks it. It never deletes or
changes an entry, and respects the table limit. Which profile a slot uses is not part of the table
and is chosen when the slot is set.

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
- Two edits of one setting on both sides within the same few seconds can lose one.
- Two Bambu presets of different types with the same name can be uploaded but not pulled.
- A new local preset for a non-BBL printer is not uploaded, even where Studio has that printer.
- The sync runs only while signed in to the Orca cloud as well.
