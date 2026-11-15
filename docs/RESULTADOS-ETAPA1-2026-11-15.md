# Etapa 1 do plano de otimização: resultados (15/11/2026)

Execução da etapa 1 de [PLANO-OTIMIZACAO-COMPLETO-2026-11-15.md](PLANO-OTIMIZACAO-COMPLETO-2026-11-15.md), autorizada pelo usuário em 15/11 ("Sim, etapa 1"; "pode seguir o planejamento sem esperar pela minha resposta"). Nada commitado, nada publicado; só o app de teste (`com.swap2native.test`) no POCO, pela fila de `POCO-FILA.md`.

## Como foi medido (M1)

- Ferramenta nova: `dm/tools/android_bench.py`. Inicia o jogo pelo intent, carrega o save de teste com teclas programadas (`DM_KEYS=35:A*6/4`, sem tocar na tela), mede uma janela fixa (100 s a 160 s depois do intent, Link parado em Kokiri, perto do Mido) com as linhas por quadro do host (`DM_NVN_TSV`), e anota temperatura (`dumpsys thermalservice`) e memória disponível no começo e no fim da janela. Variantes de chaves rodam alternadas (A B A B) com 90 s de pausa; `--ctl` manda linhas ao vivo (`ctl.txt`); `--resummarize` recalcula uma pasta antiga. Resultados em `output/bench/<data>-<tag>/` (`results.md`, `results.json`, por partida `log.txt`, `frames.tsv`, `end.png`).
- Aparelho: POCO F7 Ultra (Adreno 830, 16 GB), driver 891.14, app de teste com a geração de quadros (LSFG) **ligada** nos blocos A e B (ajuste do app de teste; `flow 2048x1152, scale 1.00`), FSR no lugar da rede do upscaler, buffer compartilhado (alias).
- Partidas de 3 minutos: o aparelho chega a 90–96 °C na CPU no fim de cada uma. Diferenças de 0,3 fps estão dentro do ruído.

## O que um quadro de Kokiri é (medido)

| Medida (p50 da janela) | Valor |
|---|---|
| Taxa | 28,2 a 28,6 fps |
| Tempo de quadro do jogo (`@frame_ms`) | 34,9 ms (p99 39 ms) |
| Intervalo na tela | 33,3 ms metade das vezes, 41,7 ms na outra (painel a 120 Hz) |
| GPU do quadro (`@gpu_ms`) | 25,2 ms |
| O jogo bloqueado em `nvnSyncWait` (`@sync_ms`) | 21,5 ms por quadro |
| Espera do ritmo (`@wait_ms`) | 0 |
| CPU de todo o processo por quadro (`@proc_cpu_ms`) | 76 ms (cerca de 2,2 núcleos) |
| Draws / passagens contadas pelo NVN | 757 / 57 |
| Gravação do replay | 9,7 a 10,5 ms por quadro, 13 a 14 µs por draw |
| Fins de passagem no Vulkan | 145 por quadro |

**O que limita:** a linha do tempo de sincronização (`DM_VK_SYNCLOG`, quadro 4802) mostra o jogo esperando o fence da submissão que **acabou de enviar** (311 draws: gravada de 89.299,1 a 89.303,5 ms; o jogo espera de 89.301,6 a 89.322,9 ms, 21,3 ms; a GPU termina em 89.322,8). O jogo envia a passagem principal e espera a GPU terminá-la antes de seguir. Então o quadro é o trabalho de CPU do jogo **mais** a gravação dessa submissão **mais** o tempo de GPU dela, em série. Não é o ritmo: o jogo nunca espera o vsync (`@wait_ms` 0).

Consequências para o plano:

- **Travar em 30 não ajuda aqui:** o quadro já custa 35 ms de trabalho (medido abaixo).
- **Cada ms de GPU dessa submissão e cada ms da gravação dela é ms de quadro.** O alvo passa a ser a submissão que o jogo espera: começar a GPU antes (enviar em partes menores) e gravar mais rápido (C1).

## Fins de passagem por lugar do código (M2, novo)

`vka_rp_end()` passou a registrar o arquivo e a linha de quem fecha cada passagem; a linha de estatística a cada 300 quadros ganhou "pass ends by site per frame". Em Kokiri:

| Lugar | Por quadro | O que é |
|---|---|---|
| `vka_draw.c:431` | 42,0 | uma limpeza de outros alvos (`vka_rp_prep_clear`) |
| `vka_mesh.inc:316` | 37,0 | draw indireto do fallback de mesh: o registro de 8 bytes do jogo |
| `vka_exec.c:1319` | 23,9 | operação fora de passagem no replay (cópia ou similar) |
| `vka_draw.c:293` | 14,4 | troca de alvo |
| `vka_exec.c:1322` | 10,8 | idem |
| `vka_mesh.inc:120` | 8,2 | controlador do fallback de mesh |
| `vka_draw.c:1539` | 4,8 | primeiro uso de uma imagem |
| `vka_exec.c:860` | 3,0 | parte de submissão |

## A/B (bloco A, 01:56–02:20, 2 partidas por variante)

| Variante | fps | quadro p50 | GPU p50 | % dos quadros em 33,3 ± 2 ms |
|---|---|---|---|---|
| base | 28,2 / 28,5 | 35,2 / 35,0 | 25,3 / 25,2 | 52 / 61 |
| `DM_VK_UB_DEFER=1` | 28,5 / 28,6 | 35,0 / 34,9 | 25,2 / 25,2 | 61 / 64 |
| `DM_VK_PUSH_DELTA=1` | 28,6 / 28,5 | 34,9 / 35,0 | 25,2 / 25,3 | 64 / 59 |

- **Uniformes adiados (G1):** sem ganho. Só 1 atualização por quadro fica na passagem; 22,9 não ficam por haver uma barreira do jogo dentro dela. **Continua desligado.**
- **Push parcial (C2):** escreve 20 % dos descritores, mas a gravação não muda (9,9 ms contra 9,9 ms) e o quadro não é limitado por ela. **Continua desligado.**

## A/B (bloco B, 02:34–03:08)

| Variante | fps | quadro p50 | GPU p50 | barreiras completas por quadro | CPU / GPU no fim (°C) |
|---|---|---|---|---|---|
| base (registro indireto ligado direto) | 28,5 / 27,9 | 34,9 / 34,9 | 25,1 / 25,1 | 130 | 96 / 92 |
| `DM_VK_MESH_IND_DIRECT=0` (a cópia antiga) | 28,5 / 28,6 | 35,0 / 34,9 | 25,3 / 25,2 | 170 | 90–94 / 88–92 |
| `DM_FPS_TARGET=30` | 28,4 / 28,6 | 35,0 / 34,9 | 25,2 / 25,2 | 129 | 86–91 / 84–89 |
| `DM_VK_GPU_FLOOR_AUTO=2` | 26,5 / 26,8 | 37,8 / 37,6 | 25,2 / 25,1 | 124 | **70–72 / 71–73** |

- **Registro indireto do mesh sem cópia (novo, `DM_VK_MESH_IND_DIRECT`, ligado):** todos os registros de Kokiri estão alinhados (65.536 ligados direto, 0 cópias). Tira 40 barreiras completas por quadro (170 → 130) e a imagem sai igual, mas a GPU e a taxa não mudam: no Adreno 830 essas barreiras não eram o custo. Fica ligado por tirar trabalho sem regressão; a medida em Mali segue aberta.
- **Trava em 30 (P1):** sem efeito, como esperado pelo que limita o quadro.
- **Piso automático da GPU (P2, modo 2):** −2 fps, mas **24 °C mais frio** em 3 minutos. A partida curta não mostra o estrangulamento térmico do piso em 100 %; a medida de 10 minutos decide (a de 13/11 mediu 27,6 contra 30,4 fps a favor do piso 0 depois de 6 minutos).

## Outras mudanças da etapa

- **H4:** o salvamento do cache de pipelines depois de um compute de mais de 1 s vai para a thread `vk-pcache` em vez de travar o quadro que já esperou o compile (`vka_pso.c`).
- **H6:** a leitura de `smaps_rollup` do censo de memória passou a ser cronometrada e sai na linha do censo ("rollup read in N ms"), para saber se ela causa o pico a cada 10 s antes de mudar o padrão.
- **P6:** `android:appCategory="game"` no manifesto.
- **M4:** o relatório do Diagnóstico automático ganhou uma linha de desempenho por rodada (taxa, estatística do replay, intervalos) e pega as linhas de estatística do replay, os quadros longos e o swapchain.
- **R6 (pressão de memória pelo `onTrimMemory`):** não feito. Desde o Android 14 o sistema não manda mais os níveis `TRIM_MEMORY_RUNNING_*` a um app em primeiro plano, então o sinal não chegaria. Vai para a etapa 3 junto com o modo de pouca RAM.
- **H8 (readahead do `romfs.bin`):** não feito às cegas; precisa de uma medida de carregamento com `DM_FS=1` primeiro.

