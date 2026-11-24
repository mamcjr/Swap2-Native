# Relatório da noite de 21 para 22/11/2026

Meta da noite: Zelda a 30 fps estáveis em Hyrule Field, com a folhagem certa e nada piscando. As sessões trabalharam pela fila do celular, sem consultar o usuário.

## Resultado do Zelda

Ao vivo, no POCO F7 Ultra, com a chain nova `4eb05e585fb0` e cópias de save. A 1ª rodada só aquece o cache de pipelines depois da troca de biblioteca; os números são das rodadas 2 e 3.

| Cena | Driver | fps | quadro p50 / p99 | GPU p50 | quadros em 30 |
|---|---|---|---|---|---|
| Hyrule Field | Turnip V39 | 29,4 / 29,2 | 34 / 40 ms | 28,0 ms | 76-79 % |
| Hyrule Field | Swap2-Turnip | 28,4 / 29,7 | 34 / 39-56 ms | 27,9 ms | 67-82 % |
| Kokiri | Turnip V39 | 19,6 / 19,7 | 51 / 58 ms | 40,1 ms | 0 % |
| Kokiri | Swap2-Turnip | 19,6 / 20,0 | 50 / 57 ms | 40,1 ms | 0 % |

- **Antes**, no Turnip: o quadro de Hyrule Field tinha p50 de ~47 ms (~21 fps), com GPU de 37,6 ms. A GPU da Kokiri estava em ~59 ms.
- **Hyrule Field chegou ao teto de 30 fps**, mas ainda não está estável: o p99 fica perto de 40 ms e uns 20 % dos quadros passam de 33 ms. **A folhagem em 3D (as folhas de grama e a vegetação rasteira) não aparece**, nem no Turnip nem no 891.14: só a textura de grama do chão (captura `output/ports-1121/bench-4eb0-hf-Swap2-Turnip/mesh-3/end.png`; a do 891.14 no mesmo ponto, `output/replay-nifty/hf3/game.png`, também está sem ela). Os 91 draws de mesh por quadro rodam, mas o que eles desenham não aparece. Então estes fps são sem a folhagem, e a meta ainda não está cumprida. Eu tinha escrito que a folhagem aparecia: estava errado (o usuário viu às 04:20).
- **O que deu o ganho** foi a correção da sessão do Turnip no gerador de SPIR-V (`dm/gfx/tools/vk_android_spv.py`, `_sink_raw_calls`). O tradutor lia toda a memória crua no começo do shader e só usava os valores no fim, e o compilador do Turnip fazia centenas de spills. Agora cada leitura desce até logo antes do primeiro uso. Nos 5 vertex shaders mais caros (`b1f06157` na Kokiri; `fbcb6db5`, `3c7f7edc`, `3ccf5c7d` e `f545b7d8` em Hyrule Field), os spills caíram de ~900/1.700 para 0 e as instruções de ~27 mil para ~17 mil. A imagem não mudou: o replay no 891.14 continua batendo com o quadro ao vivo.
- **Ainda aberto:**
  - O p99 de Hyrule Field.
  - A Kokiri, com GPU de 40 ms.
  - A imagem do Turnip difere do quadro ao vivo do 891.14 em ~321 mil pixels em Hyrule Field, uma diferença que já existia antes da correção. Talvez seja ela o pisca da folhagem. As sessões do Turnip e do nxreplay seguem nisso.

## Depois das 04:20: a folhagem

O usuário viu que a folhagem não aparecia, e depois que piscava com brilho estranho e sumia. Três achados, na ordem:

1. **A folhagem sumia ao passar de 4.095 segmentos de memória.** Por volta do quadro 1.750 em Hyrule Field, as leituras de memória crua acima do 4.095º segmento passavam a ler 0. A folhagem lia instâncias e posições zeradas, piscava e sumia. Isso aconteceu em todas as partidas, também no 891.14.
   - **Correção:** a tabela de 8.192 entradas, como no Banana Native, aplicada pela sessão do Turnip em `vk_android_spv.py` e `vka_draw.c` (chain `706d0c7262fa`, sem commit).
   - **Resultado:** a grama alta fica desenhada.
   - **Custo:** Hyrule Field caiu de ~29,5 fps (sem folhagem) para **~21,7 fps** com ela (GPU 37 ms, quadro 46 ms), no V39 e no Swap2-Turnip.
