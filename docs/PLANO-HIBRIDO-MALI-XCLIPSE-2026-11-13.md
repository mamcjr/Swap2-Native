# Plano: Swap2-Native híbrido (Snapdragon, Mali e Xclipse) — 13/11/2026

Pedido do usuário: reescrever o que for preciso para o app atender também GPUs Mali (MediaTek, Tensor) e Xclipse (Exynos), além do Snapdragon/Adreno. Este documento é o levantamento, o desenho escolhido e o estado de cada item.

**Gatilho:** o log de um POCO X7 Pro (Dimensity 8400, Mali-G720 MC7, driver r49p1, Vulkan 1.3.278, 11 GiB, Android 16) em `0400486024DEC000-20261113-215437.log`: a checagem reprova por "Placed memory maps", o jogo abre com som e nada desenha, e todo pool vira `[RED] vk: memory pool ... is not in placed memory: the GPU does not see it`.

## 1. A decisão de desenho: não trocar o modelo, trocar o que sustenta a memória

O nosso replay (`dm/gfx/vk_android/vka_*`) parte de uma ideia: **a memória de um pool do jogo é a própria memória da GPU**. Nada é copiado entre CPU e GPU. Um pool vira um "segmento": um `VkDeviceMemory`, um `VkBuffer` por cima e o endereço dele (`vka_mem.c`). As imagens têm memória própria e nunca usam a memória dos pools.

Há duas formas de atender um driver sem `VK_EXT_map_memory_placed`:

| Caminho | O que é | Custo |
|---|---|---|
| **A. Modelo de cópia** (como o DroidPlay Core) | A memória do jogo é copiada para buffers da GPU a cada draw e as escritas da GPU voltam por "write-backs" | Reescrever o coração do replay (draws, dispatches, contadores, rastreio de páginas sujas), e pagar CPU por quadro. O DroidPlay mede ~9 fps no OoT num Mali |
| **B. Mesmo modelo, outro lastro** (escolhido) | O segmento continua sendo memória compartilhada no endereço do jogo; só muda quem aloca essa memória | Um arquivo novo e ~40 linhas nos pontos que já tratam o "alias por fd" |

O caminho B já existia pela metade: o upstream (Banana Native) tem o **alias por fd** (`vka_fd_alias.h`), que exporta a memória Vulkan como descritor (`VK_KHR_external_memory_fd`, tipo OPAQUE_FD) e o host a mapeia com `mmap(MAP_FIXED)` no endereço do pool. Funciona no driver de sistema do Adreno 830, mas o OPAQUE_FD não promete ser mapeável, e no Mali do X7 Pro o teste não passou.

**O terceiro lastro (novo, `vka_ahb_alias.h`): `AHardwareBuffer`.**
- Um buffer do tipo BLOB com uso `GPU_DATA_BUFFER` é importado como memória de um `VkBuffer` por `VK_ANDROID_external_memory_android_hardware_buffer`, extensão que todo driver Vulkan de Android tem.
- Por trás dele há um dma-buf. O host pega o descritor desse dma-buf e o mapeia ele mesmo no endereço do pool, igual ao alias por fd.
- O NDK não dá o descritor direto. Ele sai do handle que `AHardwareBuffer_sendHandleToUnixSocket` escreve num socket (SCM_RIGHTS). O descritor certo é o que mostra, no próprio mapa, a marca escrita pelo `AHardwareBuffer_lock`.
- Nada é presumido do driver: um teste (`vka_ahb_probe`) cria um buffer assim, confere os dois mapas um contra o outro, confere `mprotect` no alias, e faz a **GPU copiar dados nos dois sentidos, oito vezes**. Se a GPU não vê na hora o que a CPU escreveu (ou o contrário), o teste reprova e o caminho fica desligado.

A ordem de escolha em `vka_dev.c`: mapas "placed" → alias por fd → alias por hardware buffer → nada (o jogo roda sem desenhar, como hoje). `V.fd_alias` vale 1 para o fd e 2 para o hardware buffer.

## 2. Estado

