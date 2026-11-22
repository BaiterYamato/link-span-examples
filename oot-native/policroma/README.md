# Policroma

Coremod do Link-Span OoT para organizar shaderpacks no Windows com DirectX 11.

1. Use um host OoT com a ponte Policroma v1 e o layout ID do pacote.
2. Coloque o `Policroma-*.shipmod` em `mods/` e abra o jogo. O coremod cria `shaderpacks/` na pasta de dados do aplicativo.
3. Coloque o ZIP em `shaderpacks/`. Abra **Esc → Mods → Policroma**, clique em **Atualizar lista** e selecione o pack. A seleção é guardada no armazenamento do próprio mod.

O menu também permite recarregar o pack ativo e desligar os efeitos. Um erro de leitura ou compilação preserva o passe anterior. Em uma instalação nova os efeitos começam desligados. Ao retirar o coremod, os ZIPs ficam disponíveis para uma futura instalação e o host executa seu desenho normal.

O Policroma executa HLSL Link-Span v1 e v2, fragmentos GLSL no formato Link-Span v2 e um subconjunto do Iris com `deferred`, `composite` e `final`. Com a ponte Policroma V3 do host (RFC 0035) o Complementary Unbound r5.9.3 roda os seus 9 programas; gbuffers, sombras e outros estágios do Iris ainda não existem.

## Iris reduzido

Packs Iris são ativados pelo menu como qualquer pack, em ZIP ou em pasta dentro de `shaderpacks/`. Os programas rodam na ordem `deferred` … `deferred15`, `composite` … `composite15`, `final`. Um programa em `shaders/world0/` tem preferência sobre o da raiz, como na dimensão da superfície do Iris.

Com a ponte V3 (host com `LinkSpan_GetPolicromaBridgeV3`):

- até 16 programas, cada um com até 8 alvos (`/* DRAWBUFFERS:N */` ou `/* RENDERTARGETS: N */`; vale a diretiva ativa depois do pré-processador) e até 16 texturas;
- o `.vsh` ao lado de cada `.fsh` roda como vertex shader do passe: `ftransform()`, `gl_Vertex`, `gl_MultiTexCoord0`, `gl_TextureMatrix`, `gl_*Matrix`, os atributos do núcleo (`vaPosition`, `vaUV0`, `vaColor`, `vaNormal`) e as matrizes `modelViewMatrix`/`projectionMatrix` descrevem o triângulo de tela cheia. Os varyings ligam pelo nome; um varying que o vértice não escreve lê zero;
- `colortex` que nenhum programa anterior escreveu lê zero; com `const bool colortexNClear = false;` lê o quadro anterior do último programa que o escreve (histórico, usado pelo TAA);
- texturas próprias do `shaders.properties`: `texture.noise` e `texture.<deferred|composite>.<buffer>` com PNG do pack (`composite` vale também para o `final`; a última declaração vence). O `.mcmeta` com `"clamp": true` prende a borda; sem ele a textura repete. Exige host com `PolicromaFeaturePackTextures`; `gbuffers`, `minecraft:…` e texturas cruas `.dat` ficam de fora;
- `noisetex` repete pelo sampler `s1` do host (o `noise.png` do pack, se houver; senão um ruído 256×256); `shadowtex*`, `shadowcolor*` e `sampler2DShadow` leem 1 (sem mapa de sombra, tudo iluminado);
- `textureGather` (Shader Model 5) e `gl_Fog`;
- toda amostragem usa o nível 0 de detalhe: as texturas do grafo têm um único mip, e assim o FXC não precisa desenrolar laços com derivadas.

Sem a V3 valem os limites antigos: 8 programas de `composite`/`final`, um alvo e 4 texturas por programa, `.vsh` ignorado e ler um `colortex` não escrito é recusado.

Em ambos:

