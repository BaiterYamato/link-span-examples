[CmdletBinding()]
param(
    # Obrigatório: o nei-assets-core.o2r que tools/build-nei-assets.py (host) gera. Dele saem só o ícone, o nome e o
    # modelo da pá, que vão soltos para assets/ do pacote. Não tem padrão: sem ele o pacote não sai.
    [string]$NeiAssetsCore,
    [string]$ProviderDirectory = 'build\shovel-demo-native\mod\provider',
    [string]$Validator = 'build\x64\ship-lua\Release\shiplua_manifest_validator.exe',
    [string]$LayoutHeader = 'build\x64\native-sdk\oot_layout_id.h',
    [string]$OutputDirectory = 'build\shovel-demo'
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'linkspan-zip.ps1')
Set-StrictMode -Version Latest

function InputFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Arquivo ausente: $Path" }
    return (Resolve-Path -LiteralPath $Path).Path
}

# Entradas da pá no nei-assets-core.o2r (ZIP com recursos no formato do resource manager). Os caminhos são os do
# fork NEI e não podem mudar: as display lists citam umas às outras por esse nome. O prefixo termina em '/' para não
# pegar objects/object_nei_shovel_hole_/ (buraco), que este demo não desenha.
$ShovelModelPrefix = 'objects/object_nei_shovel/'
$ShovelModelRoot = 'objects/object_nei_shovel/gShovelGiveDL_opaque_dl'   # display list de topo do get-item
# 29 desde o gerador 0.3.7: as três texturas que o fork copiou do Dampé saíram do .o2r, e o modelo aponta para
# objects/object_tk/ (e uma cópia igual em object_in/) no oot.o2r do jogador.
$ShovelModelCount = 29
$ShovelTextures = @('textures/icon_item_custom/gItemIconShovelTex', 'textures/item_name_custom/gShovelNameTex')

function Copy-ShovelAssets([string]$Archive, [string]$Destination) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archiveZip = [System.IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        $entries = @($archiveZip.Entries | Where-Object {
                -not $_.FullName.EndsWith('/') -and
                ($_.FullName.StartsWith($ShovelModelPrefix, [StringComparison]::Ordinal) -or
                 ($ShovelTextures -ccontains $_.FullName))
            })
        $names = @($entries | ForEach-Object { $_.FullName })
        foreach ($required in @($ShovelModelRoot) + $ShovelTextures) {
            if ($names -cnotcontains $required) { throw "Entrada ausente em ${Archive}: $required" }
        }
        $modelCount = @($names | Where-Object { $_.StartsWith($ShovelModelPrefix, [StringComparison]::Ordinal) }).Count
        if ($modelCount -ne $ShovelModelCount) {
            throw "O modelo da pá deveria ter $ShovelModelCount entradas em ${Archive}, tem ${modelCount}: revise a lista."
        }
        if (@($names | ForEach-Object { $_.ToLowerInvariant() } | Select-Object -Unique).Count -ne $names.Count) {
            throw "Entradas da pá que só diferem na caixa em $Archive."
        }
        foreach ($entry in $entries) {
            # Só nomes simples do VFS: nada de '..', barra invertida ou caminho absoluto fora de assets/.
            if ($entry.FullName -notmatch '^[A-Za-z0-9_][A-Za-z0-9_./-]*$' -or $entry.FullName -match '(^|/)\.\.?(/|$)') {
                throw "Nome de entrada inesperado em ${Archive}: $($entry.FullName)"
            }
            $target = Join-Path $Destination $entry.FullName.Replace('/', '\')
            [System.IO.Directory]::CreateDirectory([System.IO.Path]::GetDirectoryName($target)) | Out-Null
            $entryStream = $entry.Open()
            try {
                $fileStream = [System.IO.File]::Create($target)
                try { $entryStream.CopyTo($fileStream) } finally { $fileStream.Dispose() }
            } finally {
                $entryStream.Dispose()
            }
            if ((Get-Item -LiteralPath $target).Length -ne $entry.Length) { throw "Extração incompleta: $($entry.FullName)" }
        }
        return $entries.Count
    } finally {
        $archiveZip.Dispose()
    }
}

if ([string]::IsNullOrWhiteSpace($NeiAssetsCore)) {
    throw 'Informe -NeiAssetsCore <caminho do nei-assets-core.o2r>: o pacote leva dentro o ícone, o nome e o modelo da pá.'
}
$neiAssets = InputFile $NeiAssetsCore
$dll = InputFile (Join-Path $ProviderDirectory 'linkspan_shovel_demo.dll')
$validatorExe = InputFile $Validator
$layoutText = Get-Content -LiteralPath (InputFile $LayoutHeader) -Raw
$layoutMatch = [regex]::Match($layoutText, 'LINKSPAN_OOT_LAYOUT_ID "([0-9a-f]{64})"')
if (-not $layoutMatch.Success) { throw "Layout id ausente: $LayoutHeader" }
$layout = $layoutMatch.Groups[1].Value.Substring(0, 8)
$dllHash = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.Substring(0, 8).ToLowerInvariant()
$source = Join-Path $PSScriptRoot '..\oot-native\shovel-demo'
$output = [System.IO.Path]::GetFullPath((Join-Path (Get-Location).Path $OutputDirectory))
$stage = Join-Path $output ([System.IO.Path]::GetRandomFileName())
[System.IO.Directory]::CreateDirectory((Join-Path $stage 'provider')) | Out-Null
foreach ($name in @('manifest.toml', 'main.lua', 'NOTICE.md')) {
    Copy-Item -LiteralPath (InputFile (Join-Path $source $name)) -Destination (Join-Path $stage $name)
}
Copy-Item -LiteralPath (InputFile (Join-Path $PSScriptRoot '..\LICENSE')) -Destination (Join-Path $stage 'LICENSE')
$version = (Select-String -LiteralPath (Join-Path $source 'manifest.toml') -Pattern '^version\s*=\s*"([^"]+)"').Matches[0].Groups[1].Value
Copy-Item -LiteralPath $dll -Destination (Join-Path $stage 'provider\linkspan_shovel_demo.dll')
# assets/ ao lado de provider/: o provider monta esta pasta (ProviderAssetsDirectory) e o item usa a pá de dentro dela.
$assetCount = Copy-ShovelAssets $neiAssets (Join-Path $stage 'assets')

Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = Join-Path $output "LinkSpan-Shovel-Demo-$version-layout-$layout-$dllHash.zip"
if (Test-Path -LiteralPath $zip) { throw "Pacote já existe: $zip" }
New-LinkSpanZip -Source $stage -Destination $zip
& $validatorExe $zip
if ($LASTEXITCODE -ne 0) { throw 'Pacote inválido.' }
[ordered]@{ path = $zip; assets = $assetCount; sha256 = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash } |
    ConvertTo-Json