| Item | Estado |
|---|---|
| `vka_ahb_alias.h` (criar, achar o descritor, teste com a GPU) | escrito, compila |
| `vka_dev.c`: extensões, teste, `DM_VK_ALIAS=fd\|ahb` para forçar num aparelho que tem placed | escrito, compila |
| `vka_mem.c`: terceiro lastro em `place()`, limpeza, e uma linha no log do jogo dizendo qual lastro a rodada usa | escrito, compila |
| `vka_readiness.c`: `memory.ahbAlias`, `memory.ahbAliasProbe`; `memory.fdAlias=1` vale para qualquer alias (o launcher não muda) | escrito, compila |
| `android/host/CMakeLists.txt`: liga `libnativewindow` | feito |
| Teste no POCO (Adreno 830, 891.14) com `DM_VK_ALIAS=ahb` | **passou** (13/11): o teste do alias passa com endereço de dispositivo; título a 60 fps; Floresta Kokiri a partir do save a 27–31 fps; 1.625 MiB de pools em hardware buffers; nenhuma linha vermelha nova |
| Limite de 256 MiB por hardware buffer (o serviço de alocação do Adreno 830 aborta num BLOB de 812 MB, segundo o DroidPlay) | feito |
| `nxlaunch.c`: a janela só vai ao host depois de o ambiente estar aplicado (antes, opções como `DM_VK_MESH` do "Ambiente extra" não chegavam à criação do dispositivo) | feito, testado no POCO |
| O host repete no log do jogo os fatos do dispositivo (GPU, driver, recursos, limites) e diz qual lastro de memória a rodada usa | feito, testado no POCO |
| Teste num Mali ou Xclipse de verdade | **aberto: não tenho o aparelho**; depende de um log de quem tem |

(As seções seguintes são preenchidas com o resultado dos levantamentos.)

## 3. Tudo o que precisa ser feito (levantamento)

Fontes: o nosso código (`dm/gfx/vk_android`, `dm/gfx/tools`), a lista do próprio upstream (Banana Native v0.3.21, `android/README.md`, "GPUs other than Adreno"), e o DroidPlay Core v0.8.9, que já roda em Mali-G715 (Pixel 9 Pro XL) e Xclipse 950 (Galaxy Z Flip 7).

Legenda: **B** bloqueia (nada desenha ou o jogo morre), **E** desenha errado, **D** só degrada (mais lento, menos detalhe).

