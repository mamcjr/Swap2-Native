# Build 0.1.8.7-beta (15/11/2026)

**Situação:** compilado e assinado em 15/11 às 20:48 (o usuário digitou a senha); a atualização dos shaders conferida no POCO; publicado como pre-release (tag `v0.1.8.7-beta`).

Pedido do usuário em 15/11: "resolva esses que ficaram de fora" (a release que levasse as saídas diretas do mesh aos jogadores).

## O que muda em relação à 0.1.8.6-beta

| Mudança | Arquivos | Testado onde |
|---|---|---|
| Saídas diretas do mesh: a forma compute de cada programa de mesh grava as saídas direto no buffer, sem a cópia dos planos em memória compartilhada; cadeia de shaders `e3d2789e93f6` | `dm/gfx/tools/vk_android_mesh.py`, `dm/gfx/nvn/dmsh_chain_vk.h`, `dm/games/0400486024DEC000/vk_delta.tsv` | POCO, app de teste com a biblioteca nova: espantalho 22,8 / 23,1 fps (antes 18,6), Mido 41,9 fps (antes 34); 25 testes da cadeia e 18 do mesh no WSL |
| Reuso de oclusão desligado por padrão (não estava em nenhuma release; desenhava brilhos de luz sobre o Link) | `dm/gfx/vk_android/vka_exec.c` | POCO: 0 de 12 capturas com o brilho |
| Um pool que vai para o arquivo não ocupa a RAM duas vezes durante a cópia (Banana Native `b29c30af`) | `dm/gfx/vk_android/vka_fileback.h` | compilado; não medido |
| Uma falta de escrita libera a faixa protegida em volta (`DM_DIRTY_FAULT_RUN_KB`, Banana Native `6624426f`) | `dm/gfx/nvn/nvn_dirty.c` | compilado; partidas normais no POCO |
| O compute anotado na lista de pré-aquecimento antes do save forçado | `dm/gfx/vk_android/vka_pso.c` | compilado |
| `plat_zone_malloc` alinhado a 16 no Android (DroidPlay `e52f2026`) | `dm/host/plat/plat_android.c` | compilado; partidas normais no POCO |
| Lista de pré-aquecimento do Zelda: 3.456 chaves (865 antes), agora versionada | `dm/games/0400486024DEC000/vk_prewarm.bin`, `dm/.gitignore` | no APK (`tools.zip`); vale na instalação nova |
| Diagnósticos: `DM_VK_PIPE_STATS`, `DM_VK_MESH_DIAG_STAGE=3` | `vka_pso.c`, `vka_dev.c`, `vka.h`, `vka_mesh.inc` | POCO |

- **Os jogadores atualizam os shaders do Zelda uma vez:** quem tem a biblioteca `4e12942060ff` (0.1.8.x) traduz de novo só **500** shaders (o manifesto do perfil); com bibliotecas mais antigas, de 1.829 a 2.633. O app joga a biblioteca antiga enquanto isso (mesma ABI, `623c0a2f1e8a`).
- **Bananza, Mario Kart World e Pokopia:** os manifestos continuam de uma versão antiga da cadeia, como já estavam antes desta release.
- **Interface:** nada muda.

## Artefatos

| Arquivo | SHA-256 |
|---|---|
| `android/app/out/swap2-native-0.1.8.7-beta.apk` (42.263.467 bytes) | `6fa4974a4e2db717b2508b3a48e2dbd11174b5ad3a5b9d2f25bb35ff7fd51d1b` |
| `output/build/swap2-native-0.1.8.7-beta-source.zip` (155.129.336 bytes, 3.884 entradas) | `cb9aa663f7e6b20af479841b58c09126e30769dc8389e7d4fa51c8f04d5a3a1b` |

- **APK:** `com.swap2native`, versionCode 18, versionName 0.1.8.7-beta. Certificado `9d6f1e98…c0f1`, o de sempre, conferido com `apksigner verify`. O `tools.zip` leva o manifesto `e3d2789e93f6` do Zelda e a lista de 3.456 chaves.
- **Fontes:** `output/build/package-release-0187-source.py`, da árvore do commit `f5f75b0` (o código em `15998a3`).
- **Script de build:** `output/build/build-public-0187.ps1` (o usuário digita a senha).
- **Notas:** `output/build/release-notes-0.1.8.7-beta.md`.

## Testes

- POCO F7 Ultra, app de teste com a mesma árvore: as medidas da tabela (`docs/RESULTADOS-ETAPA1-2026-11-15.md`, décimo passo em diante).
- **A atualização dos shaders** (o caminho de quem instala por cima da 0.1.8.6), no POCO com o app de teste e só a biblioteca `4e12942060ff` no pack: ao abrir o Zelda o app começou a atualização sozinho (`expected_shaders: 500`) e o jogo rodou com a biblioteca antiga (mesma ABI) durante ela. Separação 18 min, tradução dos 500 shaders 9 min, pack 2 min: **cerca de 30 min**. A biblioteca feita no celular é **idêntica byte a byte** à do WSL (SHA-256 `ef4896221383cc26…`).
- Não testado no Redmagic 11 Air, em Mali nem em Xclipse; não rodou no Mac.