- `colortex0` é a cena e `depthtex0`, `depthtex1`, `depthtex2` e `gdepthtex` são a profundidade. Os nomes `gcolor`, `gdepth` e `gaux1-4` também valem.
- Uniforms: `near`, `far`, `viewWidth`, `viewHeight`, `aspectRatio`, `frameTimeCounter`, `frameCounter`, `worldTime`, `sunAngle`, `shadowAngle`, `gbufferProjection(Inverse)`, `gbufferModelView(Inverse)`, `cameraPosition`, `eyeAltitude`, `upPosition`, `sunPosition`, `moonPosition`, `shadowLightPosition` e `isEyeInWater`. `frameCounter` e `frameTimeCounter` andam a cada quadro desenhado (relógio de render do host na V3), também com o jogo pausado. Só na V3: `gbufferPreviousProjection`, `gbufferPreviousModelView`, `previousCameraPosition`, `frameTime`, `fogColor`, `skyColor`, `fogStart` e `fogEnd`. Conceitos que o OoT não tem recebem valores neutros fixos (`rainStrength` 0, `screenBrightness` 0,5, `eyeBrightness` (0, 240) …); qualquer outro uniform numérico lê o seu inicializador ou zero.
- As matrizes seguem o Iris: `gbufferModelView` só gira (o espaço do jogador é relativo à câmera), e a posição no mundo é `gbufferModelViewInverse * view + cameraPosition`. As unidades são as do jogo, não blocos do Minecraft.
- A UV, `gl_FragCoord` e as funções de amostragem usam a orientação do OpenGL, com a origem embaixo. Samplers podem ser passados para funções do próprio pack. Samplers só declarados, ou só em ramos `#if` desligados, não ocupam entrada.
- O host guarda os shaders compilados em `cache/` na pasta do jogo. A primeira ativação de um pack grande pode levar alguns segundos; as seguintes usam o cache.
- Os alvos do grafo têm teto de memória em `gDeveloperTools.PackTargetBudget` (MB, padrão 512). O Complementary pede ~360 MB em 1920×1009.
- Uniforms próprios do `shaders.properties` (`uniform.<tipo>.<nome> = expr` e `variable.<tipo>.<nome> = expr`) viram GLSL: aritmética, `%`, comparações, `&&`/`||`/`!`, `if(c1, a, c2, b, …, senão)`, `in`, `between`, `equals`, `frac`, `signum`, `fmod`, `atan2`, `torad`/`todeg`, `round` e as funções do GLSL de mesmo nome. Sem estado entre quadros: `smooth(…)` devolve o alvo e `random()` vale 0,5. Identificador desconhecido vale 0; `biome` e `biome_category` valem -1.

### Altura das nuvens

`cloudHeight` é a altura da camada de nuvens em unidades do OoT. O menu do Policroma (Esc → Mods → Policroma) tem o controle **Altura das nuvens** (-1000 a 8000, padrão 3000, acima da copa da Árvore Deku em Kokiri) e a ação **Altura padrão das nuvens**. O valor fica salvo no armazenamento do mod (`iris.cloud_height`). Num host com `PolicromaFeatureProviderParameters` a mudança aparece no quadro seguinte; num host mais antigo ela vale ao recarregar o pack, e o menu avisa.

Pela API nativa do próprio mod: `ship.native.call("parameters", "cloud_height=<N>")`, com N em -4000..20000; responde `ok\nlive` ou `ok\nreload`.

Limitações conhecidas no Complementary: não há sombras nem materiais (sem gbuffers). Desde a 0.7.1 (relógio de render e `noise.png` com repetição) o grão das nuvens caiu cerca de 9 vezes; sobra um resto nas bordas em movimento. O tom mais escuro que o do jogo vem do tonemap do próprio pack (Lottes, no `composite5`) e foi mantido.

Para diagnosticar um pack extraído sem abrir o jogo: `policroma_iris_tests --pack <pasta do pack> [pasta do HLSL]`. Ele traduz como o provider, compila e confere pelas regras do host e roda o grafo no WARP, passe a passe e por 4 quadros, com câmera e profundidade parecidas com as do OoT.