| # | Item | Mali | Xclipse | Onde no nosso código | O que fazer | Estado |
|---|---|---|---|---|---|---|
| 1 | Memória do jogo visível à GPU sem `VK_EXT_map_memory_placed` | **B** | **B** | `vka_mem.c`, `vka_dev.c` | Alias por hardware buffer (seção 1). O DroidPlay usa o mesmo caminho no Xclipse 950 e no Adreno 830; no Mali ninguém provou ainda | **feito, falta aparelho** |
| 2 | Texturas BC1–BC7 sem `textureCompressionBC` | **B/E** (Mali-G715 não tem; G720: conferir no log) | **E** (o 950 não tem) | `vka_tex.c:107-112` (sem o formato, a textura some) | Decodificar BCn para RGBA8 / RGBA16F quando falta o recurso. O DroidPlay decodifica na CPU com `bcdec` | aberto |
| 3 | Subgrupos: a biblioteca de shaders é feita para 64 lanes (`DMSH_VK_SUBGROUP_SIZE`) | **E** (16 lanes fixas: tudo cai em "uma lane", errado para shuffles de compute) | provável ok (64, ou 32–64 com tamanho exigível em compute) | `vka_shader.c:37-61` `sg_plan`; `vka_spvpatch.c:171` | Para 16 lanes: reescrita do tipo "cada subgrupo é seu próprio warp" (`spv_narrow.h` do DroidPlay) ou shuffles pela memória do workgroup por índice local. Não é troca de constante | aberto |
| 4 | Tabela de texturas por ID menor que a matriz de 2^20 declarada nos shaders | **E/B** (X7 Pro: 499.680) | conferir | `vka_tex.c:2024-2053`; `Program.cs:98` | Ajustar o comprimento da matriz no SPIR-V ao tamanho da tabela do aparelho, na criação do módulo (`vka_spv_array_len`, `csm_fit`) | **feito**; testado no POCO com a tabela cortada para 499.680 (`DM_VK_POOL_SLOTS`) e por `t_android_spvpatch` |
| 5 | `vertexPipelineStoresAndAtomics` ausente | **E** (Mali não tem) | ok | `V.vstores`; módulos de vértice com buffers graváveis | Marcar `NonWritable` nos estágios de vértice (o DroidPlay tem `spv_readonly.h`) e desviar o que grava | aberto |
| 6 | Divisor de instância sem `supportsNonZeroFirstInstance` | **E** (Mali) | ok | `vka_draw.c` (divisor) | Começar na instância 0 e pôr a base nos offsets dos streams | aberto |
| 7 | Mesh shaders | ok (fallback em compute) | ok (não tem a extensão: fallback) | `vka_dev.c`, `vka_mesh.inc` | Nada. Em Mali o driver não é o da Qualcomm, então, se um dia oferecer mesh nativo, ele seria usado: manter o olho | feito |
| 8 | Memória do driver por layout de descritores | **B** no OoT do DroidPlay (6,7 GB de heap nativo no Mali, 13,9 GB no Xclipse) | **B** | os nossos layouts usam números de binding altos (até 64 por estágio × 5 estágios, e mais) | Medir primeiro (o log de memória mostra). Se repetir: renumerar os bindings de forma densa na criação do módulo | aberto, medir |
| 9 | Formatos: R16/RG16 UNORM sem filtro linear (Xclipse); sem D24S8 (Xclipse) | ok | **E** | `vka_tex.c` (formatos) | Gêmeos em float de 16 bits; D32S8 já é o fallback, falta ajustar o depth bias | aberto |
| 10 | Compilação lenta de shaders no Xclipse (compila em `vkCreateShaderModule`) | — | **D** | `vka_shader.c` | `VK_EXT_shader_module_identifier` (módulos sob demanda) | aberto |
| 11 | Clock da GPU: sem `VK_QCOM_queue_perf_hint` e sem KGSL | **D** | **D** | `vka_dev.c:745-936` | Fica só o ADPF. Nada específico para Mali/Exynos existe | aceito |
| 12 | Drivers customizados (Turnip) | não se aplica | RADV existe para Xclipse (o DroidPlay aceita) | `vka_driver.c`, launcher | Esconder a oferta de Turnip fora de Adreno (interface: GPT) | aberto, interface |
| 13 | Limites não conferidos: `maxComputeWorkGroupInvocations`, `maxStorageBufferRange` (Mali: 128 MiB), `maxSamplerAllocationCount` | **E** possível | conferir | `vka_pso.c`, `vka_draw.c:580`, `vka_tex.c:2108` | Conferir e avisar em vermelho; cortar com aviso | aberto |
| 14 | Checagem de compatibilidade: exige "Placed memory maps" | resolvido pelo item 1 (`memory.fdAlias=1` passa) | idem | `Readiness.java:146`, `vka_readiness.c` | Falta a checagem informar subgrupo, BC e tabela (hoje não confere) | parcial |
| 15 | RAM: o OoT usa 8–10 GB; X7 Pro tem 11 GiB, Flip 7 tem 12 GB | **B** provável sem economia | idem | perfil, `vka_mem.c` | "Economizar RAM" e cena 720p ligados por padrão abaixo de 14 GiB; chaves `low_ram_*` do Banana v0.3.21 | aberto |
| 16 | Diagnóstico remoto: as linhas de criação do dispositivo não chegam ao log do jogo | — | — | `vka_dev.c` | O host repete os fatos do dispositivo (GPU, driver, recursos, limites) no primeiro pool | **feito** |

**Ordem de trabalho proposta.** 1 → (log de um Mali) → 2 e 4 e 5, que decidem se algo aparece na tela → 3 → 8 e 15, que decidem se o jogo fica de pé → o resto. Cada passo depois do primeiro precisa de um log do aparelho: sem ele é chute.

## 4. O que os drivers oferecem (vulkan.gpuinfo.org, relatórios lidos em 13/11)

| | Mali-G720 / G925, driver r49p1 (X7 Pro, Dimensity 8400/9400) | Mali r54+ (Tensor G4, G1-Ultra) | Xclipse 940 / 950 | Adreno 830, 891.14 |
|---|---|---|---|---|
| `VK_EXT_map_memory_placed` | não | não | não | sim |
| `VK_EXT_external_memory_host` | não | sim | sim | sim |
| `VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf` | sim | sim | sim | sim |
| Hardware buffer (`VK_ANDROID_external_memory_android_hardware_buffer`) | sim | sim | sim | sim |
| Texturas BC | **não** | **não** | **não** | sim |
| ASTC, ETC2 | sim | sim | sim | sim |
| Subgrupos (tamanho, mín–máx) | **16, 16–16**, só fragment e compute | idem | 64, 32–64, todos os estágios; tamanho exigível só em compute | 64, 64–64, fragment e compute |
| `nullDescriptor` (robustness2) | **não** | sim | sim | sim |
| Escritas em estágio de vértice | **não** | não | sim | sim |
| Clip distance (`shaderClipDistance`) | **não** | não | sim | sim |
| Geometry e tessellation shaders | sim | sim | sim | sim |
| Mesh shaders | não | não | 950 novo: sim | 830: não |
| Texturas esparsas | não | não | sim | sim |
| Pipeline library (GPL) | não | sim, sem fast linking | sim | sim |
| `fillModeNonSolid`, `shaderFloat64` | não, não | não, não | sim, não | sim |
| Imagens por tabela (update-after-bind) | **500.000** | 8.388.608 | sem limite prático | 16.777.216 |
| Memória compartilhada de compute | 32.768 | 32.768 | 32.768 (950 novo: 65.536) | 32.768 |
| Maior alocação | 8 GiB | 8 GiB | **2 GiB** | 1 GiB |
| Alinhamento de storage buffer | 64 | 64 | 4 | 16 |

