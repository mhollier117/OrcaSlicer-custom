<#
.SYNOPSIS
    Stops the agent helper processes of an OrcaSlicer installation, optionally asks its windows to close, and
    counts the processes still running.

.DESCRIPTION
    The Windows installer and uninstaller run this before they remove or write any file of the installation
    (docs/HLSD/app-update.md, "The installer and running copies"). A running orca-slicer.exe keeps
    OrcaSlicer.dll and the other files in use, and NSIS then fails with "Error opening file for writing".

    Processes named orca-slicer.exe whose executable lies inside InstallDir belong to the installation. Those
    started with the --mcp-stdio argument are helpers: the bridges AI agent clients start, which have no window.
    They are stopped here. Any other process of the installation is an OrcaSlicer window, which is the user's to
    close; so is a process whose command line cannot be read.

    With -CloseWindows, after the helpers were stopped, every other process of the installation whose main window
    is enabled is sent a normal close request (CloseMainWindow), so OrcaSlicer runs its own close path and asks
    about unsaved work there. A window is never terminated. A process without a main window, or whose main window
    is disabled because one of its dialogs is open, is not asked and still counts as running. The script then
    waits up to 30 seconds for the asked processes to end; one that is still asking about unsaved work counts as
    running when the wait ends.

    Exit code: the number of processes of the installation still running (windows, or helpers that did not
    stop), at most 100. 0 also when the processes cannot be listed at all: the installer then goes on as it
    always did. Nothing is printed.

    Runs under Windows PowerShell 5.1 and PowerShell 7.

.PARAMETER InstallDir
    The installation folder.

.PARAMETER CloseWindows
    Also ask the installation's OrcaSlicer windows to close, as described above.

.EXAMPLE
    powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File installer_close_running.ps1 "C:\Program Files\OrcaSlicer"

.EXAMPLE
    powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File installer_close_running.ps1 "C:\Program Files\OrcaSlicer" -CloseWindows
#>
param(
    [Parameter(Mandatory)][string]$InstallDir,
    [switch]$CloseWindows
)

$ErrorActionPreference = 'Stop'

# The ids among $Ids that still belong to a running orca-slicer process. The name check guards against a reused
# process id. Callers wrap the result in @(), as PowerShell unrolls a returned array.
function Get-StillRunning([int[]]$Ids) {
    $alive = @()
    foreach ($id in $Ids) {
        $process = Get-Process -Id $id -ErrorAction SilentlyContinue
        if ($process -and $process.ProcessName -eq 'orca-slicer') {
            $alive += $id
        }
    }
    return $alive
}

try {
    # Compared with a trailing backslash, so C:\X\OrcaSlicer does not match C:\X\OrcaSlicer2.
    $root = [System.IO.Path]::GetFullPath($InstallDir).TrimEnd('\') + '\'

    $installed = @(Get-CimInstance -ClassName Win32_Process -Filter "Name = 'orca-slicer.exe'" | Where-Object {
            $_.ExecutablePath -and $_.ExecutablePath.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)
        })
    $helpers = @($installed | Where-Object { $_.CommandLine -and $_.CommandLine -match '(^|\s)"?--mcp-stdio"?(\s|$)' })

    foreach ($helper in $helpers) {
        try {
            Stop-Process -Id $helper.ProcessId -Force -ErrorAction Stop
        } catch {
            # Already gone, or not ours to stop: counted below if it is still running.
        }
    }

    # Up to 5 seconds for the stopped helpers to be gone.
    $deadline = (Get-Date).AddSeconds(5)
    $running = @($helpers | ForEach-Object { [int]$_.ProcessId })
    while ($running.Count -gt 0) {
        $running = @(Get-StillRunning $running)
        if ($running.Count -eq 0 -or (Get-Date) -ge $deadline) {
            break
        }
        Start-Sleep -Milliseconds 200
    }

    if (-not $CloseWindows) {
        $remaining = ($installed.Count - $helpers.Count) + $running.Count
        exit ([Math]::Min($remaining, 100))
    }

    $helperIds = @($helpers | ForEach-Object { [int]$_.ProcessId })
    $windowIds = @($installed | ForEach-Object { [int]$_.ProcessId } | Where-Object { $helperIds -notcontains $_ })

    # Without IsWindowEnabled no window counts as enabled: nothing is asked.
    $canAsk = $true
    try {
        Add-Type -Namespace OrcaSlicerInstaller -Name User32 -ErrorAction Stop -MemberDefinition @'
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool IsWindowEnabled(System.IntPtr hWnd);
'@
    } catch {
        $canAsk = $false
    }

    # A normal close request to each enabled main window: OrcaSlicer asks about unsaved work itself.
    $asked = @()
    if ($canAsk) {
        foreach ($id in $windowIds) {
            try {
                $process = Get-Process -Id $id -ErrorAction Stop
                if ($process.ProcessName -ne 'orca-slicer' -or $process.MainWindowHandle -eq [System.IntPtr]::Zero) {
                    continue
                }
                if (-not [OrcaSlicerInstaller.User32]::IsWindowEnabled($process.MainWindowHandle)) {
                    continue
                }
                if ($process.CloseMainWindow()) {
                    $asked += $id
                }
            } catch {
                # Gone meanwhile, or not reachable: counted below if it is still running.
            }
        }
    }

    # Up to 30 seconds for the asked processes to end.
    $deadline = (Get-Date).AddSeconds(30)
    while ($asked.Count -gt 0) {
        $asked = @(Get-StillRunning $asked)
        if ($asked.Count -eq 0 -or (Get-Date) -ge $deadline) {
            break
        }
        Start-Sleep -Milliseconds 250
    }

    $remaining = @(Get-StillRunning $windowIds).Count + $running.Count
    exit ([Math]::Min($remaining, 100))
} catch {
    exit 0
}
