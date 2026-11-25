# Plano da próxima versão (depois da 0.2.0-beta) — 24/11/2026

Este documento junta o que já estava programado para depois da 0.2.0: as decisões do usuário nas sessões, o README do Swap2-Turnip ("Next version (to do)"), `ANALISE-PORTS-2026-11-24.md`, `PLANO-OTIMIZACAO-COMPLETO-2026-11-15.md`, `PLANO-HIBRIDO-MALI-XCLIPSE-2026-11-13.md` e as notas dos jogos. Quando a situação de um jogo vem das notas, ela é a de 18/11 e pode ter mudado.

## Ordem sugerida

1. Perfis e deltas baixados do repositório (item 1): é o que destrava as releases. Junto, a tabela de saída idêntica do rewriter (item 1b), para uma atualização não reescrever o código de todos os jogos.
2. A causa dos gates de CPU (item 4): é a maior alavanca de fps medida (22→37 fps no Zelda, no Banana).
3. O contador de geração das ligações (item 12): ganho de CPU em todos os jogos.
4. O Zelda a 30 fps: itens 3 e 6.

## 1. Perfis e deltas baixáveis, com o APK como reserva

**Pedido do usuário (24/11):** o `vk_delta.tsv` e os perfis dos jogos são baixados do repositório. Quando o repositório não está acessível, vale o que veio no APK.

**Por quê:** hoje o gradle lê o `vk_delta.tsv` e o perfil de cada jogo na compilação e os empacota no `tools.zip` (`android/app/build.gradle:79`). Por isso a release espera todas as bibliotecas de shaders: na 0.2.0, o Pokopia e o Fire Emblem saem sem delta e retraduzem tudo. Com o download, o delta de um jogo e as correções de perfil (tabelas de shaders, listas de prewarm, chaves) chegam aos celulares sem um APK novo.

**Direitos autorais:** os deltas só têm hashes, e os perfis só têm dados nossos (chaves, tabelas, hashes). Nenhum dos dois carrega código ou dados do jogo, então podem ficar públicos. As bibliotecas traduzidas (`dmsh_vk.lib`), não: elas são derivadas dos shaders do jogo e continuam sendo feitas no celular, a partir do dump da própria pessoa.

**Desenho proposto:**
- **O que baixar:** só arquivos de dados que o app já lê em tempo de execução:
  - o `vk_delta.tsv`;
  - as linhas de `game_profile.tsv` lidas por `dm/host/rt` (`NX_RUNTIME_PROFILE`);
  - as tabelas de shaders e as listas de prewarm.
  
  Nunca código executável. O `patches.py` e as ferramentas Python do Prepare ficam no APK, porque rodar código baixado abriria uma porta para quem controlasse a URL.
- **De onde:** arquivos raw do `mamcjr/Swap2-Native` numa referência fixa, de preferência a tag da versão do app ou um branch `profiles` só com dados, e não o `main`, que pode estar no meio de uma mudança. A requisição é um GET anônimo, sem nenhum dado do usuário nem do aparelho na URL.
- **Compatibilidade:** cada arquivo baixado declara o formato e a versão mínima do app. O delta, além disso, só vale quando `version`, `game` e `inputs` batem com o que o APK espera, conferência que já existe hoje no `deltaOf`. Um arquivo incompatível é ignorado com uma linha `[RED]`, e vale o do APK.
- **Integridade:** um manifesto com o SHA-256 de cada arquivo, baixado junto. Um arquivo que não confere é descartado. O delta ainda se protege sozinho: o celular confere o hash de cada shader no fim (estágio `check`), então um delta errado só faz retraduzir mais.
- **Reserva:** sem rede, com timeout ou com erro, vale a cópia do APK, como hoje. A última cópia baixada que conferiu fica em cache e vale até a próxima.
- **Quando consultar:** antes de começar a atualização dos shaders de um jogo e antes do Prepare. Se a tradução completa já começou e o delta chega depois, ela pode passar a usá-lo, porque os resultados já feitos ficam no cache. Isso precisa ser feito com cuidado.
- **Tamanho:** a mudança é média, no código de shaders e de perfis do app (Java do Prepare e de `ShaderLibs`) e no host (`dm/host/rt`), sem mexer na interface. Precisa de teste no celular com e sem rede antes de chegar aos usuários.