2. **As pontas da grama saíam branco-azuladas.** O jogo desenha a folhagem em duas passadas de mesh: primeiro só a profundidade, depois a cor, que só pinta onde a profundidade é EQUAL. Entre elas, ele desenha o céu. Na forma compute do mesh, o driver arredondava a conta da posição de jeito diferente nas duas passadas. Onde a profundidade diferia no último bit, o céu ficava à mostra.
   - **Correção:** `NoContraction` em todo resultado float desses módulos, no host (`vka_spv_exact_float`, `DM_VK_MESH_EXACT`, padrão ligado), como fizeram o AnkaNative (`e1159b7`) e o Banana (`7daac0fb`).
   - **Resultado:** num A/B ao vivo, com a correção a grama sai verde-oliva, como no Mac, e sem ela branca. Sem custo de fps.
3. **GPU em partes menores:** a sessão do Turnip passou o padrão de `DM_VK_CHUNK` / `DM_VK_CHUNK_PASS` de 600 para 200. Hyrule Field com a folhagem no 891.14 foi de 22,5 para 25,2 fps, e a Kokiri no V39 de 19,4 para 20,7 (sem commit).

**Onde a meta está agora:** com a folhagem certa, Hyrule Field roda a ~22-25 fps, contra os 30 da meta. Os passos seguintes:
- o pisca ao carregar o save (pista: AnkaNative `494973f`, não desenhar mesh draws num quadro em que um pipeline de mesh ainda compila);
- uns 4 ms de GPU (37 → 33);
- o intervalo de ~9 ms entre o quadro e a GPU.

**Commitados às 09:00, a pedido do usuário:** a tabela de 8.192 entradas e o `NoContraction`, com os testes (`test_vk_raw_global.py`, `t_android_spvpatch`), a documentação e o manifesto do Zelda na chain `706d0c7262fa`. **Sem commit:** o padrão de `DM_VK_CHUNK` em 200, da sessão do Turnip.

## Yoshi

- **A GPU não trava mais** (commit `300382f`). Os texel buffers passaram a ser `VkBufferView` sobre a memória do pool. Antes, um contador atômico ligado como nulo travava a GPU 30 s depois do boot.
- **O diálogo do Mr. E já não fica sobre tela preta.** O jogo usa 17 atributos de vértice, e o host recusava todo bind com mais de 16. Com os 32 atributos do NVN2 (port do Banana `e62de350`), a cena aparece atrás do diálogo e a partida segue a 60 fps.
- **Aberto:**
  - 2 shaders do UE5 ainda faltam na biblioteca.
  - 5 binds de buffer texture em slots de imagem ainda leem vazio; o Banana `ebb5dd96` resolve isso copiando para uma imagem.
  - A troca de vermelho e azul não foi localizada.

## Commits desta noite (no `main`, nada enviado ao GitHub)

- `6c0aa8f`: imagens com o handle no uniform buffer do jogo (IMG_UBO, SUATOM/SURED no banco 3), o port do Yoshi.
- `300382f`: buffer textures como texel buffers no Vulkan.

## Sem commit, aguardando a sua decisão

A árvore tem ~90 arquivos mudados, incluindo trechos de outras sessões. O que é desta sessão e da do Turnip:

1. **Ports do Banana Native v0.3.45 → v0.3.50**, a mudança líquida entre as duas tags. O próprio release desfez alguns PRs da semana (Opus, idiomas, handoff de threads, IPK, uepak v9), que ficaram de fora.
   - **Entrou:**
     - 32 atributos de vértice.
     - Breadcrumbs da GPU (`DM_VK_CRUMBS`) e `DM_VK_PART_SERIAL`.
     - Contagem da memória do host (`vka_hostmem`, `DM_VK_HOSTMEM`, `DM_VK_OBJCOUNT`).
     - `DM_VK_CB_RELEASE`.
     - Opcodes de query no encoder.
     - Fallback de slot de cubo.
     - ADPF (`DM_VK_ADPF_SPAN_ANY`).
     - Frame generation (LSFG: wavefront, cadeia do tamanho da imagem, multiplicador que acompanha a tela, esperas limitadas depois de travamento da GPU).
     - FSR 3.1 / DLSS stand-in mantendo o alfa.
     - Frame skip do engine da Shin'en.
     - HID de controle inteiro (`bd929626`).
     - Logs de commit de save sem repetição (`hle_fs`).
     - Partes do `shader_split` (`may_hold_shaders`, `pak_path`, raio do slot 6).
     - `-J` no `pack_android.py`.
   - **Ficou de fora**, por depender de coisas que esta árvore não tem:
     - A correção do crash de render pass no Qualcomm, que só vale para render passes clássicos (aqui só há dynamic rendering).
     - Gates, qtrace, RAM saver, a linha de ritmo e o limitador de quadros.
     - O split dirigido, o nxoodle e o empacotador Linux.
     - O letterbox de filmes.
     - Toda a interface Java (é do GPT).
   - Muitos arquivos daqui vinham de uma base bem mais antiga do Banana (v0.1.x), então os agentes portaram só a mudança líquida e não o que veio antes da v0.3.45.
