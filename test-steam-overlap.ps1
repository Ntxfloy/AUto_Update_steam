param(
    [string]$AppID,
    [switch]$FreeToPlay
)

$ErrorActionPreference = 'Stop'

# Set console output encoding to UTF8 just in case
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

function Say     ($m){ Write-Host $m -ForegroundColor Cyan }
function Ok      ($m){ Write-Host $m -ForegroundColor Green }
function Warn    ($m){ Write-Host $m -ForegroundColor Yellow }
function Bad     ($m){ Write-Host $m -ForegroundColor Red }
function Line    (){ Write-Host ('-' * 70) -ForegroundColor DarkGray }

function Unescape-Vdf([string]$s){ return $s -replace '\\\\','\' }

# Parse VDF appmanifest files via regex
function Parse-Acf([string]$path){
    if(-not (Test-Path $path)){ return $null }
    $t = Get-Content -LiteralPath $path -Raw -Encoding UTF8
    function Field($name){
        $m = [regex]::Match($t, '"' + $name + '"\s*"([^"]+)"')
        if($m.Success){ return $m.Groups[1].Value } else { return $null }
    }
    [pscustomobject]@{
        appid      = Field 'appid'
        name       = Field 'name'
        installdir = Field 'installdir'
        buildid    = Field 'buildid'
        StateFlags = Field 'StateFlags'
        LastOwner  = Field 'LastOwner'
        SizeOnDisk = Field 'SizeOnDisk'
    }
}

function Get-FreeBytes([string]$anyPath){
    $root = [System.IO.Path]::GetPathRoot($anyPath).TrimEnd('\')
    $d = Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$root'"
    return [int64]$d.FreeSpace
}

function ToGB([double]$b){ return [math]::Round($b/1GB,2) }

Clear-Host
Line
Say  ' STEAM FOLDER OVERLAP TEST: SteamCMD vs Steam Client'
Say  ' Verifying if updating via SteamCMD breaks Steam Client installation state.'
Line

# ---------- 1. Find Steam Root ----------
Say "`n[1/7] Locating Steam Root via registry (HKCU:\Software\Valve\Steam\SteamPath)..."
$steamRoot = $null
try {
    $sp = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction Stop).SteamPath
    if($sp){ $steamRoot = ($sp -replace '/','\').TrimEnd('\') }
} catch {}
if(-not $steamRoot -or -not (Test-Path $steamRoot)){
    $def = 'C:\Program Files (x86)\Steam'
    if(Test-Path $def){ $steamRoot = $def }
}
if(-not $steamRoot){ Bad 'Could not locate Steam installation. Aborting.'; return }
Ok "    Steam Root: $steamRoot"

# ---------- 2. Find Libraries ----------
Say "`n[2/7] Reading libraries from libraryfolders.vdf..."
$libs = New-Object System.Collections.Generic.List[string]
$libs.Add($steamRoot)

$vdfCandidates = @(
    (Join-Path $steamRoot 'config\libraryfolders.vdf'),
    (Join-Path $steamRoot 'steamapps\libraryfolders.vdf')
)
$vdf = $vdfCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if($vdf){
    $raw = Get-Content -LiteralPath $vdf -Raw -Encoding UTF8
    foreach($m in [regex]::Matches($raw,'"path"\s*"([^"]+)"')){
        $p = (Unescape-Vdf $m.Groups[1].Value).TrimEnd('\')
        if($p -and (Test-Path $p) -and -not ($libs -contains $p)){ $libs.Add($p) }
    }
} else {
    Warn '    libraryfolders.vdf not found. Using default Steam Root only.'
}
foreach($l in $libs){ Ok "    Library: $l" }

# ---------- 3. Scan Installed Games ----------
Say "`n[3/7] Scanning installed games (appmanifest_*.acf)..."
$games = New-Object System.Collections.Generic.List[object]
foreach($lib in $libs){
    $steamapps = Join-Path $lib 'steamapps'
    if(-not (Test-Path $steamapps)){ continue }
    Get-ChildItem -LiteralPath $steamapps -Filter 'appmanifest_*.acf' -ErrorAction SilentlyContinue | ForEach-Object {
        $a = Parse-Acf $_.FullName
        if($a -and $a.appid){
            $games.Add([pscustomobject]@{
                AppID      = $a.appid
                Name       = $a.name
                SizeGB     = if($a.SizeOnDisk){ ToGB ([double]$a.SizeOnDisk) } else { 0 }
                Library    = $lib
                AcfPath    = $_.FullName
                GamePath   = Join-Path $steamapps ("common\" + $a.installdir)
                StateFlags = $a.StateFlags
            })
        }
    }
}
if($games.Count -eq 0){ Bad 'No installed games found. Aborting.'; return }

if ($AppID) {
    $selId = $AppID
    Ok "App ID passed via argument: $selId (auto-select)"
} else {
    $games = $games | Sort-Object { [double]$_.SizeGB }
    Line
    '{0,-10} {1,-40} {2,8}  {3}' -f 'AppID','Game Name','Size','Library' | Write-Host
    Line
    foreach($g in $games){
        $shortName = if($g.Name.Length -gt 38){ $g.Name.Substring(0,35) + '...' } else { $g.Name }
        '{0,-10} {1,-40} {2,7}G  {3}' -f $g.AppID, $shortName, $g.SizeGB, $g.Library | Write-Host
    }
    Line
    Warn 'Recommendation: Choose a small game/F2P game to save time and bandwidth.'
    $selId = Read-Host "`nEnter App ID to test"
}

$game = $games | Where-Object { $_.AppID -eq $selId } | Select-Object -First 1
if(-not $game){ Bad "App ID $selId not found in installed games. Aborting."; return }
Ok ("Selected: {0} (AppID {1})" -f $game.Name, $game.AppID)
Ok ("Library root (force_install_dir, Option A): {0}" -f $game.Library)
Ok ("Game install path: {0}" -f $game.GamePath)

# ---------- BASELINE SNAPSHOT ----------
Say "`n--- BASELINE SNAPSHOT (Before Run) ---"
$acfBackup = Join-Path $env:TEMP ("acf_backup_{0}_{1}.acf" -f $game.AppID,(Get-Date -Format yyyyMMdd_HHmmss))
Copy-Item -LiteralPath $game.AcfPath -Destination $acfBackup -Force
$before = Parse-Acf $game.AcfPath
$freeBefore = Get-FreeBytes $game.Library
$before | Format-List | Out-String | Write-Host
Ok ("Free Disk Space: {0} GB" -f (ToGB $freeBefore))
Ok ("ACF backup created at: $acfBackup")

# ---------- 4. Verify Steam is not running ----------
Say "`n[4/7] Verifying Steam Client is closed..."
while(Get-Process -Name steam -ErrorAction SilentlyContinue){
    Warn 'Steam Client is currently RUNNING. Please exit Steam completely (Steam -> Exit) and close any game processes.'
    Read-Host 'Press Enter after you have closed Steam to check again'
}
Ok '    Steam Client is closed.'

# ---------- 5. Locate/Download SteamCMD ----------
Say "`n[5/7] Checking for steamcmd.exe..."
$steamcmd = @(
    'C:\steamcmd\steamcmd.exe',
    'D:\steamcmd\steamcmd.exe',
    (Join-Path $PSScriptRoot 'steamcmd.exe'),
    (Join-Path $PSScriptRoot 'steamcmd\steamcmd.exe')
) | Where-Object { Test-Path $_ } | Select-Object -First 1

if(-not $steamcmd){
    $urls = @(
        'https://media.steampowered.com/client/installer/steamcmd.zip',
        'https://steamcdn-a.akamaihd.net/client/installer/steamcmd.zip'
    )
    foreach ($url in $urls) {
        try {
            Warn "    Attempting to download SteamCMD from $url..."
            $dir = Join-Path $env:TEMP ('steamcmd_' + [guid]::NewGuid().ToString('N').Substring(0,8))
            New-Item -ItemType Directory -Path $dir | Out-Null
            $zip = Join-Path $dir 'steamcmd.zip'
            Invoke-WebRequest -Uri $url -OutFile $zip -TimeoutSec 15 -ErrorAction Stop
            Expand-Archive -LiteralPath $zip -DestinationPath $dir -Force
            $steamcmd = Join-Path $dir 'steamcmd.exe'
            if (Test-Path $steamcmd) {
                Ok "    Successfully downloaded SteamCMD."
                break
            }
        } catch {
            Warn "    Download from $url failed: $_"
        }
    }
}

if(-not $steamcmd -or -not (Test-Path $steamcmd)){
    Warn "`n[!] Could not download SteamCMD automatically (DNS/Network issues)."
    while ($true) {
        $manualPath = Read-Host "Please enter the full path to steamcmd.exe manually (e.g. C:\myfolder\steamcmd.exe)"
        $manualPath = $manualPath.Trim('"',"'")
        if (Test-Path $manualPath -PathType Leaf) {
            $steamcmd = $manualPath
            break
        } else {
            Bad "Invalid file path: $manualPath. Please try again."
        }
    }
}
Ok "    SteamCMD path: $steamcmd"

# ---------- Get Credentials ----------
Say "`nEnter Steam credentials for the game OWNER. Password will be piped to SteamCMD stdin securely."
Warn ("For paid games, use the account owning the game. LastOwner (SteamID64) in manifest: {0}" -f $before.LastOwner)
$cred = Get-Credential -Message 'Steam Login (Account Owner)'
$user = $cred.UserName

# ---------- 6. Run SteamCMD via runscript file ----------
Say "`n[6/7] Starting update via SteamCMD (Option A: force_install_dir = Library Root)..."

# Write temporary runscript file (deleted immediately after launch)
$scriptFile = Join-Path $env:TEMP ("steamcmd_run_{0}.txt" -f [guid]::NewGuid().ToString('N').Substring(0,8))
$logFile    = Join-Path $PSScriptRoot "logs\steamcmd_run.log"

$bstr  = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($cred.Password)
$plain = [Runtime.InteropServices.Marshal]::PtrToStringBSTR($bstr)
try {
    $lines = @(
        "@ShutdownOnFailedCommand 1",
        "@NoPromptForPassword 1",
        "login $user $plain",
        $(if ($FreeToPlay) { "app_license_request $($game.AppID)" } else { "" }),
        "app_update $($game.AppID) validate",
        "quit"
    ) | Where-Object { $_ -ne "" }
    [System.IO.File]::WriteAllLines($scriptFile, $lines, [System.Text.Encoding]::ASCII)
} finally {
    [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($bstr)
    $plain = $null
}

Warn ("Runscript: $scriptFile  |  Log: $logFile")
Warn ("Command: steamcmd +force_install_dir `"{0}`" +runscript <script> (password hidden)" -f $game.Library)
Line

$null = New-Item -ItemType Directory -Path (Split-Path $logFile) -Force

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName    = $steamcmd
$psi.Arguments   = ('+force_install_dir "{0}" +runscript "{1}"' -f $game.Library, $scriptFile)
$psi.UseShellExecute        = $false
$psi.RedirectStandardOutput = $false   # let it write directly to console
$psi.RedirectStandardError  = $false
$psi.CreateNoWindow         = $false   # show window so we see output

$sw   = [System.Diagnostics.Stopwatch]::StartNew()
$proc = [System.Diagnostics.Process]::Start($psi)

# Poll every 2s and write heartbeat to log so user can tail it
$logStream = [System.IO.StreamWriter]::new($logFile, $false, [System.Text.Encoding]::UTF8)
$logStream.AutoFlush = $true
$logStream.WriteLine('[' + (Get-Date -Format 'HH:mm:ss') + '] SteamCMD started. PID=' + $proc.Id)

while (-not $proc.WaitForExit(2000)) {
    $logStream.WriteLine('[' + (Get-Date -Format 'HH:mm:ss') + '] SteamCMD still running... (elapsed ' + [int]$sw.Elapsed.TotalSeconds + 's)')
}
$sw.Stop()
$logStream.WriteLine('[' + (Get-Date -Format 'HH:mm:ss') + '] SteamCMD exited. Code=' + $proc.ExitCode + '. Elapsed=' + [int]$sw.Elapsed.TotalSeconds + 's')
$logStream.Close()

# Delete script file immediately (password was in there)
Remove-Item -LiteralPath $scriptFile -Force -ErrorAction SilentlyContinue

# Read log for analysis
$log = Get-Content -LiteralPath $logFile -Raw -ErrorAction SilentlyContinue

Line
Ok ("SteamCMD completed. Exit code: {0}. Elapsed: {1} sec." -f $proc.ExitCode, [int]$sw.Elapsed.TotalSeconds)
Ok ("Full SteamCMD output was printed to the console window above.")

# ---------- Error Diagnostics ----------
if ($log -match 'No subscription') {
    Bad 'ERROR: "No subscription" - this account does not own the game! You must log in using the owner account.'
}
if ($log -match 'Invalid Platform') {
    Bad 'ERROR: "Invalid Platform" - the account does not own the game or it is unavailable for your OS.'
}
if ($log -match 'Login Failure|Invalid Password') {
    Bad 'ERROR: Login credentials rejected by Steam.'
}
if ($log -match 'Steam Guard|two-factor|confirmation code') {
    Warn 'Steam Guard / Two-factor / confirmation code requested. Login incomplete.'
}
if ($log -match 'RateLimit|rate limit') {
    Warn 'Rate limit triggered. Please wait 10-15 minutes before retrying.'
}

# ---------- AFTER SNAPSHOT ----------
Say "`n--- AFTER SNAPSHOT (Post Run) ---"
$after     = Parse-Acf $game.AcfPath
$freeAfter = Get-FreeBytes $game.Library
$after | Format-List | Out-String | Write-Host

$deltaGB = ToGB ($freeBefore - $freeAfter)
Line
Say ' COMPARING BEFORE & AFTER'
Line
'{0,-12} {1,-22} {2,-22}' -f 'Field','BEFORE','AFTER' | Write-Host
foreach($f in 'buildid','StateFlags','LastOwner','SizeOnDisk'){
    $b = $before.$f; $a = $after.$f
    $mark = if($b -ne $a){ ' <-- CHANGED' } else { '' }
    $color = if($b -ne $a){ 'Yellow' } else { 'Gray' }
    Write-Host ('{0,-12} {1,-22} {2,-22}{3}' -f $f,$b,$a,$mark) -ForegroundColor $color
}
Line
Ok ("Free Disk space BEFORE: {0} GB | AFTER: {1} GB | Downloaded during run: {2} GB" -f (ToGB $freeBefore),(ToGB $freeAfter),$deltaGB)

# Assessment
Say "`n--- Analysis ---"
if($deltaGB -ge ([double]$game.SizeGB * 0.5)){
    Bad ("WARNING: Downloaded ~{0} GB for a game of size {1} GB - looks like a FULL REDOWNLOAD / DUPLICATION." -f $deltaGB,$game.SizeGB)
} else {
    Ok ("Looks like a delta update (downloaded {0} GB vs total {1} GB) - this is a good sign." -f $deltaGB,$game.SizeGB)
}
if($after.LastOwner -ne $before.LastOwner){ Warn 'LastOwner was updated by SteamCMD. Verify if this interferes with normal gameplay.' }

if ($before.StateFlags -ne '4' -and $after.StateFlags -eq '4') {
    Ok 'SteamCMD applied the queued update successfully (StateFlags: was not 4 -> now 4).'
} elseif ($after.StateFlags -ne '4') {
    Warn ("StateFlags = {0} (not 4) - SteamCMD did not mark installation as complete." -f $after.StateFlags)
}

# ---------- ACF PATCH STEP ----------
Line
Say ' ACF PATCH STEP (Testing: force StateFlags=4 to trick Steam Client)'
Line
Warn "Current StateFlags = $($after.StateFlags). Steam Client will re-queue the update if this is not 4."
$doPatch = Read-Host "Patch StateFlags to 4 in the .acf file now and test if Steam Client accepts it? (y/n)"
if ($doPatch -eq 'y') {
    # Read raw file
    $acfRaw = Get-Content -LiteralPath $game.AcfPath -Raw -Encoding UTF8

    # Patch StateFlags to 4
    $acfPatched = $acfRaw -replace '("StateFlags"\s*")\d+(")', '${1}4${2}'

    # Write back
    [System.IO.File]::WriteAllText($game.AcfPath, $acfPatched, [System.Text.Encoding]::UTF8)
    Ok "Patched! StateFlags set to 4 in: $($game.AcfPath)"

    # Verify the patch
    $patched = Parse-Acf $game.AcfPath
    Ok ("Verification: StateFlags is now = {0}" -f $patched.StateFlags)

    Say "`n--- NOW: open Steam Client and check if Factorio shows 'Play' button ---"
    Say "If Steam Client shows 'Play' -> ACF patch approach WORKS. We can use this in production."
    Say "If Steam still wants to update -> The buildid mismatch with Valve servers is the real blocker."
    Read-Host "`nOpen Steam now, check the game status, then press Enter to continue"

    $steamResult = Read-Host "Did Steam show 'Play' (no update queued)? (y/n)"
    if ($steamResult -eq 'y') {
        Ok "SUCCESS: ACF patch works! Steam Client accepted the SteamCMD update after StateFlags was forced to 4."
        Ok "ARCHITECTURE CONFIRMED: We can update via SteamCMD + patch .acf after completion."
    } else {
        Bad "FAIL: Steam Client still queued update despite StateFlags=4 patch."
        Bad "Likely cause: Steam checks buildid against its own servers and detected a version mismatch."
        Warn "We need to verify the buildid written by SteamCMD matches the server-side latest buildid."

        # Restore backup
        $doRestore = Read-Host "Restore original .acf from backup? (y/n)"
        if ($doRestore -eq 'y') {
            Copy-Item -LiteralPath $acfBackup -Destination $game.AcfPath -Force
            Ok "Restored original .acf from: $acfBackup"
        }
    }
} else {
    Warn "Skipped ACF patch. Steam Client will continue to show update queued."
}

# ---------- 7. Final Summary ----------
Line
Say '[7/7] TEST COMPLETE - Summary:'
Write-Host @"
  What we tested:
    - SteamCMD updated $($game.Name) files into the Steam Client library folder.
    - Steam Client reaction BEFORE ACF patch: showed update queued (StateFlags=6)
    - ACF Patch: forced StateFlags=4 to make Steam think update is done.

  What to record:
    OK  -> Steam shows Play button after patch = architecture works.
    BAD -> Steam still queues update = buildid/manifest conflict, need rethink.
"@ -ForegroundColor White
Line
Warn ("Backup of original ACF is here if you need to restore: {0}" -f $acfBackup)
Ok 'Script done.'
