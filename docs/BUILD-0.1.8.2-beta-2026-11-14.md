# Build 0.1.8.2-beta (14/11/2026)

**Situação:** compilado, assinado e publicado como pre-release em 14/11 (https://github.com/mamcjr/Swap2-Native/releases/tag/v0.1.8.2-beta, tag `v0.1.8.2-beta` em `main`); os hashes dos três arquivos no GitHub batem com os locais. Não instalado.

Versão feita para o Redmagic 11 Air, onde o Zelda fecha no começo da cena desde a 0.1.7.1 e rodava até a 0.1.5: o próprio app passa a testar o jogo no aparelho e gerar um relatório, porque o dono não tem como mandar o relatório de bug do sistema (dados pessoais) nem digitar opções à mão.

## O que muda em relação à 0.1.8.1-beta

| Mudança | Arquivos | Testado onde |
|---|---|---|
| Diagnóstico automático: o app abre o jogo sozinho até 8 vezes, cada vez com um conjunto de opções do host, numa cópia do save, e gera um relatório único | `AutoDiag.java` (novo), `GameActivity.java`, `MainActivity.java`, `GameSettings.java`, `strings.xml` (en e pt) | POCO F7 Ultra, app de teste: 8 rodadas, 6 mortas por mim com sinal 9 (`output/zelda-1112/diag-test.sh`), relatório `diagB-report.log`; a entrada pela tela de configurações do jogo conferida |
| Censo de memória com RAM total, swap, tipos de memória do processo, huge pages e cgroup; censo extra quando o processo cresce 256 MiB | `dm/gfx/vk_android/vka_ctl.c` | POCO: linhas `memory census kinds` e `memory census system` no log |
| O dispositivo Vulkan é criado pela thread do host, não na thread de interface do launcher (Banana Native `4299044a`, issue 251 deles: "não está respondendo" em Xclipse); o callback de dispositivos de áudio também sai dela | `dm/gfx/vk_android/vka_present.c`, `GameActivity.java`, comentário em `nxlaunch.c` | POCO: título a 60 fps, Kokiri a 27; sem Xclipse para confirmar o efeito |

- **Rodadas do diagnóstico:** sem opção nenhuma; depois todas as mudanças do renderizador desde a 0.1.5 desfeitas de uma vez; se o jogo vive assim, cada uma sozinha; se não, sem mesh, sem pipelines assíncronos, sem consultas de oclusão e sem pools em arquivo. Detalhes em `android/TESTING.md`, seção "Automatic diagnosis".
- **Interface:** a linha nova em Configurações do jogo › Geral e os textos são meus; o desenho da interface é do GPT, que deve ser avisado.
- **Achado de passagem, não corrigido:** o comando de teste `--ez run true` com um caminho `/sdcard/...` não inicia o jogo (o caminho do card é normalizado para `/storage/emulated/0/...`); o `cold-run.sh` só funcionava por causa do toque em Jogar.

## Artefatos

| Arquivo | SHA-256 |
|---|---|
| `android/app/out/swap2-native-0.1.8.2-beta.apk` (42.146.971 bytes) | `b25338990c58cdc60e31847c8cda9cc0bd46cc0e67b04184875b3bcad538753b` |
| `output/build/swap2-native-0.1.8.2-beta-source.zip` (154.910.082 bytes, 3.860 entradas) | `3faaafe0a56c14ae5d23108d6ee454fad268de82e3a4ce2189d8a9289256f506` |

- **APK:** `com.swap2native`, versionCode 13, versionName 0.1.8.2-beta. Certificado `9d6f1e98…c0f1`, o de sempre, conferido com `apksigner verify`. O `libnxhost.so` traz as linhas novas do censo; o dex traz o `AutoDiag` e a thread `audio-route`; os recursos trazem os textos em português.
- **Fontes:** `output/build/package-release-0182-source.py`. Em relação ao zip da 0.1.8.1: 16 arquivos mudam (os do código desta versão, os documentos e o `RELEASE-SOURCE.txt`) e entram o `AutoDiag.java` e a nota do build da 0.1.8.1. O scan (`scan-release-zip.py`) só aponta os dois `.nso` sintéticos do corpus de fuzz do upstream.
- **Script de build:** `output/build/build-public-0182.ps1` (o usuário digita a senha).
- **Notas:** `output/build/release-notes-0.1.8.2-beta.md`.

## Testes

- POCO F7 Ultra (Adreno 830, 891.14), app de teste, save de teste: os três itens da tabela.
- `test_docs`: só os dois links quebrados antigos. `test_android_game_flow`, `test_android_touch_profile` e `test_profile_mods` (os que leem `strings.xml`) passam.
- Não testado no Redmagic 11 Air, em Mali nem em Xclipse.
- `regress.py`: não rodou (é do Mac; as mudanças do host são do caminho Android).

## O que falta

- O dono do Redmagic 11 Air rodar o diagnóstico (Opções › Configurações do jogo › Geral › Diagnóstico automático) e mandar o relatório.
