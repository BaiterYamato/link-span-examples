# Notice — Shovel Demo

- Shovel Demo is Link-Span's own work, dedicated to the public domain under CC0 1.0 Universal (`LICENSE`): the
  provider DLL and the Lua.
- It uses the Shovel item that the Not Enough Items coremod registers; that item's code ships in the NEI package,
  with its own notice.
- **Shovel icon, name and model (third party).** The `assets/` folder of this package carries 31 files from the
  Not Enough Items fork by skijer ([skijer/Shipwright, branch `Not-Enough-Items`](https://github.com/skijer/Shipwright/tree/Not-Enough-Items),
  commit `c29262b`): the item icon (`textures/icon_item_custom/gItemIconShovelTex`), the name texture
  (`textures/item_name_custom/gShovelNameTex`) and the 3D model (`objects/object_nei_shovel/`, 29 files). They are
  copied unchanged, under the fork's own paths, from the `nei-assets-core.o2r` that Link-Span's NEI tooling builds from
  that commit. **The fork publishes no license, so those files stay with their authors and this package grants no
  license over them.** The CC0 dedication above does not cover `assets/`.
- No ROM, save or game archive (`oot.o2r`) is in this package. Three textures of the fork's model are the game's own
  Dampé textures; they are not copied here, and the model reads them by their game path from the player's own
  `oot.o2r`, like the Megaton Hammer fallback.