Consequências para a lista da seção 3:
- **Item 2 (BC) é bloqueio certo** em todo Mali e todo Xclipse: nenhum tem BC. É o próximo depois da memória.
- **Item 3 (subgrupos de 16)** vale para todo Mali. O Xclipse deve rodar a biblioteca como está.
- **Novo, item 17:** sem `nullDescriptor` no Mali r49p1, o nosso host pula todo draw com texel buffer e desliga as leituras da tabela de texturas (`vka_draw.c:922-936`, `vka_tex.c:1901`). Precisa de placeholders para todos os tipos.
- **Novo, item 18:** sem `shaderClipDistance` no Mali, um módulo que declara `ClipDistance` não é aceito. Conferir quantos shaders da biblioteca declaram e reescrever (descartar no fragment, ou tirar a declaração).
- **Uma terceira forma de memória** fica de reserva para o Mali: alocar direto de `/dev/dma_heap/system` (um app comum pode) e importar como `DMA_BUF`. É o caminho preferido do Winlator. Só vale implementar se o hardware buffer reprovar no teste.
- `VK_EXT_external_memory_host` (importar as páginas do jogo onde estão) existiria no Xclipse e no Mali r54+, mas não no r49p1 do X7 Pro, e a proteção de páginas que o nosso rastreio usa é duvidosa sobre memória importada assim. Fica de fora.

## 5. O que foi posto em prática em 13/11 e o que vem depois

**Feito e testado no POCO (Adreno 830), forçando o comportamento de um driver sem placed:**
1. Memória do jogo em hardware buffers (item 1).
2. Matrizes das tabelas ajustadas ao aparelho (item 4).
3. Diagnóstico no log do jogo (item 16) e a correção da ordem janela/ambiente no lançador.

**Não testado em nenhum Mali ou Xclipse.** O que decide é o log de um aparelho desses com esta versão. A linha a procurar é `[vk] guest memory:`.
- Se disser "pools are hardware buffers", a memória funciona, e o que aparecer errado na tela vem dos itens 2, 3, 5, 17 e 18.
- Se disser que nenhum alias funciona, a frase do teste diz onde parou (alocação, descritor, importação, mapa fixo, `mprotect` ou cópia pela GPU), e o plano B é o dma-heap com importação `DMA_BUF`.

**Próximos, na ordem:**
1. **Item 2, texturas BC decodificadas** quando o aparelho não tem BC. É bloqueio certo em todo Mali e Xclipse. Dá para desenvolver no POCO com um interruptor que finge a falta de BC. O `bcdec.h` (domínio público / MIT) já está no clone do DroidPlay.
2. **Itens 17 e 5**, placeholders sem `nullDescriptor` e módulos de vértice sem escrita: também simuláveis no POCO.
3. **Item 3, subgrupos de 16:** precisa de um Mali para validar; começar pelo que o log mostrar (quantos módulos caem em "uma lane" e quais usam shuffle).
4. **Itens 8 e 15, memória do driver e RAM:** medir no primeiro log que chegar à cena.

**Arquivos desta etapa:** `dm/gfx/vk_android/vka_ahb_alias.h` (novo), `vka_dev.c`, `vka_mem.c`, `vka_readiness.c`, `vka_shader.c`, `vka_tex.c`, `vka_spvpatch.c/.h`, `vka.h`, `android/host/CMakeLists.txt`, `android/app/src/main/cpp/nxlaunch.c`, `dm/host/tests/t_android_spvpatch.c`. Referência: `docs/reference/memory.md`, `shaders.md`, `rendering.md` (três entradas novas), `docs/env_switches.tsv`.

## 6. Continuação (13/11, noite)

