[CmdletBinding()]
param(
    [string]$Validator = 'build\x64\ship-lua\Release\shiplua_manifest_validator.exe',
    [string]$OutputDirectory = 'build\leveled'
)

# Empacota o leveled (Lua puro, sem provider) como .shipmod. Sem DLL, o pacote não depende do layout id do host.

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'linkspan-zip.ps1')
Set-StrictMode -Version Latest

function InputFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Arquivo ausente: $Path" }
    return (Resolve-Path -LiteralPath $Path).Path
}

$validatorExe = InputFile $Validator
$source = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\lua\leveled'))
$output = [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $OutputDirectory))
$stage = Join-Path $output ([System.IO.Path]::GetRandomFileName())
[System.IO.Directory]::CreateDirectory((Join-Path $stage 'leveled')) | Out-Null
foreach ($name in @('manifest.toml', 'main.lua', 'attributes.toml', 'README.md', 'README.pt-BR.md')) {
    Copy-Item -LiteralPath (InputFile (Join-Path $source $name)) -Destination (Join-Path $stage $name)
}
# Só os módulos do mod; tests/ fica de fora do pacote.
foreach ($module in @(Get-ChildItem -LiteralPath (Join-Path $source 'leveled') -Filter '*.lua' -File)) {
    Copy-Item -LiteralPath $module.FullName -Destination (Join-Path $stage "leveled\$($module.Name)")
}
Copy-Item -LiteralPath (InputFile (Join-Path $PSScriptRoot '..\LICENSE')) -Destination (Join-Path $stage 'LICENSE')
$version = (Select-String -LiteralPath (Join-Path $source 'manifest.toml') -Pattern '^version\s*=\s*"([^"]+)"').Matches[0].Groups[1].Value

$zip = Join-Path $output "LinkSpan-Leveled-$version.shipmod"
if (Test-Path -LiteralPath $zip) { throw "Pacote já existe: $zip" }
try {
    New-LinkSpanZip -Source $stage -Destination $zip
} finally {
    Remove-Item -LiteralPath $stage -Recurse -Force
}
& $validatorExe $zip
if ($LASTEXITCODE -ne 0) { throw 'Pacote inválido.' }
[ordered]@{
    path = $zip
    size = (Get-Item -LiteralPath $zip).Length
    sha256 = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
} | ConvertTo-Json
