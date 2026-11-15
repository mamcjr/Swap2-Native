# Plano completo de otimização e estabilidade (15/11/2026)

Pedido do usuário em 15/11: analisar o projeto inteiro e o Switch 2, e montar o planejamento de tudo que pode ser otimizado, com abordagens mais baratas, para reduzir o tempo de quadro, dar estabilidade, gastar menos RAM, segurar 30 fps estáveis com o mínimo de travamentos e resolver os bugs gráficos.

**Como ler:** cada número traz a origem. "Medido" é uma linha de log ou de nota; "estimado" é conta minha; "suspeito" é mecanismo visto no código sem medida. **Nenhum ganho abaixo foi medido ainda, salvo quando dito.** Nada deste plano foi implementado: é o planejamento.

**Base:** árvore da 0.1.8.6-beta; logs `output/zelda-1112/gm-final.log`, `gm-placed.log`, `prefer1.log` (POCO F7 Ultra, Adreno 830, driver 891.14, Zelda: Ocarina of Time em Kokiri, cena em 720p + FSR); a nota do jogo `dm/games/0400486024DEC000/README.md` e as dos outros três jogos de Switch 2; as análises `ANALISE-FRAMETIME-2026-11-13.md`, `ANALISE-OTIMIZACAO-2026-11-12.md`, `ANALISE-ZELDA-RAM-2026-11-09.md`, `ANALISE-BANANA-0.3.12-0.3.14-2026-11-12.md`, `ANALISE-DROIDPLAY-0.8.4-2026-11-13.md`, `ANALISE-ERICK-60FPS-2026-11-12.md`; leitura do código em `dm/gfx/vk_android`, `dm/gfx/nvn`, `dm/host` e do launcher.

**Andamento:** etapa 1 executada em 15/11 de madrugada; resultados e o que mudou na ordem em [RESULTADOS-ETAPA1-2026-11-15.md](RESULTADOS-ETAPA1-2026-11-15.md). Em resumo: o quadro de Kokiri é CPU do jogo + GPU em série (o jogo espera a GPU terminar a passagem principal); na GPU, o fallback de mesh (9 a 18 ms) e as passagens (11 a 27 ms) são os alvos; o Zelda passou a ter `fps_target 30`; o ponto do espantalho em Kokiri roda a 17 fps (GPU 52 ms) e é a próxima referência.

## 1. Resumo

1. **Na cena do jogo o limite é a GPU, e o custo é fixo, não por pixel.** Medido: baixar a cena para um quinto dos pixels tira só 4 ms de 25,5; 540p deu 66 → 60 ms. O que pesa é o número de passagens (145 por quadro), os dispatches de compute (93 por quadro) e as barreiras (162 completas por quadro) numa GPU de tiles. **Baixar resolução não é a alavanca; cortar passagens e compute é.**
2. **A CPU do replay é o segundo limite** e passa a ser o primeiro em cenas com muitos draws: 9,8 ms por quadro em Kokiri (749 draws), 19 a 26 ms na casa do Link (2.270 draws), 80 ms no Donkey Kong (5.500 draws). O custo é por draw (9 a 13 µs), e a maior parte é refeita a cada draw sem necessidade.
3. **Os travamentos têm três fontes medidas:** pipelines compilados no caminho do quadro (a frio, quadros de 35 s; a quente, 1,5 a 1,9 s), paginação (16 a 31 mil faltas de página maiores por quadro ao girar a câmera) e calor (98 °C com o piso do clock da GPU em 100 %).
4. **A RAM pedida é de cerca de 8 GiB físicos** na cena do Zelda; o POCO de 16 GB aguenta porque 4 GiB ficam no zram. Um aparelho de 12 GB sem essa folga morre na entrada da cena. Não existe hoje um modo para pouca RAM.
5. **O jogo não está travado em 30:** ele pede intervalo 1 (60 Hz) e na cena todos os quadros passam de 16,7 ms, com média de 31,6 ms e p99 de 35 ms. Os quadros saem quando a GPU termina, fora do múltiplo de 33,3 ms, por isso a taxa oscila entre 27 e 35 em vez de ficar em 30.

**Ordem recomendada:** medir (fase 0) → travar em 30 e esfriar (fase 1) → cortar passagens e compute da GPU (fase 2) → pipelines fora do quadro (fase 4) → modo de pouca RAM (fase 5) → CPU por draw (fase 3) → bugs gráficos (fase 7), com a estabilidade por aparelho (fase 6) correndo em paralelo.

## 2. O Switch 2 e o que ele impõe ao port