## A/B (bloco C, 03:13–03:52): sem geração de quadros, envio em partes, tempos por submissão

Todas as variantes com `DM_LSFG=0` (a geração de quadros desligada, como no app público por padrão).

| Variante | fps (partida 1 / 2) | quadro p50 | GPU p50 |
|---|---|---|---|
| sem LSFG | 30,0 / 35,0 | 33,2 / 28,6 | 25,7 / 25,8 |
| + `DM_VK_CHUNK=100` | 35,5 / 30,0 | 28,0 / 33,1 | 25,8 / 25,7 |
| + `DM_VK_CHUNK=50` | 30,0 / 36,4 | 33,0 / 27,4 | 25,8 / 25,2 |
| + `DM_NVN_GPU_TS=submission` | 30,0 / 35,0 | 33,1 / 28,5 | 25,6 / 25,8 |

- **A geração de quadros custa caro aqui:** com ela o jogo fica em 28,5 fps; sem ela, 30 a 36. O app de teste tinha o LSFG ligado com o fluxo em 100 % (2048x1152), que disputa a GPU com o jogo. Recomendação para quem joga: LSFG só com folga de GPU, e com o fluxo em 75 % ou menos.
- **As partidas se dividem em dois grupos** que não dependem da variante: quadro de 28 ms (35 fps) ou de 33 ms (30 fps), com a mesma GPU (25,7 ms) e a mesma espera do jogo pela GPU (15–19 ms). Os 5 ms a mais estão no trabalho de CPU do próprio jogo, sem espera de ritmo; a causa está aberta (estado do jogo na partida, frequência da CPU). As partidas de 33 ms começam a janela mais frias (65–68 °C contra 69–73 °C).
- **Envio em partes (`DM_VK_CHUNK`):** sem efeito separável do ruído; com 50 a gravação sobe de 8,2 para 14,3 ms por quadro. **Fica em 600.**
- **Tempos de GPU por submissão (P7):** sem efeito separável do ruído. **Fica como está no perfil.**

### Perfil de GPU de um quadro de Kokiri (`DM_VK_GPUPROF`, sem LSFG)

| Parte | ms |
|---|---|
| **GPU do quadro** | **26,5** |
| Passagens de render (144, cena em 976x549) | 11,3 |
| Compute do fallback de mesh (`vka_mesh.inc`, 314 a 342 dispatches) | 7,2 + 1,9 = **9,1** |
| Compute do próprio jogo (`vka_draw.c:1758`, 11 dispatches) | 4,5 |
| Outros dispatches (`vka_draw.c:1825`) | 1,1 |

- **O fallback de mesh é um terço da GPU.** Cerca de 330 dispatches pequenos por quadro, com barreiras entre eles. É o maior alvo de GPU (G2/G3 do plano).
- **Um único compute do jogo de um grupo de trabalho só** (`424b9852065ec581`, 128 threads, um shader da AGL da Nintendo) leva **2,6 ms**; outros dois de um grupo levam 0,9 ms cada. No Ampere um grupo só roda rápido; no Adreno é trabalho serial num núcleo. A tradução desses shaders é o segundo alvo.

## A/B (bloco D, 03:54–04:12): trava em 30 e ADPF, sem LSFG

| Variante | fps | quadro p50 | intervalo na tela p50 / p95 | % em 33,3 ± 2 ms | quadros > 40 ms |
|---|---|---|---|---|---|
| sem LSFG | 35,0 / 30,0 | 28,6 / 33,1 | 25,0–33,3 / 33,3 | 5 / 78 | 1 / 4 |
| **+ `DM_FPS_TARGET=30`** | **30,0 / 30,0** | 33,3 / 33,3 | **33,3 / 33,3** | **80 / 79** | 11 / 15 |
| + `DM_VK_ADPF_SPAN=100` | 35,0 / 30,0 | 28,6 / 33,1 | 25,0–33,3 / 33,3 | 4 / 78 | 0 / 2 |

- **Trava em 30 sem LSFG (P1): 30 fps em todas as partidas, cada quadro na tela por exatos 33,3 ms (p50 e p95).** Sem a trava, o jogo alterna quadros de 25 e 33 ms na tela (35 fps irregulares). É o "30 estável" da meta para Kokiri; a trava já existe (painel do jogo, `DM_FPS_TARGET`).
- **ADPF com a duração real (P3):** sem efeito separável do ruído.

### Caminho de cada draw na CPU (`DM_VK_PROF`, sem LSFG)

Por draw, em µs: resolver 0,17, layout 0,08, imagens + passagem + chave 0,95, entrada de vértices 0,10, pipeline 0,28, **conjuntos 2,23** (passeio 0,58; escritas 5,07, das quais **segmentos do driver 3,73**; push 0,81), buffers de vértice + estado 0,31, draw 0,15. Os segmentos do driver (preencher e comparar 2,5 KB por banco, C3) são o maior custo identificado por draw. A gravação não está no caminho crítico de Kokiri, então C1/C3 ficam para cenas com mais draws.

## A/B (bloco E, 04:13–04:35): alvo 60 com retenção automática contra trava em 30, sem LSFG

| Variante | fps | quadro p50 | intervalo na tela p50 / p95 | % em 33,3 ± 2 ms | quadros > 40 ms |
|---|---|---|---|---|---|
| sem LSFG | 34,5 / 30,0 | 28,9 / 33,1 | 25,0 / 33,3 | 8 / 79 | 1 / 1 |
| `DM_FPS_TARGET=60` (retenção automática) | **22,9** / 30,6 | **48,8** / 33,1 | **50,0** / 33,3 | 27 / 71 | **867** / 5 |
| **`DM_FPS_TARGET=30`** | **30,0 / 30,0** | 33,3 / 33,3 | **33,3 / 33,3** | **80 / 78** | 11 / 16 |

- **Alvo 60 com retenção:** numa das duas partidas a retenção subiu para 3 vsyncs e ficou lá (22,9 fps, quadros de 50 ms). O quadro custa cerca de 2,1 vsyncs em Kokiri; a regra trata isso como "não cabe em 2" e o teste de volta falha ("probe failed", GPU 3–4 vezes o tempo de antes, a GPU baixa o clock na retenção longa). É o problema de ficar preso em 20 fps já descrito em `docs/reference/frame-rate.md`. **Não serve como padrão.**
- **Trava em 30: de novo 30,0 fps nas duas partidas, cada quadro 33,3 ms na tela.** Seis partidas de seis no total (blocos D e E).
- **A velocidade do jogo:** o passo de quadro do Zelda (`patches.py` `FRAME_STEP`, build `e0f3ccfa…`) faz o jogo avançar pelo passo do host. Com o alvo em 0 (o padrão até aqui) a razão fica em 2 a cada quadro, como no Switch a 30 fps, qualquer que seja o tempo do quadro: a 35 fps o jogo andava 1,17 vez mais rápido que o tempo real, e a 28,5 fps (com LSFG) 0,95 vez. Com o alvo em 30 o passo é sempre 2 vsyncs: tempo real.

**Decisão:** o perfil do Zelda passou a ter `fps_target 30` (`dm/games/0400486024DEC000/game.tsv`, com o porquê). Vale para packs preparados a partir de agora; um pack já preparado leva o perfil antigo dentro dele, então até ser preparado de novo quem quiser o mesmo efeito escolhe 30 no limite de FPS do painel do jogo. A retenção automática a 60 (P1 automático) fica aberta: precisa de uma regra que aceite 2 vsyncs quando o quadro custa pouco mais que 2.

## Partidas longas (bloco F, 04:37–05:03): piso fixo contra automático, a 30 fps, sem LSFG, 10 minutos

| Variante | minutos 1–6 | minutos 7–10 | GPU p50 | temperatura da CPU no fim |
|---|---|---|---|---|
| piso fixo em 100 % | 30,0 fps, GPU 25,8 ms | **17,0 fps, GPU 51,7 ms** | 26,1 | 61 °C |
| `DM_VK_GPU_FLOOR_AUTO=2` | 17,0 fps, GPU 51,8 ms desde o início | 17,0 fps | 51,8 | 61 °C |

