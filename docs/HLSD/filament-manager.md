# Filament Manager

The Filament Manager is a main tab that shows the filament spools a user keeps in their Bambu account: brand, type,
colour, remaining and full weight, and where a spool is mounted. It is the same list Bambu Studio and Bambu Handy
show. Spools can be added by hand or read from the AMS of the selected printer, edited and deleted.

The list lives in the Bambu cloud. OrcaSlicer stores no spool data of its own.

## Parts

| Part | Where | Role |
|---|---|---|
| Page | `src/slic3r/GUI/FilamentManager/page/` (source), `resources/web/filament_manager/` (built bundle) | The user interface: Bambu Studio's React page, trimmed to the one route. `NOTICE.md` records its origin and every change, `BRIDGE.md` its message contract |
| Panel | `GUI/FilamentManager/FilamentManagerPanel` | The tab: a web view, the transport in both directions |
| View-model | `GUI/FilamentManager/FmViewModel` | One handler per page request; turns service events into reports |
| Data for the page | `FmPresets`, `FmDevices`, `FmColorCodes`, `FmProtocol` | Preset tree, printer and AMS data, Bambu's colour table, packets |
| Runtime | `GUI/FilamentManager/FmRuntime` | Owns the service, the printer tap and the linker for the life of the app; reads the session from the app; ticks the linker |
| Printers | `GUI/FilamentManager/FmPrinters` | Hands printer messages to the tap; builds the linker's view of each printer; the link settings |
| Printer tap | `Utils/BblFilament/PrinterTap` | The tray contents of every printer, from its status messages |
| Linker | `Utils/BblFilament/Linker`, `LinkDriver` | Plans which spool is in which slot and how much is left on it; runs the plan through the service |
| New spools | `Utils/BblFilament/NewSpools`, `GUI/FilamentManager/FmNewSpool`, `FmNewSpoolDialogs` | The marker on a slot that holds a newly registered official spool, and the two dialogs behind it (ported from Bambu Studio) |
| Service | `Utils/BblFilament/SpoolService` | The list and the rules for changing it |
| Cloud | `Utils/BblFilament/SpoolCloud` | The five plugin calls, reply classification, paging |
| Worker and gate | `Utils/BblFilament/SpoolWorker`, `PluginGate` | The one thread that calls the plugin, and its tie to the plugin's lifetime |
| MCP tool | `GUI/Mcp/McpToolsFilamentManager.cpp` | `filament_manager`: status, list, pull, add, update, remove; for linking link_status, link_mode, assign, unassign, sync_now, soft_match, new_spools, soft_match_act (printers by name, never by serial) |

`Utils/BblFilament` has no window code and is covered by unit tests with a fake cloud and a hand-driven executor.

## The page and its transport

The page is not built with OrcaSlicer: `scripts/build_filament_manager_page.sh` builds it with npm and the bundle is
committed, stored byte for byte. Updating to a newer Bambu Studio tag is described in `NOTICE.md`.

The page posts JSON packets `{head:{version,type,seq,ts}, body:{module,submod,action,payload}}` through the web view's
script message channel and waits five seconds for a response with the same `seq`. The app delivers responses and
reports by running a script that dispatches the DOM event `cpp:device` with the packet.

The page restarts `seq` at zero each time it loads, and a script handed to the web view can run after the page has
been reloaded. Every load therefore carries a generation number in its URL (`?gen=N`). A request is attributed to the
generation in the URL it came from, and the delivery script checks the generation of the document it runs in, so a
packet meant for an earlier load never reaches a later one.

The tab loads a fresh page every time it is shown, with the language and theme in the URL. A theme or language change
while it is shown reloads it. On macOS the page is replaced by an empty one while the tab is hidden, because a hidden
web view there can starve the app's idle processing; the tab is off by default on macOS for the same reason
(`filament_manager_enabled`, changed in Preferences, effective after a restart).

Every request is answered at once from what the app already knows. Anything that takes a cloud call is answered with
the current state and followed by reports when the call ends.

Only the page may use the bridge: a message is handled only when the document it came from is the bundled page with
the current generation, and the tab never navigates to a web address (such a link opens in the default browser).
Packets carry spool lists and printer names, so they are handed to the web view through the script runner that does
not log.

## The cloud calls

The network plugin offers five calls: list (paged), create, update, delete and the catalogue of Bambu's own filaments.
Four more calls link spools to printer slots: the AMS sync, the slot mappings, and the queue of new official spools with its decision call. The first is a wrapped plugin function; the other three are resolved by name at every call (the plugin can be reloaded) and take structs by value, whose layout is fixed in `PluginLinkStructs.hpp`. They run as ops in the service queue (`link`), one call at a time, and the service never shows them in `view()`. A spool id that does not fit the plugin's 32-bit field is refused, because 0 would unbind the slot.
Three properties of these calls shape the service:

- **A call returns a code and the raw response body, never the HTTP status.** A reply is classified from both: success;
  a JSON body with a `code` of 400 to 499 is a definite refusal (`NotFound` for 404, else `Rejected`); anything else is
  `Unknown`: the request may or may not have been applied.
- **Create returns no id.** The only way to learn the new spool is to list again. The service never invents an id and
  never guesses which listed spool it created.
- **Update names its record by an `id` inside the body** and must always carry the filament name.

The list call takes an offset and a limit, but Bambu's server currently answers with the whole list whatever is asked
for. The list is therefore asked for 20 spools at a time and a reply with more rows than that is taken as the whole
list. Should the server start to page, reading continues until a short or empty page, or until the `total` it may
send. A failed page fails the whole list. A page that brings no new id ends the loop and marks the result as suspect,
so a server that ignored the offset cannot cause an endless loop. A reply that is not JSON is a failure, not an empty
account.

## The service

The service lives on the UI thread. It holds the spools of the last complete list (`truth`), a queue of operations,
and whether a new list is wanted.

- One cloud call runs at a time. A list is started only when no write is waiting, so one list follows any number of
  writes and several requests for a list collapse into one.
- What the page shows is `truth` with the waiting edits and deletions applied on top. An edit or deletion is visible
  at once. One the cloud refuses is dropped, and the old row returns. One it accepts stays on top until the next
  complete list replaces `truth`. A failed list changes nothing.
- A create whose outcome is `Unknown` is never sent again: the list is read and the failure reported.
- An update that ends `NotFound` is never turned into a create: the spool was deleted elsewhere.
- An update's body is built when it is sent, from the row as it stands then. Because every update carries the filament
  name, an edit built from a stale row would undo an earlier rename. For the same reason, when an update ends
  `Unknown`, the edits of that spool still waiting are dropped and reported.
- The body of an update carries only the fields the page's patch changed. A remaining weight of zero is sent as zero.
- Failures are reported as kinds; the panel words them.

Every job carries the session it started under: signed in, the Bambu user id, the plugin generation, the app not
closing. A result whose session no longer matches is dropped and the state is cleared. A job still waiting when the
session changes makes no cloud call at all, so a queued write can never run against another account.

## The plugin's lifetime

Plugin calls block for as long as an HTTP request takes, and the plugin can be unloaded or reloaded while the app
runs. All plugin calls of this feature are made by one worker thread. `BBLNetworkPlugin::destroy_agent()` drains that
worker before the agent goes away: no new job starts and the call in flight is waited for. `create_agent()` reopens
it. The wait is as long as the plugin's own HTTP timeout at most. A job the worker discards still completes, as
unavailable, so the service never waits for a result that will not come. The worker is never destroyed: the plugin
singleton is torn down during static destruction and must still find it.

A change of the Bambu sign-in is noticed by the service at its next request and, while the tab is open, by a check
every two seconds; no hook in the sign-in code is needed.

## Filament ids

A spool names its filament by Bambu's id (`GFA00`), or a `P…` id for a custom filament. OrcaSlicer's system presets
carry ids of its own, and the trays of its printer model carry those too. Everything sent to the page is translated to
Bambu's ids with `BBLPrinterAgent::bambu_filament_id`:

- the preset tree offers a filament only when its id means something to Bambu: a system preset whose id maps, a user
  preset derived from one, or a custom filament;
- tray data is translated back before the page sees it;
- lookups by Bambu id (support material, type) search the presets through the same mapping.

Bambu's colour table (`filaments_color_codes.json`) supplies the colour names, the palette of official colours and the
short colour code from which a spool's `trayIdName` is derived. `FmColorCodes` reads it itself because the app's
existing reader does not keep the short code.

## Linking spools to printer slots

The cloud records which spool is in which AMS slot and how much is left on it. While the Filament Manager is enabled,
the app keeps that record current for every Bambu printer of the account, not only the selected one.

- **Printer tap.** The app parses tray contents only for the selected printer. Every status message of every Bambu
  printer (both message handlers of `GUI_App`) therefore also goes to the tap, which keeps the AMS and external-spool
  parts of each printer's state, merged the way the app merges them. The tap is fed while the Filament Manager or the
  Bambu preset sync is enabled (the K copy of the preset sync reads its nozzle diameter and calibration support); linking
  itself needs the Filament Manager. Old tray contents never count as current: a message that follows the previous one
  of that printer by more than a minute makes the tap forget the printer first, so a diff after a gap leaves it without
  a full state; the runtime makes the tap forget a printer that is offline or gone, and forget everything when the Bambu
  session changes or ends.
