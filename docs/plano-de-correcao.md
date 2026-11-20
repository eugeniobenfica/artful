# Plano de correção e refinamento, em fases

**19 de novembro de 2026** · deriva de [relatorio-versao-estavel-e-refinamento.md](relatorio-versao-estavel-e-refinamento.md) e de [relatorio-tecnico-estabilidade.md](relatorio-tecnico-estabilidade.md).

Cada item diz **o que muda**, **onde** e **quando está pronto**. Os tamanhos são estimativas: P (dias), M (semanas), G (meses).

## Princípios que ordenam as fases

1. **Primeiro o instrumento, depois a mudança.** Toda correção posterior altera imagens. Sem uma métrica perceptual e sem testes que digam o que foi omitido, não há como distinguir regressão de melhoria. Por isso a Fase 0 vem antes de qualquer algoritmo.
2. **Mudanças que alteram bytes vão juntas.** O projeto fixa hashes de imagem e a igualdade entre kernels. sRGB exato, nova sequência de amostragem, compensação de energia e antialiasing especular mudam os bytes de quase todo quadro. Agrupá-las numa janela (Fase 2) permite regenerar as referências uma vez, revisando a diferença com FLIP, em vez de quatro.
3. **O contrato congela depois das mudanças de comportamento visível, e antes dos sistemas grandes.** A 1.0 sai ao fim da Fase 3. Geometria virtualizada, luzes estocásticas e backends novos entram em 1.x como recursos opcionais.
4. **Arquitetura antes de crescer.** O IR de shaders e o grafo de quadro (Fase 4) vêm antes de DAG, luzes estocásticas e Metal (Fases 5 e 6). Cada um desses toca `shaders.c` (4.781 linhas) e `vulkan.c` (5.171) em muitos pontos; fazê-los antes da reorganização multiplica o trabalho de migração.
5. **Toda técnica nova entra atrás de uma opção, com A/B reproduzível.** Só vira padrão depois de comparada em custo e imagem nos dois backends.

```
Fase 0 ─► Fase 1 ─► Fase 2 ─► Fase 3 ═► 1.0
 base      contrato   bytes     robustez
                                   │
                                   ▼
                     Fase 4 ─► Fase 5 ─► Fase 6
                     arquitetura conteúdo  plataformas
```

A Fase 3 (robustez) pode correr em paralelo com o fim da Fase 2: são arquivos diferentes (ferramentas, CI, `vulkan.c` de apresentação).

---

## Fase 0 — Base reproduzível e instrumentos

Sem esta fase, nenhuma das outras tem critério de pronto verificável.

| # | O que muda | Onde | Pronto quando | Tam. |
|---|---|---|---|---|
| 0.1 | Versionar o trabalho em andamento: biblioteca 2D, testes e ferramenta (hoje não rastreados), mais as mudanças locais em CMake e testes. | `include/artful/2d.h`, `src/2d/`, `tests/twod.c`, `tools/render2d.c`, `CMakeLists.txt` | Um checkout limpo compila com Vulkan ligado e desligado e passa os testes. Um consumidor externo usa `find_package(artful)` com `artful::artful` e `artful::2d`. | P |
| 0.2 | Um teste do CTest por caso. O executor já tem `--list` e filtro por nome (`tests/main.c`). Gerar a lista após o build, como faz `gtest_discover_tests`, e registrar cada nome. | `CMakeLists.txt`, script CMake novo | `ctest` mostra os ~200 casos individualmente. | P |
| 0.3 | Distinguir **omitido** de **aprovado**. Casos que retornam cedo por falta de Vulkan, janela ou ray tracing passam a sair com um código de omissão, mapeado em `SKIP_RETURN_CODE`. Uma variável `ARTFUL_REQUIRE=vulkan,window,rt` transforma omissão em falha para perfis certificados. | `tests/common.c`, `tests/tests.h`, os casos que retornam cedo | Num build sem Vulkan, o CTest lista os casos de GPU como *skipped*. Com `ARTFUL_REQUIRE=vulkan`, falham. | P |
| 0.4 | Portar o FLIP (LDR e HDR) para C, como ferramenta de teste, e trocar as comparações "N% de pixels acima de 24/255" por limiares FLIP por caso. Manter o critério antigo em paralelo até calibrar. | `tests/flip.c` (novo), `tests/common.c` | Os resultados batem com a referência C++ do FLIP numa amostra de imagens (erro médio dentro de 1e-4). Cada comparação CPU×GPU registra o FLIP médio e o percentil 99. | M |
| 0.5 | Linha de base de desempenho e imagem: sequências determinísticas (cena aberta, cidade ocluída, personagem, transparência, luzes densas, sala 2D), com p50/p95/p99, primeiro quadro, memória e tempo por passe. Guardar como JSON versionado. | `tools/render.c` (modo `--bench --json`), `docs/` ou repositório à parte | Duas execuções seguidas na mesma máquina ficam dentro de 3% no p50. Existe um script que compara duas execuções e aponta regressões. | M |

### Estado da Fase 0 (19/11/2026)

| # | Estado | Evidência |
|---|---|---|
| 0.1 | **Feito** (commits `7dc3fb0` e `0777f31`). | Uma cópia com apenas os arquivos versionáveis (rastreados + não rastreados não ignorados, sem `.claude/`) compila sem avisos com Vulkan ligado e desligado (GCC/MinGW, Ninja, Release). Os 157 testes passam nas duas. Instalada, a cópia atende a consumidores externos de `artful::artful` e `artful::2d` via `find_package`; os dois desenham. |
| 0.2 | **Feito.** | `cmake/artfulTests.cmake.in` pergunta `artful-tests --list` quando o CTest roda e registra cada caso com `--exact`. São 157 testes; `ctest -j4` leva 36 s, contra 64 s do teste único. |
| 0.3 | **Feito.** | `missing()` em `tests/main.c`. Os renderizadores de teste passam por invólucros que registram Vulkan ausente; janela e ray tracing por hardware são marcados onde os testes já os detectavam. Sem driver Vulkan, 83 testes saem como *skipped*. Com `ARTFUL_NO_RAY_QUERY=1`, 4 saem por falta de `rt`. Com `ARTFUL_REQUIRE=vulkan`, falham. |
| 0.4 | **Parcial.** | O FLIP LDR e HDR está em `tools/flip.c`, escrito a partir do `FLIP.h` da NVIDIA (BSD-3), com a ferramenta `artful-flip`. O teste `metrics/flip` confere as propriedades do método. Cada `differentShare` grava o FLIP ao lado em `ARTFUL_FLIP_LOG`. **Falta:** comparar com o programa da NVIDIA (o critério de ±1e-4 não foi verificado) e trocar os limiares por limiares FLIP, o que exige primeiro coletar o log em várias máquinas. |
| 0.5 | **Feito, exceto o 2D.** | `artful-render --json` e `tools/bench.py` (`run`, `compare`). Na RX 9060 XT, 1080p, 600 quadros, três execuções: `city` com p50 dentro de 0,2% e `city` com oclusão dentro de 0,1%. O p99 varia até ~50% por picos esporádicos, e por isso o comparador usa 25% para as caudas. O `artful-render2d` ainda não escreve JSON. |

