[CmdletBinding()]
param(
    [string]$Validator = 'D:\Desenvolvimento\ship-lua-worktrees\OOT-GRASS-HOST-001\build\x64\ship-lua\Release\shiplua_manifest_validator.exe',
    [string]$LayoutHeader = 'C:\FogM4\build\native-sdk\oot_layout_id.h',
    [string]$ZipHelper = 'D:\Desenvolvimento\ship-lua-worktrees\link-span-examples\tools\linkspan-zip.ps1'
)

# Empacota o Fairy Light Color (GFX-UNIFY-001) com o nome do layout id do host. Rodar pelo powershell.exe 5.1.
$ErrorActionPreference = 'Stop'
. $ZipHelper
Set-StrictMode -Version Latest

$root = $PSScriptRoot
$dll = Join-Path $root 'build\mod\provider\linkspan_fairy_light_color.dll'
if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) { throw "DLL not found: $dll" }
$layoutMatch = [regex]::Match((Get-Content -LiteralPath $LayoutHeader -Raw), 'LINKSPAN_OOT_LAYOUT_ID "([0-9a-f]{64})"')
if (-not $layoutMatch.Success) { throw "Layout id not found in $LayoutHeader" }
$layout = $layoutMatch.Groups[1].Value.Substring(0, 8)
$version = [regex]::Match((Get-Content -LiteralPath (Join-Path $root 'manifest.toml') -Raw), 'version = "([^"]+)"').Groups[1].Value

$out = Join-Path $root 'package'
$stage = Join-Path $out 'staging'
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
[System.IO.Directory]::CreateDirectory((Join-Path $stage 'provider')) | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'manifest.toml') -Destination (Join-Path $stage 'manifest.toml')
Copy-Item -LiteralPath (Join-Path $root 'main.lua') -Destination (Join-Path $stage 'main.lua')
Copy-Item -LiteralPath $dll -Destination (Join-Path $stage 'provider\linkspan_fairy_light_color.dll')

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = Join-Path $out "Fairy-Light-Color-$version-layout-$layout.shipmod"
if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
New-LinkSpanZip -Source $stage -Destination $zip
& $Validator $zip
if ($LASTEXITCODE -ne 0) { throw 'Invalid Fairy Light Color package.' }

[ordered]@{
    path = $zip
    size = (Get-Item -LiteralPath $zip).Length
    sha256 = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
} | ConvertTo-Json