## 1b. Rewriter com tabela de "saída idêntica"

**Visto na 0.2.0 (24/11):** depois de instalar a versão, o Mario Kart World pediu cerca de 1h20 de Prepare no POCO. Os shaders vieram de um zip importado. O tempo é do passo `rewrite`: o `patch_nso.py` mudou (o cache de decodificação do upstream `65b1523b`), e o Prepare reescreve o código de todo jogo já preparado quando as ferramentas mudam. Isso acontece mesmo com a saída idêntica, que foi conferida byte a byte no `main` e no `sdk` do Ocarina.

**Proposta:** fazer como o `vk_android_same.tsv` faz para os shaders. Quando uma mudança do rewriter provadamente não muda a saída (reescrita de um ou mais jogos, comparada byte a byte), uma linha numa tabela faz o digest novo valer como o antigo, e os packs já feitos continuam válidos.

## 2. EDS3 (VK_EXT_extended_dynamic_state3)

Decisão do usuário (23/11): fica para depois da 0.2.0. A ideia é tirar o blend e as máscaras de cor da chave do pipeline, o que reduz as variantes e as travadas na primeira vez que uma cena aparece. Começa pelo Turnip, porque o driver da Qualcomm tem suporte fraco. Não precisa de opção no menu, a menos que algum driver se comporte mal.

## 3. Zelda: 30 fps em Hyrule Field

Hoje são 22 a 25 fps. A lista completa está no README do Swap2-Turnip.
- **O driver não é o gargalo:** o Swap2-Turnip, o 891.14 e o V39 ficam a cerca de 1 ms de GPU um do outro, com 32 a 37 ms de GPU mais 4 a 5 ms por quadro fora dela.
- **Já testado e descartado:** limites de registradores, WFI, isam, modos do autotune, `DM_VK_TL_POLL`, `MESH_FUSE_TASK`, `MESH_HOIST` e a busca cooperativa no pool.
- **Pistas que restam:**
  - a espera de 4 a 5 ms por quadro na sincronização do jogo e do host;
  - o custo do fallback de mesh, 7,2 ms com a grama (preso a latência; uma saída menor por grupo e menos leituras raw);
  - um cache de mais de uma entrada do pool por invocação.

## 4. Gates de CPU (Banana): a causa da queda da GPU

Os gates estão portados no 1057566, mas desligados por padrão (`DM_VK_EVENT_GATE=1` liga). Ligados, o dispositivo caiu por volta do quadro 4.180 em 2 de 2 rodadas: falha de instruction pointer, no Zelda, 891.14. A hipótese da sessão do Turnip é que o replay grava trabalho antes de a CPU escrever o que ele lê. A entrada está em `docs/reference/nvn-objects.md`. No Banana, os gates levaram o Zelda de 22 para 37 fps num Honor.

## 5. Tela verde com o céu distorcido

Com os gates desligados, uma rodada de sete mostrou um quadro com grama verde clara por cima de tudo e o céu distorcido. O APK só com a liberação por worker preso nunca mostrou isso. O A/B do `DM_VK_PRESENT_ORDER` não fez diferença. Causa aberta.

## 6. Ports que não entraram direto (`ANALISE-PORTS-2026-11-24.md`)