Não executados nesta etapa: sanitizadores, MSVC e Linux nas mudanças de teste. A CI continua chamando `artful-tests` diretamente, sem `ctest`.

---

## Fase 1 — Contrato público

O que precisa estar decidido antes de congelar a API. Nenhum item aqui muda imagens.

| # | O que muda | Onde | Pronto quando | Tam. |
|---|---|---|---|---|
| 1.1 | **Handles de malha com geração**, como as texturas já têm: índice nos bits baixos e geração nos altos. A posição de uma malha removida volta a uma lista livre. As verificações hoje feitas em `scene.c:204` e `:795` (`mesh > s->meshes.len \|\| removed`) passam a comparar a geração. | `src/scene/scene.c`, `sceneInternal.h`, usos de `ArtfMesh` nos backends | Um teste adiciona e remove um milhão de malhas: o número de posições não passa do pico simultâneo, e um handle antigo é rejeitado com log. | M |
| 1.2 | **Falta de memória na GPU.** RAM continua abortando. Para VRAM: ao falhar uma alocação, o renderizador derruba mipmaps e níveis não essenciais pela política de residência, tenta de novo e, se ainda falhar, recusa o recurso (textura branca, malha não desenhada) e avisa por callback. | `src/vulkan/vulkan.c` (alocador de spans), residência em `scene.c` | Um teste com orçamento artificialmente baixo mantém quadros válidos e emite o aviso, sem `VK_ERROR_OUT_OF_DEVICE_MEMORY` sem tratamento. | M |
| 1.3 | **Callback de eventos**, além do log de texto: perda de dispositivo, VRAM esgotada, recurso recusado, variante de shader pronta. Estrutura com tipo e dados, para a aplicação reagir sem analisar strings. | `include/artful/core.h`, `src/core/base.c` | Os eventos de 1.2 e 3.4 chegam pela callback nos testes. | P |
| 1.4 | **Decisão sobre ABI**, por escrito. Recomendação: prometer compatibilidade **de fonte** em 1.x e não prometer ABI binária, como Filament e wgpu. Se a decisão for prometer ABI: biblioteca dinâmica com SONAME, macro de exportação com `visibility("hidden")` por padrão, e campo de tamanho no início de cada struct de opções. | `docs/api.md`, `CMakeLists.txt`, cabeçalhos | O documento existe e o CMake corresponde a ele. Se houver ABI, um teste compila um consumidor contra os cabeçalhos da versão N e liga contra a N+1. | P a M |
| 1.5 | **Contrato de threads**, mantendo a regra atual mas tornando-a verificável: em build de depuração, a cena registra a thread dona e afirma em cada chamada. | `src/scene/scene.c`, `src/render/renderer.c` | Um teste de depuração que chama a cena de outra thread durante um `artfRendererDraw` é detectado. | P |
| 1.6 | **Camadas de suporte publicadas**: estável, melhor esforço e experimental, por backend × plataforma × recurso. Metal, Windows on ARM, armv7 e x86-64 Android entram como experimentais até rodarem. | `docs/stability.md`, `README.md` | Cada célula aponta para o teste ou máquina que a sustenta. | P |
| 1.7 | **Mistura de clipes** na biblioteca: dois clipes com peso, e camadas por máscara de ossos. Hoje `artfAnimationSet` só amostra um clipe e todo integrador reescreve a mistura. | `src/scene/gltf.c`, `include/artful/gltf.h` | Um teste compara a mistura a uma referência calculada à mão, e a mistura é igual em CPU e GPU. | P |


### Estado da Fase 1 (19/11/2026)

| # | Estado | Evidência e diferenças em relação ao planejado |
|---|---|---|
| 1.1 | **Feito para malhas.** | As posições já eram reaproveitadas; o defeito real era outro: um handle antigo passava a apontar para a malha nova na mesma posição. Agora o byte alto é a geração da posição, num contador próprio (`place`), separado do `generation` que a residência usa para a GPU. O teste `streaming/meshHandles` faz 20.000 ciclos de adicionar e remover (não um milhão) e confirma que a cena não cresce e que o handle antigo é rejeitado com log. **Ao contrário do que o plano supunha, as texturas não têm geração no handle**; isso continua pendente e está registrado em `limits.md`. |
| 1.2 | **Feito para texturas; o resto apenas avisa.** | A textura sobe sem os mipmaps maiores, ou fica branca com `ARTF_EVENT_REFUSED`, e o quadro segue (`backends/textureMemory`, com `artfVulkanTextureMemoryLimit` simulando a GPU cheia). Para malhas, instâncias e buffers do quadro, a falha continua derrubando o renderizador, agora com `ARTF_EVENT_OUT_OF_MEMORY` antes. Também não aplicado: liberar mipmaps de *outras* texturas pela residência antes de recusar. |
| 1.3 | **Feito.** | `artfSetEvents`, com `ArtfEvent` tipado. Testes em `backends/lostDevice` (um único evento, vindo do renderizador perdido) e `backends/shadersAhead` (shaders prontos). O evento de 3.4 (`device_fault`) entra junto com o item 3.4. |
| 1.4 | **Rascunho escrito; falta a decisão do dono.** | A seção "What 1.x will keep (proposed)" em `docs/api.md` propõe compatibilidade de fonte sem ABI binária. O CMake já corresponde: a biblioteca é só estática. |
| 1.5 | **Feito, sempre ligado.** | Não só em depuração: o custo é uma comparação por chamada. A cena guarda a thread que a desenha; mudanças vindas de outra thread durante o desenho vão para o log (`core/threadRule`). A leitura é sem trava: serve para verificar a regra, sem garantir que a pegue sempre. O 2D não tem essa verificação. |
| 1.6 | **Feito.** | Tabela de camadas em `docs/stability.md`, célula a célula pelo que rodou. O 2D fica em "melhor esforço" fora do Windows, porque ainda não passou pela CI. |
| 1.7 | **Feito, sem a parte de GPU.** | `artfAnimationMix`: pesos, quaternions normalizados pelo menor caminho e máscara por nó nomeado (`only`). O teste `motion/clips` confere contra valores calculados à mão. "Igual em CPU e GPU" não se aplica, porque a pose é calculada na CPU para os dois backends. |

