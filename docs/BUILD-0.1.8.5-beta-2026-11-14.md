# Build 0.1.8.5-beta (14/11/2026)

**Situação:** compilado, assinado e publicado como pre-release em 14/11 (https://github.com/mamcjr/Swap2-Native/releases/tag/v0.1.8.5-beta, tag `v0.1.8.5-beta` em `main`); os hashes dos três arquivos no GitHub batem com os locais. Não instalado.

Opção nova em Gráficos para escolher como a memória do jogo chega à GPU, com o alias ("Buffer compartilhado") como padrão, por decisão do usuário em 14/11.

## O que o relatório da 0.1.8.4 mostrou (5º relatório do Redmagic 11 Air)

O dono pôs o driver do jogo em Sistema e o diagnóstico fez uma rodada só, viva aos 420 s, com a cena carregada (2,6 GiB de imagens), a 11 fps. O driver do sistema de lá não tem placed maps e usa o alias exportado; todos os drivers com placed maps (891.11, 891.14, Turnip V39) morreram nos relatórios anteriores. A causa no kernel daquele aparelho continua desconhecida. Não se sabe ainda se o 891.x passa da cena pelo alias lá, nem por que o driver do sistema fica em 11 fps.

## O que muda em relação à 0.1.8.4-beta

| Mudança | Arquivos | Testado onde |
|---|---|---|
| Opção **Memória compartilhada com a GPU** em Gráficos (geral e por jogo): **Buffer compartilhado** (padrão, `DM_VK_ALIAS=prefer`) ou **Mapeamento direto** (placed maps, o que todas as versões anteriores faziam) | `GameGraphics.java`, `strings.xml` (en e pt) | POCO, pela tela: a linha aparece, a escolha grava e volta a "seguir o geral"; com cada escolha o log mostra o caminho certo |
| `DM_VK_ALIAS=prefer` no host: alias exportado, senão o de hardware buffer, senão placed maps; nunca deixa a GPU sem a memória do jogo | `dm/gfx/vk_android/vka_dev.c`, `vka_mem.c`, `vka.h` | POCO, 891.14: `this driver's placed maps are left unused; pools are exported Vulkan memory`, Kokiri a 30–31 fps; driver do sistema (sem placed maps) também inicia. **O caminho "driver sem nenhum alias" não foi exercitado** (não há driver assim aqui) |
| Um jogo em Mapeamento direto morto de fora duas vezes seguidas em primeiro plano passa sozinho para o alias | `vka_placedkill.h` (novo), `vka_dev.c`, `GameActivity.java` (importância no `NX_PREV_EXIT`) | `t_android_placedkill` (fora do celular); POCO: duas mortes por `kill -9`, a 3ª partida entra no alias a 35 fps, a 4ª continua nele (`output/zelda-1112/placedkill-test.sh`) |
| Diagnóstico automático: a 2ª rodada testa o caminho de memória em que o jogo **não** está; as rodadas do diagnóstico não contam nem disparam a troca automática | `AutoDiag.java`, `GameActivity.java` | POCO: 12 rodadas, `placed-maps` viva 176 s (`output/zelda-1112/diagF-report.log`) |

- **Risco da mudança de padrão:** o alias passa a ser o caminho de todos os aparelhos com driver que o tenha, e ele foi exercitado só no POCO F7 Ultra (Adreno 830, 891.14 e driver do sistema) e, pelo driver do sistema, no Redmagic 11 Air. Em Mali e Xclipse o alias já era o caminho usado quando não há placed maps. No POCO a memória disponível no fim de uma partida ficou cerca de 0,5 GB menor com o alias do que com placed maps, numa comparação de duas partidas em momentos diferentes: não é uma medida cuidadosa.
- **Interface:** a linha nova em Gráficos e os textos são meus; o desenho é do GPT.

## Artefatos

| Arquivo | SHA-256 |
|---|---|
| `android/app/out/swap2-native-0.1.8.5-beta.apk` (42.167.859 bytes) | `85a75684a84706f96a56474bb37683158ede3b28009411f6c52ef1cabc61e476` |
| `output/build/swap2-native-0.1.8.5-beta-source.zip` (154.933.730 bytes, 3.865 entradas) | `0644d2f95f14db2f9f02525ddc865b2fa060ed3cbbe7b5df0e3dae3a0a81386f` |

- **APK:** `com.swap2native`, versionCode 16, versionName 0.1.8.5-beta. Certificado `9d6f1e98…c0f1`, o de sempre, conferido com `apksigner verify`. O dex traz a opção (`graphics_memory`, `DM_VK_ALIAS=prefer`) e a rodada `placed-maps`; o `libnxhost.so` traz as linhas do modo prefer e da troca automática; os recursos trazem o texto em português.
- **Fontes:** `output/build/package-release-0185-source.py`. Em relação ao zip da 0.1.8.4: 15 arquivos mudam (o código desta versão e a documentação) e entram `vka_placedkill.h`, `t_android_placedkill.c` e a nota do build da 0.1.8.4. O scan (`scan-release-zip.py`) só aponta os dois `.nso` sintéticos do corpus de fuzz do upstream.
- **Script de build:** `output/build/build-public-0185.ps1` (o usuário digita a senha).
- **Notas:** `output/build/release-notes-0.1.8.5-beta.md`.

## Testes

- POCO F7 Ultra, app de teste, save de teste: os itens da tabela.
- `t_android_placedkill` passa no WSL (sem sanitizadores). `test_docs`: só os dois links quebrados antigos.
- Não testado no Redmagic 11 Air, em Mali nem em Xclipse.
- `regress.py`: não rodou (é do Mac; as mudanças são do caminho Android).

## O que falta

- O dono do Redmagic 11 Air instalar, voltar o driver para 891.14 e jogar: é o teste que diz se o alias resolve lá com o driver rápido.