| Item | Switch 2 | Origem |
|---|---|---|
| SoC | Nvidia T239 sob medida | Digital Foundry, via Notebookcheck |
| CPU | 8 Cortex-A78C, 6 para o jogo e 2 para o sistema; 1.101 MHz portátil, 998 MHz na dock, máximo 1,7 GHz | idem |
| GPU | arquitetura Ampere, 1.536 núcleos CUDA; 561 MHz portátil, 1.007 MHz na dock, máximo 1,4 GHz; cerca de 1,71 / 3,07 TFLOPS | idem; TFLOPS de outras publicações |
| Memória | 12 GB LPDDR5X unificada, 9 GB para o jogo, 3 GB do sistema; 68 GB/s portátil, 102 GB/s na dock | idem |
| Upscaling | DLSS (modos 1x, 2x, 3x) e DLAA | idem |
| Tela e armazenamento | 7,9" 1080p 120 Hz com VRR e HDR; UFS com descompressão por hardware | conhecimento geral, não conferido nesta pesquisa |

Fontes: https://www.notebookcheck.net/Digital-Foundry-confirms-Nintendo-Switch-2-specs-with-CPU-GPU-and-surprising-memory-allocation-details.1015981.0.html e https://www.nintendolife.com/news/2025/05/nintendo-switch-2-final-tech-specs-have-been-confirmed (números de maio de 2025).

O que cada característica significa para nós:

- **CPU lenta e larga (6 núcleos a 1,1 GHz).** Um núcleo de Snapdragon 8 Elite é várias vezes mais rápido, então o código do jogo sobra: a thread principal do Zelda usa 19 % de um núcleo (medido). **A CPU que falta é a nossa**: `vk-enc` 42 % e `vk-replay` 32 % (medido). Os jogos distribuem trabalho em 6 threads iguais; o telefone tem núcleos desiguais e o host hoje não fixa nenhuma thread.
- **GPU de desktop em modo imediato.** Na Ampere, trocar de alvo de render e intercalar compute custa quase nada, então os jogos fazem mais de cem passagens e quase cem dispatches por quadro. Adreno, Mali e Xclipse são GPUs de tiles: cada fim de passagem grava os tiles na memória e a passagem seguinte os lê de volta. É a diferença de arquitetura que mais custa.
- **Mesh shaders, VRS, texturas esparsas e BC nativos.** No telefone: mesh shader nativo sai errado em todos os drivers testados (emulamos com compute, 8 ms por quadro); VRS é ignorado (o jogo desenha tudo em taxa cheia); BC não existe em Mali e Xclipse de fábrica (decodificamos na CPU, 4 a 8 vezes a memória).
- **DLSS.** O jogo traz a rede do upscaler, que rodamos como compute (7,2 a 7,8 ms num quadro de 33) ou trocamos por FSR 1, o padrão de hoje.
- **Memória unificada de 9 GB.** O jogo declara 6,2 GB de heap e põe as texturas e os buffers da GPU na mesma memória. No Android toda textura amostrada passa a existir duas vezes (os bytes do jogo e a imagem do Vulkan) e a memória dada à GPU não pode ir para o swap.
- **Resolução e taxa de quadros dinâmicas.** O jogo mede o tempo da GPU e ajusta sozinho a resolução e o intervalo de apresentação (60, 30 ou 20). Os tempos que devolvemos a ele decidem a nitidez e a taxa: um relatório errado derruba a cena para 683x384 (medido) ou prende o jogo em 20 fps (documentado).
- **Armazenamento rápido.** O jogo lê arquivos de forma síncrona nas próprias threads. No Android cada leitura é um `pread` no armazenamento compartilhado, sem readahead.

## 3. Como o Swap2-Native funciona hoje

- **Código do jogo:** roda nativo. As instruções que o kernel não deixa passar (SVCs, leituras do contador) viram chamadas a um portão que salva os registradores e chama o host (`dm/host/entry.S`, `hle.c`).
- **Gráficos:** a thread do jogo só grava os comandos NVN. `vk-replay` percorre os registros e resolve estado, pipeline e descritores; `vk-enc` faz as chamadas `vkCmd*` e os submits; `vk-complete` espera os fences; `vk-present` apresenta. O replay fica no máximo 1 quadro atrás do jogo.
- **Memória do jogo na GPU:** os pools são memória do Vulkan no endereço do jogo (mapeamento direto) ou um alias exportado (padrão desde a 0.1.8.5); as texturas são imagens separadas, com upload pela CPU e reenvio quando a página suja (proteção de escrita em blocos de 64 KB).
- **Shaders:** a biblioteca do jogo (1,25 GB, 42.847 shaders no Zelda) é traduzida na preparação; os pipelines são criados em tempo de jogo, com cache em disco, lista de pré-aquecimento e bibliotecas de pipeline (GPL) onde o driver aceita.
- **Ritmo:** o intervalo de apresentação é o do jogo; a espera é um `clock_nanosleep` na aquisição da textura da janela; swapchain FIFO com ajuste ao período real do painel (genlock).

## 4. Onde o tempo e a memória vão hoje (medido)

### 4.1 Um quadro em Kokiri (POCO F7 Ultra, 891.14, alias, cena 720p + FSR, `gm-final.log`)

