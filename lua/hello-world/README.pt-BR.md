[English](README.md) · **Português (Brasil)**

# Olá, mundo

Este é o primeiro mod comum do Link-Span. O mesmo pacote funciona no Shipwright e no
2Ship2Harkinian porque usa apenas a API compartilhada.

## Instalar

Copie a pasta para a pasta `mods` do diretório gravável do jogo. Para distribuir
um arquivo só, compacte o conteúdo da pasta (com `manifest.toml` na raiz do ZIP)
e chame-o de `hello-world.shipmod`. Na inicialização, o log deve registrar uma das mensagens:

```text
Hello from oot host=<versão>
Hello from mm host=<versão>
```

O mod não solicita permissões e não acessa arquivos, rede, DLLs ou ativos dos jogos.
