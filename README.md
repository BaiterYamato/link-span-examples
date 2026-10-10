**English** · [Português (Brasil)](README.pt-BR.md)

# Link-Span — example mods

Example mods and demos for [Link-Span](https://github.com/BaiterYamato/link-span), the modloader for Ship of
Harkinian (OoT) and 2 Ship 2 Harkinian (MM). Until 2026-10-10 they lived inside link-span itself (`examples/`) and
inside the OoT host (`soh/native-sdk/*-demo`, `soh/native-sdk/unbound-core/{scene,field,custom-actors}-demo`). They
were split out so the modloader only ships what belongs to it.

Guides: [writing Lua mods](https://github.com/BaiterYamato/link-span/blob/main/docs/writing-mods.md),
[native OoT mods](https://github.com/BaiterYamato/link-span/blob/main/docs/native-mods.md) and the
[wiki](https://github.com/BaiterYamato/link-span/wiki).

| Folder | Contents |
|---|---|
| `lua/` | 23 Lua mods (`manifest.toml` + `main.lua`): hello-world, hotkeys, actors, saves, MM forms, mini-rando… |
| `oot-native/` | native OoT demos (provider DLLs): crash, escape hatch, familiar, hooks, item, save, view, world, CEL rendering and Shovel (an item that uses the NEI service) |
| `oot-native/unbound/` | Unbound demos: scene registration (a copy of Hyrule Field), room actor patch and the format 0.9 actor package |
| `oot-native/common/` | `package_assets` (finds the package `assets/` folder), used by the Unbound demos |
| `tools/` | packaging scripts for the native demos (PowerShell 5.1) |

## Lua mods

Each folder under `lua/` is one mod: copy the folder into the game's `mods/` folder. To validate the manifest and run
the tests some of them ship (`tests/`), use `shipmod.py` from link-span:

```powershell
python <link-span>/tools/shipmod.py validate lua/hello-world
python <link-span>/tools/shipmod.py test lua/frame-counter
```

## Native demos (OoT)

They need the host SDK: the `LinkSpan-OoT-SDK-*.zip` from the release, or the `OotNativeSdk.cmake` from a host build
(`build/x64/native-sdk/Release/OotNativeSdk.cmake`). A DLL only loads in a host with the same layout id.

```powershell
cmake -S oot-native/crash-demo -B build/crash-demo -A x64 -DOOT_NATIVE_SDK=<...>/OotNativeSdk.cmake
cmake --build build/crash-demo --config Release
```

The Shovel Demo also needs the public NEI header: `-DLINKSPAN_NEI_INCLUDE=<include/ folder of the NEI package>`.
The Unbound demos build together: `cmake -S oot-native/unbound ...`.

The scripts in `tools/` package a built DLL. Run them **from the root of a host checkout**: the `-Validator` and
`-LayoutHeader` defaults point at its build. Each demo's source comes from this repository.

```powershell
powershell -File <link-span-examples>/tools/package-crash-demo.ps1 -CoreExtension -ProviderDirectory build/crash-demo/mod/provider
```

`crash-demo` crashes the game on purpose: it exists to prove the boot guard recovery. Do not install it outside
testing.

## License

Link-Span's own work is dedicated to the public domain under CC0 1.0 (`LICENSE`). See `NOTICE.md` for the parts that
remain with other authors.
