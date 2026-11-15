# Build 0.1.8-beta (14/11/2026)

**Situação:** compilado, assinado e publicado como pre-release em 14/11 (https://github.com/mamcjr/Swap2-Native/releases/tag/v0.1.8-beta, tag `v0.1.8-beta` em `main`); os hashes dos três arquivos no GitHub batem com os locais. Não instalado. Os caminhos novos para Mali e Xclipse não rodaram em nenhum aparelho desses.

## O que muda em relação à 0.1.7.2-beta

| Mudança | Arquivos | Testado onde |
|---|---|---|
| Memória do jogo em hardware buffers quando o driver não tem `VK_EXT_map_memory_placed` | `vka_ahb_alias.h` (novo), `vka_dev.c`, `vka_mem.c`, `vka_readiness.c`, `android/host/CMakeLists.txt` | POCO F7 Ultra com `DM_VK_ALIAS=ahb`: Kokiri a 27–31 fps |
| Texturas BC decodificadas na CPU onde a GPU não tem BC | `vka_bcdec.h` (novo), `vka_tex.c`, `vka_exec.c`, `vka_capture.c`, `dm/third_party/bcdec` | POCO com `DM_VK_BC_DECODE=1`: Kokiri a 28 fps, +581 MiB de imagens; `t_android_bcdec` |
| Matrizes das tabelas de textura ajustadas ao aparelho | `vka_spvpatch.c/.h`, `vka_shader.c`, `vka_tex.c` | POCO com `DM_VK_POOL_SLOTS=499680`; `t_android_spvpatch` |
| Fatos do dispositivo e o caminho de memória no log do jogo | `vka_dev.c`, `vka_mem.c` | POCO |
| Compilações em andamento guardadas num arquivo que sobrevive a um kill; a abertura seguinte relata e pula o shader | `vka_pso.c`, `GameActivity.java` (`NX_PREV_EXIT`) | POCO, `output/zelda-1112/bbox-test.sh` |
| A janela só vai ao host depois de o ambiente estar aplicado | `android/app/src/main/cpp/nxlaunch.c` | POCO |

Referência: `docs/reference/memory.md`, `shaders.md` e `rendering.md` (cinco entradas novas). Plano e levantamento: `docs/PLANO-HIBRIDO-MALI-XCLIPSE-2026-11-13.md`.

## Artefatos

| Arquivo | SHA-256 |
|---|---|
| `android/app/out/swap2-native-0.1.8-beta.apk` (42.125.499 bytes) | `a0f3e5e176af7a1620ec982eac5d6131f205a1ea2bd5e886c60ea97ecc662398` |
| `output/build/swap2-native-0.1.8-beta-source.zip` (156.924.456 bytes, 3.857 entradas) | `9a9c86008c32da2ebf8f4be3f4aa01d6a4ae829219f7ff97a2211e7a84add412` |

- **APK:** `com.swap2native`, versionCode 11, versionName 0.1.8-beta. Certificado `9d6f1e98…c0f1`, o de sempre, conferido com `apksigner verify`. O `libnxhost.so` e o `libnxlaunch.so` do APK trazem as linhas de log novas de cada mudança.
- **Fontes:** `output/build/package-release-018-source.py`. O scan (`scan-release-zip.py`) só aponta os dois `.nso` sintéticos do corpus de fuzz do upstream.
- **Script de build:** `output/build/build-public-018.ps1` (o usuário digita a senha).
- **Notas:** `output/build/release-notes-0.1.8-beta.md`.

## Testes

- No POCO F7 Ultra (Adreno 830, 891.14), app de teste, save de teste:
  - caminho padrão (placed): título a 60 fps e Kokiri a ~28 fps, com cache frio e quente;
  - os três caminhos forçados da tabela acima.
- `t_android_spvpatch` e `t_android_bcdec` passam no WSL (sem sanitizadores).
- `test_docs`: só os dois links quebrados antigos.
- `test_env_switches`: já falhava antes (dezenas de opções antigas sem descrição); os arquivos novos ainda não rastreados pelo git aparecem nele até serem commitados.
- `regress.py`: não rodou (é do Mac; as mudanças do host só agem no Android).

## O que falta

- Um log de Mali ou Xclipse com esta versão. Procurar `[vk] guest memory:` e `BC textures:`.
- Dois logs seguidos do Redmagic 10 Pro. O segundo traz `the run before ended with ...`.
