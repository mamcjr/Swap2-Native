# Build 0.1.8.8-beta (15/11/2026)

**Situação:** compilado e assinado em 15/11 às 23:35 (o usuário digitou a senha); publicado como pre-release (tag `v0.1.8.8-beta`).

Pedido do usuário em 15/11: "Sim, 0.1.8.8" (uma release com a correção da câmera lenta).

## O que muda em relação à 0.1.8.7-beta

| Mudança | Arquivos | Testado onde |
|---|---|---|
| Passo de quadro no Android com limite de 30 fps: a espera pode subir a 3 vsyncs (antes 2, uma regra do Apple), e depois de uma tentativa de voltar a 2 que falha, o tempo de GPU vira a referência | `dm/gfx/nvn/hle_nvn.c` (`fstep_tick`) | POCO, app de teste com a mesma árvore: espantalho, 90 s, tempo do jogo 1,000 do real (antes ~0,75), 1.800 de 1.800 quadros em 3 vsyncs (20 fps estáveis), 158 atrasados (516 só com a primeira parte); Mido 30,0 fps em 1,000 (sem mudança) |

- **Shaders:** a cadeia continua `e3d2789e93f6`; nada é traduzido de novo para quem já atualizou na 0.1.8.7.
- **Mac e iPhone:** nada muda (o código novo é `#if !PLAT_APPLE`).
- **Interface:** nada muda.
- Verbete: [Slow motion below 30 fps at a 30 fps cap on Android](reference/frame-rate.md#slow-motion-below-30-fps-at-a-30-fps-cap-on-android).

## Artefatos

| Arquivo | SHA-256 |
|---|---|
| `android/app/out/swap2-native-0.1.8.8-beta.apk` (42.263.559 bytes) | `58362aad35d7a52a4a887a976e27559755e66a4994d77facf5018b4661f0b3b4` |
| `output/build/swap2-native-0.1.8.8-beta-source.zip` (155.133.339 bytes, 3.885 entradas) | `86acc0379e54315661d6e6734c66c797830370d816679e93fdc28baf304b837d` |

- **APK:** `com.swap2native`, versionCode 19, versionName 0.1.8.8-beta. Certificado `9d6f1e98…c0f1`, o de sempre, conferido com `apksigner verify`. O `libnxhost.so` leva a correção (a mensagem "that is its reference now").
- **Fontes:** `output/build/package-release-0188-source.py`, da árvore do commit `08f75b2`.
- **Script de build:** `output/build/build-public-0188.ps1` (o usuário digita a senha).
- **Notas:** `output/build/release-notes-0.1.8.8-beta.md`.

## Testes

- POCO F7 Ultra, app de teste com a mesma árvore: as medidas da tabela.
- O APK assinado não foi instalado no POCO (o celular estava desconectado); não testado no Redmagic 11 Air, em Mali nem em Xclipse; não rodou no Mac.
- Medidas da mesma noite, sem mudança de código: geração de quadros com limite 30 (espantalho 20,6 → 20,1 fps; Mido 30/30, quadros em dia 78 % → 95 %) e o modo Desempenho do Game Turbo (pior numa partida em cada ponto: espantalho 20,0 → 18,4 fps, Mido 30 → 20 fps).
