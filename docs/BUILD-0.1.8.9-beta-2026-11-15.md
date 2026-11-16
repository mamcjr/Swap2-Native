# Build 0.1.8.9-beta (15-16/11/2026)

**Situação:** compilado e assinado na madrugada de 16/11 (o usuário digitou a senha); publicado como pre-release (tag `v0.1.8.9-beta`).

Pedido do usuário: "olhe o aplicativo assinado, continua lento", "nas configuracoes essa opção fica desabilitada no menu do jogo", depois "Sim, 0.1.8.9".

## O que muda em relação à 0.1.8.8-beta

| Mudança | Arquivos | Testado onde |
|---|---|---|
| O passo de quadro (o stub `FRAME_STEP` do rewriter) conta como limite de 30 e 60 fps para o app: o Zelda não tem limitador próprio, o host respondia "nenhum limite", e o app desabilitava o Limite de FPS (configurações do jogo e painel) dizendo que o jogo escolhe a própria taxa | `dm/host/hle_fpslock.c` (`dm_fps_lock_rates`) | caso 20 novo de `dm/host/tests/t_fpslock.c`, compilado com o NDK e rodado no POCO |
| Voltar a "o próprio jogo" com o jogo rodando chega ao passo de quadro (antes ficava o último limite) | `dm/host/hle_fpslock.c` (`dm_fps_target_set`) | o mesmo caso 20 |

- **O que o usuário viu:** o log do app assinado 0.1.8.8 tinha `DM_FPS_TARGET=0` (a escolha salva era "o próprio jogo"): o Zelda andava 2 vsyncs de tempo de jogo por quadro, a 21 fps uns 0,7 do tempo real. A correção da 0.1.8.8 vale só com o limite em 30.
- **Depois de atualizar:** abrir o Zelda uma vez (o app então vê as opções), e escolher 30 fps ou Padrão no Limite de FPS do Zelda.
- **O teste inteiro `t_fpslock`** já falhava no Android antes desta mudança (caso 0; é um teste do Mac). O caso 20 sozinho passa.
- **Shaders:** a cadeia continua `e3d2789e93f6`. **Mac e iPhone:** o launcher deles não usa essas taxas; o 0 na página de estado vale lá também.
- Verbete: [A frame step alone offered no cap in the Android launcher](reference/frame-rate.md#a-frame-step-alone-offered-no-cap-in-the-android-launcher).

## Artefatos

| Arquivo | SHA-256 |
|---|---|
| `android/app/out/swap2-native-0.1.8.9-beta.apk` (42.263.695 bytes) | `302d1d49a1ffd8e44969e4b678fbec19b87f4cb54fc4db0efdcd6ade94f7feea` |
| `output/build/swap2-native-0.1.8.9-beta-source.zip` (155.136.037 bytes, 3.887 entradas) | `250d4307fba3ba93141dcb899f19113ada794173f479efa2a3073b9ce407a12c` |

- **APK:** `com.swap2native`, versionCode 20, versionName 0.1.8.9-beta. Certificado `9d6f1e98…c0f1`, conferido com `apksigner verify`.
- **Fontes:** `output/build/package-release-0189-source.py`, da árvore do commit `946719b`.
- **Script de build:** `output/build/build-public-0189.ps1` (o usuário digita a senha). **Notas:** `output/build/release-notes-0.1.8.9-beta.md`.

## Testes

- O painel e as configurações do jogo com as opções de volta não foram vistos no celular antes da publicação.
- Não testado no Redmagic 11 Air, em Mali nem em Xclipse; não rodou no Mac.
