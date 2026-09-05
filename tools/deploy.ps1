# Push Luma and a game's scripts to the console over GoldHEN's FTP.
#   pwsh tools/deploy.ps1 -Ip <console ip> [-Game rdr2|gta5] [-Port 2121] [-SkipPrx]
#
# One .prx serves both games, but their data roots are deliberately separate so
# a script can never land in the other game's scripts/ directory -- so -Game
# picks which tree the scripts go into. The .prx itself is the same either way.
#
# The plugin loads at title launch only - restart the game after pushing a new prx.
param([string]$Ip = "", [ValidateSet("rdr2","gta5")][string]$Game = "rdr2",
      [int]$Port = 2121, [switch]$SkipPrx, [switch]$PrxOnly)

# No default console address. The one that used to sit here was the author's
# own, which on anyone else's network is not a convenience but a five-second
# connection timeout that looks like a broken script rather than a missing
# argument. Say which console you mean.
if (-not $Ip) {
    Write-Error "No console address. Pass -Ip <your PS4's IP>, e.g. -Ip 192.168.1.50"
    exit 1
}


$DataRoot = if ($Game -eq "gta5") { "/data/gtalua" } else { "/data/rdr2lua" }

# Each game's scripts live in their own tree, so -Game selects both ends at once
# and one game's natives.lua can no longer reach the other's data root. That
# used to be a guard against a flag; now it is structural.
# Resolved, not just concatenated: $_.FullName below is an absolute path, and
# the relative form ("...	ools/../resources/gta5") is longer, so subtracting
# its length chopped the front off every filename -- client.lua went out as
# ent.lua. Resolve-Path makes both sides the same shape.
$SrcScripts   = (Resolve-Path "$PSScriptRoot/../scripts/$Game" -ErrorAction SilentlyContinue)?.Path
$SrcResources = (Resolve-Path "$PSScriptRoot/../resources/$Game" -ErrorAction SilentlyContinue)?.Path
if (-not $PrxOnly -and -not $SrcScripts) {
    Write-Error "No scripts for $Game in this repo ($SrcScripts). Use -PrxOnly for the plugin alone."
    exit 1
}
Write-Host ("Target: {0}  ->  {1}" -f $Game, $DataRoot)
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent

function Ftp-Mkd($path) {
    try {
        $r = [System.Net.FtpWebRequest]::Create("ftp://${Ip}:$Port$path")
        $r.Method = [System.Net.WebRequestMethods+Ftp]::MakeDirectory
        $r.UsePassive = $true; $r.KeepAlive = $false
        $r.GetResponse().Close(); Write-Host "MKD  $path"
    } catch { Write-Host "MKD  $path (already exists)" }
}
function Ftp-Put($local, $remote) {
    if (-not (Test-Path $local)) { Write-Host "skip $remote (no local file)"; return }
    $bytes = [IO.File]::ReadAllBytes($local)
    $r = [System.Net.FtpWebRequest]::Create("ftp://${Ip}:$Port$remote")
    $r.Method = [System.Net.WebRequestMethods+Ftp]::UploadFile
    $r.UseBinary = $true; $r.UsePassive = $true; $r.KeepAlive = $false
    $r.ContentLength = $bytes.Length
    $s = $r.GetRequestStream(); $s.Write($bytes, 0, $bytes.Length); $s.Close()
    $r.GetResponse().Close()
    Write-Host ("PUT  {0} ({1} bytes)" -f $remote, $bytes.Length)
}