| Medida | Valor |
|---|---|
| Taxa | 31,6 quadros por segundo; quadro inteiro 32 a 34 ms nos longos |
| Intervalo pedido pelo jogo | 1 (60 Hz). Na cena, 300 de 300 quadros acima de 16,7 ms, média 31,6 ms, p99 35,1 ms; no título, 60 fps |
| GPU | 47,8 a 49,2 ms do envio ao fim nos quadros longos (inclui a fila atrás do quadro anterior); a thread do jogo espera fences por 14 ms |
| Trabalho por quadro | 749 draws, 145 passagens, 93 dispatches, 57 limpezas ou cópias, 10 submissões |
| Barreiras por quadro | 162 completas, 144 de anexo, 35 de transferência |
| Atualizações de uniforme embutidas | 80 por quadro em 35 cópias; cada uma fecha a passagem aberta |
| Gravação (`vk-replay`) | 9,83 ms por quadro, 13,1 µs por draw |
| Codificador (`vk-enc`) | 6.727 chamadas, 2,59 ms |
| Descritores | 62 % dos segmentos do driver iguais aos do draw anterior; só 3 % dos pushes pulados |
| Consultas de oclusão | 211 condições de render por quadro, até 414 relatórios em voo |

### 4.2 A cena mais pesada medida (porta da casa do Link, `ANALISE-FRAMETIME-2026-11-13.md`)

| Parte da GPU | Custo |
|---|---|
| Passagens de render (121 a 165) | 10,4 a 15,9 ms |
| Compute do fallback de mesh | 7,9 a 8,3 ms |
| Compute do próprio jogo | 8,0 a 8,3 ms |
| Rede do upscaler (quando ligada) | 7,2 a 7,8 ms |
| **GPU total** | **27 a 33 ms a frio; 34 a 44 ms com o aparelho quente** |
| Gravação na CPU | 19,4 a 26,5 ms (2.270 draws, 8,6 a 11,7 µs por draw) |

### 4.3 Calor (6 minutos na casa, `ANALISE-FRAMETIME`)

| Piso do clock da GPU | Taxa | CPU |
|---|---|---|
| 100 % (padrão) | 28,5 / 27,6 fps | 98 °C |
| 0 % | 30,7 / 30,4 fps | 91,9 °C |

### 4.4 Travamentos (nota do Zelda)

| Situação | Pior quadro |
|---|---|
| Pipelines a frio, compilação síncrona | 35,6 s (94 pipelines num quadro); cena preta por mais de 2 minutos |
| Pipelines a frio, compilação em segundo plano | 15,1 s; HUD sobre preto por 5 a 7 minutos; compute continua no quadro (o mais lento 5,9 s) |
| A quente | 1,5 a 1,9 s, sem pipeline novo |
| Girar a câmera com memória paginada | 1,2 a 19,5 fps, 16.000 a 31.000 faltas maiores por quadro |
| Pokopia | um pipeline de compute compilou por 99,3 s na thread do replay |

### 4.5 Memória em Kokiri (POCO de 16 GB, `gm-final.log` e `gm-placed.log`)

| Item | MiB |
|---|---|
| Pools do jogo entregues à GPU (não vão para o swap) | 1.625 |
| Imagens (983; 34 dedicadas somam 875) | 1.416 |
| Staging de upload | 80 a 96 |
| Anônima (heap, região de alias, host): residente + no zram | 820 + 3.978 no alias; 1.805 + 2.962 no mapeamento direto |
| Biblioteca de shaders (mapeada de arquivo, recuperável) | 1.254 |
| Memória disponível, do título à cena | cerca de 5.300 → 1.931 no mínimo |
| Maiores imagens | alvo de profundidade 12288² = 290; duas 4096² sobre o mesmo armazenamento = 64 cada; 3D 512²×64 = 64,5 |
| GPUs sem BC | +581 de imagens nesta cena |

**Lacunas de medida:** o tempo de apresentação, o trabalho de CPU do próprio jogo, o custo por chamada do portão e a parte do driver na memória não têm número. A fase 0 fecha isso.

## 5. Metas

| Meta | Hoje | Alvo |
|---|---|---|
| Taxa na cena do jogo | 27 a 35, irregular | 30 travados: 99 % dos quadros em 33,3 ± 2 ms durante 10 minutos |
| GPU por quadro (pior cena medida) | 27 a 33 ms a frio, 34 a 44 quente | até 26 ms, com folga de 20 % para o calor |
| Gravação na CPU | 9 a 13 µs por draw | até 5 µs por draw (2.270 draws em 11 ms) |
| Pior quadro, cache quente | 1,5 a 1,9 s | até 50 ms |
| Primeira entrada numa cena, cache frio | preto por minutos | pipelines prontos antes de jogar; nenhum quadro acima de 250 ms |
| Temperatura em 10 minutos | 98 °C | até 85 °C |
| Demanda física de RAM (Zelda) | cerca de 8 GiB | até 5,5 GiB no modo de pouca RAM |
| Mortes por falta de memória em 30 minutos num aparelho de 12 GB | morre na entrada da cena | zero |

