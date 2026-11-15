# Build 0.1.8.4-beta (14/11/2026)

**Situação:** compilado, assinado e publicado como pre-release em 14/11 (https://github.com/mamcjr/Swap2-Native/releases/tag/v0.1.8.4-beta, tag `v0.1.8.4-beta` em `main`); os hashes dos três arquivos no GitHub batem com os locais. Não instalado.

Terceira versão do diagnóstico automático, depois do relatório que a 0.1.8.3 trouxe do Redmagic 11 Air. O host é o mesmo da 0.1.8.3; só o launcher muda.

## O que o relatório da 0.1.8.3 mostrou (4º relatório do Redmagic)

Dez rodadas (REDMAGICOS11.0.15_EU, kernel 6.6.92-android15-8, `Adreno830v2`; o kernel do POCO é 6.6.118):

| Rodada | Resultado |
|---|---|
| Sem ajuste | morreu aos 47 s |
| Sem huge pages (`DM_THP=0`, conferido: 4 MiB no censo) | morreu aos 51 s |
| Sem desenhar nada (`DM_VK_DRAW=0`) | morreu aos 47 s |
| **Driver do sistema** (sem placed maps: memória por alias exportado) | **viva aos 107 s**, 60 s dentro da cena com 1,2 GiB de imagens, compilando pipelines |
| Driver 891.14 | morreu aos 52 s |
| Turnip V39 | morreu aos 50 s |
| Como na 0.1.5, com `DM_VK_UNPLACE_MREMAP=1` | morreu aos 51 s |
| Pipelines síncronos | morreu aos 70 s |
| Sem consultas de oclusão | morreu aos 53 s |
| Sem pools em arquivo | "viva", mas nunca saiu do título |

- A hipótese das huge pages está descartada, e o trabalho da GPU também.
- Todos os drivers com placed maps morrem; o único caminho que passou foi o que não usa placed maps. Não é prova (na 3ª rodada de relatórios duas rodadas viveram 50 s na cena antes de morrer), mas é a única diferença de caminho de memória.
- **Hipótese, não provada:** os placed maps no kernel daquele aparelho. O teste é o driver do próprio jogo sem eles.

## O que muda em relação à 0.1.8.3-beta

| Mudança | Arquivos | Testado onde |
|---|---|---|
| Rodadas `alias-fd` e `alias-ahb` (`DM_VK_ALIAS=fd` / `=ahb`): o driver do jogo sem placed maps, logo depois da rodada sem ajuste; depois vêm driver do sistema, sem desenhar, sem huge pages e os outros drivers | `AutoDiag.java` | POCO F7 Ultra, app de teste, driver 891.14: `alias-fd` roda a cena a 28–35 fps |
| Uma rodada só conta como viva 120 s depois do ponto em que a primeira morreu (era 60 s) | `AutoDiag.java` | POCO |
| O relatório diz até onde cada rodada chegou (`memory mapped up to +N GB`) e marca a que viveu sem chegar ao ponto da primeira | `AutoDiag.java`, `strings.xml` (en e pt) | POCO |

## Artefatos

| Arquivo | SHA-256 |
|---|---|
| `android/app/out/swap2-native-0.1.8.4-beta.apk` (42.150.063 bytes) | `a16fda5cdbb83eabe2d366951e9c30fde4cb9e139a11f5f4cb205c6902351200` |
| `output/build/swap2-native-0.1.8.4-beta-source.zip` (154.922.475 bytes, 3.862 entradas) | `ffd1b7d183fd9550783d3203b7accb24cfa1365dc831f06ca44feb6ace4cc970` |

- **APK:** `com.swap2native`, versionCode 15, versionName 0.1.8.4-beta. Certificado `9d6f1e98…c0f1`, o de sempre, conferido com `apksigner verify`. O dex traz as rodadas `alias-fd` e `alias-ahb` e a marca de profundidade; os recursos trazem o texto novo em português.
- **Fontes:** `output/build/package-release-0184-source.py`. Em relação ao zip da 0.1.8.3: seis arquivos mudam (`AutoDiag.java`, os dois `strings.xml`, a nota do jogo, `TESTING.md` e o `RELEASE-SOURCE.txt`) e entra a nota do build da 0.1.8.3. Nenhum arquivo do host muda. O scan (`scan-release-zip.py`) só aponta os dois `.nso` sintéticos do corpus de fuzz do upstream.
- **Script de build:** `output/build/build-public-0184.ps1` (o usuário digita a senha).
- **Notas:** `output/build/release-notes-0.1.8.4-beta.md`.

## O que falta

- O dono do Redmagic 11 Air rodar o diagnóstico desta versão. Se `alias-fd` ou `alias-ahb` passarem da cena com o driver dele, o passo seguinte é o app escolher esse caminho sozinho naquele aparelho.
- Enquanto isso, o driver **System** é o ajuste que passou da carga da cena num relatório.
