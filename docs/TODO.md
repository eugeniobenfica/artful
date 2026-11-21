# TODO — onde estamos no plano de correção

Mapa curto do [plano](plano-de-correcao.md): o que está feito, o que está em parte e o que falta, por fase. Os
detalhes, números e decisões estão nas tabelas "Estado da Fase N" do plano e no [CHANGELOG](../CHANGELOG.md); aqui só
o suficiente para se localizar. Atualizar a cada passo fechado.

Legenda: `[x]` feito · `[~]` em parte (o que falta vem ao lado) · `[ ]` não iniciado · 🔒 depende de decisão do dono ·
📱 depende de hardware ou máquina que não está aqui · 🌐 depende de baixar algo (pedir antes)

Branch: `unreal-parity`. Última CI conferida: run 44764119231 (Android e Windows passam; Linux cai em `post/floats`, na GPU,
num efeito ainda não identificado no lavapipe; macOS com `scale/skinLod`). O último push é anterior à 5.2.

## Fase 0 — Base reproduzível e instrumentos
- [x] 0.1 Versionar o trabalho em andamento
- [x] 0.2 Um teste do CTest por caso
- [x] 0.3 Omitido (77) ≠ aprovado; `ARTFUL_REQUIRE`
- [x] 0.4 FLIP em C — bate com a NVIDIA a 1e-5; 40 comparações CPU×GPU com limite FLIP (calibrado numa máquina; CI coleta as outras)
- [x] 0.5 Linha de base de desempenho — `--json` no 3D e no 2D, `tools/bench.py --render2d` (2D na CPU varia 4,3%, acima dos 3%)

## Fase 1 — Contrato público
- [x] 1.1 Handles com geração — malhas e texturas
- [x] 1.2 Falta de memória na GPU — texturas (com residência), malhas recusadas, instâncias/quadro pulam e se recuperam
- [x] 1.3 Callback de eventos
- [x] 1.4 Política de ABI — compatibilidade de fonte (decisão do dono)
- [x] 1.5 Contrato de threads — 3D e 2D
- [x] 1.6 Camadas de suporte publicadas
- [x] 1.7 Mistura de clipes — `artfAnimationMix`; a pose é calculada na CPU para os dois backends

## Fase 2 — Mudanças que alteram bytes
- [x] 2.1 sRGB exato (polinômios) · [x] curvas de tom (Neutral já existia)
- [x] 2.1 Gradação em ACEScct (`gradeShown` mantém a de antes)
- [x] 2.2 Sobol com Owen
- [x] 2.2 Ruído azul entre pixels
- [x] 2.2 Probes com Sobol (GPU e CPU juntas)
- [x] 2.3 Ambiente por importância + MIS · [x] VNDF + MIS especular · [x] cones de raio
- [x] 2.3 MIS para luzes com tamanho
- [x] 2.4 Compensação de energia (Turquin) · [x] antialiasing especular
- [→] 2.4 Iridescência de Belcour-Barla · camadas OpenPBR · SSS: movidos para 5.12 (a Fase 2 fecha sem eles)

## Fase 3 — Robustez e prova (fecha a 1.0)
- [~] 3.1 Fuzzing — 9 alvos, 7 defeitos corrigidos, job no CI (10 min por alvo a cada push); faltam as 24 h por alvo
- [-] 3.2 Corpus de conformidade glTF — pulado pelo dono
- [~] 3.3 Pré-rotação no Android — `preRotate`, verificada por gancho de teste; falta ver num aparelho 📱
- [x] 3.4 `VK_EXT_device_fault` (perda real não forçada aqui)
- [x] 3.5 Probes persistidas
- [~] 3.6 Ritmo e térmica — `artfRendererSetPacing`, folga térmica, espera ativa medida (20.000 → 2.000 voltas); faltam 30 min num telefone 📱
- [-] 3.7 Hardware real na CI — pulado pelo dono
- [~] 3.8 Ensaio longo — ferramenta feita; repetir as 4 h com a suspensão do Windows desligada
- [~] 3.9 Mac com CPU passa inteiro no CI; Linux com GPU e janela sem máquina 📱
- [~] CI: lavapipe — rodados um a um, só 11 testes caem: os que percorrem as árvores enquanto a cena é desenhada (probes na GPU, sombras e reflexos por raios). Não é leitura fora do buffer, pilha nem textura; causa não achada. O CI os pula por `ARTFUL_SKIP`, com o motivo no workflow; o resto do Vulkan roda lá. Limite de FLIP do upscale recalibrado com o lavapipe
- [x] CI: macOS `scale/skinLod` — os testes compilavam sem `-ffp-contract=off`; corrigido, o Mac passa inteiro
- [x] CI: Metal pelo IR — `postFinal` declarava `thread_position_in_grid` num fragment; entradas não lidas ficam de fora

## Fase 4 — Arquitetura
- [~] 4.1 IR de shaders
  - [x] passos 1–2: IR, emissor SPIR-V, todos os 103 shaders com as mesmas palavras; `irFromSpirv`, `irUses`
  - [x] passo 3: DebugInfo (`ARTFUL_SHADER_DEBUG`, `artful-shaders --debug`; por função, não por linha)
  - [~] passo 4: emissor MSL feito (94 de 103); o compilador da Apple na CI achou dois defeitos, corrigidos; falta compilar em execução no `metal.c`
  - [x] passo 5: WGSL (Fase 6.2)
- [x] 4.2 Grafo de quadro
  - [x] passo 1: descrição a partir do IR, `--graph`, medição
  - [x] passo 2: barreiras do pós pelos acessos dos shaders, e as da cena por um rastreador de acessos (`trackUse`)
  - [x] passo 3: luz em 16 bits e aliasing imagem↔buffer — −31% com tudo ligado a 1080p (meta 30%)
  - [x] passo 4: `vulkan.c` dividido sem perda de desempenho; os dois backends conferidos contra um grafo de passes comum (`render/framePasses.c`)
