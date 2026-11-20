# TODO — onde estamos no plano de correção

Mapa curto do [plano](plano-de-correcao.md): o que está feito, o que está em parte e o que falta, por fase. Os
detalhes, números e decisões estão nas tabelas "Estado da Fase N" do plano e no [CHANGELOG](../CHANGELOG.md); aqui só
o suficiente para se localizar. Atualizar a cada passo fechado.

Legenda: `[x]` feito · `[~]` em parte (o que falta vem ao lado) · `[ ]` não iniciado · 🔒 depende de decisão do dono ·
📱 depende de hardware ou máquina que não está aqui · 🌐 depende de baixar algo (pedir antes)

Branch: `unreal-parity`. Última CI conferida: run 44756688958 (Android passa; Linux com segfault ainda sem teste
identificado; macOS com `scale/skinLod`). Run 44764119231 em andamento, com `ARTFUL_TEST_TRACE` para apontar o teste.

## Fase 0 — Base reproduzível e instrumentos
- [x] 0.1 Versionar o trabalho em andamento
- [x] 0.2 Um teste do CTest por caso
- [x] 0.3 Omitido (77) ≠ aprovado; `ARTFUL_REQUIRE`
- [~] 0.4 FLIP em C — falta comparar com o programa da NVIDIA (±1e-4) e trocar limiares "% de pixels" por FLIP
- [~] 0.5 Linha de base de desempenho — falta JSON no `artful-render2d`

## Fase 1 — Contrato público
- [x] 1.1 Handles de malha com geração
- [~] 1.2 Falta de memória na GPU — feito para texturas; malhas, instâncias e buffers do quadro ainda derrubam o renderizador
- [x] 1.3 Callback de eventos
- [ ] 1.4 Política de ABI 🔒 — rascunho em `docs/api.md` ("What 1.x will keep (proposed)")
- [x] 1.5 Contrato de threads
- [x] 1.6 Camadas de suporte publicadas
- [~] 1.7 Mistura de clipes — feito na CPU; nada a fazer na GPU (pose é calculada na CPU)

## Fase 2 — Mudanças que alteram bytes
- [x] 2.1 sRGB exato (polinômios) · [x] curvas de tom (Neutral já existia)
- [ ] 2.1 Gradação em ACEScct 🔒 — muda o sentido de lift/gamma/gain e das LUTs existentes
- [x] 2.2 Sobol com Owen
- [ ] 2.2 Ruído azul entre pixels
- [ ] 2.2 Probes com Sobol (hoje hash; GPU e CPU juntas)
- [x] 2.3 Ambiente por importância + MIS · [x] VNDF + MIS especular · [x] cones de raio
- [ ] 2.3 MIS para luzes com tamanho
- [x] 2.4 Compensação de energia (Turquin) · [x] antialiasing especular
- [ ] 2.4 Iridescência de Belcour-Barla · camadas OpenPBR · SSS (listados também em 5.12)

## Fase 3 — Robustez e prova (fecha a 1.0)
- [~] 3.1 Fuzzing — 9 alvos (8 leitores + `cook`), 7 defeitos achados e corrigidos; falta pôr na CI e 24 h por alvo
- [ ] 3.2 Corpus de conformidade glTF 🌐 (glTF-Sample-Assets)
- [ ] 3.3 Pré-rotação no Android 📱
- [x] 3.4 `VK_EXT_device_fault` (perda real não forçada aqui)
- [x] 3.5 Probes persistidas
- [ ] 3.6 Ritmo e térmica 📱
- [ ] 3.7 Hardware real na CI 📱 (runner próprio, Firebase Test Lab)
- [~] 3.8 Ensaio longo — ferramenta feita; repetir as 4 h com a suspensão do Windows desligada
- [ ] 3.9 Linux com GPU e Mac 📱
- [ ] CI: segfault no Linux (lavapipe) — esperar o nome do teste da run 44764119231; lavapipe local exige instalar `mesa` 🌐
- [ ] CI: macOS `scale/skinLod` — 2 níveis no arm64 contra 3 no x86, primeiro de erro 0; investigar no Mac 📱

## Fase 4 — Arquitetura
- [~] 4.1 IR de shaders
  - [x] passos 1–2: IR, emissor SPIR-V, todos os 103 shaders com as mesmas palavras; `irFromSpirv`, `irUses`
  - [ ] passo 3: DebugInfo (`NonSemantic.Shader.DebugInfo.100`)
  - [~] passo 4: emissor MSL feito (94 de 103; os 17 do Metal ligam como os do SPIRV-Cross); falta passar no compilador da Apple (CI) e compilar em execução no `metal.c`
  - [ ] passo 5: WGSL (Fase 6)
- [~] 4.2 Grafo de quadro
  - [x] passo 1: descrição a partir do IR, `--graph`, medição
  - [~] passo 2: barreiras do pós a partir dos acessos dos shaders; faltam os passes da cena
  - [~] passo 3: alvos só dos efeitos ligados e oclusão/velocidades num buffer; faltam aliasing imagem↔buffer (+8%) e luz em 16 bits 🔒 (~22%) para os 30% com tudo ligado
  - [ ] passo 4: backend de CPU percorrendo o grafo; dividir o `vulkan.c` por passe
- [x] 4.3 Cozimento — HLOD (`artfSceneSaveHlod`), glTF (`artfPrepareModelCooked`), `artful-cook`, alvo de fuzzing

## Fase 5 — Alcance de conteúdo
- [ ] 5.1 Probes escaláveis ← **próxima**
- [x] 5.2 Luzes em clusters — 0 omitidas no corredor (antes 1.846), 0,09 ms a 1080p
- [x] 5.3 LTC para retângulos e tubos — retângulo 54–58% → 4–9% do traçado, tubo 22% → 0,5%
- [~] 5.4 Upscaler — critério atingido (grade −60%, fumaça 10 → 2 quadros); faltam máscaras reativa/composição e Lanczos
- [ ] 5.5 DAG de clusters
- [ ] 5.6 Raios contra o corte grosso; BVH largo
- [ ] 5.7 Luzes estocásticas, denoiser de sombra, A-SVGF
- [ ] 5.8 Transparência por transmitância em froxels
- [ ] 5.9 Saída HDR e gama larga
- [ ] 5.10 Basis Universal e Zstandard
- [ ] 5.11 Perspectiva aérea e nuvens a 1/4
- [ ] 5.12 Materiais (texturas das extensões, iridescência, OpenPBR, SSS)

## Fase 6 — Alcance de plataforma
- [ ] 6.1 Metal executado 📱 (depende de 4.1 passo 4 provado)
- [ ] 6.2 WebGPU (emissor WGSL e backend)
- [ ] 6.3 Caminho sem descriptor indexing

## Ferramentas de verificação montadas nesta máquina
- Validação Vulkan: camada do MSYS2 (`VK_ADD_LAYER_PATH=C:\msys64\ucrt64\bin`); a sincronização liga com
  `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true` (a variável antiga não faz nada na 1.4)
- Fuzzing: `-DARTFUL_FUZZ=ON` com o clang do MSYS2 (`tools/fuzz/`)
- MSL: `tools/msl/check.py` (sintaxe e tipos) e `tools/msl/compare.py` (convenções do SPIRV-Cross)
- Memória: `artful-soak`; `ArtfFrameStats.gpuBytes`; `artful-render --graph`
