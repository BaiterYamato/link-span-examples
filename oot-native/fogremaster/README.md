# FogreMaster

Mod nativo de névoa rasteira interativa para OoT, Windows x64. Uma grade de 25 × 25 pontos acompanha Link em Kokiri Forest, Lost Woods e na entrada do Shadow Temple. A entrada inclui a sala das tochas no Graveyard e o corredor inicial do templo. Em Kokiri, a grade cobre 1.920 unidades de lado, mantém a referência de altura ao cruzar os patamares e usa a superfície da água onde ela cobre o chão. Ao deslocar a grade, o mod conserva a região em comum. As células novas de Kokiri ganham densidade aos poucos. Na entrada de uma cena ativa, a névoa aparece gradualmente.

- DirectX 11 com feature level 11.0 usa compute para densidade, velocidade e partículas. Hosts que anunciam a extensão de elevação também simulam em compute a troca de densidade entre faixas no apoio inferior. O apoio superior e a referência de recuperação usam a CPU. O passe volumétrico usa profundidade, MSAA, detalhe em coordenadas de mundo e transições graduais na recomposição das bordas.
- OpenGL desktop desenha o volume com a grade simulada na CPU. Requer OpenGL 3.2 sem MSAA ou OpenGL 4.0 com profundidade multiamostrada compatível.
- O RC11 padrão e os backends sem o passe volumétrico usam duas camadas translúcidas com teste de profundidade.

- O cilindro de Link e os colisores OC cilíndricos ou esféricos de outros atores deslocam a névoa. Corpos acima da camada ficam de fora.
- A trajetória da base e da ponta da arma de Link abre uma faixa e empurra a névoa na direção do golpe, inclusive sem alvo. A força varia ao longo da lâmina e o sentido vertical da trajetória determina quanto a névoa sobe.
- O ataque giratório começa a empurrar a névoa perto de Link. A pressão avança até a borda da área atingida e também empurra ao redor do giro. A animação do giro e a janela ativa da arma determinam quando começa o impulso.
- Até 96 partículas GPU seguem a velocidade local, o vento e uma pequena oscilação. O contraste das partículas sobe nas regiões de fluxo rápido. As áreas vazias do campo permanecem abertas.
- Os colisores AT ativos geram pressão no campo. Bombas e Fogo de Din empurram radialmente. Quads de projéteis aplicam força na direção da varredura. O mod lê a geometria antes da etapa de colisão e altera somente a grade de névoa.
- A configuração de hotkeys do Link-Span permite atribuir comandos para ligar e desligar o mod, alternar volume e camadas, escolher compute ou a referência em CPU, alternar as partículas e registrar contadores.
- A máscara fecha pontos sem superfície de apoio. Fora de Kokiri, ela também fecha pontos submersos. Paredes de colisão bloqueiam o transporte entre vizinhos. Em Kokiri, o host compatível pode representar chão e plataforma na mesma coluna. Cada apoio tem densidade e resposta a ataques próprias.
- O campo 2,5D transporta densidade pelas faces entre células. Os ataques distribuem o impulso ao longo de vários passos. O próprio fluxo abre o rastro e acumula névoa onde encontra o campo em repouso. A vorticidade local sustenta pequenos redemoinhos enquanto o rastro fecha.

## Configuração no jogo

Abra **Esc → Mods → FogreMaster**. O painel controla a névoa interativa, o desenho volumétrico, a simulação e as partículas na GPU. Densidade e força dos ataques variam de 25% a 200%, com padrão de 100%. **Duração do empurrão (ms)** distribui a força dos próximos golpes por 150 a 1.000 ms, com padrão de 450 ms. **Elevação nos ataques** vai de 0 a 200%, com padrão de 100%. **Iluminação local da névoa** aplica a cor de luzes como tochas. A qualidade oferece o padrão do host, resolução cheia, meia resolução e meia resolução com reprojeção.

Em um host DirectX 11 com suporte a snapshots V2, desligar **Simulação de fluido na GPU** troca a fonte do campo para CPU e conserva os apoios, o volume e as luzes. **Partículas na GPU** continua independente.

O Link-Span guarda as preferências do mod entre sessões. **Restaurar padrões** recupera os valores iniciais. As hotkeys atualizam as mesmas preferências.

## Build e pacote Windows

Use o SDK OoT com o mesmo layout do executável, o header `oot_fog.h` que define `LINKSPAN_OOT_FOG_FLOW_CPU_FIELD`, MSVC x64 e CMake 3.26 ou superior. O build experimental usa o layout `875d1455` do RC11. O serviço `linkspan.oot.fog` é opcional em runtime. A V2 recebe dois lotes de impulsos por tick e negocia as extensões com o host. A V1 desenha o volume com o campo em CPU. O pacote também usa as camadas nos hosts sem o serviço.

```powershell
cmake -S oot-native/fogremaster -B build/fogremaster -G "Visual Studio 17 2022" -A x64 -DOOT_NATIVE_SDK="C:/sdk/OotNativeSdk.cmake"
cmake --build build/fogremaster --config Release
ctest --test-dir build/fogremaster -C Release --output-on-failure
. ./tools/linkspan-zip.ps1
New-LinkSpanZip -Source build/fogremaster/mod -Destination build/FogreMaster.shipmod
```

Coloque o pacote em `mods/` de uma instalação com o layout compatível. Use um arquivo de teste e entre em Kokiri Forest. Corra, espere o rastro recuperar e dê um golpe baixo no ar. Compare um giro na sala das tochas com a duração padrão e com o valor máximo do controle.

O host experimental começa no modo espacial. Para comparar, use `set gDeveloperTools.FogVolumeQuality 0` para resolução cheia, `1` para reconstrução por profundidade em meia resolução e `2` para acrescentar reprojeção temporal.

No host experimental, `set gDeveloperTools.FogFlowAudit 1` ativa a leitura assíncrona da textura de simulação. O log registra células abaixo de metade da densidade de repouso, perdas em células sem força naquele tick e acúmulos sem força direta com densidade acima de 120% do repouso. Use `0` para voltar ao modo normal.

## Build experimental para macOS

Use o SDK gerado pelo próprio build macOS do host. O provider segue a arquitetura desse build. O CMake gera a entrada `macos` com `provider/fogremaster.dylib` no manifesto. O pacote Windows e o pacote macOS têm layouts próprios.

```sh
cmake -S oot-native/fogremaster -B build/fogremaster-macos -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DOOT_NATIVE_SDK="/caminho/build-host/native-sdk/Release/OotNativeSdk.cmake"
cmake --build build/fogremaster-macos
ctest --test-dir build/fogremaster-macos --output-on-failure
(cd build/fogremaster-macos/mod && zip -X -r ../FogreMaster-macos.shipmod LICENSE manifest.toml main.lua provider)
```

O passe Metal experimental usa o campo em CPU e desenha o volume com profundidade do mundo. Em uma instalação de teste com Metal selecionado, ative `set gDeveloperTools.FogMetalExperimental 1`. O roteiro deve verificar a linha `Fast3D Metal fog volume samples=N grid=25x25`, a oclusão por terreno, os ataques, o HUD, a pausa e a reentrada com MSAA desligado e em 4×. O código Metal e o provider macOS aguardam build e teste em uma GPU Apple.
