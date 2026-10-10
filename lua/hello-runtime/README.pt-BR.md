[English](README.md) · **Português (Brasil)**

# Hello, runtime

Exemplo mínimo do release `v0.1.0-alpha.1` do SDK: demonstra a
superfície central da API comum — logging, detecção de capabilities
(`ship.capabilities.has`), eventos (`ship.events.on`) e hotkeys
(`ship.hotkeys.register`) — sem depender de recursos exclusivos de um host.

## Validar

```powershell
python <link-span>/tools/shipmod.py validate lua/hello-runtime
```

## Testar sem jogo (mock runtime)

Os testes em `tests/` usam a DSL `describe`/`it`/`assert` do
`shiplua_mod_test_runner` e rodam sem jogo ou ROM:

```powershell
python <link-span>/tools/shipmod.py test lua/hello-runtime
```

## Instalar

Copie a pasta para o diretório `mods` do jogo (ou empacote-a como
`hello-runtime.shipmod`). Na inicialização, o log deve registrar:

```text
hello-runtime loaded on oot
hello-runtime ready — press H (rebindable)
```

Ao pressionar `H` (rebindável nas configurações do Link-Span):

```text
Hello from oot!
```

O mod não solicita permissões e não acessa arquivos, rede, DLLs ou ativos dos jogos.