## 6. Plano

Colunas: **Ganho** é estimativa salvo "medido"; **Esforço** B (até um dia), M (2 a 4 dias), A (uma semana ou mais); **Risco** de quebrar imagem ou estabilidade.

### Fase 0 — Medir antes de mexer

| ID | O que | Por quê | Esforço |
|---|---|---|---|
| M1 | **Cena de referência repetível:** uma rodada sem toque, de save fixo, com duração fixa, que grava a tabela por quadro (`DM_NVN_TSV` já existe) e imprime p50, p1, pior quadro, GPU, gravação, passagens e faltas de página. Três cenas: casa do Link, Kokiri, Árvore Deku. Mais uma de 10 minutos para o calor | hoje cada A/B é manual e as medidas de documentos diferentes não são comparáveis | M |
| M2 | **Motivo de cada fim de passagem** contado por quadro (uniforme, dispatch, cópia, primeira imagem, fallback de mesh, 600 draws, fim da submissão) | decide a ordem de G1 a G3; era o item A0 da análise de 13/11, não feito | B |
| M3 | **Tempo de apresentação e CPU do próprio jogo** nas linhas de 300 quadros; contagem amostrada do portão (SVCs e leituras do contador por quadro) | as duas lacunas da seção 4 | B |
| M4 | **Seção de desempenho no Diagnóstico automático:** taxa, GPU, gravação, memória e temperatura da rodada base no relatório | os testadores remotos passam a mandar números de Mali, Xclipse e Redmagic sem instrução manual | M |
| M5 | `scripts/regress.py` no Mac a cada mudança do host | o Mac existe desde 14/11; até aqui nenhuma mudança nossa passou pela regressão do upstream | B, depois da primeira montagem |

### Fase 1 — 30 fps travados e aparelho frio

| ID | O que | Abordagem barata | Ganho | Esforço | Risco |
|---|---|---|---|---|---|
| P1 | **Travar em 30 quando a cena não segura 60** | o limite de 30 já existe (`DM_FPS_TARGET=30`, painel do jogo) e o perfil tem a retenção automática (`fps_hold`: três quadros atrasados sobem a retenção para 2 ou 3 vsyncs). Mesmo assim o log mostra quadros de 31,6 ms em média, fora de 33,3: primeiro descobrir por que a retenção não segura (o painel é de 120 Hz e o período ajustado é o de 60), depois fazê-la apresentar a cada 2 atualizações de 60 Hz e só voltar a 60 depois de 5 s com folga. Conferir com o passo de quadro do jogo (`docs/reference/frame-rate.md`), para o jogo não andar em câmera lenta | não sobe a média; os quadros passam a durar todos 33,3 ms, e a GPU ganha folga | B a M | médio: velocidade do jogo |
| P2 | **Piso do clock da GPU só enquanto a GPU é esperada** (`DM_VK_GPU_FLOOR_AUTO=2`, já existe, padrão 0), mirando 33,3 ms quando P1 trava em 30 | A/B de 10 minutos com M1 | medido em outra forma: 27,6 → 30,4 fps e 98 → 92 °C com piso 0 | B | baixo |
| P3 | **Relatórios do ADPF com a duração real** (`DM_VK_ADPF_SPAN`, existe, nunca conferido no celular) | hoje cada relatório diz "exatamente no alvo", então o governador do sistema não vê a carga | menos quedas de clock; a medir | B | baixo |
| P4 | **Folga térmica do Android** (`AThermal_getThermalHeadroom`): agir antes do estrangulamento | o host só tem a trava térmica do iOS (`hle_nvn.c`); portar a mesma regra | evita a queda para 12 fps a 97 °C já medida | M | baixo |
| P5 | **Lugar das threads:** `vk-replay` e `vk-enc` nos núcleos mais rápidos, threads do jogo fora dos núcleos pequenos e dentro da sessão do ADPF | as chaves de experiência já existem (`DM_VK_AFF_REPLAY`, `DM_VK_AFF_ALL`); medir em SoC 1+3+4 e no 8 Elite | menos variação de quadro; a medir | B para medir | baixo |
| P6 | `android:appCategory="game"` e configuração de Game Mode no manifesto | o upstream tem; nós não | política de morte e de desempenho do sistema | B | nenhum |
| P7 | **Decidir os tempos de GPU que o jogo recebe** (`gpu_timestamps segment` no perfil contra o A/B de 08/11 que escolheu `submission`) | uma rodada de M1 em cada modo, olhando a resolução da cena no log | acaba a imagem mole em 683x384 | B | baixo |

### Fase 2 — Tempo de GPU

