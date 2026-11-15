# Build 0.1.7.2-beta (13/11/2026)

**Situação:** compilado, assinado e publicado como pre-release em 13/11 (https://github.com/mamcjr/Swap2-Native/releases/tag/v0.1.7.2-beta, tag `v0.1.7.2-beta` em `main`); os hashes dos três arquivos no GitHub batem com os locais. Não instalado e não testado em Adreno 840 ou 850.

## O que muda em relação à 0.1.7.1-beta

- `dm/gfx/vk_android/vka_dev.c`: no driver da Qualcomm (driver ID 8), o `VK_EXT_mesh_shader` fica desligado por padrão. Os draws de task e mesh vão pelo fallback em compute (`vka_mesh.inc`), e `DM_VK_MESH=1` volta ao nativo.
- Motivo: o Redmagic 12 (Adreno 850, 891.14) desenhou a cena do OoT quebrada.
- Entrada de referência: `docs/reference/shaders.md`, "Native mesh shading on Qualcomm's driver".
- Análises: `docs/ANALISE-DROIDPLAY-0.8.4-2026-11-13.md` e o adendo da v0.3.21 em `docs/ANALISE-BANANA-0.3.12-0.3.14-2026-11-12.md`.

## Artefatos

| Arquivo | SHA-256 |
|---|---|
| `android/app/out/swap2-native-0.1.7.2-beta.apk` (42.091.951 bytes) | `1b45e6b328f7b2d839ed06fa2237489e933ea3fbd8c08b02ebc9522e2c250409` |
| `output/build/swap2-native-0.1.7.2-beta-source.zip` (156.888.711 bytes, 3.850 entradas) | `d869ae343abc04fd7f4bc0e5adf4ce8fd44bbeac7dfbdca84ba2b78e2a590288` |

- **APK:** `com.swap2native`, versionCode 10, versionName 0.1.7.2-beta. Certificado `9d6f1e98…c0f1`, o mesmo das versões anteriores, conferido com `apksigner verify`. O `libnxhost.so` do APK tem a linha nova do log ("the compute fallback on Qualcomm's driver").
- **Fontes:** `output/build/package-release-0172-source.py`. O scan (`scan-release-zip.py`) só aponta os dois `.nso` sintéticos do corpus de fuzz do upstream (256 e 434 bytes), já aceitos na 0.1.7.1.
- **Script de build:** `output/build/build-public-0172.ps1` (o usuário digita a senha).
- **Notas:** `output/build/release-notes-0.1.7.2-beta.md`.

## Testes

- `vka_dev.c` compila com o NDK 28.2 e as flags do build.
- `test_docs`: a checagem de links falha só por dois links quebrados que já existiam (`docs/BUILD-BANANANATIVE-v0.2.5-2026-11-09.md:7` e `docs/reference/services.md:921`); as outras checagens passam.
- `regress.py`: não rodou (é do Mac; a mudança só age no Android com o driver ID 8).
- **Falta:** uma rodada no Redmagic 12 (Adreno 850) depois da abertura. Conferir:
  - se a imagem está certa;
  - se a linha `subgroup shaders` vem sem "task/mesh";
  - os fps contra `DM_VK_MESH=1`.
