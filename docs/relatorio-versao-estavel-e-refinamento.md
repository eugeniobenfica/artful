# Artful — o que falta para uma versão estável de alto nível, e como refinar o que já existe

**19 de novembro de 2026** · branch `unreal-parity`, base `bcced12`, versão declarada 0.4.0, incluindo as alterações locais (biblioteca 2D).

Este relatório complementa [relatorio-tecnico-estabilidade.md](relatorio-tecnico-estabilidade.md), que trata dos critérios de saída da 1.0. Aqui o foco é outro: (a) o que adicionar ou refinar para que o Artful sustente aplicações reais no nível em que a Unreal é usada, (b) uma leitura crítica da arquitetura atual e (c) as técnicas publicadas mais promissoras para cada ponto fraco, com o grau de adequação às regras do projeto.

**Como ler as citações.** Os links sem marca foram abertos e conferidos durante a pesquisa. Os marcados com † foram localizados por busca, mas a página não pôde ser lida (PDF ilegível, página renderizada por JavaScript ou 403); o conteúdo atribuído a eles vem do resumo ou de fonte secundária. As páginas de documentação e notas de versão da Epic caíram quase todas nesse caso, então os rótulos de maturidade da Unreal vêm dos anúncios no fórum oficial e de cobertura secundária.

---

## 1. Conclusões

1. **O Artful já é um renderizador amplo; o que o separa de "estável" é prova, não recurso.** A lista de técnicas implementadas cobre a maior parte de um renderizador moderno. O que falta para a 1.0 é contrato de API, execução em hardware real nas plataformas prometidas, endurecimento dos leitores de arquivo e regressão visual por métrica perceptual. Nada disso depende de algoritmo novo.
2. **"Similar à Unreal" precisa ser recortado.** A Unreal é um motor com editor, física, áudio, animação, rede e cozimento de ativos. O Artful é uma biblioteca de renderização. A comparação justa é com o *renderizador* da Unreal, e nela as lacunas que decidem se uma aplicação grande cabe são quatro: geometria virtualizada, muitas luzes com sombra, iluminação indireta que escala com o mundo, e limites fixos (16 luzes principais, 1.024 texturas, 3 níveis de detalhe, caixa fixa de probes).
3. **Três correções baratas têm retorno desproporcional** e deveriam vir antes de qualquer sistema grande: amostragem de baixa discrepância com amostragem por importância do ambiente e da BSDF no traçado; função de transferência sRGB exata com gradação em espaço logarítmico; antialiasing especular com compensação de energia. São dezenas a poucas centenas de linhas cada, determinísticas, e funcionam igual na CPU e na GPU.
4. **A direção da própria Epic favorece o Artful no hardware modesto.** A UE 5.8 (junho de 2026) introduziu o *Lumen Lite*, um modo de GI para portáteis e PCs fracos que, segundo o resumo das notas†, usa campos de irradiância com oclusão por probe. É a família de técnica que o Artful já tem. O investimento certo ali é escalar as probes existentes (estados, realocação, cascatas), não reescrever a GI.
5. **A maior decisão de arquitetura pendente é o custo do SPIR-V escrito à mão.** São ~4.800 linhas em `src/vulkan/shaders.c` mais 800 no 2D. A ausência de compilador é um diferencial real, mas cada backend novo (Metal de verdade, WebGPU, D3D12) hoje exige outro caminho. Um IR intermediário próprio, com emissores de SPIR-V, MSL e WGSL, preserva a regra de zero dependências e destrava os três.
6. **No Android há uma escolha atual que contraria a orientação oficial**: o Artful delega a rotação ao compositor; a documentação do Android trata a pré-rotação como obrigatória e mede o custo de não fazê-la.

---

## 2. O recorte: renderizador, não motor

| Camada de uma aplicação "tipo Unreal" | Quem fornece |
|---|---|
| Renderização 3D e 2D, materiais, luz, pós-processamento | **Artful** |
| Janela, entrada, interface | Cascade (integração existente) |
| Física, colisão | Fora de escopo. Par comum: Jolt |
| Grafo de animação, IK, compressão de clipes | Fora de escopo hoje (`artfAnimationSet` amostra clipes; a mistura é do programa). Pares comuns: ozz-animation, ACL |
| Áudio | Fora de escopo. Par comum: miniaudio |
| Editor, scripting, rede, cozimento de ativos | Fora de escopo |

Duas dessas fronteiras merecem ser revistas, porque encostam no renderizador e hoje ficam sem dono:

- **Cozimento de ativos.** Gerar níveis de detalhe custa ~3,6 µs por triângulo no carregamento e o HLOD de 80.000 instâncias leva 1,4 s. Aplicações reais não pagam isso a cada execução. Uma ferramenta `artful-cook` e um formato de cache versionado (malha preparada, árvores, mipmaps) cabem no projeto sem violar a regra de dependências, já que ferramentas podem depender do que quiserem.
- **Mistura de animação.** Sem ao menos mistura de dois clipes e camadas por máscara de ossos dentro da biblioteca, todo integrador reescreve o mesmo código sobre `artfInstanceSetJoints`. É pequeno e determinístico.

---

## 3. Paridade com o renderizador da Unreal