O alvo é o custo fixo: passagens, compute e barreiras.

| ID | O que | Abordagem barata | Ganho | Esforço | Risco |
|---|---|---|---|---|---|
| G1 | **Atualização de uniforme sem fechar a passagem** (`DM_VK_UB_DEFER=1`, existe, padrão 0) | A/B no celular; as 80 atualizações por quadro chegam em 35 cópias, cada uma fechando uma passagem | até 35 passagens a menos; 0 a 1,5 ms | B | médio: uniforme errado se a sombra falhar |
| G2 | **Fallback de mesh antes de a passagem abrir** | cada draw emulado fecha a passagem pelo menos 3 vezes (`vka_mesh.inc`); rodar todos os passos de controle e compute da passagem antes dela, como `mdx_scan` já faz para as contagens | 0 a 4 ms; dezenas de passagens a menos | A | alto: ordem contra as escritas do jogo |
| G3 | **Controlador do fallback em paralelo e com menos passos** (`mesh_control.comp` ainda roda com `local_size_x=1`) | um dispatch por lote em vez de três por draw | 1 a 2,5 ms | M | médio |
| G4 | **Carga e gravação dos anexos:** hoje todo anexo é `LOAD` + `STORE` em layout `GENERAL`, e `DiscardColor` / `DiscardDepthStencil` do jogo são ignorados | honrar os descartes do jogo (`DONT_CARE`); olhar adiante por uma limpeza total; resolver MSAA como anexo de resolução em vez de cópia fora da passagem | 0 a 1 ms na análise anterior; suspeito de valer mais em Mali | M | médio: conteúdo errado se a previsão falhar |
| G5 | **Barreiras completas:** 162 por quadro, duas no início de cada uma das 10 submissões | trocar por barreiras do escopo exato onde o replay sabe o que foi escrito; barreira de transferência no dispatch indireto | até 0,3 ms por item; o total a medir com M2 | M | médio |
| G6 | **Imagem criada no primeiro uso fecha a passagem** | criar as imagens na varredura da submissão, antes da primeira passagem | menos passagens e menos picos | M | baixo |
| G7 | **Núcleos de compute de folhagem sem memória compartilhada** (ideia do DroidPlay) | medir primeiro o nosso | 25 % (Adreno 840) a 38 % (830) no kernel, medido lá | M | médio |
| G8 | **Taxa de sombreamento variável do jogo** (`BindShadingRateImage`, hoje sem implementação e com linha `[RED]` a cada quadro) | mapear a imagem de taxas do jogo em `VK_KHR_fragment_shading_rate` onde o driver tem anexo de taxa | desconhecido: é a economia que o próprio jogo pede no console; medir quanto da tela ele marca | M | baixo: só ativa onde o jogo usa |
| G9 | **Um blit a menos na apresentação** | FSR ou a rotação desenham direto na imagem do swapchain | cerca de 0,5 ms, estimado | B a M | baixo |
| G10 | **Mesh shader nativo** onde o driver acerta | pesquisa: hoje sai errado no Adreno 850 e derruba o Turnip V39 | 0 a 8 ms | A | alto |

**Não é alavanca (medido):** baixar a resolução (4 ms de 25,5 com um quinto dos pixels), desligar oclusão (1.100 → 48 consultas, taxa igual), VRS forçado, SGSR1 e desligar vegetação (ganho praticamente nulo e regra específica de jogo).

### Fase 3 — CPU da gravação

| ID | O que | Abordagem barata | Ganho | Esforço | Risco |
|---|---|---|---|---|---|
| C1 | **Contador de geração no estado de ligações** | hoje `walk_all`, a montagem de todos os `VkWriteDescriptorSet` e a comparação do push são refeitos a cada draw (`vka_draw.c`), embora 62 % dos segmentos repitam os do draw anterior; com a geração, o draw que não mudou shader nem ligação pula as três etapas | o maior da fase: 2 a 6 ms nas cenas de 2.000 draws | M | médio: textura velha se uma invalidação faltar |
| C2 | **Push parcial** (`DM_VK_PUSH_DELTA=1`, existe, desligado até ser conferido no celular) | A/B por driver | parte de C1 | B | médio: driver que erra push incremental |
| C3 | **Segmentos do driver:** preencher e comparar 2,5 KB por banco, por estágio, por draw | só quando a geração de C1 mudou | incluso em C1 | B | baixo |
| C4 | **Entrada de vértices** reenviada depois de cada troca de pipeline | reenviar só se o layout mudou | pequeno | B | baixo |
| C5 | **Chave de pipeline:** `memset`, preenchimento, hash FNV e mutex a cada mudança | bits de sujeira por campo e consulta sem trava na leitura | pequeno a médio | M | baixo |
| C6 | **Trava global dos primitivos de sincronização** (`G` em `hle_thread.c` serve mutex, condvar, WaitForAddress e WaitSynchronization) | medir a contenção com `simpleperf`; se aparecer, travas por balde de endereço | suspeito; relevante com 6 threads de trabalho | M | médio |
| C7 | **Leitura do contador de tempo pelo portão** (cada `mrs cntvct` salva 800 bytes de registradores) | medir com M3; se quente, o reescritor emite a leitura direta com a escala | suspeito | M | baixo |