2. **Ports do nxnative (seedds):**
   - **Entrou:**
     - `806fabe1`: enumeração de módulos do SDK; `svcQueryMemory` percorre o processo todo, com o limite de endereço no Android vindo do TASK_SIZE.
     - `9a3eb02e`: só o teste de áudio.
     - `33b0cf51`: PRMT no tradutor.
     - `3ec5b785`: gather offsets em tempo de execução viram deslocamento de coordenada, no Mac e no Vulkan.
     - `47578ded`: telas altas do Ocarina, dormente. O `main` reescrito fica idêntico e nada muda no celular.
     - `ff18e88a`: staging de upload do Catherine, só Mac.
   - **Pulado:**
     - `71a9a829`: não temos o vigia de arquivos do jogo.
     - `4381b10c`: já veio pelo Banana `bd929626`.
     - Os commits de offline (`hle_offline.c` não existe aqui).
3. **Chain nova:** `DMSH_CHAIN ac2d952ab7d6`, `DMSH_VK_CHAIN 4eb05e585fb0` e ABI `7e14cf450ab9`, que já vinha do commit do IMG_UBO. Com ela foram:
   - o tradutor recompilado (`dmsh_xlate.dll`, `Ryujinx.Graphics.Shader.dll`) e o lock atualizado;
   - o manifesto do Zelda publicado (`dm/games/0400486024DEC000/vk_delta.tsv`; a partir de 45ce faltam traduzir 2.209 shaders);
   - a biblioteca do Zelda guardada no cache do jogo (`<pasta do jogo>\0400486024DEC000-work\mesh-emulation\cache\v-4eb05e585fb0`).
4. **A correção dos vertex shaders**, da sessão do Turnip: `vk_android_spv.py` e o teste `test_vk_android_abi.py`.

**Antes de um release:** a mudança de ABI faz o host não tocar as bibliotecas `45cedbd878f7` dos outros jogos. É preciso recompilar a biblioteca de cada jogo na chain nova. Por enquanto só o Zelda está pronto; o Yoshi tem uma biblioteca da `c1686c70ebf5`, de mesmo ABI.

**Testes:**
- **Passaram:** `test_docs`, `test_dmsh_abi`, `test_android_img_ubo`, `test_env_switches`, `test_launcher_settings`, `test_ocarina_screen_shape`, `test_lint_python` e `test_nsp_chain` (este no WSL).
- **Compilação:** todos os arquivos C do host passam na checagem do NDK, e o APK compila.
- **Não rodaram, porque são do Mac:** os testes de host novos (`t_hid`, `t_fs`, `t_nro`, `t_svc_memory`, `t_audio*`) e os do tradutor (`test_dmsh_ampere_mem`). O teste do PRMT precisa de auxiliares de teste que esta árvore não tem e foi deixado de fora: o PRMT só foi compilado, não verificado.

## Estado do celular

- O app de teste está com o APK `ports-5b27`, reinstalado no fim do meu último teste. Para medir a chain final, use `android/app/out/swap2-native-test-4eb0.apk`.
- O test-pack do Zelda tem as bibliotecas `v-45cedbd878f7` e `v-4eb05e585fb0`.
- O driver padrão é o Turnip_Gen8_V39 e o `folder_save` do Zelda foi devolvido.
- O `com.swap2native` não foi tocado. Todo uso do celular ficou registrado em `POCO-FILA.md`.

## Logs

`output/ports-1121/`: `phone-ports.log`, `phone-zelda-4eb0.log`, `bench-4eb0-*`, `bench-ports-*`, `yoshi-*`. Scripts: `phone-ports.sh`, `phone-zelda-4eb0.sh`, `output/build/zelda-vklib-4eb0.sh` e `output/build/zelda-publish-delta-4eb0.sh`.
