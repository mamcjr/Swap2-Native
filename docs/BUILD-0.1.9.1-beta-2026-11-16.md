# Build 0.1.9.1-beta (16/11/2026)

**Situação:** compilado e assinado em 16/11 a partir do commit `f353c4a` (a senha da chave lida do arquivo DPAPI do usuário); publicado como pre-release (tag `v0.1.9.1-beta`). A 0.1.9.0-beta (versionCode 21) foi gerada antes da correção dos 30 fps e não foi publicada.

Pedidos do usuário: "Vamos adicionar também uma opção para desligar os meshs", "quero uma opção para trocar o idioma disponivel no jogo tbm", "Sim, corrige" (a retenção do limite de 30), "gere o apk novo apk", "depois que gerar pode publicar a release".

## O que muda em relação à 0.1.8.9-beta

| Mudança | Arquivos | Testado onde |
|---|---|---|
| Opção **Geometria por mesh** (`DM_VK_MESH_DRAWS=0`): todo draw de task/mesh fica de fora antes de gravar qualquer coisa | `dm/gfx/vk_android/vka_draw.c`, `GameGraphics.java`, textos | POCO F7 Ultra, varanda do Link: GPU 38-40 → 30,5 ms por quadro |
| Opção **Idioma do jogo** (`DM_LANG`): padrão (inglês dos EUA), idioma do celular ou um dos 16 do Switch | `GameSettings.java`, `Prefs.java`, `GameActivity.java`, textos | compilado; não visto no celular |
| `fps_hold off` no perfil do Zelda, passado pelo app a partir do APK (`DM_FPS_HOLD`, `Profile.fpsHold`), também para packs preparados antes | `dm/games/0400486024DEC000/game.tsv`, `build.gradle`, `Profile.java`, `GameActivity.java` | POCO, varanda, limite 30: sem mesh 20,0 → 30,0 fps, com mesh 19,9 → 21,9 fps, tempo de jogo 0,998-1,004 do real |
| Ferramentas de medida: o profiler de GPU lista todos os dispatches de um passo, estatística de pipelines gráficos (`DM_VK_PIPE_STATS_KEYS`), teste de VRS forçado (`DM_VK_VRS_TEST`) | `vka_gprof.c`, `vka_pso.c`, `vka_dev.c` | POCO |
| Opção de plantas do chão (`gsys_foliage.py`) e agrupamento de leituras cruas (`vk_raw_group.py`): no código, desligadas (sem efeito medido no Zelda) | | PC e POCO |

- **Shaders:** a cadeia continua `e3d2789e93f6`.
- Verbetes: [A 30 fps hold raised by loading stayed at 20 fps on Android](reference/frame-rate.md#a-30-fps-hold-raised-by-loading-stayed-at-20-fps-on-android); a análise das cenas pesadas fica local (`ANALISE-GPU-CENAS-PESADAS-2026-11-16.md`).

## Artefatos

| Arquivo | SHA-256 |
|---|---|
| `android/app/out/swap2-native-0.1.9.1-beta.apk` (42.314.191 bytes) | `94cce5ed6bdf669e59a035f6872810c5623ad2047b56099bf303e1e808d629d8` |
| `output/build/swap2-native-0.1.9.1-beta-source.zip` (155.205.611 bytes, 3.899 entradas) | `71362115a4bc7589745d7e1448c8569702458b2d5399f0c18dae75931b57fab5` |

- **APK:** `com.swap2native`, versionCode 22, versionName 0.1.9.1-beta. Certificado `9d6f1e98…c0f1`, conferido com `apksigner verify`.
- **Fontes:** `output/build/package-release-0191-source.py`, da árvore do commit `f353c4a`.
- **Script de build:** `output/build/build-signed.ps1 -Version 0.1.9.1-beta -Code 22` (a senha vem de `swap2-release/release-key.dpapi`, gravada pelo usuário com `salvar-senha.ps1`). **Notas:** `output/build/release-notes-0.1.9.1-beta.md`.

## Testes

- A opção de idioma não foi vista no celular antes da publicação.
- Não testado no Redmagic 11 Air, em Mali nem em Xclipse; não rodou no Mac.
