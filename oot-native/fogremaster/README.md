# FogreMaster 0.33.8

Mod nativo de névoa rasteira interativa para OoT, com pacote Windows x64 e build experimental para macOS. Uma grade de 25 × 25 pontos acompanha Link em Kokiri Forest, Lost Woods e na entrada do Shadow Temple. A entrada inclui a sala das tochas no Graveyard e o corredor inicial do templo. Em Kokiri, a grade cobre 1.920 unidades de lado, mantém a referência de altura ao cruzar os patamares e usa a superfície da água onde ela cobre o chão. Ao deslocar a grade, o mod conserva a região em comum. As células novas de Kokiri ganham densidade aos poucos. Na entrada de uma cena ativa, a névoa aparece gradualmente.

- Com suporte ao campo CPU, o campo principal de 25 × 25 usa a simulação de camada rasa por padrão. Em DirectX 11 com feature level 11.0 e suporte do host, a opção **Campo antigo na GPU (comparação)** volta à simulação antiga em compute. O passe volumétrico usa profundidade, MSAA, detalhe em coordenadas de mundo e transições graduais na recomposição das bordas.
- OpenGL desktop desenha o volume com a grade simulada na CPU. Requer OpenGL 3.2 sem MSAA ou OpenGL 4.0 com profundidade multiamostrada compatível.
- O RC11 padrão e os backends sem o passe volumétrico usam duas camadas translúcidas com teste de profundidade.

- O cilindro de Link e os colisores OC cilíndricos ou esféricos de outros atores deslocam a névoa. Corpos acima da camada ficam de fora.
- A trajetória da base e da ponta da arma de Link abre uma faixa e empurra a névoa na direção do golpe, inclusive sem alvo. A força varia ao longo da lâmina e o sentido vertical da trajetória determina quanto a névoa sobe.
- O ataque giratório começa a empurrar a névoa perto de Link. A pressão avança até a borda da área atingida e também empurra ao redor do giro. A animação do giro e a janela ativa da arma determinam quando começa o impulso.
- Até 96 partículas GPU seguem a velocidade local, o vento e uma pequena oscilação. O contraste das partículas sobe nas regiões de fluxo rápido. As áreas vazias do campo permanecem abertas.
- Os colisores AT ativos geram pressão no campo. Bombas e Fogo de Din empurram radialmente. Quads de projéteis aplicam força na direção da varredura. O mod lê a geometria antes da etapa de colisão e altera somente a grade de névoa.
- A configuração de hotkeys do Link-Span permite atribuir comandos para ligar e desligar o mod, alternar volume e camadas, comparar a camada rasa com o campo antigo na GPU, alternar as partículas e registrar contadores.
- A máscara fecha pontos sem superfície de apoio. Fora de Kokiri, ela também fecha pontos submersos. Paredes de colisão bloqueiam o transporte entre vizinhos. Em Kokiri, o host compatível pode representar chão e plataforma na mesma coluna. Cada apoio tem densidade e resposta a ataques próprias.
- O campo 2,5D transporta densidade pelas faces entre células. Os ataques distribuem o impulso ao longo de vários passos. O próprio fluxo abre o rastro e acumula névoa onde encontra o campo em repouso. A vorticidade local sustenta pequenos redemoinhos enquanto o rastro fecha.

## Configuração no jogo

Abra **Esc → Mods → FogreMaster**. O painel controla a névoa interativa, o desenho volumétrico, a simulação e as partículas na GPU. Densidade e força dos ataques variam de 25% a 200%, com padrão de 100%. **Duração do empurrão (ms)** distribui a força dos próximos golpes por 150 a 1.000 ms, com padrão de 450 ms. **Elevação nos ataques** vai de 0 a 200%, com padrão de 100%. **Iluminação local da névoa** aplica a cor de luzes como tochas. A qualidade oferece o padrão do host, resolução cheia, meia resolução e meia resolução com reprojeção.

**Cor da névoa acompanha a cena** segue a iluminação ambiente suavizada. **Partículas na GPU** tem controle próprio e depende do caminho GPU suportado pelo host.

**Logs de calibração** começa desligado. Ao ativar, o mod registra mudanças do vento base, as primeiras amostras de luz, golpes e pousos e os primeiros valores não nulos dos contadores observados. Os resultados de configuração, avisos de erro e a hotkey `fogremaster_stats` continuam disponíveis com a preferência desligada.