```powershell
python build_preset.py --iris IrisMinimo.zip
```

### Materiais do OoT nos passes Iris

Packs Iris podem reservar dois `colortex1` a `colortex15` no `shaders/shaders.properties`.
O primeiro recebe normal octaédrica em espaço de mundo em RG, AO em B e validade em A.
O segundo recebe F0 linear em RGB e roughness perceptual em A.

```properties
linkspan.materialGbuffer=colortex13 colortex14
```

O host DX11 precisa expor `scene.material.normal` e `scene.material.specular` na ponte V3.
No piloto de Kokiri os mapas são publicados com `gDeveloperTools.KokiriLabPbrMaterialBuffer=1`.
Um passe de tela cheia reserva um dos 16 passes e publica zeros se o material estiver ausente ou
encoberto na profundidade da cena. As duas saídas devem estar livres de escritas dos shaders,
histórico e texturas personalizadas. Para consultar validade por pixel, leia a saída de normal
com amostragem de texel. O exemplo está em `packs/IrisMaterial`.

O contrato desses alvos é do Link-Span. Os programas `gbuffers_*` do Iris continuam fora do
subconjunto suportado.

## Perfis e pack de demonstração

Packs HLSL v2 podem declarar `profiles` no manifesto `shaders/linkspan-v2.json`. Cada perfil tem um `id`, um `label` e, opcionalmente, `defines` com inteiros e nomes iniciados por `POLICROMA_`. O campo opcional `defaultProfile` indica o perfil inicial. Ao selecionar um perfil pelo menu, o Policroma recarrega os passes e guarda a escolha junto com o nome do ZIP.

O pack de demonstração combina sombras, água, transmissão colorida e oclusão ambiente em meia resolução. O menu oferece oclusão equilibrada, forte, desligada e uma máscara para inspecionar o efeito. A máscara mostra áreas ocluídas em cinza e mantém o HUD visível. As fontes ficam em `packs/`.

O perfil **Vidro com luz da cena** usa a posição, o raio e a cor das luzes pontuais próximas ao material do aquário. No preset de dois passes, a projeção cruza os triângulos do vidro e consulta a profundidade do quadro para atenuar oclusores visíveis. Para comparar com o emissor fixo da demonstração, selecione **Oclusão equilibrada**.

```powershell
python build_preset.py Policroma-Kokiri.zip
```

Com `--point-shadow`, o pack usa um passe separado para a transmissão e ativa **Vidro com luz da cena** por padrão. O host escolhe um emissor próximo do material e rasteriza a geometria que chegou ao Fast3D em seis faces ao redor dessa luz. O input v2 `scene.pointshadow` fornece o atlas 3×2 de profundidade. O buffer `LinkSpanPointShadow` no slot `b4` tem dois `float4`. O primeiro contém posição e alcance da fonte. O segundo contém índice mais um, plano próximo, lado da face e escala de projeção. As demais luzes usam a profundidade da câmera.

```powershell
python build_preset.py --fog-sun --point-shadow Policroma-Kokiri-Vidro.zip
```

`--point-lights 2` reserva dois emissores. O atlas passa a ter quatro linhas e `LinkSpanPointShadow` contém quatro `float4`, com um par por fonte. Os ZIPs com o buffer de dois `float4` continuam usando um único mapa.

```powershell
python build_preset.py --fog-sun --point-shadow --point-lights 2 Policroma-Kokiri-Duas-Luzes.zip
```

## Luz solar na névoa

O input v2 `scene.fogshadow` oferece a mesma textura de `scene.geometryshadow` ao passe do shaderpack e permite ao host compartilhar o mapa solar com o FogreMaster no quadro atual. Na build combinada DX11, a geometria atenua a luz ao longo do raio da névoa. O efeito depende do volume do FogreMaster, da luz diurna e de um mapa solar válido na cena.

