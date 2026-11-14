# Application updates

OrcaSlicer checks for a newer release at startup (and from Help > Check for Updates) and shows the update
prompt (`UpdateVersionDialog`) with the release notes. On Windows desktop builds, the prompt's Download button
downloads the installer, verifies it against the checksum published with the release and starts it once
OrcaSlicer has closed. Everywhere else, and whenever one of the conditions below is not met, Download opens the
release page in the browser. Microsoft Store builds open their Store page instead and are updated by the Store.

## Where updates come from

`GUI_App::check_new_version_sf` requests `AppConfig::version_check_url()`: the built-in URL
(`AppConfig::default_version_check_url()`, this fork's GitHub releases API,
`https://api.github.com/repos/<owner>/<repo>/releases`) unless `version_check_url` in the config overrides it.
The answer is a GitHub release list (or a single release). Each release's tag is compared with the running build;
the best stable release and the best pre-release are kept, together with their release page URL, notes and
`assets` (name, `browser_download_url`, `size`, and `digest` when GitHub gives one). "Check for stable updates
only" picks between the two.

The request runs on `Http`'s own thread. It builds a complete `VersionInfo` there and hands it to the UI thread
(`CallAfter`), where `GUI_App::version_info` is replaced and `EVT_SLIC3R_VERSION_ONLINE` is sent. The handler copies
`version_info` once at its start and uses only that copy for the prompt, the browser fallback and
`AppUpdateDialog`, so a check that completes while the prompt is open cannot change what is shown or installed.

## Version comparison

Tags look like `v2.5.0.2+custom`: the upstream version plus this fork's revision as a fourth number. The version
parser folds a fourth number into the patch, so both sides are compared in their four-part form; see
`src/slic3r/Utils/CustomVersion.hpp`. A release is offered when it is newer than `custom_build_version()`.

## When the in-app install is offered

`VersionInfo::installer` is set only when all of these hold:

- the build is a Windows desktop build (`_WIN32`, not running in an MSIX package);
- the check used the built-in URL, not an override, and that URL has the GitHub releases form, so a download
  prefix `https://github.com/<owner>/<repo>/releases/download/` follows from it
  (`AppUpdate::download_prefix_for`);
- the chosen release has exactly one asset whose name contains `Installer` and ends with this build's
  architecture suffix (`_x64.exe` or `_arm64.exe`, as CPack names the installers), with a size from 1 MB to
  1 GB, plus an asset named `<installer name>.sha256` (`AppUpdate::choose_installer`);
- both asset URLs lie below the download prefix: a URL whose remainder could climb out of it (a `..` segment, a
  backslash, an encoded dot, slash or backslash) does not count, and the installer name must be a plain file name;
- a `digest` of the installer asset that starts with `sha256:` is followed by exactly 64 hex digits (an empty
  digest, or one of another algorithm, counts as none).

The pure parts (prefix, asset choice, checksum file, hashing, verdict) live in `src/slic3r/Utils/AppUpdate.*`
without wx or network code and are covered by the `[AppUpdate]` tests.

## The download and its checks

`AppUpdateDialog` (Windows only) works in `<user temp>/OrcaSlicer_update/`. When that folder is a reparse point
(a junction or symbolic link), nothing is deleted or downloaded and the dialog reports that the download could
not be saved. Otherwise it deletes every other file there,
then downloads the `.sha256` asset (at most 4 KB). Its first token must be 64 hex digits; a file name after it
must be the installer's. When the installer is already in the folder from an earlier attempt and passes the
checks below, it is used as it is; otherwise it is downloaded (size limit: the published size plus 1 MB),
written to a new staging file with a unique name (`<name>.<random hex>.part`, created with `CREATE_NEW` and no
sharing, so no existing path is opened or truncated), flushed, closed and renamed to `<name>`; on any failure the
staging file is deleted, and leftovers go with the next start-up cleanup of the folder. Both downloads run on `Http`'s own thread with TLS
certificate verification enabled; results reach the dialog through `CallAfter` guarded by a shared alive flag,
and closing the dialog cancels the request. A callback that finds the dialog gone returns at once: it writes
nothing and posts nothing.

The file then must have the size given in the release JSON, its SHA-256 must equal the `.sha256` asset, and,
when the release JSON gives a `sha256:<hex>` digest for the installer, it must equal that too. A file that fails
any check is deleted and never started; the dialog says which check failed and offers the release page.

## Why the file handle is held