### Fase 4 — Travamentos

| ID | O que | Abordagem barata | Ganho | Esforço | Risco |
|---|---|---|---|---|---|
| H1 | **Pipelines prontos antes de jogar** | (a) a lista de chaves de pré-aquecimento vai dentro do pack e do perfil (o perfil já aceita `vk_prewarm.bin`), incluindo compute e mesh; (b) a tela de preparação compila a lista por driver, com progresso e trava de memória; (c) o app exporta a lista de chaves de quem já jogou, para ir ao pack seguinte | acaba o preto de minutos na primeira entrada (medido: 35 s no pior quadro) | M a A; a tela é desenho do GPT | baixo |
| H2 | **Compute e mesh fora do caminho do quadro** (hoje sempre síncronos) | compilar na fila de segundo plano e esperar no máximo alguns ms; para compute que só alimenta o próprio quadro, repetir a saída anterior nesse quadro | acaba o quadro de 5,9 s e o de 99 s do Pokopia | M | médio |
| H3 | **Pular só o que pode ser pulado** | com o pipeline ainda compilando, pular o draw apenas em alvo que o jogo limpa todo quadro; nunca em alvo persistente; nunca esperar mais de 50 ms | troca congelamento por um objeto ausente por instantes | M | médio: a tentativa anterior deixou a cena preta por a fila crescer mais que o serviço, por isso depende de H1 |
| H4 | **Gravação síncrona do cache depois de um compile de mais de 1 s** | passar para a thread `vk-pcache` | tira um pico | B | nenhum |
| H5 | **Paginação ao girar a câmera** | (a) a fase 5 tira memória do zram; (b) `MADV_WILLNEED` nos pools com arquivo ao carregar a área; (c) não pôr em arquivo o pool que a CPU lê todo quadro | acaba a queda para 1 a 19 fps | M | baixo |
| H6 | **Censo de memória a cada 10 s** (`smaps_rollup` sobre vários GB de mapas) | suspeito de segurar a trava de memória contra o `mprotect` das threads do jogo; em release, só quando a RSS mexe 256 MiB ou no diagnóstico | um pico periódico a menos, a confirmar | B | nenhum |
| H7 | **Upload de textura** | decodificação BC nas threads de upload em vez de serial no `vk-replay` (Mali e Xclipse); reenviar só as linhas sujas em vez da textura inteira | picos de carga menores | M | baixo |
| H8 | **Leitura de arquivos** | `posix_fadvise` e readahead no `romfs.bin` (não há nenhum hoje); medir o custo do armazenamento compartilhado com `DM_FS` | carregamentos mais curtos; a medir | B | nenhum |
| H9 | **Log em release** | cada linha vai ao arquivo e ao logcat, e a tela relê o fim do log a cada segundo | pequeno | B | nenhum |
| H10 | **Áudio:** subexecuções contadas mas fora do relatório | pôr no relatório do diagnóstico; conferir a prioridade da thread do renderizador | diagnóstico | B | nenhum |

### Fase 5 — RAM

