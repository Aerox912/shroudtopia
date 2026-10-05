$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
Set-Location $root
$common=@('/p:Configuration=Release','/p:Platform=x64','/p:PlatformToolset=v143',"/p:SolutionDir=$root\",'/m','/nologo')
& MSBuild ./example-mods/first_person_view/first_person_view.vcxproj @common
if($LASTEXITCODE) { throw 'First-person build failed' }
& MSBuild ./example-mods/first_person_view/tests/camera_test.vcxproj @common
if($LASTEXITCODE) { throw 'First-person test build failed' }
& ./example-mods/first_person_view/build/tests/camera_test.exe
if($LASTEXITCODE) { throw 'First-person checks failed' }
$stage=Join-Path $root 'dist/stage'
New-Item -ItemType Directory -Force "$stage/mods" | Out-Null
Copy-Item ./x64/Release/mods/first_person_view.dll "$stage/mods/first_person_view.dll"
Copy-Item ./LICENSE $stage
Copy-Item ./example-mods/first_person_view/README.md $stage
Copy-Item ./ci/FIRST-PERSON-CREDITS.txt "$stage/CREDITS.txt"
$info=@{version='1.1.2';repository='https://github.com/Aerox912/shroudtopia';commit=(& git rev-parse HEAD);clientDataRevision=1076226}
$info | ConvertTo-Json | Set-Content "$stage/build-info.json" -Encoding utf8
Compress-Archive -Path "$stage/*" -DestinationPath ./dist/first-person-1.1.2.zip
$hash=(Get-FileHash ./dist/first-person-1.1.2.zip).Hash.ToLowerInvariant()
"$hash  first-person-1.1.2.zip" | Set-Content ./dist/first-person-1.1.2.zip.sha256 -Encoding ascii