- **Views.** Every two seconds the runtime builds one view per printer of the account, plus the selected printer in
  LAN mode: bound to the account, LAN-only, selected, busy (printing, or the AMS doing anything but idling), fresh
  (online, a full state since the tap last forgot it, a message within a minute), the AMS serials from the printer's
  version reply, and the trays with their filament ids translated to Bambu's. The tap forgets a printer silent for 90
  seconds; a printer that sends only diffs is asked for a full state at most once a minute. A LAN-only printer takes
  part only while it is selected.
- **Matching.** A tray is matched to a spool by its RFID, else by a pending assignment (the user put that spool in
  that slot; it matches only when the tray reports the spool's filament and colour, and lapses after two minutes),
  else by the cloud's own record when the tray agrees with it. A tray with an RFID the account does not have never
  matches anything else.
- **Changes.** A matched spool not recorded in its slot is mounted there (an RFID spool through the AMS sync, a manual
  one through the slot mappings). The remaining weight of an RFID spool is sent when the printer measures another
  value: the grams the printer reports, else its percentage of the full weight. A percentage never overrules a spool
  whose own weight rounds to the same percentage, nor raises a spool's weight by 5 % of the full weight or less (a
  spool does not gain filament and the AMS estimate drifts by a few percent; the link status notes the weight was
  kept), and a busy printer gets a new weight at most every ten minutes. A
  spool recorded in a slot where no tray matched it is unbound only after three different status messages, 30 seconds
  and a newer list, and never while another spool is matched in that slot, the spool in that slot cannot be identified
  yet (its tag is being read or was not read since the AMS restarted), or the spool has a change waiting. A new
  official spool is added to the account. AMS Lite units and external spools get no slot record: the cloud names a
  slot by its AMS serial.
- **Rounds.** The driver sends a plan as one round of calls per printer through the service queue, and plans the next
  round only after the list that follows it has arrived. Something the cloud took, or refused, is not sent again for
  ten minutes, even when the next list does not show it (the cloud may store a value differently than it is compared).
  A newly registered spool is retried with a growing delay. An automatic weight push is reported to the page as
  `sync/auto_push_summary`.
- **Settings.** The app config keeps one value, `filament_manager_link_enabled` (Preferences, on by default). The mode
  (`on`, `off`, or `dry_run`, which plans without sending) is, in this order of precedence: the session override set
  through the MCP `link_mode`, which lasts until the app closes and is never stored (`default` clears it); the
  environment variable `ORCA_FM_LINK_MODE` (read once at start-up, so a new build can be tried against a real account in
  report-only mode from its first second); the Preferences value. A list of slots that linking may touch is a session
  setting of `link_mode` only (empty = every slot). `link_status` shows the mode, its `mode_source` (`session`, `env` or
  `config`), the slots and the Preferences value. While linking is on, the runtime reads the list itself when it has
  none, so linking does not depend on the tab being open.
- **Explicit writes.** An assignment, an unbind asked for by hand and an answer to the new-spool queue are refused
  unless the mode is `on`; an assignment and an unbind also for a slot outside a non-empty list of slots. In a dry run an
  unbind answers what it would send. A spool created for a slot that linking may not touch is only added. When the mode
  or the list of slots changes, the ops of the round still queued send nothing.

## A new official spool

When the linker registers a new official spool, the slot gets a marker on the Device tab (`AMSLib`, as in Bambu
Studio). The runtime keeps the marked slots in memory, forgets them when the list goes away, and drops a marker when
its slot is empty or reports another spool on a printer with fresh data. Clicking the marker reads the cloud's queue of
new spools for that AMS. When the queue has no entry for the spool, a dialog says the spool was recorded. Otherwise a
dialog offers to add it as a new spool or to confirm the match, the spool the cloud proposes or one of its candidates;
the answer is sent as `create_new`, `link_other` or `accept`, and the list is read again. The marker goes once the
"recorded" dialog was shown or the answer was sent successfully, and only while it still names the same spool; closing
the dialog keeps it. A failed call is shown with the Filament Manager's failure texts and keeps the marker. The queue
belongs to the account it was read for: when the Bambu user or the plugin load differs by the time the reply arrives,
or by the time the answer would be sent, nothing is shown or sent.

## Choosing a spool in the AMS slot dialog

The slot dialog of a printer that takes part in linking offers the spools without RFID that are not in a printer. A
spool the dialog's printer cannot take is shown greyed with the reason: no preset of its filament for that printer, or
several colours on a printer that takes one colour per slot (or more colours than the colour picker edits). The dialog
keeps the printer's dev id; after the picker, and when the dialog is confirmed, it continues only while the device
manager still holds that same printer object, because a sign-out or a refresh of the printer list deletes printers.

## Not part of this

The page has no control for favourites, archiving or marking a spool empty; those requests are answered as not
supported. The page's own control for linking is removed, and creates and updates from the page never send mount
fields.
