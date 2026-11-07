# Builds out\release\Budokai3Access-<version>.zip (version from src\access.h) from a finished build (run build.sh first).
# The zip unpacks into the game folder, next to dbz3.exe.
param([string]$Version = "")
$ErrorActionPreference = "Stop"
$src = Split-Path $PSScriptRoot
if (-not $Version) {
    # MOD_VERSION in src\access.h, without a trailing ".0" (1.0.0 -> 1.0, 1.0.1 stays)
    $Version = ([regex]::Match((Get-Content -Raw (Join-Path $src "src\access.h")), 'MOD_VERSION "([^"]+)"')).Groups[1].Value -replace '\.0$', ''
}
$game = Split-Path $src
$rel = Join-Path $src "out\release"
$stage = Join-Path $rel "stage"
# Only this script's own output is cleared; other files in that folder stay.
if (Test-Path $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $stage "Budokai3Access\loader"), (Join-Path $stage "Budokai3Access\licenses") | Out-Null

Copy-Item -LiteralPath (Join-Path $src "out\Play Budokai 3 Access.exe") $stage
foreach ($f in "out\Budokai3Access\Budokai3Access.dll", "data\text.tsv", "README.txt", "LICENSE.txt") {
    Copy-Item -LiteralPath (Join-Path $src $f) (Join-Path $stage "Budokai3Access")
}
Copy-Item -LiteralPath (Join-Path $game "Budokai3Access\prism.dll") (Join-Path $stage "Budokai3Access")
Copy-Item -LiteralPath (Join-Path $src "out\rexgpu-access.dll") (Join-Path $stage "Budokai3Access\loader\rexgpu-xenos.dll")
# port 1.5.6+: the launcher copies this one into plugins\ (the port's own plugin loader)
Copy-Item -LiteralPath (Join-Path $src "out\plugins\Budokai3Access.dll") (Join-Path $stage "Budokai3Access\loader\Budokai3Access.dll")
Copy-Item -LiteralPath (Join-Path $src "third_party\minhook\LICENSE.txt") (Join-Path $stage "Budokai3Access\licenses\MinHook-LICENSE.txt")
Copy-Item -LiteralPath (Join-Path $src "third_party\prism\LICENSE") (Join-Path $stage "Budokai3Access\licenses\Prism-LICENSE.txt")

$zip = Join-Path $rel "Budokai3Access-$Version.zip"
if (Test-Path $zip) { Remove-Item -LiteralPath $zip -Force }
# Windows' tar writes standard "/" paths; Compress-Archive and ZipFile in
# PowerShell 5.1 write "\", which some unzip tools turn into odd file names.
& "$env:SystemRoot\System32\tar.exe" -a -c -f $zip -C $stage "Play Budokai 3 Access.exe" Budokai3Access
if ($LASTEXITCODE -ne 0) { throw "tar failed" }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$z = [IO.Compression.ZipFile]::OpenRead($zip)
$z.Entries | ForEach-Object { "{0,-45} {1,9}" -f $_.FullName, $_.Length }
$z.Dispose()
"$zip  ($([math]::Round((Get-Item $zip).Length / 1KB)) KB)"