No host com a integração temporal, a névoa pode estabilizar a opacidade com o histórico enquanto a luz solar é atualizada a cada quadro. O ajuste de qualidade continua no FogreMaster.

```powershell
python build_preset.py --fog-sun Policroma-Kokiri-Sol.zip
```

## Novidades da 0.7.9

- **Luz do céu automática** (`iris.sky_light = -1`, slider "Luz do céu (caverna; -1 = automático)", mínimo -1, texto "Automático (pela cena)"): com o bit 32 (`PolicromaFeatureSkyExposure`) em `features`, o host escreve um 13º vetor `sky` no b10 (`PolicromaIrisSky`): x = abertura do céu suavizada 0..1 (1 = céu aberto, 0 = interior), y = `indoors` cru, z = `skyboxDisabled` cru, w = `skyboxId`. O GLSL gerado calcula `policroma_sky_light()` = `mix(30, 240, x)` **uma vez por invocação** (global `policroma_sky`, como `policroma_k`) e a usa em `eyeBrightness`, `eyeBrightnessSmooth` e `linkspanSkyLight` (F0).
- **Manual vence**: qualquer valor de 0 a 240 continua manual, como na 0.7.8 (0 = escuro). O **padrão continua 240** até a prova em jogo; o armazenamento antigo (240 gravado) segue manual. O provider aceita `sky_light=-1` a 240.
- **Compatibilidade**: sem o bit 32 o cbuffer b10 não declara o 13º vetor (hosts de 12 vetores continuam aceitos), o shader gerado é o da 0.7.8 (o `--pack` do Complementary com `--fog-divisor 32 --ground-y 0` dá o mesmo HLSL e os mesmos números) e o provider envia 240 no lugar de -1. Com o bit e `sky_light >= 0`, o comportamento é o da 0.7.8 (o GLSL só ganha o vetor e a função).
- Testes: `sky_light` automático/manual por vetor injetado no WARP (x 1 → 240, 0 → 30, 0,5 → 135, limites, manual vence, sem o bit) e declaração do vetor só com o bit. `policroma_iris_tests --pack <pasta> --sky-exposure` roda o pack com o bit, `sky_light = -1` e abertura 0,5.

## Novidades da 0.7.8

- **k automático pela rampa do host**: com o bit 16 (`PolicromaFeatureFogRamp`) em `features`, o host escreve em `PolicromaIrisClock.zw` (b10) a rampa da névoa nativa suavizada na escala `u` do `gSPFogPosition` (z = início, w = fim, 0..1000). O d50 passa a vir de `u50 = 0,5·(z+w)`: `d = n / max(1 − u50·(f−n)/(1000·f), n/f)`, com n/f = `PolicromaCameraInfo.xy`; o k usa a mesma fórmula fechada e o mesmo limite 1/64..1/4. Cobre `worldFog` e `n ≠ 10`. Kokiri 994/5800 dá ~1/21,3; Hyrule 996/12800 ~1/31,7.
- Contrato por quadro: `{0, 0}` = sem rampa válida, cai na fase 1 da 0.7.7; `{0, 0,0001}` = névoa total (k no limite 1/4); `w − z < 3` (fogNear 998..999) ou `{1000, 1000}` = sem névoa útil, só borda (d = far, k no limite longo). Sem o bit 16 o shader não lê `clock.zw` e nada muda em relação à 0.7.7.
- **Custo**: a escala sai de um `#define` que reexpandia `sqrt` e divisões em cada uso e passa a ser calculada **uma vez por invocação** (global `policroma_k`, inicializada no começo do `main`); com k fixo > 0 o ramo automático nem executa. O valor com k > 0 não muda (o `--pack` do Complementary com `--fog-divisor 32` sai idêntico ao da 0.7.7).
- Locale: a DLL usa CRT própria (`/MT`), então o `setlocale` do SoH (pt-BR) não chega ao `snprintf` de `GlslFloat` nem ao `%.1f` do cloudHeight; nada mudou aí.
- Testes: `policroma_iris_tests` ganhou os casos da rampa e pula (SKIP) o bloco do `IrisMaterial.zip` quando o caminho do zip não é passado, com `fs::exists` antes de ler.

