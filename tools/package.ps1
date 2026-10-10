<#
Builds the player release zip: dist\MetalGearSkate3-<rev>.zip with the plugin, the engine DLL,
setup, README and every licence that applies. No game data. Run through package.cmd.

    package.cmd            package the current build outputs
    package.cmd -Build     run build.cmd first
#>
param([switch]$Build)
$ErrorActionPreference = 'Stop'
$Repo = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $Repo

if ($Build) {
    & cmd /c (Join-Path $Repo 'build.cmd')
    if ($LASTEXITCODE -ne 0) { throw 'build.cmd failed' }
}
$asi = Join-Path $Repo 'mgs3-plugin\build\Release\MetalGearSkate.asi'
$core = Join-Path $Repo 'mgs3-plugin\build\Release\MetalGearSkate.core.dll'
$dll = Join-Path $Repo 'skate-ffi\target\x86_64-pc-windows-msvc\release\skate_ffi.dll'
foreach ($f in $asi, $core, $dll) { if (-not (Test-Path $f)) { throw "not built: $f (run build.cmd or package.cmd -Build)" } }

$rev = (& git -C $Repo rev-parse --short HEAD).Trim()
$name = "MetalGearSkate3-$rev"
$stage = Join-Path $Repo "dist\$name"
Remove-Item -Recurse -Force $stage -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $stage, "$stage\licenses" | Out-Null

Copy-Item $asi, $core, $dll $stage
Copy-Item (Join-Path $Repo 'setup.cmd'), (Join-Path $Repo 'tools\setup.ps1'), (Join-Path $Repo 'tools\ffi_smoke.py') $stage
Copy-Item (Join-Path $Repo 'mgs3-plugin\MetalGearSkate.example.ini') $stage
# The skater's sound export (setup runs it; carries its own licence notices).
Copy-Item -Recurse (Join-Path $Repo 'tools\skate_audio_export') $stage -Exclude '__pycache__'
Copy-Item (Join-Path $Repo 'README.md'), (Join-Path $Repo 'LICENSE') $stage

# Licences of what is inside the two binaries.
Copy-Item (Join-Path $Repo 'mashup\LICENSE') "$stage\licenses\2010-Rust-Rewrite-Mashup_LICENSE.txt"
Copy-Item (Join-Path $Repo 'mashup\NOTICE') "$stage\licenses\2010-Rust-Rewrite-Mashup_NOTICE.txt"
$deps = Join-Path $Repo 'mgs3-plugin\build\_deps'
foreach ($pair in @(@('safetyhook-src\LICENSE', 'safetyhook_LICENSE.txt'),
                    @('zydis-src\LICENSE', 'zydis_LICENSE.txt'),
                    @('zydis-src\dependencies\zycore\LICENSE', 'zycore_LICENSE.txt'))) {
    $src = Join-Path $deps $pair[0]
    if (Test-Path $src) { Copy-Item $src (Join-Path "$stage\licenses" $pair[1]) } else { Write-Warning "missing $src" }
}
# The Rust crates linked into skate_ffi.dll, with their declared licences.
$meta = & cargo metadata --format-version 1 --manifest-path (Join-Path $Repo 'skate-ffi\Cargo.toml') --filter-platform x86_64-pc-windows-msvc | ConvertFrom-Json
$used = @{}
foreach ($node in $meta.resolve.nodes) { $used[$node.id] = $true }
$lines = @('# Rust crates linked into skate_ffi.dll', '', '| Crate | Version | Licence |', '|---|---|---|')
foreach ($p in ($meta.packages | Where-Object { $used[$_.id] } | Sort-Object name, version)) {
    $licence = if ($p.license) { $p.license } else { "see $($p.repository)" }
    $lines += "| $($p.name) | $($p.version) | $licence |"
}
Set-Content -Encoding UTF8 "$stage\licenses\rust-crates.md" $lines

$zip = Join-Path $Repo "dist\$name.zip"
Remove-Item -Force $zip -ErrorAction SilentlyContinue
Compress-Archive -Path $stage -DestinationPath $zip
Write-Host "packaged $zip"
