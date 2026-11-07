param(
    [string]$Gcc = '',
    [string]$Go = ''
)

$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = Split-Path -Parent $here
[Console]::OutputEncoding = [Text.Encoding]::UTF8

if (-not $Gcc) { $Gcc = 'D:\code\_tools\w64devkit\w64devkit\bin\gcc.exe' }
if (-not $Go)  { $Go  = 'D:\code\_tools\go-sdk\go\bin\go.exe' }
if (-not (Test-Path $Gcc)) { throw "GCC not found: $Gcc" }
if (-not (Test-Path $Go))  { throw "Go not found: $Go" }
$env:PATH = (Split-Path -Parent $Gcc) + ';' + $env:PATH

$mock = Join-Path $here 'mockserver.exe'
& $Go build -o $mock (Join-Path $here 'mockserver.go')
if ($LASTEXITCODE -ne 0) { throw 'go build mockserver failed' }

$core = @(
    'util.c','out.c','log.c','config.c','json.c','sign.c','http.c','api.c',
    'progress.c','download.c','resolve.c','cli.c','menu.c'
) | ForEach-Object { Join-Path $root $_ }

foreach ($t in @('test_sign','test_base','test_api','test_download','test_cli')) {
    & $Gcc -std=c99 -Os -s -I"$root" -o (Join-Path $here "$t.exe") `
        (Join-Path $here "$t.c") @core -lwinhttp -lbcrypt
    if ($LASTEXITCODE -ne 0) { throw "compile $t failed" }
}

$failed = $false
foreach ($t in @('test_sign','test_base')) {
    Write-Output "--- $t ---"
    & (Join-Path $here "$t.exe")
    if ($LASTEXITCODE -ne 0) { $failed = $true }
}

# One mock instance serves all three protocol tests (GET /quit is a no-op).
$p = Start-Process -FilePath $mock -ArgumentList '18080' -PassThru -WindowStyle Hidden
$ready = $false
for ($i = 0; $i -lt 30 -and -not $ready; $i++) {
    try {
        $null = Invoke-WebRequest -Uri 'http://127.0.0.1:18080/ping' `
            -TimeoutSec 2 -UseBasicParsing
        $ready = $true
    } catch { Start-Sleep -Milliseconds 100 }
}
if (-not $ready) {
    if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }
    throw 'mock server did not start'
}

foreach ($t in @('test_api','test_download','test_cli')) {
    Write-Output "--- $t ---"
    & (Join-Path $here "$t.exe")
    if ($LASTEXITCODE -ne 0) { $failed = $true }
}

try {
    $null = Invoke-WebRequest -Uri 'http://127.0.0.1:18080/shutdown' `
        -TimeoutSec 2 -UseBasicParsing
} catch { }
if (-not $p.HasExited) { $p.WaitForExit(5000) | Out-Null }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }

if ($failed) { throw 'some tests FAILED' }
Write-Output 'ALL TESTS PASSED'