## Novidades da 0.7.7

- **Escala da névoa automática**: `iris.fog_divisor = 0` (slider "Escala da névoa (1/N; 0 = automático)", 0 a 64) faz o GLSL gerado calcular k a cada quadro pela névoa nativa da cena (`PolicromaFog.w` início, `PolicromaIrisInfo.w` fim, `PolicromaCameraInfo.xy` zNear/zFar): o d50 nativo é `2·f·S/(2S + f − n)` e k = `sqrt(3610,1·f/d50³)` (ou `88,51/d50` quando `f·k < 192`), limitado a 1/64..1/4. Kokiri dá ~1/21,3; Hyrule Field e Kakariko ~1/31,7. Interiores e cenas de névoa curta caem no limite. Vale para névoa vanilla; com `worldFog` o k sai errado.
- Qualquer divisor maior que 0 se comporta como na 0.7.6 (o shader usa o k do menu). O padrão continua **32**; o provider aceita `fog_scale=0` (automático) e recusa 0 < k < 1/256.

## Novidades da 0.7.6

- Novos padrões da névoa, validados em jogo (Kokiri, Hyrule Field e Kakariko, Complementary Unbound r5.9.3): **Escala da névoa** `iris.fog_divisor` = **32** (k = 1/32; antes 1), **Altura do chão** `iris.ground_y` = **0** e **Altura das nuvens** `iris.cloud_height` = **3000**. Valem só quando o armazenamento do mod não tem a chave: o que o jogador já gravou continua valendo. A ação **Névoa padrão (1/32)** apaga as três chaves de névoa e volta a esses padrões; para a névoa sem escala, ponha a escala em 1.
- **Normais reconstruídas (F0)** (`iris.gbuffer_f0`) continua **desligada** por padrão.
- Os limites dos controles não mudam (escala 1 a 64, chão -2000 a 8000, nuvens -1000 a 8000; o provider aceita k de 1/256 a 4).

## Novidades da 0.7.5

- Opção de menu **Normais reconstruídas (F0)** (`iris.gbuffer_f0`, padrão **desligada**; recarrega o pack ativo). Ligada, e só na ponte V3, o provider injeta um programa sintético antes do primeiro do pack que reconstrói a normal de mundo pela profundidade e preenche `colortex4` (normal crua, Y para cima), `colortex6` (smoothness, materialMask, skyLight) e `colortex3` (zero), **apenas os que algum programa do pack lê antes de qualquer escrita** (Complementary: `colortex4` e `colortex6`). Sem leitor, ou desligada, o grafo é idêntico ao da 0.7.4. O texto do passe está em `GbufferF0.h`.
- Piso quase horizontal reflete (`smoothness` 0,75 por `smoothstep` da normal.y; também a água, que o F0 não distingue); parede e ladeira não. `skyLight` = `eyeBrightness`/240 do provider (vetor 11 `.w`), 1 sem parâmetros.
- Bordas de profundidade (e vinco+degrau, por uma checagem na diagonal) voltam a normal para a direção do olho: fresnel 0, sem reflexo nem contorno na silhueta.
- `policroma_iris_tests --pack <pasta> [--gbuffer-f0]`.

## Novidades da 0.7.4

- Calibração da névoa dos packs Iris (vetor 11 do `LinkSpanIris`, só no GLSL gerado): **Escala da névoa** (k = 1/N blocos por unidade do OoT; `near`, `far`, `fogStart`, `fogEnd`, posições da câmera, `cloudHeight` e as projeções passam a valer em blocos), **Altura do chão** (leva o chão da cena ao bloco 64; 0 = sem deslocamento) e **Luz do céu (caverna)** (`eyeBrightness`, 240 = ar livre). Os padrões (N = 1, chão 0, 240) reproduzem a saída da 0.7.3.
- `policroma_iris_tests --pack <pasta> [--fog-divisor N] [--ground-y Y]` roda o grafo com esses parâmetros ao vivo.