Verificação: os 160 testes passam com Vulkan; sem Vulkan, 152.393 verificações e 84 omissões. Os testes novos passam limpos sob ASan/UBSan (clang) e sem avisos. MSVC e Linux não foram executados.

---

## Fase 2 — Janela de mudanças que alteram bytes

Tudo aqui muda pixels. Executar em sequência, regenerar as referências e os hashes **uma vez** no fim, com a diferença revisada pelo FLIP da Fase 0. Cada item entra primeiro atrás de uma opção, para A/B, e vira padrão ao fechar a fase.

### 2.1 Cor (P a M)

- **sRGB exato** no lugar da gama 2. Hoje texturas são elevadas ao quadrado na entrada e o quadro guarda a raiz (`kernels.inc:1522`, `:2285`, `gltf.c:644`, e o equivalente nos shaders). Na GPU, usar formatos `_SRGB` para textura e alvo faz a conversão em hardware. Na CPU, usar tabela de 256 entradas na entrada e, na saída, uma aproximação racional com arredondamento fixo, igual em todos os kernels.
- **Gradação em ACEScct** (lift, gamma, gain, rodas, LUT) em vez do espaço de exibição. É um `log2` com pé linear; `artfLog2Fast` já existe.
- **Curva Khronos PBR Neutral** como opção de tom, e LUT 3D para ACES 2.0 ou Tony McMapface.
- **Pronto quando:** um quadro de cinza médio (18%) sai no valor sRGB esperado ±1; uma LUT `.cube` feita no DaVinci Resolve reproduz a imagem de referência com FLIP abaixo do limiar; CPU e GPU continuam dentro da tolerância.
- **Risco:** a codificação em gama 2 também serve para compor sobre o Cascade e na sobreposição. Verificar a composição premultiplicada antes de trocar.

### 2.2 Amostragem (M)

Um arquivo novo, `src/shading/sampling.inc`, incluído nos mesmos contextos que `shading.inc`: C escalar, oito pistas e SPIR-V. A sequência fica escrita uma vez.

- **Sobol com embaralhamento de Owen por hash** (Burley 2020), só com inteiros, no lugar de `hashOf` + `randomInBall` (11 usos em `trace.c` e `shaders.c`; `particles.c` mantém o hash, que lá dá a cada partícula seus números a partir do nascimento e precisa continuar igual entre CPU e GPU).
- **Embaralhamento por pixel com ruído azul** (Heitz 2019 ou máscaras FAST), numa tabela pequena embutida no código, sem arquivo.
- Direções por mapeamento de quadrado para esfera, sem rejeição. Mantém a regra de não usar `sin`/`cos` da libc.
- **Pronto quando:** o traçado converge para a mesma imagem de 512 amostras; com 4 amostras, o erro contra ela cai (meta: pelo menos 30% menos FLIP); CPU e GPU continuam iguais à tolerância atual; os kernels seguem com bytes iguais.

### 2.3 Integrador (M)

- **Amostragem do ambiente por importância:** distribuição 2D por partes sobre o mapa octaédrico 0, com a variante compensada do PBRT para MIS. A CDF é construída uma vez em `environment.c`, com busca por inteiros.
- **VNDF por calotas esféricas** (Dupuy e Benyoub 2023) para o lóbulo GGX, e escolha difuso/especular pela energia de cada lóbulo, não ao acaso.
- **MIS** entre a amostra de luz e a da BSDF, para o sol com tamanho, as luzes com tamanho e o ambiente.
- **Cones de raio** para o mipmap das texturas no traçado. Hoje lê sempre o maior.
- **Pronto quando:** um HDRI com sol pequeno deixa de produzir vaga-lumes (contagem de pixels acima de 10× a média da vizinhança cai para perto de zero com 16 amostras); a energia média da imagem convergida não muda além de 0,5%; o tempo por amostra sobe no máximo 15%.

### 2.4 Materiais (P)

- **Compensação de espalhamento múltiplo** (Turquin): um fator 1 + F0·(1/E − 1) por uma tabela de E(μ, rugosidade), em `shading.inc`.
- **Antialiasing especular geométrico** (Tokuyoshi e Kaplanyan): alarga a rugosidade pelas derivadas da normal. A CPU já calcula derivadas entre pistas para o mipmap; a GPU tem `dFdx`/`dFdy`.
- **Pronto quando:** uma "fornalha branca" (esfera metálica rugosa num ambiente uniforme) devolve ≥ 98% da energia; uma grade de normal map fino cintila menos com a câmera parada (variação temporal medida).

**Ao fechar a Fase 2:** regenerar hashes e referências, revisar cada diferença acima do limiar, e registrar no CHANGELOG que os bytes mudaram e por quê.


### Estado da Fase 2 (19/11/2026)

