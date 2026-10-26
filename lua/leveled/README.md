**English** · [Português (Brasil)](README.pt-BR.md)

# Leveled

RPG leveling for **Ocarina of Time**, ported from Arrenton's Leveled 1.1.3.6 to pure Lua on Link-Span.
Defeat enemies and collect Gold Skulltula tokens to earn XP, reach levels 1–99, and improve Power and Courage.
Enemy levels depend on species, scene, age and parameters. Enemy health and collision damage scale with those stats.
The HUD adds an XP bar, floating damage/XP numbers and a level-up notice, with an optional fanfare.

Copy this folder into the host's `mods` folder, or install its `.shipmod` package. Settings appear under
**Mods → Leveled** when the host supports mod menus. Gameplay options affecting enemy health/levels apply to newly
spawned enemies; damage, HUD and XP options use the current settings. Bonus hearts update within 30 game frames.

## Requirements

- Link-Span API `>=0.5 <0.6`, with an OoT host implementing **RFC 0031 (actors for mods)**.
- Required capabilities: `hooks.bridge`, `actor.events`, `actor.observe`, `actor.health`, `core.save`,
  `core.modules`, `core.attributes`, `save.events`.
- Manifest permissions: `world.entities.read` and `world.entities.write`.
- Optional capabilities are checked at runtime: `core.menu` (settings page), `core.storage` (persistent settings),
  `hud.draw` (HUD), `audio.play` (fanfare), `player.fields` (hearts, age/tokens and player screen position),
  `oot.env` (scene levels). Without storage, defaults apply. Without player fields, hearts/player anchors are skipped
  and age/tokens fall back to 0. Without the scene API, species fallback levels apply.
- Enable **`linkspan.oot.item_source`** for vanilla equipment bonuses. The port declares Power/Courage attributes
  and equipment modifiers in `attributes.toml`; other mods can contribute modifiers to those attributes.

XP and the vanilla heart capacity belong to the current save (`ship.save`: `schema = 1`, `xp`, `base_capacity`).
Level is derived from XP. Options belong to the mod's global `ship.storage`. No XP is awarded without a loaded save.
Before saving, bonus heart capacity is removed and current health is capped to the vanilla capacity; both are
restored on the next game frame. A save loaded with the mod disabled therefore keeps its vanilla heart capacity.

## Options

Numeric settings are clamped to their range. `multiplier` and `damage_taken` are integers. Factors use 1.0 for 100%.
The menu has three groups; labels are in Portuguese (Brazil).

| Group | Storage key | Default | Range / effect |
| --- | --- | --- | --- |
| HUD | `xp_bar` | true | Level and XP bar |
| HUD | `numbers_enemy` | true | Enemy damage numbers |
| HUD | `numbers_player` | true | Link damage numbers |
| HUD | `numbers_xp` | true | XP numbers above Link |
| HUD | `level_up` | true | Level-up notice |
| HUD | `level_up_sound` | true | Level-up fanfare |
| Player | `bonus_hearts` | true | One bonus heart every 8 levels, up to 10 bonus hearts; total cap 20 |
| Player | `equipment` | true | Equipment attribute modifiers |
| Player | `damage_taken` | 4 | 1–32; collision damage × N/4 |
| Player | `xp_rate` | 1.0 | 0–10; all XP rewards |
| Player | `token_rate` | 1.0 | 0–10; token XP before `xp_rate` |
| Enemies | `attack_scales` | true | Enemy level affects base attack |
| Enemies | `multiplier` | 9 | 1–32; attack/health multiplier |
| Enemies | `hp_percent` | 1.0 | 0–10; enemy health factor |
| Enemies | `level_scale` | 1.0 | 0–10; enemy level scale |
| Enemies | `ganon_max_level` | false | Maximum castle scene level with exactly 100 tokens |

The original bonus-heart formula uses `(level + 1) // 8`, so the first bonus appears at level 7.
Hidden diagnostic setting `debug_log = true` logs enemy initialization, transformed collision damage and defeats
through `ship.log.info`; it is off by default and has no menu widget.

## v1 exclusions and differences

| Case | Behavior |
| --- | --- |
| Bosses (BOSS category), including manual boss health deductions | Vanilla health and damage received; their outgoing collision damage uses the formula |
| Grabs (`play->damagePlayer`), knockback damage, falls, fire and traps outside collision damage | Vanilla damage; `damage_taken` only applies in the collision hook |
| Level-based magic (72/144 units) | Outside v1; separate subproject |
| More than 20 hearts or changing units per heart | Not supported; bonus capacity caps at 20 hearts |
| Navi information, file-select levels, pause-screen stats | Not included |
| Damage numbers for actors outside `Actor_ApplyDamage` / confirmed extended debits | No number |
| Enemies setting their own health after initialization | Vanilla health; affected species require in-game identification |
| Enemies ignoring hits with zero debit in the health mirror | Affected species require in-game identification |
| Hit leaving the mirror unchanged on an actor consuming damage manually | Pending debit is discarded by the host |
| Broken Giant's Knife | Uses Biggoron modifiers (+2 Power / −8 Courage), instead of −7/−12; the item source does not distinguish it |
| Scaled health outside ENEMY category | Not included; the original scales every category except Link |
| Child strength with gauntlets | Uses the emitted item (+1/+2/+3); the original's `Player_GetStrength` returns 1 for the child |
| Formula values above 65535 | Continue until clamped; the original wraps on the u16 cast |

The original missing `break` for `ACTOR_EN_POH` is reproduced: it also uses the field-Poe rule.
The host's enemy-defeat hook supplies XP events; the port has no `ignoreExpReward` flag.
Automated tests cover formulas and mocked flows. Host rendering, save serialization, enemy-specific health changes,
token delivery and collision coverage still require the F3-2 in-game matrix.

## Tests

From the examples repository, run a core build containing the RFC 0031 mock:

```powershell
shiplua_mod_test_runner.exe lua/leveled --game oot --capability hooks.bridge --capability actor.events --capability actor.observe --capability actor.health --capability core.save --capability core.attributes --capability save.events --capability audio.play
```

`flow_test.lua` also runs without `audio.play`. The mock has no native rendering/menu adapters; those unit checks
use local service doubles. No game or ROM is needed.

## Credits and license

- **Arrenton** — original **Leveled 1.1.3.6**, [Shipwright PR #11](https://github.com/Arrenton/Shipwright/pull/11),
  reference commit `ebdc88c4daed88c139df3c72ebf4a86679458d6c` (XP curve, species/scene tables and stat formulas).
- **BaiterYamato** — Link-Span Lua port.

Link-Span-specific port code follows this repository's **CC0 1.0 Universal** dedication; see
[LICENSE](../../LICENSE) and [NOTICE.md](../../NOTICE.md). Arrenton's original work retains its own terms;
the repository's CC0 dedication does not relicense third-party work. This mod contains no ROM, extracted game
assets or save files.
