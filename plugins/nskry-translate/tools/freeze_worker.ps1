param(
    [Parameter(Mandatory=$true)][string]$RuntimePython,
    [Parameter(Mandatory=$true)][string]$OutputRoot
)
$ErrorActionPreference = 'Stop'
$workerSource = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../src/offline_worker.py'))
$outputPath = [IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
# An explicitly prepared runtime-only environment is required. No pip/network here.
& $RuntimePython -m PyInstaller --noconfirm --onedir --name nskry-translate-worker `
    --noupx --collect-all ctranslate2 --exclude-module torch --exclude-module transformers `
    --exclude-module tensorflow --distpath (Join-Path $outputPath 'dist') `
    --workpath (Join-Path $outputPath 'work') --specpath $outputPath $workerSource
if ($LASTEXITCODE -ne 0) { throw 'Offline worker freeze failed.' }
