**English** · [Português (Brasil)](README.pt-BR.md)

# Hello, world

This is the first common Link-Span mod. The same package works on Shipwright and on 2Ship2Harkinian because it only
uses the shared API.

## Install

Copy the folder into the `mods` folder of the game's writable directory. To ship it as a single file, zip the folder
contents (with `manifest.toml` at the root of the archive) and name it `hello-world.shipmod`. On startup, the log
should show one of these messages:

```text
Hello from oot host=<version>
Hello from mm host=<version>
```

The mod requests no permissions and does not touch files, the network, DLLs or game assets.
