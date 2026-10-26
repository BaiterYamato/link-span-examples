[English](README.md) · **Português (Brasil)**

# Leveled

Progressão de RPG para **Ocarina of Time**, portada do Leveled 1.1.3.6 de Arrenton para Lua puro no Link-Span.
Derrote inimigos e colete tokens de Gold Skulltula para ganhar XP, chegar aos níveis 1–99 e aumentar Power e Courage.
O nível dos inimigos depende da espécie, cena, idade e parâmetros. A vida e o dano de colisão seguem esses stats.
O HUD acrescenta barra de XP, números de dano/XP e aviso de nível, com fanfarra opcional.

Copie esta pasta para `mods` do host ou instale o pacote `.shipmod`. As opções aparecem em
**Mods → Leveled** quando o host oferece menus de mods. Opções de vida/nível dos inimigos valem para os próximos
spawns; dano, HUD e XP usam as opções atuais. Corações bônus atualizam em até 30 frames do jogo.

## Requisitos

- API Link-Span `>=0.5 <0.6`, com host OoT que implemente a **RFC 0031 (atores para mods)**.
- Capacidades exigidas: `hooks.bridge`, `actor.events`, `actor.observe`, `actor.health`, `core.save`,
  `core.modules`, `core.attributes`, `save.events`.
- Permissões de manifesto: `world.entities.read` e `world.entities.write`.
- Capacidades opcionais conferidas em runtime: `core.menu` (página de opções), `core.storage` (opções persistentes),
  `hud.draw` (HUD), `audio.play` (fanfarra), `player.fields` (corações, idade/tokens e posição do Link na tela),
  `oot.env` (nível da cena). Sem storage, usam-se os padrões. Sem campos do jogador, corações e âncoras do Link
  são omitidos e idade/tokens valem 0. Sem API de cena, valem os níveis alternativos por espécie.
- Ative **`linkspan.oot.item_source`** para os bônus de equipamento vanilla. O port declara os atributos de
  Power/Courage e os modificadores em `attributes.toml`; outros mods podem acrescentar modificadores a eles.

XP e capacidade de corações vanilla pertencem ao arquivo carregado (`ship.save`: `schema = 1`, `xp`,
`base_capacity`). O nível deriva do XP. As opções ficam no `ship.storage` global do mod. Sem save, não há ganho
de XP. Antes de salvar, o bônus de capacidade sai e a vida é limitada à capacidade vanilla; ambos voltam no
próximo frame. Assim, ao carregar o save com o mod desligado, a capacidade de corações continua vanilla.

## Opções

Valores numéricos são cortados à faixa. `multiplier` e `damage_taken` são inteiros. Fatores usam 1.0 para 100%.
O menu tem três grupos.

| Grupo | Chave no storage | Padrão | Faixa / efeito |
| --- | --- | --- | --- |
| HUD | `xp_bar` | true | Nível e barra de XP |
| HUD | `numbers_enemy` | true | Números de dano nos inimigos |
| HUD | `numbers_player` | true | Números de dano no Link |
| HUD | `numbers_xp` | true | XP sobre o Link |
| HUD | `level_up` | true | Aviso de nível |
| HUD | `level_up_sound` | true | Fanfarra ao subir de nível |
| Jogador | `bonus_hearts` | true | Um coração a cada 8 níveis, até 10 bônus; teto total de 20 |
| Jogador | `equipment` | true | Modificadores de equipamento |
| Jogador | `damage_taken` | 4 | 1–32; dano de colisão × N/4 |
| Jogador | `xp_rate` | 1.0 | 0–10; todas as recompensas de XP |
| Jogador | `token_rate` | 1.0 | 0–10; XP de token antes de `xp_rate` |
| Inimigos | `attack_scales` | true | Nível afeta o ataque base |
| Inimigos | `multiplier` | 9 | 1–32; multiplicador de ataque/vida |
| Inimigos | `hp_percent` | 1.0 | 0–10; fator de vida |
| Inimigos | `level_scale` | 1.0 | 0–10; escala de nível |
| Inimigos | `ganon_max_level` | false | Nível máximo do castelo com exatamente 100 tokens |