# Replacing the plugin cannot be a plain PUT.
#
# GoldHEN has this file mapped in every game it is loaded into, and overwriting
# it in place SUCCEEDS while leaving the file holding something that is neither
# the old build nor the new one. Observed on 2026-09-06: the upload reported
# success and the file came back the new build's size, but its contents matched
# no local build -- while the SAME bytes written to any other path on the same
# console read back byte-exact. A silently wrong plugin is the worst outcome
# this script has, because nothing looks wrong until the next boot.
#
# So stage beside it and rename over it. The unlink drops only the directory
# entry, the running games keep the inode they already mapped, and the name
# ends up pointing at a file that was never written to while it was in use.
function Ftp-Delete($remote) {
    try {
        $r = [System.Net.FtpWebRequest]::Create("ftp://${Ip}:$Port$remote")
        $r.Method = [System.Net.WebRequestMethods+Ftp]::DeleteFile
        $r.UsePassive = $true; $r.KeepAlive = $false
        $r.GetResponse().Close()
    } catch { }   # not being there yet is fine: this is a replace, not a removal
}
function Ftp-Rename($from, $to) {
    $r = [System.Net.FtpWebRequest]::Create("ftp://${Ip}:$Port$from")
    $r.Method = [System.Net.WebRequestMethods+Ftp]::Rename
    $r.RenameTo = $to
    $r.UsePassive = $true; $r.KeepAlive = $false
    $r.GetResponse().Close()
}
function Ftp-PutReplace($local, $remote) {
    if (-not (Test-Path $local)) { Write-Host "skip $remote (no local file)"; return }
    $tmp = "$remote.new"
    Ftp-Put $local $tmp
    Ftp-Delete $remote
    Ftp-Rename $tmp $remote
    Write-Host "MOVE $tmp -> $remote"
}

# MKD does not create parents, so walk the path and make each level. Ftp-Mkd
# already treats "already exists" as success, so re-making a level is
# harmless on its own -- but a resource with N files under the same
# subdirectory would otherwise re-walk and re-MKD every level once per file
# (ten files four levels deep is forty round trips to the console for
# directories that exist after the first one). $madeDirs remembers which
# levels this run has already made, so each directory is MKD'd at most once.
#
# Defined up here, above the first directory this script needs, rather than
# further down next to its first loop: the data root and its scripts/
# used to be MKD'd unconditionally before $madeDirs existed, so the set never
# learned about them and the resources loop below re-made both every run --
# the very redundancy the set is for, in the two directories every deploy
# touches.
$madeDirs = New-Object 'System.Collections.Generic.HashSet[string]'
function Ftp-MkdRecursive($remoteDir) {
    $path = ''
    foreach ($p in $remoteDir.Trim('/').Split('/')) {
        $path = "$path/$p"
        if ($madeDirs.Add($path)) { Ftp-Mkd $path }
    }
}

if (-not $PrxOnly) {
Ftp-MkdRecursive "$DataRoot/scripts"    # makes the data root on the way
Ftp-Put "$SrcScripts/natives.lua" "$DataRoot/natives.lua"
Get-ChildItem $SrcScripts -Filter *.lua | Where-Object { $_.Name -ne "natives.lua" } | ForEach-Object {
    Ftp-Put $_.FullName "$DataRoot/scripts/$($_.Name)"
}

Get-ChildItem $SrcResources -Directory -ErrorAction SilentlyContinue | ForEach-Object {
    Get-ChildItem $_.FullName -Recurse -File | ForEach-Object {
        # .Replace, not -replace: -replace takes a regex, and a lone '\' is an
        # invalid one (a trailing escape). This is a literal character swap.
        $rel    = "resources/" + $_.FullName.Substring($SrcResources.Length + 1).Replace('\', '/')
        $remote = "$DataRoot/$rel"
        Ftp-MkdRecursive ($remote -replace '/[^/]+$', '')
        Ftp-Put $_.FullName $remote
    }
}
if (Test-Path "$SrcScripts/autostart.cfg") {
    Ftp-Put "$SrcScripts/autostart.cfg" "$DataRoot/autostart.cfg"
}

}   # end -PrxOnly guard

if (-not $SkipPrx) { Ftp-PutReplace "$root/build/Luma.prx" "/data/GoldHEN/plugins/Luma.prx" }
Write-Host "Done. List Luma.prx under the game's title id in /data/GoldHEN/plugins.ini and restart it."