| ID | O que | Abordagem barata | Economia | Custo | Risco |
|---|---|---|---|---|---|
| R1 | **Modo de pouca RAM automático** (aparelho de 12 GiB ou menos) | portar o portão `vka_low_ram` do Banana Native 0.3.24 e derivar os limites da memória do aparelho em vez de chave por jogo: partes de 256 MiB na colocação, teto de 1.024 MiB no cache de imagens | medido lá: memória colocada 887 → 553 MiB num AYN Thor | B a M | baixo |
| R2 | **Informar ao jogo um total menor** (`low_ram_app_pool_mb` do upstream) | o SDK guarda menos páginas liberadas | medido lá em outro jogo: região de alias 5,5–6,3 → 4,65 GB e mortes 3 de 3 → 0 de 4 | nenhum se o SDK se adapta | médio: não se sabe se o SDK do Zelda dimensiona pelo total |
| R3 | **Economia de RAM e cena em 720p ligadas por padrão** em 12 GiB ou menos (o item 15 do plano híbrido, ainda aberto) | duas preferências | 1 a 2 GiB de anônima, estimado (2,4 GiB de pools passam a ter arquivo) | cerca de 2 GB de escrita por área; carregamento mais longo | médio: armazenamento lento |
| R4 | **Avisar o kernel que o pool de textura esfriou** depois do upload (`MADV_COLD`) | a textura existe duas vezes: os bytes do jogo e a imagem; os bytes só voltam a ser lidos se o jogo os reescreve. Uma chamada por pool, sem arquivo. Vale só para os pools que ficam anônimos; os entregues à GPU não saem da memória. Não há nenhum `madvise` desse tipo hoje | a mesma faixa de R3 sem a escrita em disco, estimado | uma falta de página se o jogo reler | baixo |
| R5 | **Texturas suaves** (`vka_soft.h` do upstream: sem o mip mais fino das texturas BC grandes) no modo de pouca RAM | port | 300 a 450 MiB; divide por quatro o custo do BC decodificado em Mali e Xclipse | um mip menos nítido | médio |
| R6 | **Sinal de pressão que funciona:** `/proc/pressure/memory` não é legível (medido) e o limite do cache nunca atua no POCO | `onTrimMemory` e `ActivityManager.getMemoryInfo` do Java para o host; regra que conta o swap | o cache passa a encolher antes da morte | B a M | baixo |
| R7 | **Imagens sobre o mesmo armazenamento dividem a memória** | 64 + 64 MiB aqui; o Anka mediu 345 em 110 quadros | 64 a 345 MiB | nenhum | médio-alto: reinterpretação de formato |
| R8 | **BC em GPUs sem BC:** hoje RGBA8, +581 MiB nesta cena | primeiro R5; depois transcodificar para ASTC ou ETC2 na preparação | até os 581 MiB | tempo de preparação | médio |
| R9 | **Alvo de sombra de 12288² (290 MiB)** | predefinição de cena por jogo com alvo menor; é regra de jogo, só se as gerais não bastarem | cerca de 216 MiB | sombra | médio |
| R10 | **Launcher vivo ao lado do jogo** | soltar capas e bitmaps ao iniciar o jogo | suspeito, dezenas de MiB | nenhum | nenhum |

**Fica fora:** despejar imagens escritas pela GPU sem prova de que morreram (perde a única cópia; o próprio upstream deixou aberto).

### Fase 6 — Estabilidade por aparelho

| ID | Aparelho | Situação | Próximo passo |
|---|---|---|---|
| E1 | Redmagic 11 Air (Adreno 830) | morto por sinal 9 na carga da cena em todo driver com mapeamento direto; joga pelo alias, padrão desde a 0.1.8.5. Causa no kernel desconhecida | log da partida do vídeo; rodadas do diagnóstico com variantes do mapeamento direto |
| E2 | Galaxy Tab S10 Ultra e outros de 12 GB em Mali | falta de memória na entrada da cena (disponível 3.412 → 626 MiB) | R1 a R5; relatório do diagnóstico numa versão com o censo |
| E3 | POCO X7 Pro (Mali-G720) | falha aos 3,5 s em `main+0x773270` nos dois drivers; dump danificado é a suspeita | **conferir o dump na checagem de prontidão**: comparar o build ID do NSO com os do perfil e avisar "dump danificado ou versão desconhecida" |
| E4 | Redmagic 10 Pro | morto enquanto o driver compila um pipeline no fim da abertura | H1 com a trava de memória; rodada do diagnóstico |
| E5 | Redmagic 12 (Adreno 850) | mesh nativo sai preto; o fallback é o padrão em Qualcomm mas não foi validado lá | validar o fallback pelo diagnóstico |
| E6 | Xclipse | nenhuma partida registrada | achar um testador; M4 traz os números |
| E7 | POCO F7 Ultra, driver 891.11 | 3 de 9 partidas mortas por falta de memória (PSS 7,4 GB) | fase 5 |
| E8 | Turnip V39 | 13 fps; a versão corrigida perde o dispositivo aos 130 s | manter fora das recomendações; lista de drivers conhecidos por aparelho |
| E9 | Todos | espera longa no caminho do quadro vira "não está respondendo" | vigia: acima de alguns segundos, mostrar "compilando shaders" em vez de congelar |

### Fase 7 — Bugs gráficos

Primeiro o método, depois a lista.

| ID | O que | Por quê |
|---|---|---|
| B0 | **Rodada com as camadas de validação do Vulkan** no app de teste, em cada cena de M1, corrigindo cada violação | a análise da porta já achou duas (armazenamento float contra `R32_UINT`; limite de distâncias de clip e cull). Comportamento indefinido é o que sai certo num driver e lixo em outro |
| B1 | **Passada de conferência no celular** dos oito consertos que estão no código e nunca foram vistos: contagem de oclusão (névoa sobre o Link, folhas e luzes piscando), somas atômicas em float (cores do céu e da névoa), ponteiro da folhagem, profundidade com máscara parcial, NDVE, texto da interface, `nonuniform_index` (água e luzes agrupadas), `render_cond` | um save em cada lugar e uma captura de referência; sem isso não se sabe quais bugs ainda existem |
| B2 | **Triagem por captura:** o mesmo quadro capturado (`vka_capture.c`) reproduzido em outra GPU | se sai certo numa e errado na outra, é driver ou fallback; se sai errado nas duas, é tradução. Corta o tempo de cada investigação |