- **Não foi o calor:** o aparelho terminou a 60–61 °C. Por volta dos 12 minutos da primeira partida o jogo carregou outra coisa (um quadro de 182 ms com as threads `ModuleSy` do jogo ocupadas, 260 imagens novas) e **gravou o save de teste** (`slot_01` às 04:45): Link apareceu em outro ponto de Kokiri, perto do espantalho, olhando para a Árvore Deku. A segunda partida já carregou esse ponto.
- **Esse ponto de Kokiri custa o dobro de GPU: 51,7 ms, 17 fps.** É o lugar mais pesado medido até aqui no POCO e o próximo alvo da meta de 30.
- **Consequência para as medidas anteriores:** o jogo grava o save enquanto roda, então cada partida começava de onde a anterior deixou Link. Os blocos A a E mostram Link no mesmo lugar (perto do Mido) nas capturas de fim de janela, mas a variação de 30 contra 35 fps entre partidas da mesma variante pode ter vindo disso. A ferramenta agora repõe o save antes de cada partida (`--save-ref`): duas cópias de referência ficam em `/data/local/tmp/s2bench/` no POCO, `0400486024DEC000-mido` (a cópia do diagnóstico de 14/11 22:17, o ponto perto do Mido) e `0400486024DEC000-scarecrow` (o ponto pesado).
- **Piso automático:** nesse ponto a GPU fica no mesmo tempo com o piso fixo e com o automático, então a comparação de calor ficou sem resposta; refazer com o save reposto.

## Com o save reposto (bloco G, 05:28–06:04), sem LSFG

| Ponto de Kokiri | Variante | fps | quadro p50 / p99 | na tela p50 / p95 | % em 33,3 ± 2 ms | > 40 ms | GPU p50 |
|---|---|---|---|---|---|---|---|
| perto do Mido | sem trava | 30,1 / 29,7 | 32,8–33,1 / 47–49 | 33,3 / 33,3 | 63 / 67 | 40 / 46 | 27,1 |
| perto do Mido | trava em 30 | 29,7 / 30,0 | 33,2–33,3 / 37–46 | 33,3 / 33,3 | 69 / 77 | 43 / 7 | 25,8–28,4 |
| espantalho, olhando a Árvore Deku | sem trava | 17,1 / 17,1 | 58,2 / 64 | 58,3 / 66,7 | 0 | 1.028 | 52,0 |
| espantalho | trava em 30 | 17,0 / 17,1 | 58,3–58,6 / 65–68 | 58,3 / 66,7 | 0 | 1.022 | 52,0 |

- **Com o save sempre igual, as partidas repetem** (diferença de 0,4 fps entre duas partidas da mesma variante). Os 35 fps de algumas partidas dos blocos C a E vieram do save que mudava entre partidas, não das chaves.
- **Perto do Mido o jogo já fica em 30** com ou sem a trava (o quadro custa uns 33 ms); a trava deixa os quadros um pouco mais regulares (69–77 % contra 63–67 % perto de 33,3 ms) e garante a velocidade certa do jogo quando o quadro sai mais rápido.
- **Perto do espantalho: 17 fps, GPU de 52 ms.** Perfil de um quadro: **53,0 ms** de GPU = passagens de render **27,2 ms** (185), compute do fallback de mesh **15,9 + 2,0 = 17,9 ms** (538 dispatches), compute do jogo 5,5 + 1,7 ms. Para 30 fps ali a GPU precisa cair para uns 28 ms: o fallback de mesh e as passagens respondem por 45 ms dos 53.

## Conclusão da etapa 1

| Item do plano | Resultado |
|---|---|
| M1 medição repetível | **Feito:** `dm/tools/android_bench.py` (save reposto, teclas programadas, variantes alternadas, perfil por linha ao vivo). Lição: o save tem de ser reposto; o LSFG do app de teste distorce tudo |
| M2 motivo de cada fim de passagem | **Feito:** "pass ends by site" na linha de estatística |
| M3 tempo de apresentação e CPU do jogo | **Já existia** nas linhas por quadro (`@display_ms`, `@presented_ms`, `@sync_ms`, `@proc_cpu_ms`); a ferramenta resume |
| M4 desempenho no Diagnóstico automático | **Feito:** linha `performance:` por rodada e as linhas do replay no trecho do log |
| M5 `regress.py` no Mac | **Não feito:** precisa de uma sessão no Mac |
| P1 30 fps travados | **Feito para o Zelda:** `fps_target 30` no perfil (velocidade certa do jogo e quadro de 33,3 ms na tela). A retenção automática a 60 fica presa em 20 fps quando o quadro custa ~2,1 vsyncs: aberto |
| P2 piso da GPU automático | **Sem decisão:** −2 fps e −24 °C em 3 minutos com LSFG; no ponto pesado o mesmo tempo de GPU. Refazer 10 minutos no ponto do Mido com o save reposto |
| P3 ADPF com a duração real | **Sem efeito medido** (`DM_VK_ADPF_SPAN=100`) |
| P6 categoria de jogo | **Feito** (manifesto) |
| P7 tempos de GPU devolvidos ao jogo | **Sem efeito medido** em Kokiri (`DM_NVN_GPU_TS=submission`) |
| G1 uniformes adiados | **Sem efeito**, fica desligado |
| C2 push parcial | **Sem efeito**, fica desligado |
| H4 cache gravado fora do quadro | **Feito** |
| H6 censo de memória | **Cronometrado**; decidir com os números do próximo log |
| H8 readahead do RomFS | **Não feito:** medir carregamento com `DM_FS=1` antes |
| R6 pressão de memória | **Movido para a etapa 3** (o Android 14 não manda mais o sinal) |
| Novo: registro indireto do mesh sem cópia | **Feito** (`DM_VK_MESH_IND_DIRECT`, ligado): −40 barreiras completas por quadro, sem mudança de tempo no Adreno 830 |

**O que muda no plano:** o maior alvo é a GPU, nesta ordem: o **fallback de mesh** (9 ms perto do Mido, 18 ms perto do espantalho, em 330 a 540 dispatches pequenos; G2/G3) e as **passagens de render** (11 e 27 ms; G4 a G6); depois os **compute de um grupo só** da AGL (2,6 ms num só kernel). A CPU da gravação não está no caminho crítico de Kokiri (o envio em partes não mudou nada), então C1/C3 descem de prioridade até uma cena com mais draws pedir. A geração de quadros custa entre 2 e 7 fps aqui e só deve ser recomendada com folga.

## Etapa 2, primeiro passo: o fallback de mesh (15/11, 06:36–07:09)

Pedido do usuário: "Sim, fallback de mesh".

**O que foi feito:**

- **Controlador fundido (`DM_VK_MESH_FUSE`, ligado):** um draw sem task shader roda as três fases do controlador (a contagem, os grupos, os registros do primeiro lote) num dispatch só, a fase 3 nova de `selftest/mesh_control.comp` (SPIR-V regenerado com o `glslc` do NDK, validado com `spirv-val`). Corta 2 dispatches e 2 barreiras por draw desses.
- **Contadores do fallback** na linha de estatística ("task/mesh fallback per frame": draws por tipo, lotes, dispatches) e o **programa de mesh nomeado no perfil de GPU** (`DM_VK_GPUPROF`).

**Medida** (2 partidas por variante, save reposto, trava em 30, sem LSFG):

| Ponto | Variante | fps | GPU p50 | dispatches do fallback por quadro |
|---|---|---|---|---|
| Mido | fundido | 30,1 / 30,1 | 25,7 / 25,7 | — |
| Mido | antigo | 30,1 / 30,1 | 25,9 / 25,8 | — |
| espantalho | fundido | 17,1 / 17,1 | 52,2 / 51,9 | 405 |
| espantalho | antigo | 17,1 / 17,1 | 52,2 / 52,2 | 538 |

**Não mudou o tempo de GPU.** No espantalho são 81 draws emulados por quadro: 35 sem task shader (34 indiretos) e 46 com (39 diretos, 7 indiretos). O fundido cobre os 35, e a contagem de dispatches cai 25 %, mas o tempo fica igual. **O custo do fallback não é o vaivém de dispatches pequenos: é o trabalho dos próprios task/mesh shaders traduzidos.** Fica ligado por não piorar nada e gravar menos comandos.

**Para onde vai a GPU no ponto pesado** (um quadro, 58,7 ms, perfil passo a passo):

| Passo | ms |
|---|---|
| passagem principal da cena, 527 draws (976x549) | 7,75 |
| outra passagem da cena, 510 draws | 5,25 |
| mesh `fed302d791a4ba43` (compute) | 4,29 |
| compute da AGL `424b9852065ec581`, **um grupo de trabalho só** | 3,42 |
| mesh `604134a5262ff670`: compute 3,37 + o draw que puxa os vértices **3,14** | 6,51 |
| mesh `d98076e4148db34f` | 2,67 |
| mesh `73dc0b90c4082e43` (o da lista `.deadly` do PanVK) | 1,86 |
| mesh `eb021183bf9b0092` (com task shader) | 1,63 |
| as 343 etapas pequenas do controlador, juntas | 1,63 |

