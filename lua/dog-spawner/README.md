**English** · [Português (Brasil)](README.pt-BR.md)

# dog-spawner

Press **F** (rebindable) to spawn a dog in front of the player.

- **OoT (Ship of Harkinian):** uses `ship.actor.spawn("oot.en_dog", ...)`, like
  [actor-spawn](../actor-spawn/main.lua). It checks `ship.actor.exists` and destroys the previous owned dog
  before creating another one. An expired handle (for example after a scene change) is discarded.
- **MM (2Ship2Harkinian):** keeps `ship.mm.spawn_dog()` and its `mm.spawn_dog` capability.

The [Lua entrypoint](main.lua) selects the active game with `ship.game.id()` and checks its capabilities before
registering the hotkey. The generic OoT path needs `actor.spawn`, `actor.destroy` and `actor.exists`; the MM path
only needs `mm.spawn_dog`. They are optional in the manifest so that the same mod can load on both hosts.

## Install

Copy this folder (or package it as `dog-spawner.shipmod`) into the game's `mods/` folder and start the game.
Press **F** in game.

## Notes

- The manifest grants `world.entities.create`, `world.entities.destroy` and `world.entities.read` for the generic
  actor API, and limits owned actors to one. The MM helper retains its original behavior.
- **OoT:** the host interprets position `(0, 0, 0)` as “in front of the player”. Spawn returns an opaque handle
  or an operation error, which is logged. The dog object must be loaded in the scene. Try Hyrule Market
  (`SCENE_MARKET_DAY` / `SCENE_MARKET_NIGHT`); outside suitable scenes, `En_Dog` may stay still or remove itself.
- **MM:** `object_dog` must be loaded in the scene (Clock Town and areas with dogs) for rendering.

## Test without the game

```powershell
python <link-span>/tools/shipmod.py test lua/dog-spawner --game oot
python <link-span>/tools/shipmod.py test lua/dog-spawner --game mm
```

The tests cover owned-handle replacement, expired handles, operation failures and the unchanged MM route.
