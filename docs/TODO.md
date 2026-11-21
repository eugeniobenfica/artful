# TODO — o que falta do plano de correção

Só o que resta fazer ou está incompleto no [plano](plano-de-correcao.md). O que já foi feito sai daqui: os detalhes,
números e decisões ficam nas seções "Estado" do plano e no [CHANGELOG](../CHANGELOG.md). Atualizar a cada passo fechado.

Duas listas: o que dá para fazer com o que esta máquina e a CI já têm, e o que depende de hardware, de um sistema ou de
uma ferramenta que não está aqui. Legenda: `[~]` em parte (o que falta vem ao lado) · `[ ]` não iniciado.

Branch: `unreal-parity`. Última CI conferida: run 44948960739 (21/11/2026): Windows, Android e macOS passam, com o Metal
rodando no macOS; os fuzzers passam. O Linux cai só nas camadas da 5.8, no lavapipe do runner (abaixo).

## Dá para fazer aqui

### CI
- [ ] Linux: `layers/layersBehindSolid` e `layersTransmittance` (camadas na GPU, da 5.8) caem no lavapipe do runner
  (Mesa 26.0.8, LLVM 21, 256 bits) e não no do MSYS2 (Mesa 26.2.6), nem com a validação de sincronização da CI, nem
  sem post-depth coverage (`ARTFUL_NO_POST_DEPTH`). Próximo passo: um passo da CI que, quando os testes falham, rode
  esses dois sob o gdb e mostre o backtrace e as extensões do lavapipe. Os outros 18 testes que a CI pulava passam lá
  agora (run 44948960739), com a causa corrigida (ray queries lendo a interseção committed sem acerto)

### Fase 5 — Alcance de conteúdo
- [~] 5.7 Gradientes da A-SVGF (CPU, no stash "A-SVGF temporal gradients") — ainda quebram com objetos em movimento e
  sombra suave: tirar dos gradientes os pixels de instâncias que se moveram e normalizar pelo ruído da amostra
- [~] 5.9 Saída HDR — faltam os metadados HDR e o scRGB (o teste numa tela HDR está na outra lista)
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
- [~] 5.9 Saída HDR — testar numa tela HDR
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