- **Cinco programas de mesh somam 14 ms**, mais 3 ms do draw que puxa os vértices de um deles. Cada um gera todos os vértices e primitivas num buffer de 64 MiB que o draw seguinte lê de volta. O alvo seguinte é o que esses shaders traduzidos fazem: se o descarte de primitivas do mesh shader (o "cull") ainda grava vértices, o tamanho do que é gravado por vértice, e a forma do grupo de trabalho no Adreno. É trabalho no tradutor (a cadeia de shaders), não no replay.
- **As duas passagens principais somam 13 ms** com cerca de 1.000 draws: o outro alvo.

## Etapa 2, segundo passo: os mesh shaders (15/11, 07:10–07:30)

Pedido do usuário: "Os mesh shaders".

**Como um mesh shader vira compute aqui** (`dm/gfx/tools/vk_android_mesh.py`, parte da cadeia de shaders; a entrada nativa em `dm/third_party/ryujinx/.../SpirvMeshEntry.cs`): cada grupo grava todas as suas saídas numa estrutura no buffer de 64 MiB, termina com o número de primitivas, e o draw seguinte puxa um canto de primitiva por invocação de vértice, sobre **todos os cantos possíveis** do lote (grupos × primitivas máximas × 3). A entrada traduzida também inicializa as saídas em (0, 0, 0, 1) para todos os vértices máximos e grava os índices de **todas** as primitivas máximas (degeneradas depois da contagem), em memória global, mesmo num grupo que não emitiu nada.

**Tentativa: compactar os lotes (`DM_VK_MESH_COMPACT`, desligado por padrão).** Um compute pequeno (`selftest/mesh_compact.comp`) lê a contagem de cada grupo e monta um draw só com os cantos vivos; o lote vai com `vkCmdDrawIndirectCount`. O deslocamento da contagem na estrutura é lido do SPIR-V do módulo que puxa os vértices (`vka_shader.c mesh_count_offset`), então não muda a cadeia.

| Ponto do espantalho | fps | GPU p50 | passagens |
|---|---|---|---|
| sem compactar | 17,1 | 52,0 ms | 27 ms |
| compactado | **15,2** | **59,5 ms** | **35 ms** |

**Mais lento.** A imagem saiu certa, mas no Adreno 830 um draw indireto por grupo vivo custa mais do que os cantos mortos que ele evita: esses só leem a contagem e desviam. Fica desligado; o código fica para uma GPU de draws mais baratos (Mali, Xclipse: não medido).

**O que isso diz:** nem o vaivém de dispatches do controlador (passo anterior) nem os cantos mortos do draw são o custo. O que pesa é o trabalho vivo: o compute dos mesh shaders (14 ms nos cinco maiores) e os vértices vivos puxados sem reaproveitamento (um vértice compartilhado por 6 triângulos é lido e processado 6 vezes).

**O que sobra para os mesh shaders, todos na cadeia de shaders** (cada um obriga todo jogador a preparar os shaders do jogo de novo, horas no Zelda):

1. **Não gravar os índices das primitivas mortas** no compute (só as primitivas abaixo da contagem; o draw já as trata como mortas pela contagem) e **não inicializar as saídas de um grupo vazio**: corta escrita em memória global por grupo. Ganho estimado de alguns ms no espantalho; não medido.
2. **Draw indexado** no lugar do draw por canto: o compute grava um índice por canto e o vértice é processado uma vez por vértice único, não uma vez por triângulo que o usa (3 a 6 vezes menos trabalho no draw que puxa). É a mudança maior: cadeia mais o replay.
3. **Saídas em meia precisão** no buffer do mesh (metade da banda para gravar e ler), onde a saída tolera.

## Etapa 2, terceiro passo: a cadeia de shaders do mesh (15/11, 07:30–08:00)

Pedido do usuário: "Sim, mudar a cadeia".

**Ferramenta de teste sem preparar o jogo de novo:** `dm/gfx/tools/vk_android_mesh_override.py` abaixa de novo cada task/mesh de uma biblioteca já feita com o `vk_android_mesh.py` do repositório e grava os módulos que mudaram como substituição do replay (`DM_VK_SPV_OVERRIDE=<pasta>`, `vka_shader.c spv_override`), depois do `spirv-val`. Com o `vk_android_mesh.py` sem mudança ela reproduz a biblioteca da cadeia atual (`v-4e12942060ff`, na pasta de trabalho do Zelda) byte a byte: 520 entradas, 1.020 módulos iguais.

**Tentativa: gravar só os índices das primitivas vivas** (no compute, a gravação de `select(t < contagem, índices, (0,0,0))` virou uma gravação sob condição). 495 computes, todos válidos; 51 usados em Kokiri.

| Ponto | Variante | fps | GPU p50 |
|---|---|---|---|
| espantalho | novos | 16,9 / 16,9 | 52,7 / 52,9 |
| espantalho | biblioteca | 17,1 / 17,2 | 51,8 / 51,9 |
| Mido | novos | 30,0 / 29,9 | 26,7 / 27,1 |
| Mido | biblioteca | 30,0 / 30,0 | 25,7 / 25,7 |

**Pior em 1 ms.** O desvio dentro do laço custa mais que as escritas que evita. **Desfeito**: `vk_android_mesh.py` voltou a ser o do repositório (a ferramenta confirma a biblioteca reproduzida de novo), nada entrou na cadeia, nenhuma preparação nova é necessária.

**O que a leitura dos computes mostrou:** os quatro mesh shaders mais caros (`604134a5262ff670`, `fed302d791a4ba43`, `d98076e4148db34f`, `73dc0b90c4082e43`) têm **grupos de trabalho de 32 threads** (o tamanho de um warp da Nvidia). O Adreno 830 roda ondas de 64 lanes, então **metade de cada onda fica parada** nesses shaders, o tempo todo. Os shaders têm 7 a 9 laços, 3 barreiras e cerca de 50 variáveis de memória compartilhada cada.

**O que sobra, do mais promissor ao menos:**

1. **Dois grupos de 32 numa onda de 64** (no abaixamento para compute: tamanho local 64, cada metade é um grupo virtual com a sua cópia da memória compartilhada). Pode chegar perto de dobrar a vazão desses computes (14 ms no espantalho). O difícil são as operações de subgrupo que o tradutor gera (o código conta com subgrupos de 32) e as barreiras dentro de condicionais. Trabalho de dias.
2. **Draw indexado no lugar do draw por canto** (cada vértice processado uma vez). Também dias, na cadeia e no replay.
3. **As passagens principais da cena** (13 ms no espantalho), sem mexer na cadeia.

## Etapa 2, quarto passo: dois grupos de mesh por onda (15/11, 08:00–08:45)

Pedido do usuário: "Dois grupos por onda".

**O que foi feito** (testado por substituição de módulos, `DM_VK_SPV_OVERRIDE`, sem preparar o jogo):

- **No abaixamento** (`vk_android_mesh.py`, `lower(pair=True)`): um programa de mesh de 32 invocações passou a rodar em grupos de 64, cada metade um grupo virtual (o grupo `2x + metade` do lote), com a sua própria cópia da memória compartilhada (cada variável de memória compartilhada virou um vetor de 2), o índice local `& 31`, e a metade que sobra num lote ímpar repetindo o último grupo mas gravando no espaço livre do buffer. As operações de subgrupo não precisaram de mudança: o tradutor já põe cada warp de 32 no seu segmento de 32 lanes do subgrupo (o `Ballot` escolhe a palavra pelo bit 32 do lane, o shuffle soma `lane & 96`). 498 dos 520 programas de task/mesh do Zelda saíram pareados, todos válidos no `spirv-val`.
- **No replay:** o controlador gravava a metade da contagem de grupos (palavras 20 a 22 do bloco de informação, que passou a 96 bytes), o compute pareado era despachado dali, um grupo do buffer de saída ficava livre, e o módulo pareado era reconhecido por uma marca no SPIR-V (`OpSourceExtension "dm.mesh.pair"`), só onde o compute roda o subgrupo da biblioteca em 64 lanes.

**Medida no espantalho** (1 partida; as anteriores sem pareamento davam 17,1 fps e GPU de 51,8 a 52,2 ms):

| | fps | GPU p50 | compute do fallback no perfil |
|---|---|---|---|
| pareado | 17,0 | 52,1 ms | 15,5 ms (433 passos) |
| sem pareamento | 17,1 | 52,0 ms | 15,9 ms (405 passos) |

**A imagem saiu certa (81 computes pareados por quadro), mas não acelerou.** Os computes de mesh não estão limitados pelas lanes paradas da onda: com o dobro de grupos por onda o tempo ficou o mesmo. O que sobra como suspeito é a memória que eles gravam (todas as saídas de todos os vértices, no buffer de 64 MiB) e que o draw seguinte lê de volta.

**Desfeito por inteiro:** o abaixamento voltou a ser o de antes (o resumo das ferramentas dá de novo `4e12942060ff`, a cadeia publicada; a ferramenta reproduz a biblioteca byte a byte), o replay e o controlador também. Nada mudou para os jogadores.

