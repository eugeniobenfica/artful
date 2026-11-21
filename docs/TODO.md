# TODO — o que falta do plano de correção

Só o que resta fazer ou está incompleto no [plano](plano-de-correcao.md). O que já foi feito sai daqui: os detalhes,
números e decisões ficam nas seções "Estado" do plano e no [CHANGELOG](../CHANGELOG.md). Atualizar a cada passo fechado.

Duas listas: o que dá para fazer com o que esta máquina e a CI já têm, e o que depende de hardware, de um sistema ou de
uma ferramenta que não está aqui. Legenda: `[~]` em parte (o que falta vem ao lado) · `[ ]` não iniciado.

Branch: `unreal-parity`. Última CI conferida: run 44948960739 (21/11/2026): Windows, Android e macOS passam, com o Metal
rodando no macOS; os fuzzers passam. O Linux cai só nas camadas da 5.8, no lavapipe do runner (abaixo).

## Dá para fazer aqui

### CI
- [ ] A queda do Linux em `2d/vulkanFrames`, no código gerado pelo llvmpipe (não reproduz aqui; o passo de falha da
  CI roda o teste com `ARTFUL_2D_STEP=1`, que nomeia o shader, e com `ARTFUL_ROBUST=1`). As camadas e as ray queries,
  que caíam antes, foram corrigidas e passam na CI.

### Fase 5 — Alcance de conteúdo
- [~] 5.10 Basis Universal — falta transcodificar para blocos da GPU (hoje vai para pixels)
- [~] 5.12 OpenPBR (camadas no grafo de materiais em C) — no stash "5.12 OpenPBR": API, `graph.c`, flags do material e
  rasterização da CPU feitos; faltam o traçador da CPU, os shaders da GPU (fragmento e traçador), o teste e a
  documentação

### Fase 6 — Alcance de plataforma
- [~] 6.1 Metal — tem só o quadro simples: faltam o quadro em floats, as probes, o traçado e as camadas, e rodar nele os
  demais testes de imagem (dá para conferir pela CI do macOS, no dispositivo virtual)
- [~] 6.2 WebGPU — tem só o quadro simples: faltam o quadro em floats, as probes, o traçado, as camadas e os materiais
  em C (conferidos aqui no wgpu-native e no Chromium)

## Depende de hardware ou ferramenta que não está aqui
- [~] 3.3 Pré-rotação no Android — ver num aparelho (o gancho de teste já confere)
- [~] 3.6 Ritmo e térmica — 30 min num telefone
- [~] 3.9 Linux com GPU de verdade e janela — uma máquina Linux
- [~] 5.6 Reinserção na árvore — medir numa GPU integrada
- [~] 5.9 Saída HDR — testar numa tela HDR (HDR10 e scRGB, com os metadados) e o Display-P3 de 10 bits no Android
- [~] 6.1 Metal — um Mac de verdade (só rodou no dispositivo virtual da CI) e uma janela
- [~] 6.2 WebGPU e CPU em WebAssembly — o Safari, nos dois caminhos (precisa de um Mac ou iPhone); o Firefox

## Ferramentas de verificação montadas nesta máquina
- Validação Vulkan: camada do MSYS2 (`VK_ADD_LAYER_PATH=C:\msys64\ucrt64\bin`); a sincronização liga com
  `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true` (a variável antiga não faz nada na 1.4)
- lavapipe: `VK_DRIVER_FILES=C:\msys64\ucrt64\share\vulkan\icd.d\lvp_icd.x86_64.json`
- Fuzzing: `-DARTFUL_FUZZ=ON` com o clang do MSYS2 (`tools/fuzz/`)
- MSL: `tools/msl/check.py` (sintaxe e tipos)
- WGSL: `tools/wgslCheck.c` (naga, do wgpu-native) e `tools/web/wgslCheck.html` (Tint, no navegador)
- Web: Emscripten do MSYS2 (`emcmake`, com `BINARYEN_CORES=1`), node para os testes, `tools/web/serve.py`
- Memória: `artful-soak`; `ArtfFrameStats.gpuBytes`; `artful-render --graph`
