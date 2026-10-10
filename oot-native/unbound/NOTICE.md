# Provenance of the Unbound 0.9 port

Link-Span's own work in this package (DLL, Lua, tool, header, schemas and documentation) is dedicated to the public
domain under CC0 1.0 Universal (`LICENSE`). The parts adapted from Unbound, described below, remain with roborich and
contributors; that project publishes no license, and this package grants none over them. The DLL and the tool embed
nlohmann/json 3.12.0 (MIT), with its text in `docs/LICENSES.md`.

The type reader in `actor_registry.cpp` was adapted from `ActorRegistry.cpp` and `DeclaredActorType.h` of
[roborich/Shipwright, tag 9.2.3-unbound0.9](https://github.com/roborich/Shipwright/releases/tag/9.2.3-unbound0.9),
commit `cf7db7f7f9b65247347d54abea386669ce9c909f`.

The Link-Span host adapts `DeclaredActor.cpp` in the generic `linkspan.oot.actor-models` service. The JSON format and
behaviors follow `unbound-docs/SPEC.md`, §§4.3 and 7.2.

No license file was found at the root of that tag. This record preserves authorship and does not grant a
redistribution license. The examples use resource paths; game models and textures are not included in the package.