| ID | Bug aberto | Causa conhecida ou suspeita | Caminho |
|---|---|---|---|
| B3 | Entrada do upscaler sem armazenamento (`a335c1db6270c355`, ligação 3, formato E5B9G9R9) | o formato não pode ser imagem de armazenamento; a rede e o FSR leem bits velhos | imagem de armazenamento em outro formato com conversão |
| B4 | Alvo de cor no formato 0x49 não pode ser anexo | falta o mapeamento de formato | formato substituto |
| B5 | `DrawMeshTasksIndirect` ignora `first`; as formas de vários draws e 3D são vazias | não implementado | implementar |
| B6 | Taxa de sombreamento do jogo ignorada | não implementado | G8 |
| B7 | Draws pulados pela lista `.deadly` (PanVK, Turnip) | shader que derruba o driver | causa por shader, com correção no SPIR-V em vez de pular |
| B8 | Mesh shader nativo errado em todo telefone | aberta | G10; reprodução mínima com `vka_mesh_test` |
| B9 | Imagem mole pela resolução dinâmica | tempos de GPU devolvidos ao jogo | P7 |
| B10 | Porta do Link ao vivo, brilho roxo, artefatos nas janelas, névoa 3 a 4 vezes mais clara | abertas | B1 e B2 em cada uma |
| B11 | Filme NDVE com blocos corrompidos e a 8 fps | aberta; contornada pelo reprodutor simples | manter o contorno; causa depois |
| B12 | Mario Kart: texturas da pista somem às vezes; vistas de armazenamento sem ligação. Pokopia: chão quebrado em 720p | abertas | depois do Zelda, com as mesmas cenas de M1 |

## 7. Sequência

| Etapa | Itens | Por que nesta ordem |
|---|---|---|
| 1. Medir e ligar o que já existe | M1 a M4; P1, P2, P3, P6, P7; G1; C2; H4, H6, H8; R6 | quase tudo é chave que já está no código, esperando só o A/B no celular |
| 2. Pipelines e passagens | H1, H2, H3; G2 a G6 | os dois maiores custos medidos: travamento de segundos e GPU fixa |
| 3. Pouca RAM | R1 a R5; E2, E7 | abre os aparelhos de 12 GB, a maior parte dos testadores |
| 4. CPU por draw | C1, C3 a C5; depois C6 e C7 se medidos | destrava cenas pesadas e os outros jogos (Mario Kart e Donkey Kong são limitados pela gravação) |
| 5. Imagem | B0 a B12; G8 | com a medição e a captura prontas, cada bug custa menos |
| Sempre | E1 a E9; M5 | conforme os relatórios chegam |

**Critério de saída de cada item:** a rodada de M1 antes e depois, nas três cenas, com a taxa, o pior quadro, a GPU, a gravação e a memória; captura de tela igual à de referência; nenhum `[RED]` novo; entrada em `docs/reference` quando for mecanismo geral e na nota do jogo quando for fato do jogo.

## 8. Dependências e riscos

- **Aparelhos:** só o POCO F7 Ultra está à mão. Mali, Xclipse e Redmagic dependem de testadores remotos, por isso M4 vem cedo.
- **Interface:** a tela de preparação de shaders (H1), o modo de pouca RAM visível (R1, R3) e a trava de 30 (P1) pedem linhas de interface, que são do GPT.
- **Mac:** `regress.py` e os testes do host ainda não rodaram lá; a nota de entrada está em `output/mac/`.
- **Regras do repositório:** preferir a correção geral à regra por jogo (por isso R9 fica por último e VRS forçado fica fora); mudança na cadeia de shaders exige `dmsh_chain.py --header` e reconstrução.
- **Maior risco técnico:** G2 (fallback de mesh antes da passagem) e C1 (geração de ligações). Os dois precisam das cenas de M1 e da captura de B2 antes de entrar.

## 9. Inconsistências encontradas na análise

- `docs/env_switches.tsv` diz que o pré-aquecimento usa 2 threads; o código usa 4 (e 0 em Qualcomm antes do 512.800).
- A seção do replay em `android/PORTING.md` está desatualizada: é anterior ao worker, ao codificador e às barreiras por escopo.
- A análise de 13/11 diz que 30 fps é o teto do próprio jogo; o log de 14/11 mostra o jogo pedindo intervalo 1 (60 Hz), 60 fps no título e 31,6 na cena. P1 resolve qual das duas vale em cada cena.
- O perfil do Zelda usa `gpu_timestamps segment`, e o A/B de 08/11 tinha escolhido `submission` (P7).
- Vários ganhos das análises anteriores estão marcados "medida no celular em aberto": lote de uniformes, escopo das barreiras de fim de passagem, lote de oclusão e a deduplicação de slots de pool. M1 fecha todos de uma vez.
