# Build 0.1.9.3-beta (17/11/2026)

**Situação:** compilado e assinado em 17/11 a partir do commit `2160474` (a senha da chave lida do arquivo DPAPI do usuário); instalado no POCO F7 Ultra do usuário às 09:02 e publicado como pre-release (tag `v0.1.9.3-beta`). A 0.1.9.2-beta (versionCode 23, commits `e4673a5` e `310e7be`, sem o limite de FPS em todos os jogos) foi gerada e não foi publicada.

Pedidos do usuário: "o menu lateral durante o jogo está em inglês… ajustar a tradução de todos os textos, e adicionar todos os idiomas presentes no Switch 2", "às vezes eu toco na opção de parar o jogo… ele fala que o jogo crashou", "Remova essa msg no início de mario kart world", "ajustar o controle da interface utilizando um controle externo… inverter os botões A com B e X com Y", "ao conectar o controle na tela do jogo não deve aparecer nem aquela seta", "quero que a opção de FPS fique disponível em todos os jogos", a interface do painel em jogo (`docs/SWAP2-NATIVE-SIDE-PANEL-v1.md`), "Commit and build now", "Yes, build 0.1.9.3", "pode publicar".

## O que muda em relação à 0.1.9.1-beta

| Mudança | Arquivos | Testado onde |
|---|---|---|
| Os textos do app em recursos e traduzidos para os 16 idiomas do Switch 2; opção **Idioma do app** (LocaleManager) e `locales_config.xml` | `res/values*/strings*.xml`, `res/xml/locales_config.xml`, ~30 classes Java, `dm/tools/tests/test_android_strings.py` | teste de strings (16 idiomas, chaves, placeholders, escapes); APK com as 15 configurações de idioma; painel visto no POCO em português |
| **Parar o jogo** no painel grava `state=stopped` antes de encerrar o processo (antes o launcher via um "crash") | `SidePanel.java` (`ExitWatch.stopped`) | compilado; não reproduzido no celular |
| Mario Kart World sem o aviso "Ready, with limits": `status playable`, sem a linha de memória compartilhada | `dm/games/0400C3F00006E000/readiness.tsv` | POCO, 891.14: nenhum pipeline de compute recusado com o limite de 32 KiB |
| Navegação por controle (A joga, Y configurações, L/R abas, layout Nintendo); seta do painel escondida com controle; HOME ou − e + segurados abrem o painel | `PadNav.java`, `MainActivity.java`, `GameActivity.java` | compilado e teclas emuladas; não testado com o GameSir X4 Aileron do usuário |
| Painel em jogo novo (proposta v1): claro/escuro, corpo rolável, ações fixas, rodapé em uma ou duas linhas conforme os rótulos cabem | `SidePanel.java` | POCO, claro e escuro, toque e teclas emuladas, fonte 1,3× |
| Limite de FPS em todos os jogos: onde não há limitador conhecido, o host segura os quadros na tela (`dm_fps_lock_paced`) | `dm/host/hle_fpslock.c`, `dm/gfx/nvn/hle_nvn.c`, `GameSettings.java`, `SidePanel.java` | `t_fpslock` 25/25 no POCO; MKW abaixo de 30 fps sem piora; velocidade do Pokopia com limite em aberto |
| Lint do Python (B023) que falhou no CI da 0.1.9.1 | `dm/tools/gsys_foliage.py`, `dm/gfx/tools/vk_raw_group.py` | testes locais |

- **Shaders:** a cadeia continua `e3d2789e93f6`.
- Verbetes: o limite por apresentação em [frame-rate.md](reference/frame-rate.md); as medidas do MKW e do Pokopia nas notas de cada jogo.

## Artefatos

| Arquivo | SHA-256 |
|---|---|
| `android/app/out/swap2-native-0.1.9.3-beta.apk` | `f63a81aaf5957d90888d31fd0c3c7e74d975eac04cb2d5773e2bd8df5babec7c` |
| `output/build/swap2-native-0.1.9.3-beta-source.zip` (155.741.826 bytes, 4.014 arquivos) | `362f97ca30ff5c07e142afdf0782cffb3ecf995466f6cf3754ec20b058cbf8ed` |

- **APK:** `com.swap2native`, versionCode 24, versionName 0.1.9.3-beta. Certificado `9d6f1e98…c0f1`, conferido com `apksigner verify`; carimbo do build `2160474`, sem `+dirty`.
- **Fontes:** `output/build/package-release-0193-source.py`, da árvore do commit `2160474`.
- **Script de build:** `output/build/build-signed.ps1 -Version 0.1.9.3-beta -Code 24`. **Notas:** `output/build/release-notes-0.1.9.3-beta.md`.

## Testes

- Testes Python/JVM do app: passam, menos `frontend`, `packfind`, `saves` e `signer`, que já falham no Windows no commit anterior; teste de docs passa.
- `t_nvn` e `regress.py` (só no Mac) não rodaram.
- Não testado no Redmagic 11 Air, em Mali nem em Xclipse; não rodou no Mac.
- Em aberto: um travamento depois de pausar e continuar um jogo (no host, visto pela sessão do painel), a velocidade do Pokopia com limite e o MKW acima de 30 fps com limite.