| # | Estado | Evidência e diferenças em relação ao planejado |
|---|---|---|
| 2.1 sRGB exato | **Feito.** | Mudou o caminho. A tabela de 256 entradas + busca binária não cabia porque vários pontos decodificam valores já filtrados, que não são bytes. Polinômios via log2/exp2 custaram +45% na CPU. Tabelas de 4.096 passos foram piores, porque os *gathers* são lentos neste Zen. Ficaram dois polinômios ajustados: grau 6 no valor exibido e grau 5 na raiz quarta da luz. Erro abaixo de 8e-6, todo byte volta a si mesmo, sem *gather* e sem `pow`, e a mesma conta em C, nas pistas e nos shaders. Custo: CPU +14% em `spheres` (uma thread), +7% em `heavy`, −2,5% em `outdoors` em floats (a codificação final foi para 8 pistas); GPU +9% e +1%. A filtragem de texturas continua sendo feita em bytes, antes de virar luz, como antes; filtrar em luz exigiria formatos `_SRGB` e classificar texturas de cor e de dados. |
| 2.1 Curvas de tom | **Já existia.** | Khronos PBR Neutral (`ARTF_TONE_NEUTRAL`) já estava implementada. Uma LUT ACES 2.0/Tony fica para quem quiser carregar via `.cube`. |
| 2.1 Gradação em ACEScct | **Pendente.** | Muda o significado de lift/gamma/gain e do domínio das LUTs; precisa de decisão sobre compatibilidade das LUTs existentes. |
| Revisão das referências | **Feito.** | Os hashes de `core/pictures`, `views/orthographic` e `post/temporalBytes` foram regenerados. FLIP antes×depois: de 0,042 (`spheres`) a 0,13 (`outdoors`), igual nos dois backends. A concordância CPU×GPU ficou como estava (`spheres` 0,0225 → 0,0222). |
| 2.2 Sobol com Owen | **Feito.** | Câmera, ponto da bola e escolha do lóbulo vêm de um Sobol 4D por evento, com embaralhamento de Owen por pixel (`scene/sampling.h`), e a mesma conta inteira no shader de traçado. A direção é gerada sem rejeição. Com 4 amostras, o erro contra 512 caiu de 2,21 para 1,43 (−35%; meta −30%). FLIP contra 1.024 amostras: −24% com 4 amostras e −40% com 16. Custo: +11% por amostra na CPU, nada na GPU. O teste do denoiser passou a exigir um limite absoluto, porque o ruído de partida ficou menor. |
| 2.2 Ruído azul entre pixels | **Pendente.** | Heitz 2019 ou FAST: uma tabela pequena embutida que decide o embaralhamento por pixel. |
| 2.2 Probes | **Mantidas no hash.** | A GPU coleta as probes na mesma ordem da CPU; trocar exige mudar os dois lados e o teste `tracing/probesGpu`. |
| 2.3 Ambiente por importância + MIS | **Feito para o lóbulo difuso.** | As tabelas (CDF marginal e condicional sobre o mapa octaédrico, pesadas por luminância × ângulo sólido) ficam no fim do buffer do ambiente, e a GPU as lê sem binding novo. O NEE cobre o difuso, com MIS por heurística da potência contra o salto difuso, que agora é cosseno exato. Os saltos especulares continuam pegando o ambiente sem peso, o que mantém o estimador sem viés. Com sol 40 e 16 amostras: RMSE de 0,041 para 0,020 e pixels quentes de 169 para 1. Convergido: a mesma luz, menos 0,5% de viés do salto difuso antigo. Fornalha exata nos dois backends. Diferença em relação ao plano: o critério de "energia não muda além de 0,5%" não vale para sóis acima do corte de 64 por caminho, porque o estimador antigo perdia essa energia; esses casos ficam mais claros, corretamente. |
| 2.3 VNDF + MIS especular | **Feito para o ambiente.** | O salto especular amostra normais visíveis da GGX (calotas esféricas) com peso m.mirror × G2/G1, e é calculado só quando escolhido. O raio mirado no ambiente traz também o termo especular, com MIS por lóbulo. Sem viés: mira e salto puro diferem 0,12% em 768 amostras, e com 16 amostras o ruído cai 6,6×. CPU×GPU: RMSE 0,0006 sob sol. Na GPU, o raio mirado virou uma volta a mais do laço de luzes. Custo na GPU: 2,9 → 4,3 ms por amostra em `spheres`, dos quais ≈0,9 ms vêm dos caminhos mais longos do cosseno correto. Ainda pendente: MIS para luzes com tamanho (sol e lâmpadas continuam só por NEE). |
| 2.3 Cones de raio | **Feito.** | Cone por caminho (largura + abertura, alargado por salto) e nível de mipmap trilinear. A GPU obtém tamanho e níveis da textura por `OpImageQuerySizeLod`/`OpImageQueryLevels`. Com 1 amostra, o erro contra 256 cai de 0,25 para 0,054; CPU = GPU. Custo: +15% por amostra na GPU, nada na CPU. Expôs um erro antigo: mipmaps eram médias de bytes sRGB. **Decisão (pedida ao dono, que deixou a escolha comigo):** o papel de cada textura é inferido dos materiais, com salvaguardas. Dado vence quando a textura é usada dos dois modos, com aviso no log. O loader de glTF prepara as imagens já no papel certo, fora da thread principal. Mipmaps de KTX2 e de streaming não são refeitos. Comparado ao flag na criação: não muda a API e acerta texturas criadas à mão, mas o primeiro desenho refaz os mipmaps das texturas criadas fora do glTF. A filtragem entre texels continua em bytes nos dois backends, para manter a paridade sem formatos `_SRGB`. |
| 2.4 Materiais | **Feito** (compensação e antialiasing especular; iridescência de Belcour-Barla, camadas OpenPBR e SSS ficam para depois). | **Compensação de Turquin**: cada lobo é multiplicado por 1 + F0·(1/E − 1). E é o albedo do GGX com Smith correlacionado por altura. Em vez de tabela, um ajuste por mínimos quadrados de 30 coeficientes em x = n·v/(n·v + α·sen) e rugosidade, com erro ≤ 0,8%; serve igual em lanes, no tracer escalar e em SPIR-V. **Fornalha branca**: metal branco sob luz uniforme devolve 0,994–1,002 na CPU e na GPU (antes, 0,307 com rugosidade 1 e 0,916 com 0,5). No rasterizador a `Mirror` do ambiente não é compensada, porque para metal branco já dava 1. **Antialiasing especular**: α² cresce 2σ², limitado a 0,18 (Tokuyoshi-Kaplanyan). σ² soma a variação geométrica da normal (`dFdx` na GPU; regra do quociente sobre as baricêntricas na CPU, por pixel) e a de normal map (Toksvig, pelo encurtamento da média dos texels). Onde nada varia, a rugosidade fica idêntica ao bit. Num piso com normal map fino deslocado por ¼ de pixel, a cintilação cai de 0,0026 para 0,0009. A luz média fica 0,0144 contra 0,0213 de uma referência 8× supersampled (a cauda do GGX é mais longa que a distribuição desse mapa); sem alargar, fica 0,0252. Custo: nenhum mensurável nos dois backends. Hashes refeitos após FLIP contra o commit anterior: média 0,001–0,007 por cena, máximo 0,61 nas esferas metálicas de `surfaces`. |