- **Item 2, texturas BC: escrito.** `vka_bcdec.h` (sobre o `bcdec`, em `dm/third_party/bcdec`), mais os pontos de cópia em `vka_tex.c` e `vka_exec.c`. Onde o aparelho não tem BC, a imagem é criada sem compressão e os blocos são decodificados na CPU a cada envio.
  - `DM_VK_BC_DECODE=1` força o caminho num aparelho que tem BC, para testar no POCO.
  - Teste automático `t_android_bcdec` passa; o app de teste compila.
  - **Testado no POCO em 14/11** com `DM_VK_BC_DECODE=1`: título a 60 fps e Floresta Kokiri a 28 fps, imagem igual à de antes. Foram 16.384 envios decodificados, 200 MiB de texels em 0,26 s no total. A memória de imagens foi de 1.447 para 2.028 MiB (+581 MiB nessa cena), bem abaixo do pior caso. Falta um Mali ou Xclipse.
  - **Custo conhecido:** 4 a 8 vezes a memória das texturas, e a decodificação roda na thread de envio. Se a memória não couber num aparelho de 11–12 GB, as saídas são transcodificar para ETC2/ASTC ou descartar o nível mais fino.
- **Redmagic 10 Pro (Adreno 830): o diagnóstico de "falta de RAM" estava errado.** O jogo é morto por sinal 9 perto do fim da abertura, com 5 GB livres segundos antes. A suspeita, não provada, é a compilação de um shader grande no driver.
  - O host agora guarda as compilações em andamento num arquivo que sobrevive ao kill (`vka_pso.c`, `bb_open`). A abertura seguinte diz no log o que estava compilando e quanta memória havia, e pula o shader que compilava havia 2 s ou mais.
  - O lançador passa ao host como a rodada anterior terminou (`NX_PREV_EXIT`).
- **Registro de compilações, testado no POCO em 14/11** (`output/zelda-1112/bbox-test.sh`): com o jogo de teste morto por sinal 9 de fora durante uma compilação, a abertura seguinte relatou o shader e a memória da última amostra, e o deixou de fora. Uma rodada encerrada pelo usuário não marca nada.
- **A morte do Redmagic 10 Pro não se reproduziu no POCO.** O carregamento do save com cache frio sobreviveu: a memória livre caiu de 4,7 GB para 1,4 GB em 45 s, e 3,9 GB do jogo foram para a swap. O Redmagic não mostra uso de swap nos logs, então lá o mesmo carregamento tem uns 5 GB a menos de folga. Falta de memória durante o carregamento frio continua candidata, ao lado do compilador do driver.
- **Estado do POCO:** cache de pipelines e lista `.deadly` do app de teste restaurados; `env.txt` removido; aparelho liberado às 23:56.

## 7. Primeiros logs da 0.1.8 (14/11)

- **POCO X7 Pro (Mali-G720): o bloqueio não é a GPU.** O jogo dá crash no próprio código 3,5 s depois de abrir (`main+0x773270`, ponteiro nulo ao ler o descritor de uma textura), com os mesmos registradores no driver da Arm e no PanVK. O log de ontem já terminava nesse crash, e eu não tinha visto.
  - Suspeita, não provada: cópia danificada do dump (trecho preenchido com zeros). O formato `.nup` tem hash do índice, mas não do conteúdo dos arquivos.
  - O dono testou com o **PanVK** (driver Mesa para Mali). Com ele o aparelho tem mapas "placed", texturas BC e null descriptor, então nem os hardware buffers nem a decodificação BC entram em uso ali. Os subgrupos continuam de 16.
  - Enquanto o crash não sair do caminho, nada dos itens da seção 3 pode ser avaliado nesse aparelho.
- **Redmagic 11 Air (Adreno 830):** morre com sinal 9 no primeiro quadro que desenha pelo fallback de mesh, com 5,1 GB livres e nenhuma compilação em andamento. As duas explicações anteriores (RAM e compilador) estão descartadas. Causa aberta; detalhes na nota do jogo.
- **Mudanças de diagnóstico depois desses logs** (build 0.1.8.1-beta, `docs/BUILD-0.1.8.1-beta-2026-11-14.md`): rastro no log dos quatro primeiros draws do fallback de mesh (`vka_mesh.inc`), e os últimos 24 arquivos diferentes abertos no relatório de crash (`hle_fs.c`, `crash.c`).

## 8. Novidades dos outros ports (14/11)

- **Xclipse: criação do dispositivo Vulkan leva mais de 10 s** (Banana Native 0.3.23, issue 251). Feita na thread de interface, vira "não está respondendo". Portado em 14/11: o dispositivo passa a ser criado pela thread do host. Depois da 0.1.8.1-beta; ainda não publicado.
- **Mali: o driver da Arm descarta as camadas 8 em diante escritas por um vertex shader** (DroidPlay Core 0.8.10, #483, tirado das notas deles). A conferir quando houver um Mali passando do início do jogo.

