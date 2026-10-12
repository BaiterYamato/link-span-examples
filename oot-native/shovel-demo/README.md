**English** · [Português (Brasil)](README.pt-BR.md)

# Shovel Demo (OoT)

Standalone mod that proves Link-Span's newer OoT contracts: asset preflight, an item with a stable key, use by child
Link, V2 registration with an owner and a mod that mounts its own `assets/` folder. The package carries the shovel's
icon, name texture and 3D model (34 files in `assets/`), so the demo's own item shows a shovel. They come from the Not
Enough Items fork by skijer ([skijer/Shipwright, branch `Not-Enough-Items`](https://github.com/skijer/Shipwright/tree/Not-Enough-Items),
commit `c29262b`). The fork publishes no license, so those files stay with their authors and the package grants no
license over them (see `NOTICE.md`). The demo is **not** a copy of Skijer's shovel and is not meant to be a final
gameplay mod.

At load the provider mounts the package's `assets/` folder (`ProviderAssetsDirectory` and `mount_archive`) and unmounts
it on shutdown. If the folder cannot be mounted or does not hold the shovel, the item falls back to the Megaton Hammer
icon and model from the game's `oot.o2r`. Those two files are the only resources the manifest's `[resources]` requires:
the preflight runs before the DLL loads, when the package's own `assets/` is not mounted yet. The `stats` function
(which `main.lua` logs every 180 frames) reports `assets=package` or the reason for the fallback: `noservice`,
`nofolder`, `mountfail` or `incomplete`. The name texture travels with the set, but this demo does not draw it.

With NEI loaded, the demo grants NEI's own shovel. It shows up on the second inventory page, in the last row, and can be
equipped from the menu. The demo's own item, if an earlier version left it equipped, is swapped for the NEI shovel.
Without NEI, the demo keeps its own item on a C button: on solid ground, each use creates a green rupee in
front of Link. That standalone item has no inventory cell and is not meant for a main playthrough.

To package it, `tools/package-shovel-demo.ps1` requires `-NeiAssetsCore <nei-assets-core.o2r>` (the file the host's
`tools/build-nei-assets.py` generates) and extracts only the shovel's entries from it.