---

## Fase 3 — Robustez e prova (fecha a 1.0)

Pode começar durante a Fase 2.

| # | O que muda | Onde | Pronto quando | Tam. |
|---|---|---|---|---|
| 3.1 | **Fuzzing guiado por cobertura**: um alvo libFuzzer por leitor (glTF/GLB, PNG, JPEG, HDR, KTX2 e blocos, IES, meshopt), sob ASan e UBSan, com corpus semente e dicionário de JSON. | `tools/fuzz/` (novo), alvo CMake opcional | Cada alvo roda 24 horas sem achado novo. Os achados viram casos de regressão em `tests/files.c`. | M |
| 3.2 | **Corpus de conformidade**: a lista "Testing" do glTF-Sample-Assets e arquivos gerados pelo `gltfpack` com meshopt. Cada um carrega, e os principais têm imagem de referência. | `tests/files.c`, script de download para CI | Todos carregam; nenhuma falha silenciosa; o que não é suportado é logado. | M |
| 3.3 | **Pré-rotação no Android.** Hoje, com `upright`, o swapchain recebe transformação identidade com os lados trocados, e o compositor gira (`vulkan.c:703`, `:723`). Passar a usar `currentTransform`, manter a extensão na orientação nativa e girar o espaço de recorte na projeção. Vale para a cena, a sobreposição e os passes em tela; os efeitos que dependem de direção de tela (motion vectors, TAA) giram com ela. Recriar ao receber `VK_SUBOPTIMAL_KHR`. | `src/vulkan/vulkan.c`, `src/scene/views.c` ou onde a projeção é montada, `src/2d/vulkan2d.c` | Num aparelho girado nas quatro orientações: a imagem é a mesma do retrato girada; o tempo de quadro não sobe na paisagem. | M |
| 3.4 | **Perda de dispositivo diagnosticável:** habilitar `VK_EXT_device_fault` quando existir e registrar o relatório pela callback de 1.3. | `src/vulkan/vulkan.c` | Com a falha injetada, o evento chega; num aparelho com a extensão, o relatório é lido. | P |
| 3.5 | **Probes persistidas na cena**: a GPU copia o estado de volta periodicamente ou ao perder o dispositivo, e o renderizador novo continua dele. Hoje recomeçam do zero. | `src/scene/trace.c`, `src/vulkan/vulkan.c` | Após recriar o renderizador, a GI do primeiro quadro difere da anterior abaixo do limiar FLIP. | P |
| 3.6 | **Ganchos de ritmo e térmica**: `renderScale` dinâmico por orçamento de tempo de quadro, e um campo para a aplicação informar folga térmica (vinda da ADPF ou similar). A biblioteca não depende da ADPF; só expõe o controle. Medir aqui os workers que giram em espera. | `include/artful/renderer.h`, `src/software/software.c`, `src/core/threads.c` | Num telefone, em 30 minutos de carga constante, o tempo de quadro fica dentro do alvo enquanto a resolução interna se ajusta; o consumo com e sem a espera ativa está medido. | M |
| 3.7 | **Hardware real na CI**: um runner próprio com GPU AMD (o desktop atual) e um laço de jogo no Firebase Test Lab em dois ou três telefones. Lista de testes instáveis por plataforma, com meta de no máximo uma falha espúria por semana. | `.github/workflows/` | Cada PR roda os testes de GPU com `ARTFUL_REQUIRE=vulkan,rt` no runner; os telefones rodam diariamente. | M |
| 3.8 | **Ensaio de longa duração**: quatro horas de troca contínua de ativos com orçamento fixo, registrando pico e tendência de RAM e VRAM, handles e alocações restantes. | `tools/soak.c` (novo) | Sem tendência de crescimento na última hora; zero alocações ao encerrar. | P |
| 3.9 | **Linux com GPU e janela; Mac com o backend de CPU.** É o mínimo para as camadas de 1.6 deixarem de dizer "nunca executado" nas plataformas que a 1.0 anunciar. | máquinas | Tabela de 1.6 preenchida com a execução. | P (com acesso) |

### Estado da Fase 3

