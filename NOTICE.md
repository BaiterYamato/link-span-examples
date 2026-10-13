# Notice — Link-Span example mods

- **Link-Span's own work** in this repository (Lua mods, native demos, CMake files and tools) is dedicated to the
  public domain under CC0 1.0 Universal (`LICENSE`).
- **Unbound** (roborich): the demos in `oot-native/unbound/` use the Unbound 0.9 data format, and the actors demo
  adapts the official Unbound 0.9 example. Unbound publishes no license; those parts stay with their authors and
  nothing here grants a license for them. See `oot-native/unbound/NOTICE.md`.
- **Not Enough Items** (skijer): the Shovel Demo calls the NEI service through the public header of the Link-Span NEI
  port, and its package carries the shovel's icon, name and model from skijer's NEI fork (commit `c29262b`). The fork
  publishes no license; those files stay with their author and nothing here grants a license for them. They are
  extracted when the package is built, not stored in this repository. See `oot-native/shovel-demo/NOTICE.md`.
- **Game data:** no ROM, extracted asset or save is part of this repository. The demos read models and scenes from
  the player's own `oot.o2r`.