- **`replay_raw_loads`** (Banana #515): levou Castle Town de 9,8 para 18,8 fps no Turnip deles. Ele foi feito sobre outro formato de cache, então precisa ser redesenhado para o nosso. Muda a cadeia.
- **`row_fetch_buffers`** (#608): precisa de um tipo de binding novo (TBUF_UBO). Muda a cadeia.
- **Zelda a 40 fps** (upstream `5fc927d5`): precisa do stub `FRAME_STEP_120`, de suporte no loader e de um present a 120 Hz.
- **Ritmo do acquire passado ao `nvnSyncWait`** (upstream `c17a5498`): é sobre o tempo de CPU que o jogo mede.
- **Chaves `low_ram_*` do Ocarina** (Banana #524): o nosso host não tem as chaves.
- **Imagens de storage do NVN2 em pool** (upstream `2fc4e70a`): precisam ser refeitas no ABI do Vulkan.

Mudanças de cadeia devem entrar juntas num único bump, com o rebuild de todas as bibliotecas, ou com uma linha em `vk_android_same.tsv` se a saída não mudar.

## 7. Traduzir shaders durante o jogo (Banana v0.3.40)

O Prepare deixa de traduzir todos os shaders, e o celular traduz cada um quando aparece pela primeira vez (`vka_layerreq.h`). Ajuda muito quem testa, mas é um port grande.

## 8 a 11. Jogos em bring-up (situação de 18/11)

| Jogo | Problema |
|---|---|
| Splatoon Raiders | 3D branco só no driver 891.14 |
| Fire Emblem Fortune's Weave | filme de abertura preto |
| Star Fox | imagem clara demais (o substituto do DLSS em sRGB) |
| Yoshi and the Mysterious Book | imagem superexposta em todos os drivers (shaders de compute do motor falham no tradutor: handle SUATOM no banco 3) |

## 12. Plano de otimização (15/11): o que falta

- **C1, contador de geração no estado das ligações:** um draw que não mudou de shader nem de ligação pula o `walk_all`, a montagem dos `VkWriteDescriptorSet` e a comparação do push. Estimativa de 2 a 6 ms nas cenas de 2.000 draws. É o maior item da fase de CPU.
- **M2:** contar por quadro o motivo de cada fim de passe. Ajuda a ordenar os itens G1 a G3.
- As outras fases (travadas, RAM, estabilidade por aparelho, bugs gráficos) estão no plano de 15/11.

## 13. Mali e Xclipse (`PLANO-HIBRIDO-MALI-XCLIPSE-2026-11-13.md`)

A memória, o decode de BC e o ajuste da tabela de texturas estão feitos. Faltam:
- subgrupos de 16 lanes;
- estágios de vértice sem gravação;
- divisor de instância sem `supportsNonZeroFirstInstance`;
- memória do driver por layout;
- formatos do Xclipse;
- compilação lenta no Xclipse;
- limites do aparelho;
- RAM abaixo de 14 GiB;
- esconder a oferta de Turnip fora de Adreno (interface, com o GPT).

Cada passo depende de um log de aparelho desses pelo diagnóstico automático, nunca de uma lista de chaves para o testador digitar.

## 14 a 16. Infraestrutura e pendências

- **Mac para o `regress.py`** (adiado pelo usuário em 24/11):
  - **O que já existe:** SSH `mamcjr@192.168.1.7` com a chave deste PC. O Mac é um M5 Pro com 24 GB, Xcode 27.2 com o toolchain Metal, o Python do NX Play (`~/Library/Application Support/NX Play/venv`, 3.14) e o .NET 10 em `~/.dotnet` (instalado em 24/11). Há um worktree `~/Projetos/Android/Swap2-Native-regress` (o clone do usuário ao lado tem mudanças próprias: não mexer) e a raiz do harness em `~/Projetos/Android/regress-root`.
  - **Por que não rodou:**
    - As builds `--platform mac` de lá (Zelda e Yoshi) foram feitas pelo NX Play, com o nxnative upstream. O mapa de shaders do nosso perfil do Zelda aponta para outro split do pacote (`IndexError` em `dmsh_build.py` `rec_of`).
    - O perfil do Zelda não tinha `shader_map.tsv.conflicts.tsv`. Já corrigido: o `dmsh_build.py` agora trata a falta como tabela vazia, com a linha `89d28af00910 → 706d0c7262fa` em `vk_android_same.tsv` provada por uma build completa.
  - **O que falta:** fazer as builds com o nosso `build_ipa_from_nsp.py --platform mac`, dois ou três jogos (umas horas e ~60 GB cada; o Yoshi passa de 200 GB), e gravar uma rota `smoke.tsv` por jogo. Deixar o Mac sem hibernar durante as builds (`caffeinate`).
  - **O valor:** testar mudanças do host compartilhado em vários jogos de forma automática. A cadeia já é coberta pela build Vulkan no WSL e pelo `vk_delta`, e o jogo pelo celular.
- **Trabalho do replay sem commit:** o dump de mesh (`DM_VK_MESH_DUMP`), o `DM_VK_MESH_TWICE` e o `DM_VK_BUF_WHOLE`. Falta decidir se vira commit ou é descartado.
- **Swap2-Turnip:** ainda não está decidido se vai dentro do APK, e o ganho do swapchain UBWC não foi medido.
- **Biblioteca do Pokopia na 706d:** compilando no WSL. O delta dele entra pelo item 1 ou pela próxima versão.
