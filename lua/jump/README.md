**English** · [Português (Brasil)](README.pt-BR.md)

# Jump

Example mod that combines the common hotkey infrastructure with the active game's jump capability. It works in **both
supported games**.

- In **Ocarina of Time (Ship of Harkinian)**: uses `oot.player.jump`.
- In **Majora's Mask (2Ship2Harkinian)**: uses `mm.player.jump`.

Copy the folder into the `mods` folder of SoH or 2Ship. In game, press `J` to jump; the shortcut can be changed in the
Link-Span settings. The jump only applies while the player is on the ground.

It declares `games = ["oot", "mm"]` and lists `oot.player.jump` / `mm.player.jump` as **optional** capabilities. At
runtime, the jump function is resolved from the capability the host advertises, so the same mod loads in both games
without faking an incompatible action.
