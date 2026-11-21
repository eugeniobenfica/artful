# TODO — o que falta do plano de correção

Só o que resta fazer ou está incompleto no [plano](plano-de-correcao.md). O que já foi feito sai daqui: os detalhes,
números e decisões ficam nas seções "Estado" do plano e no [CHANGELOG](../CHANGELOG.md). Atualizar a cada passo fechado.

Legenda: `[~]` em parte (o que falta vem ao lado) · `[ ]` não iniciado · 📱 depende de hardware ou máquina que não está
aqui · 🌐 depende de baixar algo (pedir antes)

Branch: `unreal-parity`. Última CI conferida: run 44945833108 (21/11/2026): Windows, Android e macOS passam, com o Metal
rodando no macOS; os fuzzers passam. O Linux cai no lavapipe do runner em `layers/layersTransmittance` (abaixo). O último
push é o 0cf336a.

## CI
- [ ] Linux: conferir no runner se `layers/layersTransmittance` e `layersBehindSolid` (camadas na GPU, da 5.8) ainda
  caem no lavapipe dele; o lavapipe do MSYS2 roda os dois limpos, também sob validação. A causa dos outros 18 que a CI
  pulava foi achada e corrigida (ray queries lendo a interseção committed sem acerto): a CI não pula mais nada, falta um
  push para conferir

## Fase 3 — Robustez e prova
- [~] 3.3 Pré-rotação no Android — falta ver num aparelho 📱
- [~] 3.6 Ritmo e térmica — faltam 30 min num telefone 📱
- [~] 3.9 Linux com GPU e janela, sem máquina 📱

## Fase 5 — Alcance de conteúdo
- [~] 5.6 Reinserção na árvore — falta medir numa GPU integrada 📱
- [~] 5.7 Gradientes da A-SVGF (CPU, no stash "A-SVGF temporal gradients") — ainda quebram com objetos em movimento e
  sombra suave: tirar dos gradientes os pixels de instâncias que se moveram e normalizar pelo ruído da amostra
- [~] 5.9 Saída HDR — faltam metadados HDR, scRGB e teste numa tela HDR 📱
- [~] 5.10 Basis Universal — falta transcodificar para blocos da GPU (hoje vai para pixels)
- [~] 5.12 OpenPBR (camadas no grafo de materiais em C) — no stash "5.12 OpenPBR": API, `graph.c`, flags do material e
  rasterização da CPU feitos; faltam o traçador da CPU, os shaders da GPU (fragmento e traçador), o teste e a
  documentação

## Fase 6 — Alcance de plataforma
- [~] 6.1 Metal — rodou só na CI (Apple Paravirtual device) e só `backends/metal`; faltam os demais testes de imagem
  nele (o quadro em floats, probes, traçado e camadas não existem no Metal), um Mac de verdade e uma janela 📱
- [~] 6.2 WebGPU e CPU em WebAssembly — falta o Safari, nos dois caminhos 📱; o WebGPU tem só o quadro simples (sem
  quadro em floats, probes, traçado, camadas nem materiais em C)

## Ferramentas de verificação montadas nesta máquina
- Validação Vulkan: camada do MSYS2 (`VK_ADD_LAYER_PATH=C:\msys64\ucrt64\bin`); a sincronização liga com
  `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true` (a variável antiga não faz nada na 1.4)
- lavapipe: `VK_DRIVER_FILES=C:\msys64\ucrt64\share\vulkan\icd.d\lvp_icd.x86_64.json`
- Fuzzing: `-DARTFUL_FUZZ=ON` com o clang do MSYS2 (`tools/fuzz/`)
- MSL: `tools/msl/check.py` (sintaxe e tipos)
- WGSL: `tools/wgslCheck.c` (naga, do wgpu-native) e `tools/web/wgslCheck.html` (Tint, no navegador)
- Web: Emscripten do MSYS2 (`emcmake`, com `BINARYEN_CORES=1`), node para os testes, `tools/web/serve.py`
- Memória: `artful-soak`; `ArtfFrameStats.gpuBytes`; `artful-render --graph`