| Passo | Estado | Notas |
|---|---|---|
| 3.1 Fuzzing | **Feito em parte**: alvos, sementes, dicionário e regressões; falta a CI e as 24 h por alvo. | Oito alvos libFuzzer sob ASan e UBSan. No MinGW, o clang não aceita `-fsanitize=fuzzer`; o build pede a cobertura e liga a libFuzzer pelo nome. Três rodadas de 15 min nos oito alvos acharam 5 defeitos, todos corrigidos e mantidos em `files/fuzzFindings` (minimizados pela libFuzzer): estouro de heap em `placeNode` (nó com pele e primitivas que não são objetos); overflow do preditor DC do JPEG; contagem de inclinação do IES sem validação; KTX2 de 64 bytes reservando 4 GB; e um nó filho de si mesmo, posto 2^256 vezes (agora cada nó entra uma vez, com aviso no log). |
| 3.2 Corpus de conformidade | **Não iniciado.** | Precisa baixar o glTF-Sample-Assets: pedir autorização antes. |
| 3.3 Pré-rotação no Android | **Não iniciado.** | Precisa do aparelho. |
| 3.4 `VK_EXT_device_fault` | **Feito.** | Habilitado quando o driver tem `deviceFault`. Na perda, o relatório (descrição, endereços, códigos do fabricante) vai ao log e à mensagem de `ARTF_EVENT_DEVICE_LOST`. A Radeon RX 9060 XT tem a extensão; com a perda injetada, o relatório é lido (vem sem descrição, como esperado). Uma perda real não foi forçada: um TDR nesta máquina afetaria a área de trabalho. |
| 3.5 Probes persistidas | **Feito.** | A cada 30 passos, a GPU copia as probes para a cena (no próprio quadro; lidas quando a cerca termina). Um renderizador novo parte delas e do passo em que estavam: seu primeiro quadro é idêntico (FLIP 0,0000) ao quadro seguinte do anterior. Do zero, a diferença é 0,086. CPU e Vulkan continuam um do outro. |
| 3.6 Ritmo e térmica | **Não iniciado.** | O critério exige um telefone sob carga por 30 min. |
| 3.7 Hardware na CI | **Não iniciado.** | Depende de runner próprio e do Firebase Test Lab. |
| 3.8 Ensaio longo | **Feito em parte**: ferramenta pronta; a execução de 4 h foi interrompida pela suspensão do Windows aos 73 min. | Nos 73 min (202 mil ativos criados e removidos), nada cresceu: processo −14 MB/h, GPU +0,8 MB/h (limite: 1 MB/h); picos de 90 MB e 32 MB. A conferência final de alocações não chegou a rodar; em execuções curtas, zero alocações restantes. Falta repetir as 4 h com a suspensão desligada. `artful-soak` cria e remove malhas e texturas sob orçamento, a 60 passos/s, e registra RAM, VRAM (`ArtfFrameStats.gpuBytes`, novo), handles e alocações. A contagem de VRAM achou um vazamento limitado: a textura removida ficava na GPU até outra ocupar seu índice. Corrigido. |
| 3.9 Linux e Mac | **Não iniciado.** | Precisa das máquinas. |

**Saída da 1.0:** fases 0 a 3 fechadas; API congelada com a regra de 1.4; camadas de 1.6 publicadas.

---

## Fase 4 — Arquitetura (1.x, antes dos sistemas grandes)

### 4.1 IR de shaders (G)

O objetivo é que cada shader seja descrito uma vez e emitido para SPIR-V, MSL e WGSL, sem compilador externo.

1. Definir um IR em SSA mínimo: tipos, operações aritméticas, controle estruturado, recursos com binding. As macros `S_ADD`, `S_MUL`, `S_SELECT` de `shading.inc` já são essa linguagem em forma textual; o grafo de materiais (`artfExpr*`) é outro IR em miniatura.
2. Escrever o emissor SPIR-V a partir do IR e migrar um shader por vez. **Critério de cada migração:** `artful-shaders` produz as **mesmas palavras SPIR-V** que antes (comparação binária), ou uma diferença justificada que passa `spirv-val` e mantém as imagens.
3. Emitir `NonSemantic.Shader.DebugInfo.100` apontando para os arquivos C, para depuração no RenderDoc.
4. Escrever o emissor MSL, em texto, compilado pelo Metal em execução. Isso fecha a lacuna dos materiais em C no Metal, que hoje não alcança a tradução antecipada.
5. Emissor WGSL fica para a Fase 6.

**Pronto quando:** todos os shaders 3D e 2D passam pelo IR com saída SPIR-V igual à atual, e o MSL de pelo menos as 17 variantes de hoje compila no compilador da Apple na CI.

**Estado da 4.1:** passos 1 e 2 feitos, para todos os shaders de uma vez. Em vez de migrar shader a shader, o construtor (`spirv.c`) passou a gravar no IR (`src/shaderir/`), e o SPIR-V sai do IR (`irSpirv`). O IR tem instruções em SSA nas quatro seções, tipo, resultado, operandos classificados (id, literal, string) pela gramática dos 102 opcodes em uso, consultas (`irDef`, `irTypeOf`), verificador (`irCheck`) e forma de texto (`irPrint`). Por escolha, o vocabulário de operações é o próprio do SPIR-V: os shaders já são escritos contra ele, e sua gramática dá a semântica. **Critério:** os 103 shaders (3D e 2D) saem com as mesmas palavras SPIR-V, byte a byte (clang). Cada módulo passa no `irCheck`, no `artful-shaders` e em toda a suíte, e um teste confere que o verificador acusa cada defeito plantado. O tempo de construção não mudou (~60 ms para todos). **Passo 4 (emissor MSL): feito, falta a prova da Apple.** `irMsl` gera Metal de qualquer IR, nas convenções do SPIRV-Cross, para o `metal.c` ligar os dois do mesmo jeito: um argument buffer por set, recursos na ordem de declaração, amostradores `bNSmplr`, arrays ocupando um id por elemento e `main0`. O controle de fluxo vem dos merges estruturados, e os phis viram variáveis atribuídas nas arestas. Escreve 94 dos 103 shaders; os 9 de fora usam ray queries, que o backend Metal não tem. **Verificado aqui:** os 17 que o Metal roda têm tabela de recursos e interface idênticas às do SPIRV-Cross (`tools/msl/compare.py`). Os 94 passam em `clang++ -fsyntax-only` contra um substituto da biblioteca do Metal (`tools/msl/check.py`), que acusa nomes, tipos e sintaxe, mas não substitui o compilador da Apple. Todo shader construído nos testes também é escrito em Metal. **Pendente:** a CI do macOS passou a compilar cada um com o compilador da Apple, mas isso só roda com push. Até lá, o `shaders.inc` segue o do SPIRV-Cross. Também faltam DebugInfo (passo 3), a compilação em tempo de execução no `metal.c` (que fecha os materiais em C no Metal) e WGSL (Fase 6).

### 4.2 Grafo de quadro (G)

