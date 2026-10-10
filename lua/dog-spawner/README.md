**English** · [Português (Brasil)](README.pt-BR.md)

# dog-spawner

Example mod that works in **both supported games**: press **F** (rebindable) and a dog appears in front of you.

- In **Ocarina of Time (Ship of Harkinian)**: spawns the `Hyrule Market` dog (`En_Dog`) via `ship.oot.spawn_dog()`.
- In **Majora's Mask (2Ship2Harkinian)**: spawns the `Clock Town` dog (`En_Dg`) via `ship.mm.spawn_dog()`.

It shows the pattern for a **multi-game mod that branches on capabilities**: the same hotkey is registered on both
hosts, but the spawn function is resolved at runtime from the advertised capability (`oot.spawn_dog` or
`mm.spawn_dog`).

```lua
local function resolveSpawnDog()
    if ship.capabilities.has("oot.spawn_dog") and ship.oot and ship.oot.spawn_dog then
        return ship.oot.spawn_dog
    end
    if ship.capabilities.has("mm.spawn_dog") and ship.mm and ship.mm.spawn_dog then
        return ship.mm.spawn_dog
    end
    return nil
end
```

## Install

Copy this folder (or package it as `dog-spawner.shipmod`) into the `mods/` folder of 2Ship or SoH and start the game.
In game, press **F**.

## Notes

- It declares `games = ["oot", "mm"]` and lists `oot.spawn_dog` / `mm.spawn_dog` as **optional** capabilities: it
  loads on both hosts, but only spawns when the host offers the matching capability.
- **OoT**: the dog (`ACTOR_EN_DOG`) is the NPC from the Dog Lady side quest. Spawning it outside `Hyrule Market` can
  leave the actor standing still or make it remove itself, because `En_Dog` validates the scene and params on init.
  Use it in `SCENE_MARKET_DAY`/`SCENE_MARKET_NIGHT` for a reliable result.
- **MM**: the `object_dog` object must be loaded in the scene (Clock Town and areas with dogs) for the actor to render.