Estado da Unreal: 5.6 (jun/2025), 5.7 (nov/2025), [5.8 (17/jun/2026)](https://forums.unrealengine.com/t/unreal-engine-5-8-released/2729274).

| Área | Unreal 5.8 | Artful hoje | Distância |
|---|---|---|---|
| Geometria densa | Nanite: DAG de clusters, corte por erro < 1 px, visibility buffer, rasterização de triângulos pequenos em compute, streaming por páginas. Foliage com voxels ainda experimental (5.7). | Meshlets (64 vértices/124 triângulos), até 3 níveis discretos por malha, HLOD por cubo do mundo, descarte de meshlets na GPU só acima de 512 meshlets. | **Grande.** É a lacuna que mais limita o tamanho das cenas. |
| GI dinâmica | Lumen (cache de superfície, final gather em espaço de tela, RT por hardware; o caminho de SDF por malha foi descontinuado em 5.6/5.7) e Lumen Lite (beta em 5.8). | Grade de probes estilo DDGI em caixa fixa, SH de 1ª ordem, visibilidade de Chebyshev 8×8, todas as probes a cada quadro, CPU e GPU com a mesma imagem. | **Média.** Próximo do Lumen Lite; longe do Lumen HQ em reflexos e detalhe de contato. |
| Muitas luzes | MegaLights, pronto para produção em 5.8: número fixo de raios por pixel para luzes amostradas por importância, mais denoiser. Exige RT por hardware. | 16 luzes com sombra + 1.024 em listas por quadrado de 64×64 px (64 por lista, sem fatias de profundidade), 32 com sombra em mapas de um quarto. | **Grande** para cenas noturnas/internas densas. |
| Sombras | Virtual Shadow Maps (16k virtual, páginas 128², cache). | Atlas único: 3 cascatas do sol, 16 + 48 mapas, PCF 3×3, PCSS opcional; sombras por raios para o sol e as 16. | **Média.** Raios cobrem a qualidade onde há hardware; mapas ficam aquém em cenas grandes. |
| Antialiasing/upscaling | TSR (histórico em resolução de tela, rejeição por sombreamento, detector de cintilação, ressurreição de histórico). | MSAA 4× ou TAA com mistura fixa 9:1; upscaler temporal próprio, sem máscaras reativas, cintila abaixo de 0,5. | **Média.** |
| Materiais | Substrate (pronto para produção e padrão desde 5.7): lajes e operadores, consistente entre raster e path tracing. | glTF PBR + coat, sheen, anisotropia, filme fino aproximado, grafo de material em C. Sem compensação de espalhamento múltiplo; texturas das extensões não lidas. | **Média.** O grafo em C é um bom alicerce. |
| Transparência | Ordenada por padrão; OIT por voxels adaptativos experimental. | Camadas por objeto ou listas de 8 por pixel a 96 bytes/pixel. | **Pequena** (a Unreal também não resolveu), mas o custo de memória do Artful é alto. |
| Pós-processamento | Completo, gradação em log, saída HDR. | Completo em SDR; gradação em gama 2; saída só BGRA de 8 bits. | **Pequena em recursos, média em correção de cor.** |
| Céu e volumes | Sky Atmosphere, nuvens volumétricas na cena, volumes heterogêneos (beta). | Céu de Hillaire com LUTs e nuvens assados numa imagem de ambiente; névoa em froxels 16 px × 64 fatias. | **Média.** Falta perspectiva aérea sobre a cena e nuvens à frente dela. |
| Path tracer | Referência offline com os mesmos materiais. | Traçado progressivo em CPU e compute, com denoiser; integrador simples (ver §6). | **Média**, e barata de fechar. |
| Texturas em escala | Virtual texturing (SVT/RVT), streaming. | Residência de mipmaps por orçamento; teto de 1.024 texturas. | **Média a grande** conforme o conteúdo. |
| Compilação de pipelines | PSO precaching. | Variantes compiladas em threads, cache em arquivo, `artfRendererWarm`. | **Pequena.** O desenho do Artful aqui é bom. |
| Plataformas | Windows, consoles, Linux, macOS, iOS, Android. | Windows e um Android comprovados; Linux só em Vulkan por software; Metal nunca executado; sem D3D12, WebGPU ou consoles. | **Grande.** |
| Depuração | Modos de visualização, perfis de GPU, captura. | Vistas (`artfRendererSetView`), tempo por passe em CPU e GPU. | **Pequena.** |

Onde o Artful está à frente, e deve permanecer: um backend de CPU completo com os mesmos bytes em x86 e ARM, a mesma imagem em CPU e GPU verificada por teste, zero dependências, e funcionamento em GPU integrada antiga e em telefone sem hardware de ray tracing. A Unreal não oferece nenhum dos quatro.

---

## 4. Para a versão estável

O relatório anterior lista critérios de saída. Abaixo, o que a pesquisa acrescenta, pela ordem em que bloqueia.

### 4.1 Contrato público

Estado: ~230 funções públicas; biblioteca só estática (`add_library(... STATIC)`); nenhum controle de visibilidade de símbolos; falta de memória aborta; handles de malha removida nunca voltam a ser usados; regra de uma thread por vez.

| Item | Por que importa | Referência |
|---|---|---|
| Promessa escrita de compatibilidade de fonte em 1.x, e decisão explícita sobre ABI | As bibliotecas comparáveis (Filament, wgpu, bgfx) não prometem ABI; prometem camadas de suporte e marcadores de quebra. SDL3 e libcurl prometem ABI e pagam por isso. Qualquer das duas é defensável; não decidir não é. | [SDL3](https://wiki.libsdl.org/SDL3/README-versions) · [libcurl](https://curl.se/libcurl/abi.html) · [wgpu](https://github.com/gfx-rs/wgpu) |
| Structs extensíveis de verdade | "Structs crescem" só é seguro com recompilação. Para biblioteca dinâmica é preciso tamanho ou versão no primeiro campo. | — |
| Reciclagem de handles de malha com geração | Texturas já têm geração; malhas consomem posição para sempre. Uma aplicação com streaming que roda por horas esgota o espaço. | [limits.md](limits.md) |
| Falha de memória recuperável onde é provável | Abortar em RAM é aceitável. Em VRAM, não: orçamento estourado é evento normal num telefone. Precisa de caminho de degradação (cair mipmaps, recusar o recurso) e de callback. | — |
| Formato de cache em disco estável | Se houver cozimento, dados antigos devem continuar legíveis. | [meshoptimizer](https://github.com/zeux/meshoptimizer) |
| Camadas de suporte publicadas | "Estável", "melhor esforço" e "experimental" por backend e plataforma. Metal e Windows on ARM entram como experimentais até rodarem. | [Godot](https://docs.godotengine.org/en/stable/tutorials/rendering/renderers.html) · wgpu |

### 4.2 Robustez de plataforma

- **Pré-rotação no Android.** A [documentação do Android](https://developer.android.com/games/optimize/vulkan-prerotation) diz que implementar pré-rotação é obrigatório, mede 1 a 3 ms de custo por quadro quando o compositor gira (Pixel 4 XL) e recomenda detectar a mudança por `VK_SUBOPTIMAL_KHR`. O Artful hoje pede transformação nenhuma e deixa o compositor girar ([stability.md](stability.md)). Como o projeto já planeja testar rotação num aparelho, vale implementar a pré-rotação antes desse teste.
- **Ritmo de quadros e térmica.** [Swappy](https://developer.android.com/games/sdk/frame-pacing) usa Choreographer e `VK_GOOGLE_display_timing` para evitar acúmulo de buffers; a [ADPF](https://developer.android.com/games/optimize/adpf)† expõe folga térmica. O Artful não precisa depender delas, mas precisa expor os ganchos: tempo de apresentação alvo e um sinal para `renderScale` dinâmico. Isso conversa com a pendência de medir os workers que giram em espera.
- **Linha de base Vulkan declarada.** Android 14+ exige o perfil Baseline 2021; dispositivos lançados com Android 16 exigem Vulkan 1.4 ([AOSP](https://source.android.com/docs/core/graphics/implement-vulkan)). Cobertura em outubro de 2025: perfil 2021 em 95,5% dos aparelhos Vulkan, 2022 em 86,5% ([Android Vulkan Profiles](https://developer.android.com/ndk/guides/graphics/android-vulkan-profile)). O Artful exige 1.2 com descriptor indexing; convém declarar contra qual perfil ele roda e testar a queda para CPU nos demais.
- **Perda de dispositivo.** O teste atual injeta o erro. A [Arm documenta](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/diagnosing-vk-error-device-lost-on-mali-gpus-with-vk-ext-device-fault) que a perda ocorre mesmo com uso correto da API e que `VK_EXT_device_fault` devolve o endereço da falha. Habilitar a extensão quando existir e registrar o relatório via `artfSetLog` é barato e transforma um "travou no campo" em diagnóstico. Além disso: as probes de GI recomeçam do zero quando o renderizador é recriado; o estado deveria sobreviver na cena.

### 4.3 Leitores de arquivo

`gltf.c`, `imageDecode.c`, `texture.c`, `blocks.c`, `meshopt.c` e o leitor de IES são código próprio que recebe bytes não confiáveis. Os testes cortam e corrompem um arquivo bom, o que não é guiado por cobertura.

- Alvos de libFuzzer por formato, sob ASan/UBSan, com dicionário e corpus semente. É o padrão do [OSS-Fuzz](https://google.github.io/oss-fuzz/getting-started/new-project-guide/); cgltf e tinygltf têm esses alvos, e o OSS-Fuzz encontrou a CVE-2022-3008 no tinygltf†. Cabe em `tools/`, sem tocar a biblioteca.
- Corpus de conformidade: [glTF-Sample-Assets](https://github.com/KhronosGroup/glTF-Sample-Assets) (tem listas "Testing" e "Core Only") e arquivos produzidos por `gltfpack` para a pendência do meshopt.
- **Compatibilidade de formato.** `KHR_texture_basisu` está ratificada e é comum; o Artful não lê. Transcodificadores ETC1S e UASTC mais um descompressor Zstandard em C são alguns milhares de linhas ([Basis Universal](https://github.com/BinomialLLC/basis_universal), Apache-2.0, C++). É o maior item de porte desta seção e o que mais arquivos reais destrava. Draco pode ficar de fora: o decodificador é grande e meshopt cobre o caso.

### 4.4 Testes que distinguem "passou" de "não rodou"

- Um único teste agregado no CTest esconde casos omitidos. Registrar cada grupo como teste próprio e fazer os que dependem de GPU ou janela reportarem *skipped*, com modo estrito para os perfis certificados.
- **Métrica perceptual no lugar de "N pixels diferem em mais de 24/255".** O [FLIP](https://github.com/NVlabs/flip) (NVIDIA, BSD-3) é um cabeçalho único em C++ com variantes LDR e HDR; o algoritmo cabe em poucas centenas de linhas de C. Com ele, cada comparação CPU×GPU e cada imagem de referência ganha um número que corresponde ao que se vê.
- Imagens de referência por configuração, fora do Git se crescerem, como fazem o [Skia Gold](https://skia.org/docs/dev/testing/skiagold/) e o renderdiff do Filament†.
- **Hardware real na CI.** O padrão da área é fazenda própria ou alugada: [Mesa CI](https://docs.mesa3d.org/ci/index.html) com controle de testes instáveis; [Firebase Test Lab](https://firebase.google.com/docs/test-lab/android/game-loop) roda laços em aparelhos físicos e registra tempos de quadro. Um runner próprio com a Radeon e um laço no Test Lab cobrem a maior parte do risco por pouco.
- Ensaio de longa duração: horas de troca de ativos com orçamento fixo, registrando pico de RAM e VRAM, handles consumidos e alocações restantes.

---

## 5. Análise da arquitetura atual

### 5.1 O que sustenta o projeto e deve ser preservado

| Decisão | Por que é forte |
|---|---|
| Cena separada do renderizador, sincronizada por números de versão | Perda de dispositivo e troca de superfície deixam de ser problema de carregamento. Poucos renderizadores têm isso tão limpo. |
| Modelo de sombreamento escrito uma vez (`shading.inc`) e incluído em quatro contextos | A CPU vira o oráculo da GPU. É o que permite não ter validador em execução. |
| Mesmos bytes em todo processador | Torna os caminhos rápidos testáveis contra o simples e dá reprodutibilidade que nenhum concorrente oferece. |
| GPU dirigida por dados: um buffer de vértices, um de índices, um arranjo de texturas, draws indiretos | Custo de CPU constante (0,05 ms) com 10 ou 90.000 objetos. É a base certa para geometria virtualizada. |
| Rasterizador de CPU em tiles, diferido, sem overdraw | Mesma estrutura de um visibility buffer; a transição na GPU fica natural. |
| Variantes de shader só para o que a cena usa, compiladas fora do quadro | Resolve engasgos de pipeline melhor que a média. |

### 5.2 Onde a arquitetura cobra juros

**a) SPIR-V escrito por chamadas em C.** `shaders.c` tem 4.781 linhas e cresce a cada recurso. Consequências observáveis:

- O Metal ficou parado na 0.4 porque depende de tradução antecipada pelo SPIRV-Cross e não alcança materiais em C.
- Não há depuração em nível de fonte: o RenderDoc precisa de `NonSemantic.Shader.DebugInfo.100`, que o construtor não emite ([LunarG](https://www.lunarg.com/source-level-shader-debugging-in-vulkan-with-renderdoc/)†).
- A pesquisa não encontrou outro renderizador em produção que escreva shaders assim. O precedente mais próximo de geração em tempo de execução é o DXVK, que usa um IR próprio†.

A saída que respeita as regras do projeto: promover o que hoje são macros (`S_ADD`, `S_MUL`, `S_SELECT`...) a um **IR pequeno em SSA**, construído pelas mesmas chamadas em C, com emissores para SPIR-V, MSL (texto, compilado pelo Metal em execução) e WGSL. O grafo de materiais já é um IR desse tipo em miniatura. Slang seria a alternativa de mercado, mas é dependência de compilador e seus alvos Metal e WGSL ainda são experimentais ([Slang](https://github.com/shader-slang/slang)).

**b) Um arquivo de 5.171 linhas e bindings numerados à mão.** `vulkan.c` organiza passes, barreiras, alvos e descritores manualmente (bindings até 70, 27 conjuntos para o pós-processamento). Cada recurso novo toca vários pontos distantes. Um **grafo de quadro** resolve isso de forma conhecida: passes declaram o que leem e escrevem; o compilador do grafo descarta passes sem consumidor, reaproveita memória transitória e emite barreiras. No Frostbite isso reduziu a memória de alvos em 4K de 1.042 para 472 MB e o código do renderizador de 15 mil para 5 mil linhas ([O'Donnell, GDC 2017](https://www.slideshare.net/DICEStudio/framegraph-extensible-rendering-architecture-in-frostbite)). No Artful o grafo pode ser compartilhado: a CPU executa os mesmos passes em tiles. É pré-requisito prático para um segundo backend de GPU.

**c) Limites fixos embutidos no formato do quadro.** 16 luzes "do quadro", 1.024 texturas, 3 níveis de detalhe, 8 juntas, 256 decals, caixa fixa de probes, 64 luzes por tile. Cada um foi uma escolha razoável, mas juntos definem o teto do conteúdo. As seções 6.1 a 6.3 tratam dos que mais pesam.

**d) Raios e raster veem geometrias diferentes.** As árvores guardam a malha inteira; o raster desenha um nível mais leve. Isso custa memória e tempo de travessia, e produz auto-sombreamento que hoje é contornado com deslocamento de um milésimo da distância. Lloyd, Klehm e Stich mostram que raios secundários precisam ver o mesmo nível que o acerto primário, sob pena de vazamentos e sombras erradas ([NVIDIA, 2020](https://developer.nvidia.com/blog/implementing-stochastic-lod-with-microsoft-dxr/)).

**e) Codificação de cor em gama 2.** Texturas são elevadas ao quadrado e o quadro guarda a raiz. É rápido e idêntico nos dois backends, mas difere da curva sRGB por partes: os tons médios saem deslocados em relação a ferramentas de autoria e visualizadores glTF, e LUTs feitas para sRGB ou log não se transferem. Para uso profissional isso é defeito de correção, não de estilo.

**f) O integrador de caminhos é mais simples que o resto.** Números aleatórios vêm de um hash por pixel com rejeição numa esfera (`trace.c:771-782`); o salto é escolhido ao acaso entre espelho e difuso; não há amostragem por importância do ambiente; lê-se sempre o maior mipmap. Tudo o que depende de raios (traçado, probes, sombras suaves, reflexos, denoiser) herda esse ruído.

**g) A exatidão de bytes tem custo crescente.** Cada função transcendental precisa de polinômio próprio, FMA fica proibido, e técnicas com atômicos ou colisões de hash na GPU não reproduzem. Recomendo manter a garantia para o que ela protege hoje (raster, sombreamento, pós) e definir explicitamente uma segunda classe, "mesma imagem dentro de tolerância FLIP", para caches estocásticos. O 2D já opera assim ("um passo de byte").

**h) Árvores de aceleração binárias, construídas na CPU.** SAH com 12 bins é bom em qualidade, mas a travessia em compute numa GPU integrada é limitada por tráfego de memória, e o nó binário é o pior caso para isso.

---

## 6. Técnicas promissoras, por problema

Critérios: funcionar na CPU e em Vulkan compute sem hardware de ray tracing; ser determinística; não exigir dependência. "Custo" é estimativa minha, não medição.

### 6.1 Geometria: de três níveis discretos a um DAG de clusters

| Técnica | O que é | Adequação | Custo |
|---|---|---|---|
| **DAG de clusters (Nanite)** — Karis, Stubbe, Wihlidal, [SIGGRAPH 2021](https://advances.realtimerendering.com/s2021/index.html) | Clusters de ~128 triângulos são agrupados, simplificados com as bordas do grupo travadas e redivididos. Em cada quadro escolhe-se um corte pelo erro projetado. Não há estalo entre níveis nem degrau de um quarto. | A construção é só CPU e reaproveita os meshlets e o simplificador existentes. O simplificador precisa de três acréscimos: travar bordas, operar sobre subconjunto e pesar atributos. | Médio para a construção. |
| **Referência aberta** — meshoptimizer `clusterlod.h`, [Kapoulkine 2025](https://zeux.io/2025/09/30/billions-of-triangles-in-minutes/) | 1,64 bilhão de triângulos processados em ~2 min 35 s num 7950X. | É C++ (MIT): serve de referência de algoritmo, a portar. Risco documentado: a simplificação emperra em malhas facetadas e costuras de UV ([jglrxavpok](https://jglrxavpok.github.io/2024/01/19/recreating-nanite-lod-generation.html)). | — |
| **Seleção do corte sem mesh shaders** — [Bevy 0.14](https://jms55.github.io/posts/2024-06-09-virtual-geometry-bevy-0-14/) e [0.16](https://jms55.github.io/posts/2025-03-27-virtual-geometry-bevy-0-16) | Um draw indireto num visibility buffer R32Uint (26 bits de cluster, 6 de triângulo), oclusão em dois passes. | Encaixa no caminho indireto atual e elimina o limiar de 512 meshlets. A [Arm mostrou Nanite em Mali](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/mali-and-unreal-engine-s-nanite-enabling-the-future-of-mobile-graphics) sem mesh shaders: 18 M de triângulos em 26 ms contra 199 ms. | Médio. |
| **Rasterização de triângulos pequenos em compute** | Triângulos de poucos pixels escritos por atômicos de 64 bits (profundidade + id). | Exige atômicos de imagem de 64 bits, ausentes numa Vega 8. Empacotar profundidade de 16 bits em 32 gera artefatos graves ([Scthe](https://github.com/Scthe/nanite-webgpu)). Só depois dos dois itens acima, e com caminho alternativo. | Alto. |
| **Raios contra um corte mais grosso do mesmo DAG** | BLAS por nível, com regra de LOD consistente entre raio primário e secundário. | Resolve a tensão 5.2 (d) com memória limitada. O trabalho de [Wang et al., SIGGRAPH 2026](https://research.nvidia.com/labs/rtr/publication/wang2026levelofdetail/) trata do reúso de ReSTIR quando a topologia muda; só interessa se o Artful adotar reservatórios. | Médio. |

A CPU já rasteriza por tiles sem overdraw e a GPU já desenha por comandos indiretos, então o DAG entra como mudança na *escolha* do que desenhar, não nos rasterizadores. O ganho medido pelo próprio projeto com LOD manual (2 a 3×) sugere que o retorno é alto.

### 6.2 Luzes: das 16 + listas a amostragem estocástica

| Técnica | O que é | Adequação | Custo |
|---|---|---|---|
| **Clustered shading** — [Olsson, Billeter, Assarsson, HPG 2012](https://www.cse.chalmers.se/~uffe/clustered_shading_preprint.pdf)† | Fatias de profundidade nos tiles. | Extensão direta de `artfSceneLightTiles`. Remove a saturação das 64 entradas em corredores. | Baixo. |
| **Seleção estocástica por tile** — Lempiäinen, "Stochastic Tile-Based Lighting in HypeHype", [Advances 2025](https://advances.realtimerendering.com/s2025/index.html) (só o resumo foi lido) | Reamostragem por reservatório em dois níveis de tile, custo fixo, em GPU móvel de entrada. | É a publicação mais próxima da estrutura do Artful. Remove a distinção entre "as 16" e as demais. | Médio. |
| **MegaLights** — Narkowicz e Costa, [Advances 2025](https://advances.realtimerendering.com/s2025/index.html) · [doc](https://dev.epicgames.com/documentation/en-us/unreal-engine/megalights-in-unreal-engine) | Raios fixos por pixel para luzes amostradas por importância, traço curto em tela, depois RT, depois denoiser. | A Epic exige RT por hardware. A Arm rodou uma versão modificada em Mali-G1: ~41 fps contra ~8 com ~195 luzes pontuais com sombra ([Arm, jun/2026](https://developer.arm.com/community/arm-community-blogs/b/mobile-graphics-and-gaming-blog/posts/lighting-at-scale-bringing-hundreds-of-dynamic-lights-to-mobile-with-unreal-megalights)). O algoritmo mapeia para as árvores do Artful; o risco está no denoiser. | Médio a alto. |
| **ReSTIR DI** — [Bitterli et al., SIGGRAPH 2020](https://research.nvidia.com/publication/2020-07_spatiotemporal-reservoir-resampling-real-time-ray-tracing-dynamic-direct) · [curso de Wyman et al., 2023](https://intro-to-restir.cwyman.org/) | Reamostragem espaço-temporal: 6 a 60× mais rápido para o mesmo erro. | Precisa só de reservatórios, RNG por hash e raios de visibilidade. Determinístico se as sementes forem. | Médio. |
| **LTC para luzes de área** — [Heitz, Dupuy, Hill, Neubelt, SIGGRAPH 2016](https://eheitzresearch.wordpress.com/415-2/) | Integral exata do polígono sob a BRDF por transformação linear. | Substitui o ponto representativo nos retângulos (limite documentado). Exige `acos` determinístico, que o projeto já tem. | Baixo a médio. |

### 6.3 Iluminação indireta: escalar o que existe

| Técnica | O que é | Adequação | Custo |
|---|---|---|---|
| **DDGI para produção** — [Majercik, Marrs, Spjut, McGuire, JCGT 2021](https://arxiv.org/abs/2009.10796) | Máquina de estados que desliga probes inúteis, realocação para fora de paredes, viés único, volumes em cascata de várias resoluções. | Ataca os três limites documentados: todas as probes a cada quadro, caixa fixa, vazamento em parede fina. Melhor retorno por linha desta seção. | Baixo. |
| **Persistir as probes na cena** | — | Hoje recomeçam quando o renderizador é recriado. | Baixo. |
| **Brixelizer GI / GI-1.0** — [AMD](https://gpuopen.com/fidelityfx-brixelizer/) (MIT) | Cache de radiância sobre campos de distância esparsos, só compute, sem RT por hardware. | Arquiteturalmente o vizinho mais próximo, para um degrau acima das probes. | Alto. |
| **SHaRC** — [NVIDIA](https://github.com/NVIDIA-RTX/SHARC/blob/main/docs/Integration.md) | Grade de hash em espaço de mundo; ~4% dos pixels atualizam o cache. | Portável em conceito; atômicos e colisões quebram a exatidão entre backends (ver 5.2 g). | Alto. |
| **ReSTIR GI / PT** — [Ouyang et al. 2021](https://research.nvidia.com/publication/2021-06_restir-gi-path-resampling-real-time-path-tracing) · [Lin et al. 2022](https://graphics.cs.utah.edu/research/projects/gris/) · [ReSTIR PT Enhanced, I3D 2026](https://research.nvidia.com/labs/rtr/publication/lin2026restirptenhanced/) | Reúso de caminhos; a versão de 2026 é 2 a 3× mais rápida. | Qualidade de Lumen HQ, mas com mapeamentos de deslocamento e jacobianos. Pesquisa para depois da 1.0. | Alto. |
| **Radiance cascades em 3D** — [Split RC, Freeman, Sannikov, Pitt-Francis, arXiv 2607.20384](https://arxiv.org/abs/2607.20384) | Probes em hash esparso com divisão de raios. | Ainda em nível de pesquisa. O artigo original de Sannikov nunca foi submetido a periódico ([repositório](https://github.com/Raikiri/RadianceCascadesPaper)), então não deve ser citado como JCGT. | — |
| **Holographic Radiance Cascades** — [Freeman, Sannikov, Margel, arXiv 2505.02041](https://arxiv.org/abs/2505.02041) | Sem ruído nem cache temporal; 1,85 ms a 512² e 7,67 ms a 1024² numa RTX 3080 Laptop. | Para o **2D**: ataca o vazamento e o anel documentados. Só compute, determinístico. | Médio. |

### 6.4 Traçado e ruído: os fundamentos que faltam

Este é o grupo de menor custo e maior efeito.

| Técnica | O que resolve | Fonte |
|---|---|---|
| **Sobol com embaralhamento de Owen por hash** | Substitui o hash com rejeição. Só operações inteiras: idêntico em C e SPIR-V. Converge mais rápido em tudo que é estocástico. | [Burley, JCGT 2020](https://jcgt.org/published/0009/04/01/paper.pdf)† |
| **Ruído azul em tela / FAST** | Distribui o erro de modo que o filtro o remova. Uma textura pequena e dois XOR. | [Heitz et al., 2019](https://eheitzresearch.wordpress.com/762-2/) · [Donnelly et al., I3D 2024](https://www.ea.com/seed/news/spatio-temporal-sampling) |
| **Amostragem por importância do ambiente com MIS compensado** | Os vaga-lumes do sol pequeno no HDRI (limite documentado). Distribuição 2D por partes sobre imagem octaédrica de área igual, que é a parametrização que o Artful já usa. CDF construída uma vez, busca por inteiros. | [PBRT 4ª ed.](https://www.pbr-book.org/4ed/Light_Sources/Infinite_Area_Lights) |
| **Amostragem de normais visíveis por calotas esféricas** | Amostragem correta do lóbulo GGX no lugar da escolha espelho/difuso. | [Dupuy e Benyoub, 2023](https://arxiv.org/abs/2306.05044) |
| **Cones de raio** | Mipmap correto no traçado (hoje lê o maior). ~10% menos tempo de quadro num path tracer de GPU. | [Akenine-Möller et al., JCGT 2021](https://research.nvidia.com/publication/2021-04_improved-shader-and-texture-level-detail-using-ray-cones) |
| **A-SVGF** | Gradientes temporais por re-sombreamento com a mesma sequência aleatória: menos atraso e fantasma que histórico de 256 quadros. Exige sementes reproduzíveis, que o Artful tem. | [Schied, Peters, Dachsbacher, 2018](https://cg.ivd.kit.edu/atf.php) |
| **Denoiser dedicado para sombras** | Penumbra de 4 raios sem TAA hoje é padrão 2×2. O SIGMA do NRD custa 0,40 ms a 1440p numa RTX 4080; serve de referência de desenho, não de dependência. | [NRD](https://github.com/NVIDIA-RTX/NRD) |
| **BVH largo e quantizado (4 ou 8 filhos)** | 1,9 a 2,1× na travessia incoerente com 35 a 60% da memória. Com otimização por reinserção após a construção. | [Ylitie, Karras, Laine, HPG 2017](https://research.nvidia.com/publication/2017-07_efficient-incoherent-ray-traversal-gpus-through-compressed-wide-bvhs) · [tinybvh](https://github.com/jbikker/tinybvh) |

### 6.5 Reconstrução temporal

| Técnica | O que resolve | Fonte |
|---|---|---|
| **Travas para detalhe fino, máscaras reativa e de composição, reprojeção Lanczos** | A cintilação abaixo de 0,5 e a cor velha em transparências. O FSR 2 é a referência aberta; custo reportado de 0,6 ms (RX 7900 XTX) a 4,4 ms (RX 590) em 4K modo Performance. | [FSR 2](https://github.com/GPUOpen-Effects/FidelityFX-FSR2) |
| **Arm ASR** | Derivado do FSR 2 e otimizado para móvel; referência de orçamento para Android e GPU integrada. | [Arm ASR](https://github.com/arm/accuracy-super-resolution) (MIT) |
| **Detector de cintilação e ressurreição de histórico** | Ideias baratas do TSR: relaxar a rejeição quando o luma oscila em ~3 quadros; reusar quadros mais antigos em desoclusão. | [TSR](https://dev.epicgames.com/documentation/en-us/unreal-engine/temporal-super-resolution-in-unreal-engine) |
| **Antialiasing especular geométrico** | MSAA não filtra o brilho dentro do triângulo (limite documentado). Alarga a rugosidade pelas derivadas da normal: ~10 linhas de shader. | [Tokuyoshi e Kaplanyan, JCGT 2021](https://www.jcgt.org/published/0010/02/02/)† |

Upscalers neurais (FSR 4, DLSS 4, XeSS 3, Arm NSS) são fechados ou dependem de hardware de 2026 em diante. Não cabem nas regras do projeto.

### 6.6 Materiais e cor

| Técnica | O que resolve | Fonte |
|---|---|---|
| **sRGB exato, gradação em ACEScct** | Tensão 5.2 (e). ACEScct é um log2 com pé linear: trivial com `artfLog2Fast`. | [ACEScct](https://docs.acescentral.com/encodings/acescct/) |
| **Curvas de tom atuais** | Khronos PBR Neutral (analítica, ~10 linhas, fiel às cores de material) e uma LUT estilo Tony McMapface ou ACES 2.0 assada. | [PBR Neutral](https://github.com/KhronosGroup/ToneMapping/blob/main/PBR_Neutral/README.md) · [Tony](https://github.com/h3r2tic/tony-mc-mapface) · [ACES 2.0](https://docs.acescentral.com/system-components/output-transforms/technical-details/rendering-overview/) |
| **Saída HDR e gama larga** | `VK_EXT_swapchain_colorspace` (HDR10, scRGB, Display-P3); a aplicação faz a codificação. No Android, o alvo seguro é Display-P3 de 10 bits, enumerado em execução. O backend de CPU precisa de formato de saída de 10 bits ou float. | [Vulkan](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_swapchain_colorspace.html) · [Android](https://developer.android.com/training/wide-color-gamut) |
| **Compensação de espalhamento múltiplo** | Metais rugosos escurecem hoje. A forma de Turquin é uma multiplicação, 1 + F0·(1/E − 1), reutilizando a tabela existente. | [Turquin 2019](https://blog.selfshadow.com/publications/turquin/ms_comp_final.pdf)† · [Fdez-Agüera, JCGT 2019](https://jcgt.org/published/0008/01/03/)† |
| **OpenPBR como especificação-alvo do grafo** | Versão 1.1.2 de outubro de 2026: mistura metal/dielétrico, difuso EON, coat com escala de albedo, filme fino (recomenda Belcour-Barla). Dá ao grafo em C um modelo em camadas comparável ao Substrate, com especificação pública. | [OpenPBR](https://academysoftwarefoundation.github.io/OpenPBR/) |
| **Iridescência de Belcour e Barla** | Limite documentado; é o modelo que `KHR_materials_iridescence` referencia. | [Belcour e Barla, 2017](https://belcour.github.io/blog/research/publication/2017/05/01/brdf-thin-film.html) |
| **Subsuperfície em espaço de tela** | Difusão de verdade no lugar do envolvimento. SSS separável são dois passes 1D. A difusão normalizada de Burley tem uma patente associada (US9659404, encontrada na busca); verificar antes de adotar. | [Jimenez et al., 2015](https://iryoku.com/separable-sss)† |
| **Ler as texturas das extensões glTF** | Hoje só os fatores de specular, iridescence, anisotropy e volume. É conformidade, não pesquisa. | [registro glTF](https://github.com/KhronosGroup/glTF/blob/main/extensions/README.md) |

### 6.7 Transparência, volumes, sombras, texturas

| Problema | Técnica | Observação |
|---|---|---|
| Listas de 8 fragmentos a 96 bytes/pixel (~190 MiB em 1080p) | **Transmitância em froxels** ([Drobot, AVBOIT, Advances 2025](https://research.activision.com/publications/2026/adaptive-voxel-based-order-independent-transparency)) ou **OIT por momentos** com 4 momentos ([Münstermann et al., I3D 2018](https://momentsingraphics.de/I3D2018.html)) | A primeira reaproveita a grade de névoa existente e tem memória fixa. É também o método que a Unreal adotou como experimental. Ambas determinísticas. |
| Céu sem perspectiva aérea sobre a cena | LUT de perspectiva aérea em froxels, de [Hillaire, EGSR 2020](https://sebh.github.io/publications/egsr2020.pdf) | As outras LUTs do artigo já existem em `sky.c`. |
| Nuvens só na imagem de ambiente | Marcha a um quarto da resolução com reprojeção, composta contra a profundidade da cena ([Hillaire 2016](https://www.ea.com/frostbite/news/physically-based-sky-atmosphere-and-cloud-rendering)) | O ruído e a iluminação já estão escritos. |
| Sombra do sol em mundos grandes | Clipmaps do sol com cache e separação estático/dinâmico no atlas atual | Degrau barato em direção a [Virtual Shadow Maps](https://dev.epicgames.com/documentation/en-us/unreal-engine/virtual-shadow-maps-in-unreal-engine). VSM completo só compensa com geometria em clusters. |
| Oclusão na CPU com célula de 16×16 | **Masked Software Occlusion Culling** ([Hasselgren, Andersson, Akenine-Möller, HPG 2016](https://www.intel.com/content/www/us/en/developer/articles/technical/masked-software-occlusion-culling.html)) | 3× mais rápido que o anterior e 98% da eficácia de um buffer completo. Conservador: não altera pixels. |
| Teto de 1.024 texturas | Consultar o limite real do dispositivo; a longo prazo, texturas virtuais esparsas | SVT remove o teto e a tabela de páginas é dado puro, mas o custo é alto e cada amostra passa a custar duas leituras. |
| Splats gaussianos | `KHR_gaussian_splatting` foi [ratificada](https://github.com/KhronosGroup/glTF/tree/main/extensions/2.0/Khronos/KHR_gaussian_splatting) em 2026 | Mistura ordenada é difícil de manter idêntica entre backends e pesada em móvel. Baixa prioridade; acompanhar. |

---

## 7. Sequência recomendada

Sem prazos: a ordem é o que importa. Tamanho: P (dias), M (semanas), G (meses).

**Fase A — fechar a 1.0 (estabilidade).**

| # | Entrega | Tam. |
|---|---|---|
| A1 | Contrato: compatibilidade, camadas de suporte, handles de malha com geração, política de VRAM esgotada | M |
| A2 | Alvos de fuzzing para os seis leitores; corpus glTF-Sample-Assets e gltfpack | P |
| A3 | Porte do FLIP; imagens de referência; CTest por grupo, com *skipped* | M |
| A4 | Pré-rotação no Android; `VK_EXT_device_fault`; probes persistidas na cena | P |
| A5 | Runner com GPU real; laço em aparelhos; ensaio de horas | M |
| A6 | sRGB exato e gradação em log. Muda bytes de todos os quadros, portanto tem de entrar antes de congelar a API e as referências | P |

**Fase B — qualidade barata (pode intercalar com A).**

| # | Entrega | Tam. |
|---|---|---|
| B1 | Sobol/Owen + ruído azul; importância do ambiente com MIS; VNDF; cones de raio | M |
| B2 | Antialiasing especular; compensação de energia; texturas das extensões glTF | P |
| B3 | Estados, realocação e cascatas de probes | M |
| B4 | Fatias de profundidade nos tiles de luz; LTC | M |
| B5 | Travas e máscaras no upscaler; detector de cintilação | M |
| B6 | Transcodificadores Basis e Zstandard | M |

**Fase C — alcance de conteúdo (depois da 1.0).**

| # | Entrega | Tam. |
|---|---|---|
| C1 | Ferramenta de cozimento e cache versionado | M |
| C2 | DAG de clusters: construção, depois seleção do corte nos dois backends | G |
| C3 | Raios contra corte grosso do DAG; BVH largo quantizado | M |
| C4 | Luzes estocásticas por tile com denoiser de sombra; A-SVGF | G |
| C5 | Transparência por transmitância em froxels; saída HDR | M |

**Fase D — alcance de plataforma.**

| # | Entrega | Tam. |
|---|---|---|
| D1 | IR de shaders com emissores SPIR-V e MSL; grafo de quadro | G |
| D2 | Metal executado e promovido; depois WGSL/WebGPU | G |

D1 é independente de C e pode começar cedo se Apple for requisito comercial. O WebGPU continua como rascunho de recomendação candidata no W3C, mas já está em Chrome, Firefox (Windows e macOS ARM) e Safari 26 ([web.dev](https://web.dev/blog/webgpu-supported-major-browsers)).

---

## 8. O que não fazer agora

- **Técnicas neurais** (upscalers, compressão neural de texturas, materiais neurais, cache neural de radiância). O RTXNTC é beta, sob licença fechada, e depende de extensão de fornecedor não ratificada; o caminho DX12 traz aviso de não distribuir†. São incompatíveis com GPU integrada antiga e com a paridade de bytes.
- **Virtual Shadow Maps e texturas virtuais completos** antes de geometria em clusters.
- **Estruturas de aceleração por cluster** (`VK_NV_cluster_acceleration_structure`, CLAS/PTLAS do DXR): só fornecedor ou desktop recente.
- **Draco, cabelos por fios, volumes heterogêneos.** Custo alto, público pequeno.
- **Perseguir paridade recurso a recurso com a Unreal.** O argumento do Artful é rodar igual em todo lugar sem dependências. Cada recurso deveria entrar por demanda de um consumidor concreto.

---

## 9. Limites deste relatório

- Nenhum teste ou medição foi executado para este documento; os números de desempenho do Artful vêm dos documentos do projeto e os das técnicas, dos autores, em hardware e implementações deles.
- As estimativas de tamanho são julgamento, não cronograma.
- As notas de versão oficiais da Unreal 5.7 e 5.8 não puderam ser lidas. Os rótulos de Nanite, Lumen, TSR, path tracer, virtual texturing e World Partition na tabela da seção 3 são "em produção, rótulo não confirmado". A descrição do Lumen Lite como campos de irradiância vem de um trecho de busca.
- Fontes marcadas com † não foram abertas. Entre elas: os slides de Nanite e Lumen, Burley 2020, Olsson 2012, Fdez-Agüera 2019, Tokuyoshi 2021.
- Não foram encontrados: dados por dispositivo sobre limites de descriptor indexing em Android, artigo de 2023 a 2026 sobre rasterização por software em CPU, nem análise independente de Virtual Shadow Maps.