## Novidades da 0.7.3

- Catálogo nativo v3: `shaders/linkspan-v2.json` com `"version": 3` aceita até 16 passes HLSL de tela cheia. A versão 2 continua limitada a 8 passes. Na v3 só valem passes `.hlsl` sem vertex shader próprio.
- Os PNGs declarados em `textures` (de 1 a 6, até 1 MiB cada, assinatura PNG verificada) são lidos do ZIP antes do parser compartilhado, para que a ponte V3 os receba como includes.
- Entrada `scene.material.normal` nos passes HLSL v3, ao lado de `scene.material.specular` (ver "Materiais do OoT nos passes Iris").
- Testes nativos: o alvo `policroma_native_tests` (`ctest`: `policroma_native`) cobre o manifesto do catálogo v2/v3 e compila o shader de material com as entradas `MaterialNormal`, `MaterialSpecular` e `SceneDepth`. O `policroma_iris` agora também recebe `IrisMaterial.zip`, gerado pelo CMake a partir de `packs/IrisMaterial`.
- Pack de exemplo `packs/IrisMaterial`: reserva dois `colortex` para o buffer de material do OoT.
- A biblioteca estática `policroma_catalog` (Catalog, GlslFragment, IrisPack) é compartilhada entre o provider e os testes.

## Fragmentos GLSL em DirectX 11

Um passe v2 pode apontar para um `.fsh` em `shaders/`. O provider traduz GLSL 450 para SPIR-V e HLSL em memória. O host compila o resultado para DX11 antes de trocar o grafo ativo. Um pack pode combinar passes `.fsh` e `.hlsl`.

- Declare uma saída `vec4` em `layout(location=0)`. A entrada opcional `vec2` no mesmo location recebe UV com origem no canto superior esquerdo.
- Use texturas 2D de ponto flutuante em `layout(binding=N)`, com `N` igual ao índice em `inputs`. Use um `sampler2D` combinado ou texturas separadas com um `sampler` compartilhado em `binding=4`.
- Os includes `.glsl` são lidos do próprio ZIP. Caminhos iniciados por `/` partem de `shaders/`. As definições de perfil são aplicadas antes da compilação.
- O contrato atual cobre composição de tela cheia com um alvo de cor por passe. Vertex shaders, buffers de uniforms do Iris, compute e múltiplas saídas por passe são etapas futuras.

```powershell
python build_preset.py --glsl Policroma-GLSL.zip
```

## Build do provider

Use o SDK portátil OoT correspondente ao host, libzip e nlohmann-json para Windows x64. `POLICROMA_BRIDGE_DIR` aponta para a pasta que contém `PolicromaBridge.h` e `PassSource.h` no checkout do host. Forneça os fontes do glslang 15.1.0 (`1062752a891c95b2bfeed9e356562d88f9df84ac`) e SPIRV-Cross vulkan-sdk-1.4.304.0 (`ebe2aa0cd80f5eb5cd8a605da604cacf72205f3b`). As duas bibliotecas são ligadas estaticamente.

```powershell
cmake -S . -B build -A x64 `
  "-DOOT_NATIVE_SDK=C:/sdk/OotNativeSdk.cmake" `
  "-DPOLICROMA_BRIDGE_DIR=D:/host/soh/soh/Enhancements/Graphics/ShaderPacks" `
  "-DPOLICROMA_GLSLANG_DIR=D:/deps/glslang" `
  "-DPOLICROMA_SPIRV_CROSS_DIR=D:/deps/spirv-cross"
cmake --build build --config Release
```

Empacote `build/mod` com o `tools/package-linkspan-mod.ps1` do host. O pacote inclui o provider e a interface Lua.