1. Descrever cada passe de `vulkan.c` com o que lê e escreve. Começar só pela descrição, gerando um diagrama, sem mudar a execução.
2. Gerar barreiras e transições a partir do grafo; comparar com as atuais sob a validação de sincronização (que já roda na CI).
3. Descartar passes sem consumidor e reutilizar memória transitória entre alvos que não coexistem.
4. Fazer o backend de CPU percorrer o mesmo grafo, com passes por tile ou por quadro.

**Pronto quando:** a validação de sincronização segue limpa; a memória de alvos em 1080p com pós completo cai (meta: 30%); `vulkan.c` se divide em módulos por passe sem perda de desempenho na linha de base da Fase 0.

**Estado da 4.2 (atualizado): passo 1 feito; passos 2 e 3 feitos no pós; passo 4 não iniciado.**
- **Passo 3, alvos e memória:** os buffers de pós passaram a ser criados só para os efeitos ligados (`postNeedsOf`), e oclusão e velocidades do motion blur, que vivem em passes disjuntos, dividem um buffer. A 1080p, a memória de alvos caiu 7,5% com todos os efeitos, 15% com TAA e bloom e 46% só com bloom.
- **Correção da medição:** os três buffers de luz trocam de papel fisicamente a cada passe, e o grafo agora os trata como um bloco que vive todo o pós; antes superestimava o aliasing possível.
- **O que falta para os 30% com todos os efeitos:** (a) aliasing entre imagem e buffer, colocando o buffer compartilhado na memória das imagens float da cena, mortas após o gather (+8%); (b) guardar a luz em 16 bits em vez de 32 (−47 MB, cerca de 22%). Esta última muda os bytes das imagens, por isso **é decisão sua**.
- **Passo 2, barreiras:** cada pipeline de pós sabe o que lê e escreve a partir das próprias palavras (`irFromSpirv`, `irUses`), e o `postRecord` só põe barreira onde há conflito, com os papéis traduzidos para o buffer físico do momento. O pós é quase todo em série: 24 despachos tomam 22 barreiras, contra 24. A validação de sincronização fica limpa nos 8 modos.
- **Passo 4 e a divisão do `vulkan.c` por passe:** não feitos. Os passes da cena continuam com barreiras escritas à mão. `src/vulkan/frameGraph.c` descreve os passes do `vulkan.c` na ordem e com os nomes dos timestamps. O que cada passe lê e escreve sai do IR dos shaders (`irUses`), não de uma lista à mão; os anexos de render pass são declarados. `artful-render --graph` gera tabela e Graphviz. Um teste garante que todo passe executado está descrito e em ordem, com todos os efeitos, com só alguns, e com e sem as duas metades da oclusão. **Medição a 1080p, com tamanhos reais de alocação:** com todos os efeitos, 212,6 MB em alvos; o aliasing economizaria 39,8 MB (19%), e 8 MB são de efeito desligado. Com TAA e bloom: 7% por aliasing e 15% de alvos sem uso. Só bloom: 0% por aliasing e 31% sem uso. **Conclusão:** a meta de 30% exige as duas coisas, criar alvos só para os efeitos ligados e fazer aliasing do que vive dentro do quadro. Os passos 2 e 3 dependem da validação de sincronização, que não roda nesta máquina sem a camada de validação instalada.

**Estado da 4.3 (atualizado): feito.** Além do HLOD, o modelo glTF também é cozido: `artfPreparedModelSave` e `artfPrepareModelCooked` guardam imagens com mipmaps e malhas com níveis, amarradas ao hash do glTF. A ferramenta `artful-cook model.glb` grava o arquivo e confere que a cena sai idêntica, byte a byte. Num modelo de 980 mil triângulos com três texturas de 2048, a carga cai de 2.395 ms para 65 ms (2,7%), com imagem idêntica. O novo alvo de fuzzing `cook` achou dois defeitos no leitor, ambos de memória reservada pelo que o arquivo declara antes de conferir; foram corrigidos e viraram regressão, e mais 30 minutos de fuzzing nada acharam. Estado anterior, só o HLOD: `artfSceneSaveHlod`/`artfSceneLoadHlod` guardam as malhas fundidas como construídas (arrays, meshlets, posições, níveis, membros, erro e distância). O formato é versionado: "ARTFCOOK", versão e tipo, e registros prefixados pelo comprimento, de modo que uma versão nova lê arquivos antigos. A leitura valida tudo antes de tocar a cena: contagens, índices dentro da malha, meshlets dentro dos arrays, membros existentes, e a impressão digital das instâncias. **Critério:** a `forest` cozida carrega em 45 ms contra 1.166 ms crua (**3,9%**, meta abaixo de 10%), com imagem idêntica byte a byte na CPU e na GPU; `scale/hlodCooked` confere malhas idênticas e as recusas. **Falta:** malhas preparadas de glTF e mipmaps (o mesmo formato serve), um alvo de fuzzing para o leitor, e uma ferramenta `artful-cook` separada (hoje são opções do `artful-render`).

### 4.3 Ferramenta de cozimento (M)

`artful-cook` grava malhas preparadas (meshlets, níveis, árvore do traçado), mipmaps e HLOD num formato versionado, com a garantia de que versões novas leem arquivos antigos. Pré-requisito do DAG de clusters, cuja construção não cabe no carregamento.

**Pronto quando:** carregar a cena `forest` cozida leva menos de 10% do tempo da carga crua, e o resultado é byte a byte o mesmo.

---

## Fase 5 — Alcance de conteúdo (1.x)

Cada item entra atrás de uma opção (`ARTF_FEATURE_*` ou campo de descrição), com o caminho atual mantido até o novo provar-se melhor nas cenas da linha de base.