The installer runs elevated. Between the check and the start, another process could replace the file in the
user's temp folder. So the dialog opens the file once, shared for reading only (no write and no delete sharing),
hashes it through that handle, and keeps the handle (`HeldFile`) open until the installer process has been
started. Nobody can write, rename or delete the file in between. The launch goes by path, so the file must be a
plain file: it is opened with `FILE_FLAG_OPEN_REPARSE_POINT` and accepted only when the handle shows neither
`FILE_ATTRIBUTE_REPARSE_POINT` nor `FILE_ATTRIBUTE_DIRECTORY`. A symbolic link at that name, which could be verified
through and re-pointed before the launch, fails the check like any other: it is deleted and never started. Later
closes the dialog, releases the handle and keeps the file for the next attempt.

## Closing OrcaSlicer and starting the installer

Install now hands the held file to `GUI_App::install_update_on_exit`. After the modal loops have unwound, the app
calls `mainframe->Close(false)`, the normal close path, so unsaved projects and presets are asked about as usual.
If the user cancels there (`Close` returns false, or the frame is not scheduled for destruction afterwards), the
held file is released and nothing else happens. Otherwise, at the end of `GUI_App::OnExit`, the app ends the
helper processes (below), starts the installer with `ShellExecuteExW` (verb `open`, its normal user interface;
Windows shows its administrator prompt), and then releases the handle. A failed start can only be logged.

`ShellExecuteExW` returns once the administrator prompt has been answered. The exit watchdog started by the main
frame's close (`GUI_App::start_exit_watchdog`) normally ends the process 30 seconds after the close began. While an
installer is waiting to be started (a process-wide flag, set once the close went through and cleared after the
start returned), it waits in one-second steps for up to ten more minutes, then gives the rest of the shutdown
another 30 seconds.

## Other OrcaSlicer processes

Other processes running the same executable (same full image path, compared without regard to case) would keep
the installed files in use. `find_other_instances` opens one handle per such process and uses it for the path
check, the classification and any termination, so a reused process id is never hit. A process is

- a **helper** when it says so: an `--mcp-stdio` bridge (`run_stdio_bridge`) creates the named manual-reset event
  `Local\OrcaSlicerMcpStdio.<process id>` (`AppUpdate::helper_event_name`) at start, keeps it for its lifetime and
  ends itself at once when it is set (the bridge holds no state);
- a **window** when it is not a helper and owns a visible top-level window;
- an **other** when it is neither: an instance still starting, or a command-line job such as `--slice`.

Windows and others block the in-app install: the prompt asks for them to be closed first, and Install now repeats
the check until none is left. The prompt says how many agent connections will be closed. Only helpers are ever
ended, and only on the Install now path at exit: their event is set, and one still running after 3 s is terminated
through the same handle, with up to 3 s more. The launch goes ahead even when a helper could not be ended or another
instance appeared meanwhile: the installer makes its own check and asks the user (below).

## The installer and running copies

A running `orca-slicer.exe` keeps `OrcaSlicer.dll` and the other installed files in use, so the installer must not
replace them while any process of that installation runs, a window or not. The NSIS installer (CPack's template
plus `orca_close_running_install.nsh`, generated by `CMakeLists.txt`) therefore starts its install section with
`scripts/installer_close_running.ps1` for the target folder, and for the previous installation's folder (from its
uninstall registry entry) when that is another one. The script looks at the processes named `orca-slicer.exe`
whose executable lies inside the folder. Those started with `--mcp-stdio` are helpers, the bridges AI agent clients
start; it stops them and waits up to 5 seconds for them to end. Any other one, including a process whose command
line cannot be read, is an OrcaSlicer window, which the user has to close: the installer asks with Retry / Cancel,
and Cancel aborts the installation (a silent install aborts). When PowerShell cannot be started, the installer goes
on as before.

The order is: check, then remove the previous version by running its uninstaller silently in place, then write the
new files. CPack's own "uninstall before install" is off because it runs in `.onInit`, before any hook. The
uninstaller (`orca_close_running_uninstall.nsh`) does the same check for its folder before it deletes any file.

## What the checksum proves

The checksum proves the integrity of the download against the release: the bytes started are the bytes the
release published, not a truncated, corrupted or swapped file. It does not prove that the release itself is
trustworthy. The release is trusted through HTTPS to `github.com` (certificates verified on the downloads) and
through the repository it belongs to: the `.sha256` asset comes from the same release as the installer, so anyone
who can publish a release in that repository can publish a matching checksum. The update check request verifies
the server's certificate whenever it can lead to an in-app install (the built-in URL). When that request fails
without an HTTP status (a transport or certificate error), it is repeated once without verification and without
the in-app install, so the prompt still appears and Download opens the release page; an overridden URL is
requested without verification, as before.
