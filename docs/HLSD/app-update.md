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
  backslash, an encoded dot, slash or backslash) does not count, and the installer name must be a plain file name.

The pure parts (prefix, asset choice, checksum file, hashing, verdict) live in `src/slic3r/Utils/AppUpdate.*`
without wx or network code and are covered by the `[AppUpdate]` tests.

## The download and its checks

`AppUpdateDialog` (Windows only) works in `<user temp>/OrcaSlicer_update/`. It deletes every other file there,
then downloads the `.sha256` asset (at most 4 KB). Its first token must be 64 hex digits; a file name after it
must be the installer's. When the installer is already in the folder from an earlier attempt and passes the
checks below, it is used as it is; otherwise it is downloaded (size limit: the published size plus 1 MB),
written to `<name>.part` and renamed to `<name>`. Both downloads run on `Http`'s own thread with TLS
certificate verification enabled; results reach the dialog through `CallAfter` guarded by a shared alive flag,
and closing the dialog cancels the request.

The file then must have the size given in the release JSON, its SHA-256 must equal the `.sha256` asset, and,
when the release JSON gives a `sha256:<hex>` digest for the installer, it must equal that too. A file that fails
any check is deleted and never started; the dialog says which check failed and offers the release page.

## Why the file handle is held

The installer runs elevated. Between the check and the start, another process could replace the file in the
user's temp folder. So the dialog opens the file once, shared for reading only (no write and no delete sharing),
hashes it through that handle, and keeps the handle (`HeldFile`) open until the installer process has been
started. Nobody can write, rename or delete the file in between. Later closes the dialog, releases the handle and
keeps the file for the next attempt.

## Closing OrcaSlicer and starting the installer

Install now hands the held file to `GUI_App::install_update_on_exit`. After the modal loops have unwound, the app
calls `mainframe->Close(false)`, the normal close path, so unsaved projects and presets are asked about as usual.
If the user cancels there (`Close` returns false, or the frame is not scheduled for destruction afterwards), the
held file is released and nothing else happens. Otherwise, at the end of `GUI_App::OnExit`, the app ends the
helper processes (below), starts the installer with `ShellExecuteExW` (verb `open`, its normal user interface;
Windows shows its administrator prompt), and then releases the handle. A failed start can only be logged.

`ShellExecuteExW` returns once the administrator prompt has been answered, and the exit watchdog started by the
main frame's close (`GUI_App::start_exit_watchdog`) ends the process 30 seconds after the close began.

## Other OrcaSlicer processes

Other processes running the same executable (same full image path, compared without regard to case) would keep
the installed files in use. A process with a visible top-level window is another OrcaSlicer window; the update
prompt asks for those to be closed first, and Install now repeats the check until none is left. The others are
helpers: the `--mcp-stdio` bridges that AI agent clients start, which have no window. The prompt says how many
agent connections will be closed; only those helpers are ever terminated, and only on the Install now path at
exit (`TerminateProcess`, then up to 3 s of waiting for each).

## What the checksum proves

The checksum proves the integrity of the download against the release: the bytes started are the bytes the
release published, not a truncated, corrupted or swapped file. It does not prove that the release itself is
trustworthy. The release is trusted through HTTPS to `github.com` (certificates verified on the downloads) and
through the repository it belongs to: the `.sha256` asset comes from the same release as the installer, so anyone
who can publish a release in that repository can publish a matching checksum. The update check request itself
does not verify the server's certificate; it can only point to assets below the repository's download prefix.
