<#
.SYNOPSIS
    Stops the agent helper processes of an OrcaSlicer installation and counts the processes still running.

.DESCRIPTION
    The Windows installer and uninstaller run this before they remove or write any file of the installation
    (docs/HLSD/app-update.md, "The installer and running copies"). A running orca-slicer.exe keeps
    OrcaSlicer.dll and the other files in use, and NSIS then fails with "Error opening file for writing".

    Processes named orca-slicer.exe whose executable lies inside InstallDir belong to the installation. Those
    started with the --mcp-stdio argument are helpers: the bridges AI agent clients start, which have no window.
    They are stopped here. Any other process of the installation is an OrcaSlicer window, which is the user's to
    close; so is a process whose command line cannot be read.

    Exit code: the number of processes of the installation still running (windows, or helpers that did not
    stop), at most 100. 0 also when the processes cannot be listed at all: the installer then goes on as it
    always did. Nothing is printed.

    Runs under Windows PowerShell 5.1 and PowerShell 7.

.PARAMETER InstallDir
    The installation folder.

.EXAMPLE
    powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File installer_close_running.ps1 "C:\Program Files\OrcaSlicer"
#>
param([Parameter(Mandatory)][string]$InstallDir)

$ErrorActionPreference = 'Stop'

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

    # Up to 5 seconds for the stopped helpers to be gone. The name check guards against a reused process id.
    $deadline = (Get-Date).AddSeconds(5)
    $running = @($helpers | ForEach-Object { [int]$_.ProcessId })
    while ($running.Count -gt 0) {
        $running = @($running | Where-Object {
                $process = Get-Process -Id $_ -ErrorAction SilentlyContinue
                $process -and $process.ProcessName -eq 'orca-slicer'
            })
        if ($running.Count -eq 0 -or (Get-Date) -ge $deadline) {
            break
        }
        Start-Sleep -Milliseconds 200
    }

    $remaining = ($installed.Count - $helpers.Count) + $running.Count
    exit ([Math]::Min($remaining, 100))
} catch {
    exit 0
}
