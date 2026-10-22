param([switch]$SourceOnly)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$pins = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'native-source-pins.json') -Raw | ConvertFrom-Json
foreach ($pin in $pins) {
    if ($pin.path -match '(^/|\\|(^|/)\.\.(/|$)|:)') { throw 'Invalid pinned source path' }
    $path = Join-Path $root $pin.path
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing reviewed source: $($pin.path)" }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $pin.sha256) {
        throw "Reviewed source bytes changed: $($pin.path)"
    }
}
Write-Output "Reviewed source bytes verified: $($pins.Count)"
if ($SourceOnly) { return }
if ($env:VSCMD_ARG_TGT_ARCH -ne 'x64' -or $env:VCToolsVersion.TrimEnd('\') -ne '14.44.35207') {
    throw 'The reviewed x64 v143 toolset 14.44.35207 is required'
}
if (!$env:WindowsSDKVersion) { throw 'Missing Windows SDK identity' }
$compiler = (Get-Command cl.exe -ErrorAction Stop).Source
$assembler = (Get-Command ml64.exe -ErrorAction Stop).Source
foreach ($tool in @($compiler,$assembler)) {
    $expectedDirectory = [IO.Path]::GetFullPath((Join-Path $env:VCToolsInstallDir 'bin/Hostx64/x64')).TrimEnd('\') + '\'
    if (![IO.Path]::GetFullPath($tool).StartsWith($expectedDirectory,[StringComparison]::OrdinalIgnoreCase)) {
        throw 'Native compiler and assembler must belong to the selected reviewed toolset'
    }
}
$compilerBanner = (& $compiler /? 2>&1 | Out-String)
$assemblerBanner = (& $assembler /? 2>&1 | Out-String)
if ($compilerBanner -notmatch 'Version 19\.44\.35229(\.0)?\b') { throw 'Expected exact CI compiler 19.44.35229.0' }
if ($assemblerBanner -notmatch 'Version ([0-9]+\.[0-9]+\.[0-9]+(?:\.[0-9]+)?)') { throw 'Missing actual selected assembler version' }
$assemblerVersion = $Matches[1]
$runtime = @('msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll') | ForEach-Object {
    $item = Get-Item -LiteralPath (Join-Path $env:WINDIR "System32/$_") -ErrorAction Stop
    [ordered]@{name=$item.Name;fileVersion=$item.VersionInfo.FileVersion;sha256=(Get-FileHash -LiteralPath $item.FullName).Hash.ToLowerInvariant()}
}
$record = [ordered]@{
    compilerProfile='ci-msvc194435229';compilerVersion='19.44.35229.0';assemblerVersion=$assemblerVersion
    compilerSha256=(Get-FileHash -LiteralPath $compiler).Hash.ToLowerInvariant()
    assemblerSha256=(Get-FileHash -LiteralPath $assembler).Hash.ToLowerInvariant()
    toolset=$env:VCToolsVersion;windowsSdk=$env:WindowsSDKVersion;target=$env:VSCMD_ARG_TGT_ARCH
    sdkHeaderSha256=(Get-FileHash -LiteralPath (Join-Path $root 'include/shroudtopia.h')).Hash.ToLowerInvariant()
    runnerRuntime=$runtime
    runtimeEvidenceScope='Runner DLL identities; deployed runtime and gameplay remain unverified'
}
New-Item -ItemType Directory -Path (Join-Path $root 'build') -Force | Out-Null
$record | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $root 'build/toolchain.json') -Encoding utf8