A preferência é o booleano `settings.calibration_logs` no armazenamento privado de `linkspan.fogremaster`. Para a matriz que coleta linhas `FogreMaster: active`, adicione `"settings.calibration_logs": true` ao objeto `base.storage["linkspan.fogremaster"]` do plano de teste. Com ela ligada, a coleta periódica mantém o intervalo, o formato e os limites por sessão usados pela matriz.

O Link-Span guarda as preferências do mod entre sessões quando oferece `core.storage`. **Restaurar padrões** recupera os valores iniciais. As hotkeys de configuração atualizam as mesmas preferências.

## Stable Fluids em Kokiri

O detalhe fino de Kokiri usa Stable Fluids em uma grade MAC 2D no plano X/Z, com pressão por SOR red-black e advecção MacCormack com limitador. A janela segue Link em passos inteiros de célula. Golpes e pousos aplicam forças ao fluido. O campo largo fornece o repouso, os sólidos e a máscara de abertura que modula a saída fina.

O solver roda quando o mod usa volume, a grade de Kokiri está ativa, o campo é simulado na CPU e o host anuncia `VORTICES`, `DETAIL_GRID` e capacidade para o lado solicitado. O bit `LINKSPAN_OOT_FOG_FLOW_DETAIL_WIDE` anuncia detalhe de até 128 × 128. Sem WIDE, o mod limita o envio a 32 × 32 e suspende o solver fino. Quando há suporte ao detalhe estreito, o mod envia a grade anterior de 25 × 25. `gDeveloperTools.FogreDetailGrid=0` desliga a grade fina.

### CVars do solver

Use os nomes abaixo com o prefixo `gDeveloperTools.` no console. Por exemplo, `set gDeveloperTools.FogreStableSide 128` escolhe a grade de 128 × 128.

| CVar | Padrão | Efeito |
| --- | ---: | --- |
| `FogreStable` | 1 | Ativa o solver quando o caminho de detalhe é compatível. Use 0 para comparar com o detalhe anterior. |
| `FogreStableSide` | 96 | Lado da grade, em células. Aceita 64, 96 ou 128. Outros valores voltam a 96. |
| `FogreStableCell` | 100 | Tamanho em décimos de unidade. Aceita 50–200 (5–20 unidades), sujeito ao limite de encaixe no campo principal. |
| `FogreStableVorticity` | 40 | Ganho de confinamento de vorticidade em décimos, limitado a 0–400. |
| `FogreStableRecovery` | 8 | Taxa de retorno da densidade ao repouso em centésimos de 1/s, limitada a 0–500. |
| `FogreStableDamping` | 60 | Taxa de amortecimento em centésimos de 1/s, limitada a 0–1000. |
| `FogreStableStir` | 100 | Ganho percentual do impulso no fluido, multiplicado pelo controle de força dos ataques. |
| `FogreStableCurl` | 0 | Intensidade de curl-noise em unidades/s², limitada a 0–200. |
| `FogreStableWisps` | 110 | Contraste percentual dos fiapos no campo de repouso. |
| `FogreStableWispScale` | 180 | Escala espacial dos fiapos em unidades, com mínimo de 20. |

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

## Testes do campo CPU

Os alvos `fog_field_tests`, `fog_color_tests`, `fog_stable_tests` e `follow_hysteresis_test` registram, respectivamente, os testes CTest `fog_field`, `fog_color`, `fog_stable` e `follow_hysteresis`. O teste `fog_stable` usa `--checks-only`. É possível compilar os quatro sem SDK e sem o provider.

```powershell
cmake -S oot-native/fogremaster -B build/fogremaster-tests -DFOG_FIELD_TESTS_ONLY=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build/fogremaster-tests --config Release
ctest --test-dir build/fogremaster-tests -C Release --output-on-failure
```

Com `-DFOG_STABLE_TIMING_TESTS=ON` na configuração, o CMake registra também `fog_stable_timing` com `--timing-only` e o rótulo `benchmark`. Execute as metas de tempo em Release.

```powershell
ctest --test-dir build/fogremaster-tests -C Release -L benchmark --output-on-failure
```

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
