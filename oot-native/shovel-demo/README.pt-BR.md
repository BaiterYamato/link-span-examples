[English](README.md) · **Português (Brasil)**

# Pá de demonstração (OoT)

Mod independente para provar os contratos novos do Link-Span no OoT: preflight de assets, item com chave estável, uso
pelo Link criança, registro V2 com dono e um mod que monta a própria pasta `assets/`. O pacote traz o ícone, a
textura do nome e o modelo 3D da pá (31 arquivos em `assets/`), então o item próprio do demo aparece como uma pá. Eles
vêm do fork Not Enough Items do skijer ([skijer/Shipwright, branch `Not-Enough-Items`](https://github.com/skijer/Shipwright/tree/Not-Enough-Items),
commit `c29262b`). O fork não publica licença, então esses arquivos continuam com os autores e o pacote não concede
licença sobre eles (veja `NOTICE.md`). O demo **não** é uma cópia da pá do Skijer nem pretende ser um mod de gameplay
final.

No carregamento o provider monta a pasta `assets/` do pacote (`ProviderAssetsDirectory` e `mount_archive`) e a desmonta
no shutdown. Se a pasta não puder ser montada ou não tiver a pá, o item volta ao ícone e ao modelo do Martelo Megaton
do `oot.o2r` do jogo. Esses dois arquivos são os únicos recursos que o `[resources]` do manifesto exige: o preflight
roda antes de a DLL carregar, quando o `assets/` do próprio pacote ainda não está montado. A função `stats` (que o
`main.lua` registra no log a cada 180 frames) informa `assets=package` ou o motivo da reserva: `noservice`,
`nofolder`, `mountfail` ou `incomplete`. A textura do nome viaja com o conjunto, mas este demo não a desenha.

Com o NEI carregado, o demo concede a pá do próprio NEI. Ela aparece na segunda página do inventário, na última linha,
e pode ser equipada pelo menu. O item próprio do demo, já equipado por uma versão anterior, é trocado pela pá do NEI.
Sem o NEI, o demo mantém seu item independente no botão C: em solo firme, cada uso cria uma rupee verde à frente do
Link. Esse item independente não tem célula de inventário e não serve para uma partida principal.

Para empacotar, `tools/package-shovel-demo.ps1` exige `-NeiAssetsCore <nei-assets-core.o2r>` (o arquivo que o
`tools/build-nei-assets.py` do host gera) e extrai dele só as entradas da pá.