**O que isso deixa claro sobre o fallback de mesh no Adreno 830:** quatro tentativas (controlador fundido, compactar os lotes, gravar só os índices vivos, dois grupos por onda) e nenhuma mudou o tempo, salvo a compactação, que piorou. O limite parece ser a banda de memória do caminho compute → buffer → draw. Os próximos passos que atacam isso são maiores: saídas em meia precisão no buffer (metade da banda) ou o draw indexado (cada vértice lido uma vez). Antes deles, vale medir a banda: um teste que só grava e lê o buffer de saída no tamanho do Zelda diria quanto do tempo é memória.

## Etapa 2, quinto passo: as passagens principais (15/11, 08:29–08:45)

Pedido do usuário: "Sim, as passagens".

**Onde vai a GPU no espantalho, por subtração** (1 partida cada, save reposto, trava em 30, sem LSFG; `DM_VK_MESH_DIAG_STAGE`, diagnóstico que tira partes do fallback de mesh e deixa a imagem incompleta):

| Variante | fps | GPU p50 | o que sai |
|---|---|---|---|
| tudo | 17,0 | 51,9 ms | — |
| sem os draws que puxam os vértices do mesh (`=2`) | 18,0 | 49,4 ms | **2,5 ms**: os draws do mesh |
| sem o fallback de mesh inteiro (`=1`) | 26,6 | 33,8 ms | **18 ms**: o fallback, quase todo o seu compute |

- **Os draws que puxam os vértices do mesh custam pouco** (2,5 ms), mesmo com milhões de cantos por draw: os cantos mortos são baratos. Isso confirma por que compactar os lotes não ajudou.
- **Sem o mesh, as duas passagens principais continuam caras:** 8,1 ms (528 draws) e 5,6 ms (509 draws), cerca de 15 µs de GPU por draw, numa cena de 976x549. Antes já se mediu que um quinto dos pixels tira só 4 de 25,5 ms: não é o número de pixels.
- **Sem o mesh o jogo vai a 26,6 fps no ponto pesado.** O fallback de mesh é a maior parte do que separa esse ponto dos 30 fps, e as quatro tentativas no replay e no abaixamento não o baratearam.

**O que falta para atacar as passagens:** saber se o custo por draw é dos vértices (a fase de binning do Adreno roda os vértices duas vezes), da troca de estado entre draws ou dos shaders de fragmento caros com descarte (folhagem). Os interruptores do replay não separam isso; precisa dos contadores de GPU do Adreno (Snapdragon Profiler ou Android GPU Inspector, no PC com o celular ligado).

### Contadores de GPU do Adreno (15/11, 09:16–09:24)

- **O Snapdragon Profiler instalado (Windows) não traz ferramenta de linha de comando** (`sdpcli`): só a janela, o serviço do aparelho (`service/android/arm64-v8a/sdpservice`) e os plugins.
- **O POCO declara profiling de GPU pelo perfetto** (`graphics.gpu.profiler.support=true`). Com `debug.graphics.gpu.profiler.perfetto=1` o driver 891.14 registra `gpu.renderstages` no processo do jogo (os estágios de cada passagem: binning, renderização, carga e gravação da GMEM). Não registra `gpu.counters`.
- **Mas o rastreamento do driver derruba o jogo:** no fim da primeira captura de 2 s o jogo perdeu o dispositivo (`VK_ERROR_DEVICE_LOST`, cerca de fence 15965, "GPU: fence not reached after 2.0 s") e fechou (log `0400486024DEC000-20261115-091901.log` no app de teste). A captura só trouxe as passagens do launcher. A propriedade voltou ao vazio.
- **O que sobra:** o modo de tempo real do Snapdragon Profiler (a janela), que lê os contadores globais da GPU (`pluginGPU-GGPM`) sem instrumentar o driver do jogo: dá a divisão do tempo da GPU entre vértice, fragmento, textura e memória ao longo do tempo, não por passagem. Com o jogo parado no ponto pesado, a comparação com a cena sem o mesh (`DM_VK_MESH_DIAG_STAGE=1`) separa as passagens do fallback.

### Contadores pelo Snapdragon Profiler, tempo real (15/11, 09:30–09:43)

O usuário capturou pela janela do Profiler (Realtime, contadores globais da GPU); o jogo parado no ponto do espantalho, primeiro completo (17 fps, quadro de ~58 ms) e depois sem o fallback de mesh (`DM_VK_MESH_DIAG_STAGE=1`: sem grama nem folhagem, 26 fps, ~38 ms). Arquivos em `output/sdp/` (`full.csv`, `nomesh.csv`: a segunda exportação tem as duas fases, a sem mesh de 395 s em diante). Medianas:

| Métrica | completo | sem mesh |
|---|---|---|
| Barramento de memória ocupado | 9,5 % | 7,7 % |
| Leitura / escrita | 6,1 / 3,6 GB/s | 5,7 / 2,3 GB/s |
| **Shaders ocupados** | **61 %** | **49 %** |
| Do tempo dos shaders: compute / fragmento / vértice | 61,5 / 29,8 / 9,2 % | 43,2 / 42,6 / 11,6 % |
| Paradas: memória do sistema / textura / busca de vértice | 1,1 / 3,5 / 2,6 % | 0,5 / 4,2 / 0,9 % |
| Fragmentos / vértices por segundo | 474 M / 50 M | 545 M / 32 M |
| Primitivas descartadas de cara | 18,9 % | 13,2 % |

**O que isso diz:**

- **Não é banda de memória:** o barramento fica livre mais de 90 % do tempo, e as paradas por memória somam poucos por cento. Por isso nem compactar, nem gravar menos, nem parear o fallback de mesh mudou o tempo.
- **A GPU passa 40 % (completo) a 50 % (sem mesh) do tempo com os shaders parados**, sem esperar memória. O tempo vai em vazios entre trabalhos: as centenas de passos por quadro separados por barreiras (cada barreira completa esvazia a GPU), os computes do jogo que ocupam um grupo de trabalho só (`424b9852065ec581`, 3,4 ms num grupo; outros dois de 1 ms) enquanto o resto da GPU espera, e a gravação e carga dos anexos a cada fim e início de passagem.
- **O fallback de mesh soma compute** (de 43 % para 61 % do tempo dos shaders): é trabalho de verdade, mas em passos curtos entre barreiras, o mesmo padrão.

**Próximo alvo, por estes números:** os vazios entre passos. Medir quanto tempo de GPU fica entre o fim de um passo e o começo do próximo (o perfil passo a passo já tem os carimbos de tempo), achar as barreiras que serializam trabalho independente (sobretudo em volta dos computes de um grupo só e dos lotes do mesh) e deixar o que não depende um do outro correr junto.

## Etapa 2, sexto passo: os vazios entre os passos (15/11, 10:17–10:50)

Pedido do usuário: "Sim, os vazios". (Antes, com o serviço do Snapdragon Profiler ainda ligado no POCO, o driver da Qualcomm deixou de guardar pipelines, cache de 32 bytes, e cada compilação levou ~1,7 s: as partidas das 09:50–10:15 não valem. Com o Profiler fechado e o cache antigo do app de teste reposto, tudo voltou ao normal.)

**As barreiras quase não custam** (ponto do espantalho, 1 partida cada):

| Variante | barreiras completas por quadro | GPU p50 |
|---|---|---|
| base | 132 | 51,8 ms |
| `DM_VK_RP_SCOPE=0` (completa em todo fim de passagem) | 312 | 51,8 ms |
| `DM_VK_DISPATCH_ORDER=1` (completa depois de todo dispatch) | 238 | 52,6 ms |

**O jogo não descarta anexos** nesse ponto (nenhuma chamada a `DiscardColor` / `DiscardDepthStencil` em 20 quadros): honrar os descartes (G4) não ajudaria aqui. A contagem de chamadas NVN por quadro: 2.354 `ReportCounter`, 1.115 `SetRenderEnableConditional`, 1.113 `SetRenderEnable`, 367 `DrawElementsInstanced`, 129 `DrawElements`, 136 `Barrier`, 132 `SetRenderTargets`, 107 `DispatchCompute`.

**As consultas de oclusão são o vazio:**

| Variante | fps | GPU p50 | passagens no perfil |
|---|---|---|---|
| base (2.416 consultas por quadro, contadas) | 17,3 | 51,9 ms | 26,2 ms |
| `DM_VK_OCCLUSION=0` (todo relatório vale 1) | **21,5** | **41,7 ms** | **16,4 ms** |
| `DM_VK_OQ_PRECISE=0` (consultas não precisas, novo) | 17,2 | 51,9 ms | — |

