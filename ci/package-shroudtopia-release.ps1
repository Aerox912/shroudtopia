$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$version = (Get-Content -LiteralPath (Join-Path $root 'FORK_VERSION') -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+-shroudweave\.\d+$') {
  throw "Unexpected Shroudtopia release version: $version"
}

$dll = Join-Path $root 'build\shroudtopia.dll'
$proxy = Join-Path $root 'build\winmm.dll'
foreach ($path in @($dll, $proxy)) {
  if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing required build output: $path" }
}

$out = Join-Path $root 'dist'
New-Item -ItemType Directory -Path $out -Force | Out-Null
$stage = Join-Path $out "stage-shroudtopia-$version"
$archive = Join-Path $out "Shroudweave-Shroudtopia-$version.zip"
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $archive)) {
  throw 'Release staging already exists; use a clean Actions workspace.'
}
New-Item -ItemType Directory -Path $stage -Force | Out-Null
Copy-Item -LiteralPath $dll -Destination (Join-Path $stage 'shroudtopia.dll')
Copy-Item -LiteralPath $proxy -Destination (Join-Path $stage 'winmm.dll')
Copy-Item -LiteralPath (Join-Path $root 'release\default-configs\shroudtopia.json') -Destination (Join-Path $stage 'shroudtopia.json')
Copy-Item -LiteralPath (Join-Path $root 'LICENSE') -Destination $stage
Copy-Item -LiteralPath (Join-Path $root 'third-party\nlohmann-json\LICENSE.MIT') -Destination (Join-Path $stage 'LICENSE-nlohmann-json.txt')
Copy-Item -LiteralPath (Join-Path $root 'ci\SHROUDTOPIA-CREDITS.txt') -Destination (Join-Path $stage 'CREDITS.txt')
Copy-Item -LiteralPath (Join-Path $root "docs\releases\$version.md") -Destination (Join-Path $stage 'RELEASE-NOTES.md')
@'
Shroudweave Shroudtopia is an independent community release of the Shroudtopia compatibility loader.

Extract this archive into the Enshrouded game folder. It contains the WinMM proxy and Shroudtopia loader required by this release. If the game folder already contains winmm.dll or shroudtopia.json, preserve a backup before replacing either file. The included shroudtopia.json is the default configuration; it does not enable any mods.

The optional Minimap file is distributed separately. Install its ZIP into the same game folder so its files merge under mods/minimap_mod.
'@ | Set-Content -LiteralPath (Join-Path $stage 'INSTALL.txt') -Encoding utf8
$buildInfo = [ordered]@{
  displayName = 'Shroudweave Shroudtopia'
  upstreamProject = 'Shroudtopia'
  version = $version
  sdkApiVersion = '0.1.1'
  sourceCommit = (& git rev-parse HEAD).Trim()
  workflowRunId = $env:GITHUB_RUN_ID
  platform = 'Windows x64'
  dlls = [ordered]@{
    shroudtopia = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant()
    winmmProxy = (Get-FileHash -LiteralPath $proxy -Algorithm SHA256).Hash.ToLowerInvariant()
  }
  configSha256 = (Get-FileHash -LiteralPath (Join-Path $stage 'shroudtopia.json') -Algorithm SHA256).Hash.ToLowerInvariant()
}
$buildInfo | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'build-info.json') -Encoding utf8
$checksums = Get-ChildItem -LiteralPath $stage -File -Recurse | Sort-Object FullName | ForEach-Object {
  $relative = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
  '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $relative
}
$checksums | Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt') -Encoding ascii
Add-Type -AssemblyName System.IO.Compression.FileSystem
[System.IO.Compression.ZipFile]::CreateFromDirectory($stage, $archive)
$archiveHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
('{0}  {1}' -f $archiveHash, [System.IO.Path]::GetFileName($archive)) | Set-Content -LiteralPath ($archive + '.sha256') -Encoding ascii
Get-Item -LiteralPath $archive | Select-Object FullName, Length
Write-Output "SHA256: $archiveHash"
