[English](README.md) · **Português (Brasil)**

# Link-Span — mods de exemplo

Mods de exemplo e demos do [Link-Span](https://github.com/BaiterYamato/link-span), o modloader de Ship of
Harkinian (OoT) e 2 Ship 2 Harkinian (MM). Até 2026-10-10 eles moravam no próprio link-span (`examples/`) e no
host OoT (`soh/native-sdk/*-demo`, `soh/native-sdk/unbound-core/{scene,field,custom-actors}-demo`); foram separados
para o modloader carregar só o que é dele.

Guias: [mods em Lua](https://github.com/BaiterYamato/link-span/blob/main/docs/writing-mods.pt-BR.md),
[mods nativos do OoT](https://github.com/BaiterYamato/link-span/blob/main/docs/native-mods.pt-BR.md) e a
[wiki](https://github.com/BaiterYamato/link-span/wiki).

| Pasta | O que tem |
|---|---|
| `lua/` | 23 mods Lua (`manifest.toml` + `main.lua`): hello-world, hotkeys, atores, saves, formas do MM, mini-rando… |
| `oot-native/` | demos nativos do OoT (DLL de provider): crash, escape hatch, familiar, hooks, item, save, view, world, render CEL e o Shovel (item que usa o serviço do NEI) |
| `oot-native/unbound/` | demos do Unbound: registro de cena (Hyrule Field copiado), patch de atores de sala e o pacote de atores do formato 0.9 |
| `oot-native/common/` | `package_assets` (achar `assets/` do pacote), usado pelos demos do Unbound e pelo Shovel |
| `tools/` | scripts de pacote dos demos nativos (PowerShell 5.1) |

## Mods Lua

Cada pasta de `lua/` é um mod: copie a pasta para `mods/` do jogo. Para validar o manifesto e rodar os testes que
alguns trazem (`tests/`), use o `shipmod.py` do link-span:

```
python <link-span>/tools/shipmod.py validate lua/hello-world
python <link-span>/tools/shipmod.py test lua/frame-counter
```

## Demos nativos (OoT)

Precisam do SDK do host: o ZIP `LinkSpan-OoT-SDK-*.zip` da release, ou o `OotNativeSdk.cmake` de um build do host
(`build/x64/native-sdk/Release/OotNativeSdk.cmake`). Uma DLL só carrega no host com o mesmo layout id.

```
cmake -S oot-native/crash-demo -B build/crash-demo -A x64 -DOOT_NATIVE_SDK=<...>/OotNativeSdk.cmake
cmake --build build/crash-demo --config Release
```

O Shovel Demo também pede o header público do NEI: `-DLINKSPAN_NEI_INCLUDE=<pasta include/ do pacote NEI>`.
O pacote dele traz o ícone, o nome e o modelo da pá do fork NEI do skijer, então `tools/package-shovel-demo.ps1` também
exige `-NeiAssetsCore <nei-assets-core.o2r>` (gerado pelo `tools/build-nei-assets.py` do host) e extrai só as entradas
da pá; nenhum desses arquivos fica guardado neste repositório.
Os demos do Unbound compilam juntos: `cmake -S oot-native/unbound ...`.

Os scripts de `tools/` empacotam a DLL compilada. Rode-os **da raiz de um checkout do host**: os padrões de
`-Validator` e `-LayoutHeader` apontam para o build dele. A fonte de cada demo vem deste repositório.

```
powershell -File <link-span-examples>/tools/package-crash-demo.ps1 -CoreExtension -ProviderDirectory build/crash-demo/mod/provider
```

O `crash-demo` derruba o jogo de propósito: serve para provar a recuperação do boot guard. Não instale fora de teste.

## Licença

O trabalho próprio do Link-Span está em domínio público pela CC0 1.0 (`LICENSE`). Veja `NOTICE.md` para as partes
que seguem com outros autores.