- **10 ms de GPU vão nas 2.400 consultas de oclusão por quadro** (cerca de 4 µs cada), e não na contagem precisa: tirar o bit de precisão não muda nada. A suspeita é que o Adreno precise esvaziar o pipeline a cada fim de consulta para ler o contador de amostras (e repetir isso por bloco de tiles no modo de binning), o que bate com os 40 % de shaders parados nos contadores do Profiler.
- **A imagem com tudo "visível" saiu parecida** neste ponto (comparação de capturas de fim de janela), mas o jogo usa as contagens: com o valor fixo 1, o relato do Lichii é névoa desenhada por cima do Link e folhas e luzes piscando. Não pode virar padrão às cegas.

**Opções:**

1. **Uma opção de gráficos por jogo, "Consultas de oclusão: exatas / rápidas"**, com a rápida = valor fixo, para quem prefere desempenho. Barato e reversível; o risco visual fica na mão do jogador.
2. **Menos fins de consulta mantendo a contagem:** cada relatório do jogo hoje termina uma consulta; juntar os relatórios que o jogo lê só como "visível ou não" exige saber quais são (por endereço, ao longo dos quadros). Pesquisa.
3. **Reaproveitar o resultado do quadro anterior** em quadros alternados (uma latência de um quadro na visibilidade, comum nos próprios motores). Menos consultas por quadro, com o risco de objetos aparecerem um quadro atrasados.

## Etapa 2, sétimo passo: uma contagem de oclusão mais barata (15/11, 10:59–11:56)

Pedido do usuário: "Pesquisar contagem barata" (a opção 3 acima). Novo no replay (`vka_exec.c`), desligado por padrão:

- `DM_VK_OQ_REUSE=<n>`: o k-ésimo reset de amostras de um quadro só é contado nos quadros em que (quadro + k) % n == 0; nos outros, seus draws não abrem consulta e seus relatórios escrevem a última contagem que aquele endereço de relatório recebeu (1 se nunca recebeu).
- `DM_VK_OQ_REUSE_CHECK=1` (diagnóstico): conta assim mesmo os relatórios reaproveitados, escreve o valor repetido e diz por quadro quantas vezes ele contradisse o real: "oculto quando estava visível" (um objeto que sumiria) e "visível quando estava oculto" (só um draw a mais).
- `DM_VK_OQ_TRACE=<n>` (diagnóstico): os 48 primeiros relatórios de 4 quadros seguidos, a cada n quadros (lugar, endereço, consultas, contagem).

**Ganho, câmera parada** (ponto do espantalho, 2 partidas cada):

| Variante | fps | GPU p50 | consultas abertas por quadro |
|---|---|---|---|
| base | 17,1 / 17,2 | 52,0 ms | ~2.400 |
| `DM_VK_OQ_REUSE=2` | 18,6 / 18,7 | 47,4 ms | — |
| `DM_VK_OQ_REUSE=3` | **19,6 / 19,6** | **45,6 ms** | ~800 |
| (`DM_VK_OCCLUSION=0`, para comparar) | 21,5 | 41,7 ms | 0 |

As capturas de fim de janela da base e do `REUSE=3` mostram a mesma cena, com todos os objetos.

**Mas as respostas repetidas erram demais**, com a câmera girando (`DM_KEYS` com `RRight`) e também com ela parada:

| Variante (câmera) | relatórios conferidos por quadro | oculto quando visível | visível quando oculto |
|---|---|---|---|
| `REUSE=2` + check (girando) | ~1.100 | ~86 (8 %) | ~385 (35 %) |
| `REUSE=3` + check (girando) | ~1.500 | ~115 (8 %) | ~525 (35 %) |
| `REUSE=2` + check (parada) | ~1.150 | ~88 (8 %) | ~395 (35 %) |

Como o erro é o mesmo com a câmera parada, não é movimento: **o endereço de relatório não identifica o mesmo objeto de um quadro para o outro** (o jogo reaproveita endereços entre objetos, ou a contagem do mesmo objeto oscila). A chave "lugar do reset no quadro" também foi testada, mas aquela medida não vale: o replay numerava os quadros pelo contador do jogo (`g_frame`), e não pelo do quadro reproduzido (`g_job_frame`), então o lugar não recomeçava a cada quadro. Corrigido; falta medir de novo, e o trace dos endereços vai dizer qual é a identidade estável de cada objeto.

**O que identifica um objeto (trace, 12:13–12:30, câmera parada, todos os ~2.320 relatórios de 4 quadros seguidos, duas vezes):**

- **O endereço não identifica nada:** o jogo tira os endereços de resultado de um anel que anda ~0x34000 por quadro e dá a volta; de 2.321 endereços de um quadro, só 55 voltam 3 quadros depois (os primeiros objetos, num bloco de 3 posições de 0x20). Por isso a conferência por endereço errava igual com a câmera parada.
- **O lugar no quadro quase identifica:** casando objetos pela contagem (estável com a câmera parada), o lugar muda 0 ou −1 de um quadro para o outro (objetos que entram e saem antes). Com o quadro do replay corrigido, a chave pelo lugar ainda erra ~107 "oculto quando visível" e ~107 o contrário em ~1.150 conferidos por quadro (9 %).
- 956 dos 2.321 relatórios leem 0 amostras; cada relatório tem 1 consulta (18.292 de 18.655 no trace), 306 nenhuma (sem draw entre o reset e o relatório).

Dois erros meus no caminho, corrigidos: a chave pelo lugar usava o quadro do jogo (`g_frame`) e não o do replay (`g_job_frame`), e com o reuso desligado o lugar não era contado (um `++` dentro de um `&&`).

**Estado:** desligado por padrão e não deve ser ligado: nenhuma das duas identidades é segura (8–9 % de respostas "oculto" erradas). Repetir resultados só funcionaria com uma identidade pelo conteúdo do objeto (por exemplo, o buffer de vértices e a transformação do primeiro draw da consulta), que é a próxima pesquisa possível, ou com uma opção por jogo "consultas rápidas" (o valor fixo 1, a escolha do jogador).

### A identidade pelo conteúdo (15/11, 12:30–13:20)

Pedido do usuário: "Identity by content". O replay passou a nomear cada relatório pelo **primeiro draw depois do reset** (`vka_draw.c`): k1 = shaders, contagens, buffer de índices e primeiro buffer de vértices; k2 = os bytes dos uniform buffers do estágio de vértice (a transformação do objeto). Trace com a câmera parada (todos os relatórios de 4 quadros, duas vezes):

| Chave | únicas por quadro | voltam em 1–3 quadros | "oculto quando visível" entre as que voltam |
|---|---|---|---|
| endereço (antes) | — | 2 % | 8 % |
| lugar no quadro (antes) | — | — | 9 % |
| k1 (só o draw) | 518 de 2.317 | 100 % | 0–0,1 % |
| **k1 + k2** | **2.256 de 2.317** | **92–93 %** | **0,12–0,17 %** |

O reuso foi reescrito em volta dessa chave (`vka_exec.c`, `oqk_*`): a decisão é tomada no primeiro draw do relatório; um objeto já contado só é contado de novo a cada n quadros (fase própria por chave, para espalhar o trabalho); objeto novo, ou com a chave repetida no quadro nos últimos 8 quadros, é contado sempre; a tabela de 16.384 entradas reaproveita as que não aparecem há 64 quadros.

**Conferência (`DM_VK_OQ_REUSE_CHECK=1`), por quadro:**

| Variante | conferidos | oculto quando visível | visível quando oculto |
|---|---|---|---|
| `REUSE=3`, câmera parada, uniforms todos na chave | ~1.400 | ~3 (0,2 %) | ~3 |
| `REUSE=2`, câmera girando, uniforms todos | ~200 | 0,2–0,4 | 0,3–0,4 |
| `REUSE=3`, câmera girando, uniforms todos | ~250 | 0,3–0,5 | 0,5–0,6 |
| `REUSE=3`, câmera girando, **sem o uniform da cena** | ~600 | **10–12 (~2 %)** | 11–13 |

Com todos os uniforms na chave, a câmera girando muda a chave de quase todo objeto (~1.000 "novos" por quadro: a matriz da câmera está nos uniforms) e o reuso quase se desliga sozinho. Tirando da chave o uniform que fica no mesmo endereço do objeto anterior (o da cena), o reuso continua girando, mas repete ~2 % de respostas "oculto" para objetos que entraram na tela.

**fps com a câmera girando** (1 partida cada, chave sem o uniform da cena): base 19,8 fps (GPU 45,4 ms); `REUSE=2` **21,3 fps (GPU 40,5 ms)**; `REUSE=3` inválido (o jogo foi para o fundo no meio da partida, às 13:19, e o adb sem fio caiu).

**Nova regra (compilada, ainda não medida):** só se repete a contagem de um objeto **visível**; um objeto cuja última contagem foi 0 é contado todo quadro. Assim a resposta repetida nunca é "oculto" para algo que entrou na tela (o erro que aparece na imagem); o único erro possível é "visível" para algo que saiu dela, um draw que o jogo corta alguns quadros depois. Cobre ~59 % dos objetos (956 de 2.321 leem 0 no ponto do espantalho). `DM_VK_OQ_REUSE_ZERO=1` volta a repetir zeros também.


