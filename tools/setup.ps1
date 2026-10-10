<#
Metal Gear Skate 3 setup: installs the mod into METAL GEAR SOLID 3 (Master Collection, Steam) and
builds the Skate 3 data it needs from the player's own Xbox 360 copy. Run it through setup.cmd.

    setup.cmd                          interactive: finds MGS3, asks for your Skate 3 ISO or folder
    setup.cmd -DryRun                  print every action without changing anything
    setup.cmd -Skate3 "D:\Skate 3.iso" -Yes

Nothing from either game is downloaded or bundled. Downloads (each asked for, pinned by version
and SHA-256): Ultimate ASI Loader (MIT), optionally MGSHDFix (MIT), extract-xiso (BSD-style),
the Skate 3 data converter from 2010 Rust Rewrite Mashup v0.4.0 (Apache-2.0; it runs the
GPL-3.0 skate-3-rust-engine tools on your own default.xex) and, for the skater's sound,
vgmstream r2117 (ISC). The sound export runs with Python 3 and numpy when they are installed.
#>
param(
    [string]$GameDir,        # MGS3 folder (with "METAL GEAR SOLID3.exe"); found through Steam if omitted
    [string]$Skate3,         # your Skate 3: an .iso, an extracted folder, or its default.xex
    [string]$DataDir,        # where the converted Skate 3 data goes (default %LOCALAPPDATA%\MetalGearSkate)
    [switch]$WithHDFix,      # also install MGSHDFix (includes its own ASI loader)
    [switch]$SkipConvert,    # keep existing converted data
    [switch]$DataOnly,       # only (re)build the Skate 3 data; leave the game folder alone
    [switch]$Yes,            # don't ask before downloading or overwriting
    [switch]$DryRun          # print the actions only
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$UserAgent = 'OpenAI File Downloader, XaiImageApiFetch/1.0'
$SteamAppId = '2131650'
$GameExe = 'METAL GEAR SOLID3.exe'
$Downloads = @{
    AsiLoader = @{ Url = 'https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/v9.7.4/Ultimate-ASI-Loader-NoPDB_x64.zip'
                   Sha256 = 'e5860e7d9a1805267535b65749575b5e406cc6ea3325c7392189c578815045d1' }
    HDFix     = @{ Url = 'https://github.com/ShizCalev/MGSHDFix/releases/download/4.1.2/MGSHDFix_4.1.2.zip'
                   Sha256 = 'fbac84acc36bd395ab649beb576cf60e861fb43d3b36c26057fe900f5fa38e1d' }
    XIso      = @{ Url = 'https://github.com/XboxDev/extract-xiso/releases/download/build-202609111233/extract-xiso-Win64_Release.zip'
                   Sha256 = 'fc891dfa72f08e3838a2f8b7ec34a8e4d61678da1cc8fee879677f00bf114ce3' }
    Converter = @{ Url = 'https://github.com/chasmlol/2010-rust-rewrite-mashup/releases/download/v0.4.0/2010-Rust-Rewrite-Mashup-windows-x64.zip'
                   Sha256 = 'f7c02cfb4dd5650be29762947a0b17d3e6f9b9f1259025ee89a99bd031e1ebca' }
    # Decodes the disc's EA-XMA sounds for the skater's audio (the pin upstream's audio export uses).
    Vgmstream = @{ Url = 'https://github.com/vgmstream/vgmstream/releases/download/r2117/vgmstream-win64.zip'
                   Sha256 = '6c4a8a3813864fefed081bbd337dbc0ad93bf88e0b92f5db98d7ab258b22dc6c' }
}
# Loaders Ultimate ASI Loader is commonly installed as; any of these means one is present.
$LoaderNames = @('wininet.dll', 'winhttp.dll', 'dinput8.dll', 'version.dll', 'winmm.dll', 'dsound.dll', 'd3d11.dll', 'dxgi.dll')

$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Repo = Split-Path -Parent $Here

function Say($text) { Write-Host $text }
function Step($text) { Write-Host ''; Write-Host "== $text" -ForegroundColor Cyan }
function Act($text, [scriptblock]$do) {
    if ($DryRun) { Write-Host "  [dry run] $text" -ForegroundColor DarkGray } else { Write-Host "  $text"; & $do }
}
function Ask($question) {
    if ($Yes -or $DryRun) { return $true }
    $answer = Read-Host "$question [Y/n]"
    return ($answer -eq '' -or $answer -match '^[Yy]')
}
function Fail($text) { Write-Host ''; Write-Host "Setup stopped: $text" -ForegroundColor Red; exit 1 }

# ---- finding things ---------------------------------------------------------------------------

function Find-SteamGame {
    $steam = $null
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam') {
        try {
            $item = Get-ItemProperty -Path $key -ErrorAction Stop
            if ($item.SteamPath) { $steam = $item.SteamPath; break }
            if ($item.InstallPath) { $steam = $item.InstallPath; break }
        } catch {}
    }
    if (-not $steam) { $steam = "${env:ProgramFiles(x86)}\Steam" }
    $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
    $libraries = @($steam)
    if (Test-Path $vdf) {
        foreach ($m in [regex]::Matches((Get-Content -Raw $vdf), '"path"\s+"([^"]+)"')) {
            $libraries += $m.Groups[1].Value.Replace('\\', '\')
        }
    }
    foreach ($library in $libraries | Select-Object -Unique) {
        $manifest = Join-Path $library "steamapps\appmanifest_$SteamAppId.acf"
        if (-not (Test-Path $manifest)) { continue }
        $m = [regex]::Match((Get-Content -Raw $manifest), '"installdir"\s+"([^"]+)"')
        if ($m.Success) {
            $dir = Join-Path $library ("steamapps\common\" + $m.Groups[1].Value)
            if (Test-Path (Join-Path $dir $GameExe)) { return $dir }
        }
    }
    return $null
}

# The mod's two binaries: beside this script in a release, else the repo's build outputs.
function Find-Built($name, $repoPath) {
    foreach ($candidate in (Join-Path $Here $name), (Join-Path $Here "..\$name"), (Join-Path $Repo $repoPath)) {
        if (Test-Path $candidate) { return (Resolve-Path $candidate).Path }
    }
    return $null
}

function Get-Pinned($name, $outFile) {
    $d = $Downloads[$name]
    if (Test-Path $outFile) {
        if ((Get-FileHash -Algorithm SHA256 $outFile).Hash.ToLower() -eq $d.Sha256) { return }
        Remove-Item -Force $outFile
    }
    Act "download $($d.Url)" {
        Invoke-WebRequest -Uri $d.Url -OutFile $outFile -UserAgent $UserAgent -UseBasicParsing
        $hash = (Get-FileHash -Algorithm SHA256 $outFile).Hash.ToLower()
        if ($hash -ne $d.Sha256) { Remove-Item -Force $outFile; Fail "$name download has the wrong checksum ($hash)." }
    }
}

function Expand-Entry($zip, $entrySuffix, $dest) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $entry = $archive.Entries | Where-Object { $_.FullName.Replace('\', '/').EndsWith($entrySuffix) } | Select-Object -First 1
        if (-not $entry) { Fail "$entrySuffix is not in $zip." }
        New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $dest, $true)
    } finally { $archive.Dispose() }
}

# ---- 1. MGS3 --------------------------------------------------------------------------------------

Step 'METAL GEAR SOLID 3'
if ($DataOnly) { Say '  skipped (-DataOnly)' }
if (-not $GameDir -and -not $DataOnly) { $GameDir = Find-SteamGame }
if (-not $DataOnly -and -not $GameDir -and -not $DryRun) {
    $GameDir = Read-Host 'MGS3 not found through Steam. Paste the folder that has "METAL GEAR SOLID3.exe"'
}
if (-not $DataOnly) {
    if (-not $GameDir -or -not (Test-Path (Join-Path $GameDir $GameExe))) { Fail "no $GameExe in '$GameDir'." }
    $GameDir = (Resolve-Path $GameDir).Path
    $Plugins = Join-Path $GameDir 'plugins'
    Say "  found $GameDir"
    if (Get-Process -Name 'METAL GEAR SOLID3' -ErrorAction SilentlyContinue) {
        if ($DryRun) { Say '  (the game is running; a real run would ask you to close it)' } else { Fail 'close the game first.' }
    }
}

if (-not $DataDir) { $DataDir = Join-Path $env:LOCALAPPDATA 'MetalGearSkate' }
$Cache = Join-Path $DataDir 'downloads'
$Tools = Join-Path $DataDir 'tools'
$SkateData = Join-Path $DataDir 'skate-data'
if (-not $DryRun) { New-Item -ItemType Directory -Force $Cache, $Tools | Out-Null }

# ---- 2. ASI loader (and optional MGSHDFix) --------------------------------------------------------

if (-not $DataOnly) {
Step 'ASI loader'
$present = $LoaderNames | Where-Object { Test-Path (Join-Path $GameDir $_) }
if ($WithHDFix) {
    if (Ask 'Download and install MGSHDFix 4.1.2 (includes Ultimate ASI Loader)?') {
        $zip = Join-Path $Cache 'MGSHDFix_4.1.2.zip'
        Get-Pinned 'HDFix' $zip
        Act "extract MGSHDFix into $GameDir" { Expand-Archive -Force -Path $zip -DestinationPath $GameDir }
    }
} elseif ($present) {
    Say "  already installed ($($present -join ', '))"
} elseif (Ask 'No ASI loader in the game folder. Download Ultimate ASI Loader v9.7.4 and install it as wininet.dll?') {
    $zip = Join-Path $Cache 'Ultimate-ASI-Loader-NoPDB_x64.zip'
    Get-Pinned 'AsiLoader' $zip
    Act "install dinput8.dll as $GameDir\wininet.dll" { Expand-Entry $zip 'dinput8.dll' (Join-Path $GameDir 'wininet.dll') }
    Act "write UltimateASILoader_LICENSE.md (MIT)" {
        Set-Content -Encoding UTF8 (Join-Path $GameDir 'UltimateASILoader_LICENSE.md') `
            "Ultimate ASI Loader, MIT License, Copyright (c) 2023 ThirteenAG. https://github.com/ThirteenAG/Ultimate-ASI-Loader"
    }
} else {
    Fail 'the mod needs an ASI loader (or rerun with -WithHDFix).'
}

# ---- 3. the mod -------------------------------------------------------------------------------------

Step 'Metal Gear Skate 3'
$asi = Find-Built 'MetalGearSkate.asi' 'mgs3-plugin\build\Release\MetalGearSkate.asi'
$core = Find-Built 'MetalGearSkate.core.dll' 'mgs3-plugin\build\Release\MetalGearSkate.core.dll'
$dll = Find-Built 'skate_ffi.dll' 'skate-ffi\target\x86_64-pc-windows-msvc\release\skate_ffi.dll'
if (-not $asi -or -not $core -or -not $dll) { Fail 'MetalGearSkate.asi / MetalGearSkate.core.dll / skate_ffi.dll not found (in a release they sit beside setup.cmd; from source, run build.cmd first).' }
# The .asi is a small loader; the plugin itself is the core DLL in plugins\MetalGearSkate\, out of
# the ASI loader's way (it can be swapped while the game runs).
$coreDir = Join-Path $Plugins 'MetalGearSkate'
Act "create $coreDir" { New-Item -ItemType Directory -Force $coreDir | Out-Null }
Act "copy $asi -> $Plugins" { Copy-Item -Force $asi $Plugins }
Act "copy $core -> $coreDir\core.dll" { Copy-Item -Force $core (Join-Path $coreDir 'core.dll') }
Act "copy $dll -> $Plugins" { Copy-Item -Force $dll $Plugins }
}

# ---- 4. Skate 3 data --------------------------------------------------------------------------------

Step 'Skate 3 data'
$assets = Join-Path $SkateData 'assets'
$haveData = Test-Path (Join-Path $assets 'private\skater.glb')
if ($SkipConvert -or ($haveData -and -not $Skate3)) {
    if ($haveData) { Say "  using existing $assets" } else { Fail "no converted data in $SkateData (drop -SkipConvert)." }
} else {
    if (-not $Skate3 -and -not $DryRun) {
        Say '  Skate 3 (Xbox 360) is needed: your own disc image (.iso) or an extracted copy (folder with default.xex).'
        $Skate3 = (Read-Host '  Path to your Skate 3 .iso, folder or default.xex').Trim('"')
    }
    if (-not $Skate3) { Fail 'no Skate 3 copy given (use -Skate3).' }
    if (-not (Test-Path $Skate3)) { Fail "'$Skate3' does not exist." }
    $xex = $null
    $extracted = $null
    if ((Get-Item $Skate3).PSIsContainer) {
        $xex = Join-Path $Skate3 'default.xex'
    } elseif ($Skate3 -match '\.iso$') {
        $extracted = Join-Path $DataDir 'Skate3'
        if (-not (Test-Path (Join-Path $extracted 'default.xex'))) {
            if (-not (Ask 'Download extract-xiso to unpack the ISO?')) { Fail 'extract the ISO yourself and pass its folder with -Skate3.' }
            $zip = Join-Path $Cache 'extract-xiso-Win64_Release.zip'
            Get-Pinned 'XIso' $zip
            $xiso = Join-Path $Tools 'extract-xiso.exe'
            Act "unpack extract-xiso.exe" { Expand-Entry $zip 'extract-xiso.exe' $xiso; Expand-Entry $zip 'LICENSE.TXT' (Join-Path $Tools 'extract-xiso_LICENSE.TXT') }
            Act "extract-xiso -d `"$extracted`" -x `"$Skate3`"" {
                # From inside the data folder, so nothing lands in the caller's directory.
                Push-Location $DataDir
                try { & $xiso -d $extracted -x $Skate3 } finally { Pop-Location }
                if ($LASTEXITCODE -ne 0) { Fail 'extract-xiso failed (is this an Xbox 360 Skate 3 ISO?).' }
            }
        } else { Say "  using already extracted $extracted" }
        $xex = Join-Path $extracted 'default.xex'
    } else {
        $xex = $Skate3
    }
    if (-not $DryRun -and -not (Test-Path $xex)) { Fail "no default.xex at $xex (a PS3 copy won't work; it must be Xbox 360)." }

    $converter = Join-Path $Tools 'iw4l-skate-convert.exe'
    if (-not (Test-Path $converter)) {
        if (-not (Ask 'Download the Skate 3 data converter (2010 Rust Rewrite Mashup v0.4.0, 70 MB)?')) { Fail 'the converter is needed.' }
        $zip = Join-Path $Cache '2010-Rust-Rewrite-Mashup-v0.4.0.zip'
        Get-Pinned 'Converter' $zip
        Act "unpack skate/iw4l-skate-convert.exe and its licences" {
            Expand-Entry $zip 'skate/iw4l-skate-convert.exe' $converter
            foreach ($l in 'LICENSE', 'NOTICE') { Expand-Entry $zip "2010-Rust-Rewrite-Mashup/$l" (Join-Path $Tools "converter_$l") }
            foreach ($l in 'NumPy.txt', 'Pillow.txt', 'Python.txt', 'CustomEngineLayer.txt', 'UTT.txt') {
                Expand-Entry $zip "skate/licenses/$l" (Join-Path $Tools "converter_licenses\$l")
            }
        }
    }
    Act "convert $xex -> $SkateData (a few minutes)" {
        & $converter --xex $xex --out $SkateData
        if ($LASTEXITCODE -ne 0) { Fail 'the converter failed (see its output above).' }
    }

    # The skater's sound: the disc's own sound banks, decoded (tools\skate_audio_export).
    $disc = Split-Path -Parent $xex
    $export = @((Join-Path $Here 'skate_audio_export\run.py'), (Join-Path $Repo 'tools\skate_audio_export\run.py')) |
        Where-Object { Test-Path $_ } | Select-Object -First 1
    $py = Get-Command python -ErrorAction SilentlyContinue
    $numpy = $false
    if ($py) { & python -c 'import numpy' 2>$null; $numpy = $LASTEXITCODE -eq 0 }
    if (-not $export) {
        Say '  (no audio exporter in this copy; skating will be silent)'
    } elseif (-not ($py -and $numpy)) {
        Say '  (Python 3 with numpy is needed for the skater''s sound; install them and rerun setup with -Skate3 to add it)'
    } elseif (Ask 'Export the skater''s sounds from your disc (downloads vgmstream r2117 to decode them; about 1.3 GB, a few minutes)?') {
        $vgm = Join-Path $Tools 'vgmstream\vgmstream-cli.exe'
        if (-not (Test-Path $vgm)) {
            $zip = Join-Path $Cache 'vgmstream-win64-r2117.zip'
            Get-Pinned 'Vgmstream' $zip
            Act "unpack vgmstream into $Tools\vgmstream" { Expand-Archive -Force -Path $zip -DestinationPath (Join-Path $Tools 'vgmstream') }
        }
        Act "export the skater's sounds from $disc -> $assets\private\audio" {
            & python -I $export $disc $assets $vgm
            if ($LASTEXITCODE -ne 0) { Say '  the sound export failed (see above); skating works, silently.' }
        }
    }
    if ($extracted -and (Test-Path $extracted) -and (Ask "Delete the extracted disc copy in $extracted (about 6 GB; not needed any more)?")) {
        Act "delete $extracted" { Remove-Item -Recurse -Force $extracted }
    }
}

# ---- 5. settings ---------------------------------------------------------------------------------------

if (-not $DataOnly) {
Step 'Settings'
$ini = Join-Path $Plugins 'MetalGearSkate.ini'
$example = @((Join-Path $Here 'MetalGearSkate.example.ini'), (Join-Path $Repo 'mgs3-plugin\MetalGearSkate.example.ini')) |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (Test-Path $ini) {
    Act "set skate_data=$assets in the existing $ini" {
        $lines = Get-Content $ini
        if ($lines -match '^skate_data=') { $lines = $lines -replace '^skate_data=.*', "skate_data=$assets" }
        else { $lines = $lines -replace '^\[skate\]$', "[skate]`r`nskate_data=$assets" }
        Set-Content -Encoding ASCII $ini $lines
    }
} elseif ($example) {
    Act "write $ini from $example" {
        (Get-Content $example) -replace '^skate_data=.*', "skate_data=$assets" | Set-Content -Encoding ASCII $ini
    }
} else {
    Act "write a minimal $ini" { Set-Content -Encoding ASCII $ini "[skate]`r`nskate_data=$assets" }
}
}

# ---- 6. check ---------------------------------------------------------------------------------------------

Step 'Check'
$smoke = @((Join-Path $Here 'ffi_smoke.py'), (Join-Path $Repo 'tools\ffi_smoke.py')) | Where-Object { Test-Path $_ } | Select-Object -First 1
$python = Get-Command python -ErrorAction SilentlyContinue
if ($python -and $smoke) {
    Act "ride the engine for 10 s on a flat floor (python $smoke)" {
        $engine = if ($DataOnly) { Find-Built 'skate_ffi.dll' 'skate-ffi\target\x86_64-pc-windows-msvc\release\skate_ffi.dll' } else { Join-Path $Plugins 'skate_ffi.dll' }
        & python -I $smoke $assets $engine
        if ($LASTEXITCODE -ne 0) { Fail 'the engine check failed: the converted data or skate_ffi.dll is not right.' }
    }
} else {
    Say '  (no Python, skipping the engine check; start skating in game with F5 instead)'
}

Step 'Done'
Say '  Start MGS3, load a stage (Groznyj Grad, s091a, is a good one) and press F5 to skate.'
Say "  Uninstall: delete plugins\MetalGearSkate.asi, plugins\MetalGearSkate\, plugins\skate_ffi.dll and plugins\MetalGearSkate.ini; converted data is in $DataDir."
