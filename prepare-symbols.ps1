$ErrorActionPreference='Stop'
$helper=Join-Path $PSScriptRoot 'dist\NativeControl.exe'
if(-not(Test-Path -LiteralPath $helper)){throw 'Run build.ps1 first.'}
$metadataText=& $helper pdb-info
if($LASTEXITCODE){throw 'The installed Windows taskbar image could not be identified.'}
$metadata=$metadataText | ConvertFrom-Json
if($metadata.Pdb -notmatch '^[a-zA-Z0-9_-]+\.pdb$' -or $metadata.Key -notmatch '^[0-9A-F]+$'){throw 'Unexpected symbol identifier.'}
$symbols=Join-Path $PSScriptRoot 'native\symbols'
New-Item -ItemType Directory -Force -Path $symbols | Out-Null
$destination=Join-Path $symbols $metadata.Pdb
$url='https://msdl.microsoft.com/download/symbols/'+$metadata.Pdb+'/'+$metadata.Key+'/'+$metadata.Pdb
# This downloads debug symbols, not software to install. It sends only the public
# Windows module's symbol identifier, not monitor IDs or user diagnostic logs.
Invoke-WebRequest -UseBasicParsing -Uri $url -OutFile $destination
& $helper resolve $symbols
if($LASTEXITCODE){throw 'Matching required symbols were not found. Do not enable this build.'}
Write-Output 'Local symbol configuration prepared in dist\bridge.ini (excluded from Git).'