**A regra "só visível", medida (13:30–13:47, pelo USB, celular mais quente que de manhã):** conferência com a câmera girando, `REUSE=3`: **0,0 oculto quando visível** por quadro, ~11 visível quando oculto (de ~250–360 conferidos). fps: girando, base 17,0 (GPU 52,8 ms), `REUSE=2` 17,7 (50,7), `REUSE=3` 17,5 (50,1); parada, base 15,8 (55,7), `REUSE=3` 16,8 (51,4). Ganho menor: os ~860 objetos que leem 0 são contados todo quadro, e só ~38 % dos relatórios são repetidos (contra 67 % repetindo tudo).

**Próxima regra (compilada às 13:50):** um zero também é repetido, mas só enquanto os uniform buffers da cena (os que ficam no mesmo endereço do objeto anterior: câmera, constantes da cena; k3 = os primeiros 512 bytes deles) estão byte a byte iguais a quando ele foi contado. Câmera parada e objeto parado: nada pode entrar na tela; a câmera mexeu: o zero é contado de novo.

**A regra com a cena, medida e desfeita (13:48–14:10):** com a câmera parada, ~810 objetos por quadro continuaram contados como "ocultos": os uniforms da cena mudam todo quadro mesmo parados (provavelmente o jitter do anti-aliasing temporal na matriz de projeção), então a comparação byte a byte quase nunca bate. Ainda deixou passar 0,2 "oculto quando visível" por quadro. Mesmo fps que a regra "só visível" (parada: base 15,9, `REUSE=3` 16,9, `REUSE=2` 16,9; girando: base 15,3, `REUSE=3` 17,4). Desfeita.

**Estado final (14:10):** `DM_VK_OQ_REUSE=<n>` com chave pelo conteúdo e só respostas "visível" repetidas. Erro que aparece na imagem: 0,0 por quadro na conferência. Ganho no ponto do espantalho com o celular quente: ~+1 fps (15,8 → 16,8), GPU −4 ms; a imagem de fim de janela é a mesma cena. Desligado por padrão (n = 1).

O que ficou de fora e por quê:

- **Repetir zeros também** (`DM_VK_OQ_REUSE_ZERO=1`): +2 a +3 fps a mais no ponto parado, mas ~2 % das respostas repetidas são "oculto" para objetos que entraram na tela com a câmera girando: objetos aparecendo 1–2 quadros atrasados nas bordas.
- **Endereço ou lugar no quadro como identidade:** o anel de endereços do jogo e a ordem que muda erram 8–9 %.

**Ligado por padrão (14:20) e conferido em outro jogo (14:44–15:00):** Zelda, ponto do espantalho, mesma partida seguida: padrão (`REUSE=3`) 17,3 fps, GPU 51,6 ms; `DM_VK_OQ_REUSE=1` 16,1 fps, 55,6 ms. **Donkey Kong Bananza** no app de teste (o pack do app assinado de 09/11 copiado para `Switch2/Work/040074A01BF12000/test-pack`, o save de teste reposto a cada partida, `android_bench.py --pack`), tutorial de abertura: só ~3 relatórios e 81 consultas de oclusão por quadro, então o reuso quase não age; ligado e desligado deram 30,1 e 29,8 fps (GPU 21,3 e 21,2 ms), a mesma imagem de fim de janela e as mesmas 14 linhas `[RED]`. A primeira partida com o reuso teve um quadro de 2,2 s (cache de pipelines aquecendo depois das sondagens), que não se repetiu.


## Etapa 2, oitavo passo: o fallback de mesh fora da passagem? (15/11, 15:40–16:05)

Pedido do usuário: "Fallback de mesh" (o G2 do plano: rodar o controlador e o compute antes de a passagem abrir, para que cada draw emulado não feche a passagem). Antes de reescrever, a medida do que as quebras custam: `DM_VK_MESH_DIAG_STAGE=3` (novo, diagnóstico) larga o draw emulado antes de ele fechar a passagem; `=1` roda só o controlador (a passagem ainda é quebrada, sem o compute nem o draw que puxa). Ponto do espantalho, 1 partida cada, save reposto, trava em 30, sem LSFG:

| Variante | fps | GPU p50 | passagens por quadro |
|---|---|---|---|
| completo | 18,7 | 48,2 ms | 186 |
| só o controlador (`=1`) | 28,2 | 31,4 ms | 114 |
| nada do fallback (`=3`) | 28,6 | 29,5 ms | 104 |

- **O controlador e as suas quebras de passagem custam 1,9 ms.** No completo cada draw emulado reabre uma passagem (cerca de 72 a mais que o `=1`), mas os draws que puxam os vértices **junto com** essas reaberturas já tinham sido medidos em 2,5 ms (`=2`, quinto passo).
- **O G2 ganharia no máximo 2 a 4 ms de 17**, com uma reescrita grande e arriscada (simular o estado de cada draw adiante e dar a cada um a sua região no buffer de saída). Não foi feito.
- **Os ~15 ms restantes são o compute dos próprios mesh shaders traduzidos**, o mesmo que o perfil passo a passo mede (15,5 ms). Nem banda (contadores), nem lanes paradas (pareamento), nem cantos mortos (compactação), nem as quebras de passagem: o que sobra é o código dos shaders (registradores e ocupação no Adreno, os laços e as barreiras de memória compartilhada de cada grupo), que pede o compilador do driver ou uma ferramenta de estatística de shader para ver.

## Etapa 2, nono passo: a estatística dos shaders pelo driver (15/11, 16:05–16:50)

Pedido do usuário: "Estatística dos shaders". O driver 891.14 do POCO tem `VK_KHR_pipeline_executable_properties`; o replay passou a pedir as estatísticas de cada pipeline de compute que faz (`DM_VK_PIPE_STATS=1`, diagnóstico, lido ao criar o dispositivo; `vka_pso.c pipe_stats`). 192 pipelines no ponto do espantalho:

| Programa | instruções | registradores | spill | memória compartilhada por grupo de 32 | **ocupação da ALU** |
|---|---|---|---|---|---|
| mesh `604134a5262ff670` | 26.474 | 48 | 520 B | 12.144 B | **4 %** |
| mesh `fed302d791a4ba43` | 25.618 | 43 | 0 | 13.200 B | **4 %** |
| mesh `73dc0b90c4082e43` | 47.662 | 48 | 536 B | 10.296 B | **6 %** |
| task `eb021183bf9b0092` | 29.111 | 42 | 24 B | 8.316 B | **6 %** |
| mesh `d98076e4148db34f` | 2.597 | 15 | 0 | 3.696 B | 10 % |

- A ocupação segue cerca de 300 ÷ registradores (10 → 25-28 %, 20 → 12 %, 30 → 9 %, 40+ → 4-6 %), e a memória compartilhada a baixa ainda mais (10 registradores com 3,7 KB: 10 %). Os computes comuns do jogo: mediana de 10 registradores e 28 %; os de mesh: 23 e 9 %, e os pesados 4-6 %, com spill.
- **É isso que deixa a GPU "ocupada" mas parada** (os 40 % de shaders parados do Profiler): quase nenhuma onda em voo para esconder a latência.
- **`spirv-opt -O` nos 520 computes de mesh (por substituição, `DM_VK_SPV_OVERRIDE`) não mudou nada:** as mesmas instruções, registradores e ocupação no driver, o mesmo fps (17,2 contra 17,3 / 18,6). O compilador da Qualcomm já faz essas otimizações; o limite está na forma do código.
- **De onde vem a memória compartilhada:** a cópia dos planos de `defer_mesh_plane_stores` (`vk_android_spv.py`, de 12/11: cada saída do mesh vai primeiro para uma cópia em memória compartilhada, de até ~20 KB, com uma máscara de `OpAtomicOr`, e um laço no fim a entrega às saídas; feita porque o Turnip morria compilando a cadeia de stores inlinada em cada chamada). A DroidPlay (MIT, `2e802839`) achou o mesmo gargalo nos seus kernels de mesh (11-13 KB por grupo de 32; dobrar o vetor dobrou o tempo) e o resolveu gravando cada saída direto no buffer de saída, no layout de planos `(v/32)*nvc*32 + k*32 + v%32`: no Adreno 830, kernels de mesh de 5,1 para 3,3 ms, o jogo de 27,0 para 29,4 fps. O fork do Erick ganhou 7,47 → 6,05 ms trocando a cadeia por uma tabela (`eed329f`, GPL: só a ideia). Análises: `docs/ANALISE-DROIDPLAY-0.8.5-0.8.11-2026-11-15.md`, `docs/ANALISE-ERICK-2026-11-15.md`.