A fórmula original de corações usa `(level + 1) // 8`: o primeiro bônus aparece no nível 7.
A opção oculta `debug_log = true` registra inicializações, danos de colisão transformados e abates em
`ship.log.info`; vem desligada e não tem widget no menu.

## Exclusões e diferenças do v1

| Caso | Comportamento |
| --- | --- |
| Chefes (categoria BOSS) e débitos manuais de chefe | Vida e dano recebido vanilla; o dano de colisão que causam usa a fórmula |
| Agarrões (`play->damagePlayer`), knockback com dano, queda, fogo e armadilhas fora do funil de colisão | Dano vanilla; `damage_taken` só vale no hook de colisão |
| Magia por nível (72/144 unidades) | Fora do v1; outro subprojeto |
| Mais de 20 corações ou mudança das unidades por coração | Fora; o bônus para em 20 corações |
| Informação da Navi, nível no seletor de arquivos e stats na pausa | Fora |
| Números para atores fora de `Actor_ApplyDamage` / débitos estendidos confirmados | Sem número |
| Inimigos que definem a própria vida depois do init | Vida vanilla; espécies afetadas precisam ser identificadas em jogo |
| Inimigos que ignoram golpe com débito 0 no espelho | Espécies afetadas precisam ser identificadas em jogo |
| Golpe que não muda o espelho em ator que consome dano à mão | O host descarta o débito pendente |
| Faca de Biggoron quebrada | Conta como Biggoron (+2 Power / −8 Courage), em vez de −7/−12; a fonte não distingue |
| Vida escalada fora da categoria ENEMY | Fora; o original escala todas as categorias exceto Link |
| Força da criança com manopla | Segue o item emitido (+1/+2/+3); `Player_GetStrength` do original retorna 1 para criança |
| Valores acima de 65535 nas fórmulas | Seguem até o corte; o original dá a volta no cast para u16 |

O `break` ausente de `ACTOR_EN_POH` no original é reproduzido: ele também usa a regra do Poe de campo.
O hook de derrota do host fornece os eventos de XP; não existe `ignoreExpReward` no port.
Os testes automatizados cobrem fórmulas e fluxos com mock. Renderização, serialização do save, alterações de vida
específicas de inimigos, entrega de tokens e cobertura de colisão ainda precisam da matriz em jogo da F3-2.

## Testes

A partir do repositório de exemplos, use um build do core com o mock da RFC 0031:

```powershell
shiplua_mod_test_runner.exe lua/leveled --game oot --capability hooks.bridge --capability actor.events --capability actor.observe --capability actor.health --capability core.save --capability core.attributes --capability save.events --capability audio.play
```

`flow_test.lua` também roda sem `audio.play`. O mock não possui adaptadores nativos de desenho/menu; esses testes
unitários usam substitutos locais dos serviços. Não precisa de jogo nem ROM.

## Créditos e licença

- **Arrenton** — **Leveled 1.1.3.6** original, [PR #11 do Shipwright](https://github.com/Arrenton/Shipwright/pull/11),
  referência `ebdc88c4daed88c139df3c72ebf4a86679458d6c` (curva de XP, tabelas de espécies/cenas e fórmulas).
- **BaiterYamato** — port Lua para Link-Span.

O código próprio do port Link-Span segue a dedicação **CC0 1.0 Universal** do repositório; consulte
[LICENSE](../../LICENSE) e [NOTICE.md](../../NOTICE.md). O trabalho original de Arrenton mantém seus próprios
termos; a dedicação CC0 do repositório não relicencia trabalho de terceiros. O mod não contém ROM, assets
extraídos do jogo nem saves.
