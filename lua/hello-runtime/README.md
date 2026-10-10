**English** · [Português (Brasil)](README.pt-BR.md)

# Hello, runtime

Minimal example from the SDK `v0.1.0-alpha.1` release. It shows the core of the common API (logging, capability
detection with `ship.capabilities.has`, events with `ship.events.on` and hotkeys with `ship.hotkeys.register`)
without relying on features exclusive to one host.

## Validate

```powershell
python <link-span>/tools/shipmod.py validate lua/hello-runtime
```

## Test without the game (mock runtime)

The tests in `tests/` use the `describe`/`it`/`assert` DSL of `shiplua_mod_test_runner` and run without the game or a
ROM:

```powershell
python <link-span>/tools/shipmod.py test lua/hello-runtime
```

## Install

Copy the folder into the game's `mods` folder (or package it as `hello-runtime.shipmod`). On startup, the log should
show:

```text
hello-runtime loaded on oot
hello-runtime ready — press H (rebindable)
```

When you press `H` (rebindable in the Link-Span settings):

```text
Hello from oot!
```

The mod requests no permissions and does not touch files, the network, DLLs or game assets.