## Etapa 2, décimo passo: saídas diretas do mesh (15/11, 16:55–17:46)

Pedido do usuário: "Sim, saídas diretas". Testado por substituição de módulos (`DM_VK_SPV_OVERRIDE`), sem preparar o jogo; a ferramenta ficou em `dm/gfx/tools/vk_android_mesh_direct.py` (de agente, fora da cadeia: reproduz byte a byte os módulos medidos).

- **A (sem a cópia):** as chamadas de `mesh_plane_defer` voltam para `mesh_plane_store`, e a cópia dos planos em memória compartilhada, a máscara e os laços de limpar e entregar saem (344 dos 520 computes de mesh tinham a cópia).
- **A2:** A com `mesh_plane_store` marcado DontInline: o driver ignora (as mesmas instruções), e o compilado sobe para 8,6-9,4 s por pipeline.
- **B (saídas diretas, como a DroidPlay `2e802839`, MIT):** A, e as cadeias de `if (plano == p) saída.membro[índice] = valor` de `mesh_plane_store` viram uma consulta em duas tabelas (deslocamento e passo de cada plano, das decorações do bloco de saída) e uma gravação numa vista `uint[]` do mesmo buffer de saída. A cópia de profundidade dos planos 2 e 3 (`mesh_position_zw`) fica. 500 dos 520 módulos mudaram, todos válidos.

**Ponto do espantalho** (trava em 30, save reposto, partidas alternadas):

| Variante | fps | GPU p50 | instruções (`604134a5`) | ocupação (pesados / `d98076e4`) |
|---|---|---|---|---|
| biblioteca | 18,7 / 18,5 | 47,7 / 48,0 ms | 26.474 | 4-6 % / 10 % |
| A | 21,4 / 21,6 | 40,6 / 40,7 ms | 81.906 | 6 % / 25 % |
| A2 | 21,5 | 40,8 ms | 81.906 | 6 % |
| **B** | **22,7 / 22,5** | **39,1 / 38,9 ms** | **31.741** | **6 % / 18 %** |

**Ponto do Mido** (sem trava): biblioteca 30,0 fps (GPU 25,2 ms, presa na cadência); **B 41,6 fps (GPU 20,7 ms)**.

- **+21 % de fps e −8,8 ms de GPU no ponto pesado; −4,5 ms no Mido.** A imagem de fim de janela é a mesma cena nos dois pontos.
- **O tempo de compilação não piorou:** nas partidas que recompilam tudo (`DM_VK_PIPE_STATS`), a biblioteca levou 2,4-4,8 s nos pipelines de mesh mais pesados e B 1,5-4,2 s. Os 7-8 s vistos numa partida de B foram compilações junto com o pré-aquecimento, disputando a CPU, o que vale para qualquer versão.
- **O que falta para os jogadores:** levar B para a cadeia (`vk_android_mesh.py`, versão da cadeia nova, biblioteca completa de cada jogo com o seu `vk_delta.tsv`). No celular, a atualização incremental traduz de novo só as entradas de mesh que mudaram (cerca de 500 no Zelda), não as 42 mil.

## A biblioteca nova na cadeia, e o reuso de oclusão desligado de novo (15/11, 17:50–19:05)

Pedido do usuário: "Sim, para a cadeia".

- **Cadeia:** `vk_android_mesh.py` `direct_outputs` aplica as saídas diretas (passo anterior) na forma compute de todo programa de mesh; a versão da cadeia passou de `4e12942060ff` a **`e3d2789e93f6`**, com a mesma ABI (`623c0a2f1e8a`: um celular joga a biblioteca antiga enquanto a atualização roda). Abaixar de novo as 520 entradas da biblioteca antiga dá byte a byte os módulos medidos no celular. Testes: os 25 da cadeia e os 18 do mesh no WSL (um teste novo em `test_vk_mesh_defer.py`: sem cópia, sem a cadeia de `if`, uma gravação direta).
- **Biblioteca completa do Zelda** no WSL (`output/build/zelda-vklib-e3d2.sh`, 44 min, J=12): 42.847 de 42.847 shaders, todos os módulos válidos, nenhuma linha `[RED]`, 1,25 GB. **Manifesto** `dm/games/0400486024DEC000/vk_delta.tsv` publicado: um celular com a biblioteca `4e12942060ff` traduz de novo só **500** shaders (os de mesh); com as anteriores, de 1.829 a 2.633. Uma cópia da biblioteca ficou em `mesh-emulation/cache/v-e3d2789e93f6` na pasta de trabalho do Zelda.
- **No POCO, sem substituições** (a biblioteca no `test-pack`, o replay carregou `v-e3d2789e93f6`): espantalho 22,8 / 23,1 fps (GPU 39,0 / 38,8 ms), igual às substituições; Mido 41,8 fps.
- **O brilho no Link** (o usuário viu partes de luz no Link voltarem): A/B no Mido com 4 capturas de tela por partida, a cada 8 s, dentro da janela:

| Variante | capturas com o brilho | fps |
|---|---|---|
| biblioteca nova + reuso 3 (o padrão de então) | **2 de 4** (riscos brancos fortes nas costas) | 41,8 |
| biblioteca nova, reuso desligado | 0 de 4 | 41,0 |
| biblioteca antiga, reuso desligado | 0 de 4 | 34,2 |

  **É o reuso de oclusão**, não a biblioteca nova: um efeito de luz que o jogo mede com uma consulta é desenhado por cima de tudo enquanto ela lê visível, e o "visível" repetido o mantém por até 2 quadros depois de o Link ficar na frente da luz. O erro "visível quando oculto" da conferência (cerca de 11 por quadro) não é inofensivo para esses efeitos. **O reuso voltou a ser desligado por padrão** (`DM_VK_OQ_REUSE=1`); fica como opção, e o que falta para religar está no verbete (`docs/reference/rendering.md`, "Open"). As saídas diretas do mesh ficam: a biblioteca nova não mostrou o brilho.

## Portes dos outros projetos, primeiro lote (15/11, 19:00–19:20, sem o celular)

Da análise consolidada (`docs/ANALISE-PORTS-2026-11-15.md`), o lote só de host, de baixo risco, sem cadeia nem pack:

- **`vkfb_drop`** (Banana Native `b29c30af`, MIT, o mesmo `vka_fileback.h` que o nosso): um pool que vai para o arquivo não segura mais cada página copiada duas vezes; a cada 64 MiB copiados, `sync_file_range`, `MADV_DONTNEED` e `POSIX_FADV_DONTNEED`. Verbete em `docs/reference/memory.md`.
- **A falta de escrita libera a faixa protegida em volta** (`fault_run` em `nvn_dirty.c`, Banana Native `6624426f`, MIT; `DM_DIRTY_FAULT_RUN_KB`, padrão 1024): menos mapas do driver por falta (a frente de `vm.max_map_count` do Zelda no Turnip e no Redmagic). Verbete em `docs/reference/memory.md`.
- **A ordem do save do cache de pipelines** (`vka_pso.c` `cs_obtain`): o compute é anotado na lista de pré-aquecimento antes do save forçado que um compile de mais de 1 s dispara (antes o save saía sem ele; achado do Banana `67a2da0a`).
- **A lista de pré-aquecimento do Zelda** juntada com a da Banana v0.3.25 (`dm/tools/vk_prewarm.py seed`): de 865 para **3.456** chaves (396 compute, 95 mesh, 2.965 gráficas), o mesmo layout de chave (`3710792d`).
- **`plat_zone_malloc` alinhado a 16** no Android (DroidPlay `e52f2026`, MIT): o `malloc` do bionic pode dar blocos alinhados a 8 (jemalloc), e o `hle_uemalloc.c` (o malloc do Unreal) conta com 16.
- **Não portado:** `DM_VK_CS_ASYNC` (Banana `67a2da0a`): ele pula o dispatch enquanto o compute compila, e aqui os draws esperam o pipeline justamente para não perder efeitos persistentes; pular um compute que grava um buffer uma vez só o deixaria velho. A lista de pré-aquecimento maior ataca a mesma travada sem pular nada.

Compilado (APK de teste); ainda não rodado no celular (a fila do POCO tinha as sessões do BOTW e do TOTK).

**Conferência do brilho com o reuso desligado (20:16–20:22):** biblioteca `e3d2789e93f6`, `DM_VK_OQ_REUSE` no novo padrão (1), Mido, 2 partidas com capturas a cada 8 s: **brilho em 0 de 12 capturas** (contra 2 de 4 com o reuso em 3), 41,9 / 41,1 fps (igual aos 41,8 com o reuso: no Mido ele não ganhava nada). A lista de pré-aquecimento maior só vale como semente de uma instalação nova: o app de teste usa a lista das próprias execuções (1.204 chaves, `vk_pipelines.keys`).
