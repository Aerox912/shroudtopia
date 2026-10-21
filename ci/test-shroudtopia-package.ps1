$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$fixture = Join-Path 'E:\Scratch' ('shroudtopia-package-' + [guid]::NewGuid().ToString('N'))
$previousLocation = Get-Location
$previousGitDir = $env:GIT_DIR
$previousGitWorkTree = $env:GIT_WORK_TREE
function Assert-True($Condition, $Message) {
  if (!$Condition) { throw $Message }
}
try {
  foreach ($directory in @('ci', 'build', 'docs\releases', 'release\default-configs', 'third-party\nlohmann-json')) {
    New-Item -ItemType Directory -Path (Join-Path $fixture $directory) -Force | Out-Null
  }
  foreach ($relative in @('FORK_VERSION', 'LICENSE', 'ci\package-shroudtopia-release.ps1', 'ci\SHROUDTOPIA-CREDITS.txt', 'docs\releases\0.2.1-shroudweave.1.md', 'release\default-configs\shroudtopia.json', 'third-party\nlohmann-json\LICENSE.MIT')) {
    Copy-Item -LiteralPath (Join-Path $repo $relative) -Destination (Join-Path $fixture $relative)
  }
  # Inert bytes test packaging behavior only; no DLL is loaded or built here.
  [IO.File]::WriteAllBytes((Join-Path $fixture 'build\winmm.dll'), [byte[]](1, 2, 3))
  [IO.File]::WriteAllBytes((Join-Path $fixture 'build\shroudtopia.dll'), [byte[]](4, 5, 6))
  $env:GIT_DIR = (& git -C $repo rev-parse --absolute-git-dir).Trim()
  $env:GIT_WORK_TREE = $fixture
  & (Join-Path $fixture 'ci\package-shroudtopia-release.ps1')
  $version = (Get-Content -LiteralPath (Join-Path $fixture 'FORK_VERSION') -Raw).Trim()
  $archive = Join-Path $fixture "dist\Shroudweave-Shroudtopia-$version.zip"
  $upgrade = Join-Path $fixture 'upgrade'
  New-Item -ItemType Directory -Path $upgrade | Out-Null
  $config = Join-Path $upgrade 'shroudtopia.json'
  $userConfig = '{"active":true,"mods":{"minimap_mod":{"enabled":true,"position":"custom"}}}'
  [IO.File]::WriteAllText($config, $userConfig)
  [IO.Compression.ZipFile]::ExtractToDirectory($archive, $upgrade, $true)
  Assert-True ([IO.File]::ReadAllText($config) -ceq $userConfig) 'Extract-all overwrote user settings.'
  foreach ($name in @('winmm.dll', 'shroudtopia.dll', 'examples\shroudtopia.default.json', 'LICENSE', 'LICENSE-nlohmann-json.txt', 'CREDITS.txt')) {
    Assert-True (Test-Path -LiteralPath (Join-Path $upgrade $name) -PathType Leaf) "Missing payload: $name"
  }
  $zip = [IO.Compression.ZipFile]::OpenRead($archive)
  try {
    Assert-True (!($zip.Entries.FullName -contains 'shroudtopia.json')) 'Archive contains a root configuration.'
    Assert-True (!($zip.Entries.FullName -match '^mods[/\\]')) 'Standalone package unexpectedly bundles mods.'
  } finally { $zip.Dispose() }
  $info = Get-Content -LiteralPath (Join-Path $upgrade 'build-info.json') -Raw | ConvertFrom-Json
  Assert-True ($info.configExamplePath -ceq 'examples/shroudtopia.default.json') 'Incorrect example metadata.'
  Assert-True ($info.configInstallPolicy -ceq 'copy-only-if-missing') 'Unsafe config install policy.'
  $exampleHash = (Get-FileHash -LiteralPath (Join-Path $upgrade 'examples\shroudtopia.default.json') -Algorithm SHA256).Hash.ToLowerInvariant()
  Assert-True ($info.configSha256 -ceq $exampleHash) 'Example hash mismatch.'
  $instructions = Get-Content -LiteralPath (Join-Path $upgrade 'INSTALL.txt') -Raw
  Assert-True ($instructions.Contains('Never copy it over an existing configuration')) 'Missing upgrade warning.'
  Assert-True ($instructions.Contains('Only if the game folder has no shroudtopia.json')) 'Missing first-install condition.'
  Write-Output "PASS: extract-all preserves configured mods, both required DLLs and credits remain, example metadata matches. Fixture: $fixture"
} finally {
  Set-Location $previousLocation
  $env:GIT_DIR = $previousGitDir
  $env:GIT_WORK_TREE = $previousGitWorkTree
}
