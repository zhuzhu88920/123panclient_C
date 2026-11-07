param(
    [switch]$Upx,
    [string]$Gcc = '',
    [string]$UpxPath = ''
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path

if (-not $Gcc) { $Gcc = 'D:\code\_tools\w64devkit\w64devkit\bin\gcc.exe' }
if (-not (Test-Path $Gcc)) { throw "GCC not found: $Gcc" }
$env:PATH = (Split-Path -Parent $Gcc) + ';' + $env:PATH

$names = @(
    'util.c','out.c','log.c','config.c','json.c','sign.c','http.c','api.c',
    'progress.c','download.c','resolve.c','cli.c','menu.c','main.c'
)
$src = @()
foreach ($n in $names) { $src += (Join-Path $here $n) }

$exe = Join-Path $here '123panclient_C.exe'
& $Gcc -std=c99 -Os -s -fno-ident -ffunction-sections -fdata-sections `
    '-Wl,--gc-sections' '-Wl,--build-id=none' -o $exe @src -lwinhttp -lbcrypt
if ($LASTEXITCODE -ne 0) { throw "gcc failed with exit $LASTEXITCODE" }
Write-Output ("RAW {0:N2} KB" -f ((Get-Item $exe).Length / 1KB))

if ($Upx) {
    if (-not $UpxPath) { $UpxPath = 'D:\code\_tools\upx\upx-5.3.0-win64\upx.exe' }
    if (-not (Test-Path $UpxPath)) {
        throw "UPX not found: $UpxPath (compression must NOT be silently skipped)"
    }
    & $UpxPath --best --lzma $exe
    if ($LASTEXITCODE -ne 0) { throw "UPX failed with exit $LASTEXITCODE" }
    Write-Output ("UPX {0:N2} KB" -f ((Get-Item $exe).Length / 1KB))
}