| # | O que muda | Pronto quando | Tam. |
|---|---|---|---|
| 5.1 | **Probes escaláveis** (Majercik 2021): estados (ativa, dormente, dentro de parede), realocação, atualização por orçamento de raios em vez de todas a cada quadro, volumes em cascata seguindo a câmera. Remove a caixa fixa. | Mesmo orçamento de raios: porta abrindo e luz acesa de súbito convergem em no máximo 2× os quadros atuais; luminância vazada por parede fina menor que a atual contra referência traçada. | M |
| 5.2 | **Luzes em clusters**: fatias de profundidade em `artfSceneLightTiles` (`scene.c:1106`), contagem de luzes omitidas por tile exposta em `artfRendererStats`. | Num corredor de vários andares, zero luzes omitidas onde hoje há; custo de construção abaixo de 0,2 ms. | P |
| 5.3 | **LTC** para retângulos e tubos, com `acos` determinístico. | Contra referência traçada, erro de iluminação de área abaixo do da aproximação de ponto representativo. | M |
| 5.4 | **Upscaler**: travas para detalhe fino, máscaras reativa e de composição, reprojeção Lanczos, detector de cintilação, mistura adaptativa no lugar de 9:1 fixo. | Grade fina a 50% de escala: variação temporal com câmera parada cai pelo menos pela metade; fumaça sobre fundo contrastado sem rastro além de 4 quadros. | M |
| 5.5 | **DAG de clusters**: construção no cozimento (simplificador com bordas travadas, operação em subconjunto e erro por atributo); seleção do corte calculada uma vez, como a escolha de nível já é hoje, e desenhada pelos dois backends pelo caminho indireto existente. Sem mesh shaders e sem atômicos de 64 bits nesta etapa. | `city` e `heavy` com o mesmo erro em pixels desenham menos triângulos que os 3 níveis atuais; nenhum estalo acima do limiar com `lod-fade` desligado; CPU e GPU escolhem o mesmo corte. | G |
| 5.6 | **Raios contra corte grosso** do DAG, com regra consistente entre raio primário e secundário; **BVH largo** (4 filhos, quantizado) com reinserção após a construção. | Memória das árvores cai; o auto-sombreamento entre nível desenhado e traçado some sem o deslocamento de um milésimo; a travessia em compute numa GPU integrada fica mais rápida. | M |
| 5.7 | **Luzes estocásticas por tile** (estilo HypeHype/ReSTIR DI) com raios de sombra e um **denoiser de sombra dedicado**; **A-SVGF** no denoiser atual. Remove a distinção entre as 16 luzes e as demais. | Cena com 200 luzes com sombra: custo constante independente do número de luzes; imagem dentro do limiar FLIP de uma referência com todas as luzes avaliadas. | G |
| 5.8 | **Transparência por transmitância em froxels** (estilo AVBOIT), reaproveitando a grade da névoa, no lugar das listas de 96 bytes/pixel. | Memória fixa; interpenetração correta nos casos do teste de camadas; custo abaixo das listas em 1080p. | M |
| 5.9 | **Saída HDR e gama larga**: `VK_EXT_swapchain_colorspace` (HDR10, scRGB, Display-P3), enumerada em execução; saída de 10 bits ou float no backend de CPU. Depende de 2.1. | Num monitor HDR, o pico configurado chega à tela; no Android, Display-P3 de 10 bits quando oferecido. | M |
| 5.10 | **Basis Universal** (ETC1S, UASTC LDR) e **Zstandard**, transcodificadores em C. | Os arquivos `KHR_texture_basisu` do corpus carregam e coincidem com a saída do transcodificador de referência. | M |
| 5.11 | **Céu**: LUT de perspectiva aérea aplicada à cena; nuvens marchadas a um quarto da resolução e compostas contra a profundidade. | Montanhas distantes ganham a cor do ar; uma torre atravessa as nuvens. | M |
| 5.12 | **Materiais**: texturas das extensões glTF hoje ignoradas; iridescência de Belcour e Barla; camadas no estilo OpenPBR no grafo em C; SSS separável (verificar antes a patente associada à difusão de Burley). | Os modelos de extensão do glTF-Sample-Assets coincidem com o visualizador de referência dentro do limiar. | M a G |

**Ordem sugerida dentro da fase:** 5.1, 5.2 e 5.4 primeiro (baratos, atacam limites documentados); depois 5.5 e 5.6 (dependem de 4.3); depois 5.7 (depende de 2.2 e se beneficia de 5.6). 5.8 a 5.12 são independentes e entram por demanda de consumidor.

---

## Fase 6 — Alcance de plataforma

| # | O que muda | Depende de | Pronto quando | Tam. |
|---|---|---|---|---|
| 6.1 | **Metal executado**: o backend passa a ler os shaders do emissor MSL de 4.1, e o lado C é atualizado para os bindings de hoje. | 4.1, um Mac | Todos os testes de imagem rodam em Metal dentro do limiar contra a CPU; promovido a "melhor esforço" e depois "estável". | G |
| 6.2 | **WebGPU**: emissor WGSL e um backend sobre `webgpu.h`. O backend de CPU em WebAssembly com SIMD vem antes, por ser só compilar o que existe. | 4.1, 4.2 | A cena `spheres` roda em Chrome e Safari, nos dois caminhos. | G |
| 6.3 | **Caminho sem descriptor indexing** (arranjos de texturas por material) para GPUs como a Mali-G52, que hoje caem para a CPU. | 4.2 | Um dispositivo sem a funcionalidade desenha na GPU. | M |

---

## Fora do plano, por decisão

Upscalers e compressão neurais, Virtual Shadow Maps e texturas virtuais completos, estruturas de aceleração por cluster de fornecedor, Draco, cabelo por fios, volumes heterogêneos e splats gaussianos. Os motivos estão na seção 8 do relatório. Reavaliar quando um consumidor concreto pedir.

## Riscos do plano

- **A Fase 2 muda os bytes que o projeto prometeu manter.** Isso é aceitável antes da 1.0 (o [roadmap](roadmap.md) permite quebrar), mas só se a revisão de cada diferença for feita com o FLIP e registrada. Sem a Fase 0 pronta, não começar.
- **O IR de shaders é a peça mais arriscada.** Mitigação: migração shader a shader com igualdade binária do SPIR-V como critério, de modo que o projeto nunca fique num estado intermediário quebrado.
- **O DAG de clusters pode emperrar** em malhas facetadas e costuras de UV, como relatado por quem o reimplementou. Mitigação: manter os 3 níveis como recurso ativo até o DAG vencer em todas as cenas da linha de base.
- **Acesso a hardware** (Mac, Linux com GPU, telefones variados) é o gargalo de 3.7, 3.9 e 6.1, não o código.
