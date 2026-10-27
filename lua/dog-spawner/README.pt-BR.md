[English](README.md) · **Português (Brasil)**

# dog-spawner

Aperte **F** (reconfigurável) para criar um cachorro na frente do jogador.

- **OoT (Ship of Harkinian):** usa `ship.actor.spawn("oot.en_dog", ...)`, como
  [actor-spawn](../actor-spawn/main.lua). Consulta `ship.actor.exists` e destrói o cão anterior do mod
  antes de criar outro. Um handle expirado (por exemplo após trocar de cena) é descartado.
- **MM (2Ship2Harkinian):** mantém `ship.mm.spawn_dog()` e a capability `mm.spawn_dog`.

O [entrypoint Lua](main.lua) seleciona o jogo ativo com `ship.game.id()` e verifica suas capabilities antes de
registrar a tecla. O caminho genérico do OoT exige `actor.spawn`, `actor.destroy` e `actor.exists`; o caminho MM
só exige `mm.spawn_dog`. São opcionais no manifesto para o mesmo mod carregar nos dois hosts.

## Instalar

Copie esta pasta (ou empacote como `dog-spawner.shipmod`) para `mods/` do jogo e abra o jogo.
Aperte **F** em jogo.

## Notas

- O manifesto concede `world.entities.create`, `world.entities.destroy` e `world.entities.read` para a API
  genérica de atores e limita os atores próprios a um. O helper MM conserva o comportamento original.
- **OoT:** o host interpreta a posição `(0, 0, 0)` como “na frente do jogador”. O spawn retorna um handle opaco
  ou um erro de operação, que é registrado no log. O objeto do cão precisa estar carregado na cena. Tente
  Hyrule Market (`SCENE_MARKET_DAY` / `SCENE_MARKET_NIGHT`); fora de cenas adequadas, `En_Dog` pode ficar parado
  ou se remover.
- **MM:** `object_dog` precisa estar carregado na cena (Clock Town e áreas com cachorros) para renderizar.

## Testar sem o jogo

```powershell
python <link-span>/tools/shipmod.py test lua/dog-spawner --game oot
python <link-span>/tools/shipmod.py test lua/dog-spawner --game mm
```

Os testes cobrem substituição do handle próprio, handles expirados, falhas de operação e a rota MM preservada.