- [x] 4.3 Cozimento — HLOD (`artfSceneSaveHlod`), glTF (`artfPrepareModelCooked`), `artful-cook`, alvo de fuzzing

## Fase 5 — Alcance de conteúdo
- [x] 5.1 Probes escaláveis — estados e histerese adaptativa (critério atingido); volumes que seguem a câmera (`follow`), em cascata (`cascades`, até 4); realocação descartada
- [x] 5.2 Luzes em clusters — 0 omitidas no corredor (antes 1.846), 0,09 ms a 1080p
- [x] 5.3 LTC para retângulos e tubos — retângulo 54–58% → 4–9% do traçado, tubo 22% → 0,5%
- [x] 5.4 Upscaler — critério atingido (grade −60%, fumaça 10 → 2 quadros); histórico cúbico (`sharpHistory`); máscara reativa (`reactive`); composição de camadas de fora
- [x] 5.5 DAG de clusters — construção, corte, desenho nos dois backends e cozimento, junto com os 3 níveis; sem ganho em `city`/`heavy` (nada lá pede o grafo); erro por atributo de fora
- [x] 5.6 Raios contra o corte grosso (`ArtfTraceDesc.coarse`); sombras saem pelo erro do nível de cada instância, sem o 1/1000 (autossombreamento 100% → 0%); árvore de 4 filhos quantizada na GPU (−40–48% de memória); reinserção — sem GPU integrada para medir
- [~] 5.7 Luzes estocásticas — sombras por raio para todas as luzes; amostragem (`sampled`) com custo constante; denoiser (acúmulo temporal + 5×5): 0,2–0,4% dos pixels fora, FLIP 0,06–0,09 contra todas as luzes, também com a câmera em movimento; ReSTIR temporal testado e descartado (piora com o acumulador); filtro da SVGF (momentos, variância, 5 passes à-trous) nos dois denoisers do traçador, com erro menor em todos os testes (0,91 contra 1,13 com 4 amostras); gradientes da A-SVGF (CPU, em stash) ainda quebram com objetos em movimento e sombra suave
- [x] 5.8 Transparência por transmitância — `layers-sorted` por peso, com a transmitância de uma grade 8×8 px × 64 fatias, nos dois backends; exato para camadas paralelas; GPU 1080p 0,41 ms contra 0,87 das listas (4 amostras: 0,91 contra 1,29); memória 49 MB contra 207 (4 amostras: 174); só quadros de 8 bits, como antes
- [~] 5.9 Saída HDR e gama larga — `output` P3/HDR10 nos dois backends, swapchain por `VK_EXT_swapchain_colorspace`; faltam metadados HDR, scRGB e teste em tela HDR
- [x] 5.10 Basis Universal e Zstandard — Zstd (RFC 8878) e ETC1S/UASTC para pixels, idênticos ao transcodificador do basisu nos 46 KTX2 do glTF-Sample-Assets (472 níveis); `KHR_texture_basisu` no glTF; falta transcodificar para blocos da GPU
- [x] 5.11 Perspectiva aérea (`ArtfSkyDesc.aerial`, forma fechada, os dois backends, desenhado e traçado); nuvens na cena, numa grade de colunas a cada 4×4 px até o que é desenhado, compostas por profundidade (uma torre atravessa as nuvens; GPU = CPU; 0,59 ms GPU / 4,8 ms CPU em 1080p); só em quadros em floats
- [~] 5.12 Materiais — texturas das extensões (especular, iridescência, anisotropia) feitas; Belcour-Barla feito (igual à referência do glTF a 2e-6); SSS separável feito (duas gaussianas, não Burley: patente da Disney; borda de sombra 1 → 10 px, GPU = CPU, 0,12 ms GPU em 1080p); falta OpenPBR

## Fase 6 — Alcance de plataforma
- [ ] 6.1 Metal executado 📱 (depende de 4.1 passo 4 provado)
- [~] 6.2 WebGPU (emissor WGSL e backend)
  - [x] CPU em WebAssembly com SIMD (`kernelsWasm.c`), threads em workers; suíte da CPU passa no node; `spheres` no Chromium a 3,0 ms (960×540, 16 threads); Safari não testado
  - [x] emissor WGSL (`irWgsl`): os 79 shaders que podem ser WGSL passam no naga (wgpu-native, `tools/wgslCheck.c`) e no Tint (Chromium, `tools/web/wgslCheck.html`); os de fora leem texturas por índice não uniforme (o WebGPU usará `ARTF_TEXTURES_UNIFORM`), PrimitiveId ou ray queries
  - [ ] backend sobre `webgpu.h`
- [x] 6.3 Caminho sem descriptor indexing — árvore de ifs por índice constante, gradientes tomados antes; nenhuma funcionalidade pedida; até 64 texturas; suíte inteira passa forçada (`ARTFUL_UNIFORM_TEXTURES`); 40 texturas iguais ao caminho normal (0,000%)

## Ferramentas de verificação montadas nesta máquina
- Validação Vulkan: camada do MSYS2 (`VK_ADD_LAYER_PATH=C:\msys64\ucrt64\bin`); a sincronização liga com
  `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true` (a variável antiga não faz nada na 1.4)
- Fuzzing: `-DARTFUL_FUZZ=ON` com o clang do MSYS2 (`tools/fuzz/`)
- MSL: `tools/msl/check.py` (sintaxe e tipos) e `tools/msl/compare.py` (convenções do SPIRV-Cross)
- Memória: `artful-soak`; `ArtfFrameStats.gpuBytes`; `artful-render --graph`
