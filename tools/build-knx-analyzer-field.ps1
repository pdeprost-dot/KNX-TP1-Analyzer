param(
    [string]$OutputDirectory = (Join-Path $env:TEMP "knx-analyzer-field-build"),
    [string]$ArduinoCli = "C:\Tools\ArduinoCLI\arduino-cli.exe"
)

$ErrorActionPreference = "Stop"
$fqbn = "esp32:esp32:XIAO_ESP32S3:USBMode=hwcdc,CDCOnBoot=cdc,PartitionScheme=default_8MB,PSRAM=opi"
$repository = Split-Path -Parent $PSScriptRoot
$sketch = Join-Path $repository "firmware\KNXAnalyzerField"

if (-not (Test-Path -LiteralPath $ArduinoCli)) { throw "arduino-cli not found: $ArduinoCli" }
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

Write-Host "KNXAnalyzerField canonical FQBN: $fqbn"
& $ArduinoCli compile --fqbn $fqbn --output-dir $OutputDirectory $sketch
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$binary = Join-Path $OutputDirectory "KNXAnalyzerField.ino.bin"
$elf = Join-Path $OutputDirectory "KNXAnalyzerField.ino.elf"
if (-not (Test-Path -LiteralPath $binary) -or -not (Test-Path -LiteralPath $elf)) {
    throw "Canonical build did not produce both BIN and ELF."
}
$hash = Get-FileHash -Algorithm SHA256 -LiteralPath $binary
Write-Host "BIN: $binary"
Write-Host "ELF: $elf"
Write-Host "SHA-256: $($hash.Hash)"
