# Build 0.2.0-beta (24/11/2026)

**Situação:** compilado e assinado em 24/11 a partir do commit local `77a7a2e`, com a senha da chave lida do arquivo DPAPI do usuário. Publicado como pre-release ([v0.2.0-beta](https://github.com/mamcjr/Swap2-Native/releases/tag/v0.2.0-beta)). O commit público `44309eb` tem a mesma árvore e entrou no `main` do GitHub depois da CI verde no branch temporário `ci-fix`, que já foi apagado. A 0.2.0-beta-pre (versionCode 25, de `aaffe01`) foi instalada no celular do usuário em 23/11 e não foi publicada.

Pedidos do usuário:
- "ok, me avisa quando subir";
- "Segurar e corrigir", sobre o piscar das folhagens do Zelda;
- "vamos portar tudo e adiar a 0.2.0";
- "pode lançar sem o delta do pokopia e fire emblem".

## O que muda em relação à 0.1.9.3-beta

- **Commits locais desde a publicação anterior** (`093bd74`, público `561f086`):
  - ports do Banana v0.3.50 a v0.3.52-preview e do nxnative até `ffb06d60`;
  - o escalonador de esperas do Banana, que corrige o piscar das folhagens (de 34 quadros em 60 s para 0);
  - a grama, a neblina e as opções do Ocarina;
  - o menu de gráficos em quatro seções;
  - as bibliotecas e os deltas da cadeia 706d;
  - correções de CI (`bd6a0d2`, `77a7a2e`).
  
  A lista completa está na mensagem do commit público `44309eb`, e o detalhe dos ports em `docs/ANALISE-PORTS-2026-11-24.md`.
- **Shaders:** a cadeia Vulkan é `706d0c7262fa`. O `DMSH_CHAIN` é `96b5e74c34b6`, e o digest das ferramentas, `d167a5a59651`, volta para a 706d pelo `vk_android_same.tsv`, porque a biblioteca inteira do Ocarina saiu idêntica.
- **Deltas (vindos da 45ce):** retraduzem
  - 4 shaders em MKW, DKB e Splatoon;
  - 0 em Mario Tennis;
  - 1 no Star Fox;
  - 16 no HWAoI;
  - 2.282 no Ocarina;
  - 40.226 no Yoshi.
  
  O Pokopia e o Fire Emblem saem sem delta e retraduzem tudo, cerca de 131.000 e 17.000 shaders. A biblioteca 706d do Pokopia continua compilando no WSL.
- **Desligado por padrão:** os gates de CPU (`DM_VK_EVENT_GATE`), que derrubaram a GPU no Ocarina. Causa aberta em `docs/reference/nvn-objects.md`.
- **Testes:**
  - CI do repositório público verde (tests, lint-c, lint-python);
  - no celular, POCO F7 Ultra com 891.14, Ocarina em Hyrule Field com uma cópia do save do usuário: o APK de teste com todos os ports chegou aos quadros 6.561 e 4.853 sem travar e sem as folhagens sumirem;
  - não rodou: o `regress.py check --games`, que é só do Mac.

## Artefatos

| Arquivo | SHA-256 |
|---|---|
| `android/app/out/swap2-native-0.2.0-beta.apk` (62.120.821 bytes) | `a67bde6f05b6bc2e9dc363f936593514501d56b42e685e6b59f156d8de6e96d9` |
| `output/build/swap2-native-0.2.0-beta-source.zip` (174.856.520 bytes, 4.214 arquivos) | `e7d0f68492aef52cff083f5145470fbc86742c2ddd136b334791fbaf1311c5a4` |

- **APK:** `com.swap2native`, versionCode 25, versionName 0.2.0-beta. Certificado `9d6f1e98…c0f1`, conferido com `apksigner verify`. O carimbo do build é `77a7a2e`, sem `+dirty`: o trabalho do replay sem commit foi guardado com `git stash` durante a compilação e restaurado depois.
- **Fontes:** feitas por `output/build/package-release-020-source.py`. Os arquivos versionados vêm do commit `77a7a2e` (HEAD), e não da cópia de trabalho, então o trabalho do replay fica de fora, assim como o relatório noturno de 20/11, que não está commitado.
