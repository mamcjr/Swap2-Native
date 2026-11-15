# Build 0.1.8.3-beta (14/11/2026)

**Situação:** compilado, assinado e publicado como pre-release em 14/11 (https://github.com/mamcjr/Swap2-Native/releases/tag/v0.1.8.3-beta, tag `v0.1.8.3-beta` em `main`); os hashes dos três arquivos no GitHub batem com os locais. Não instalado.

Corrige o diagnóstico automático da 0.1.8.2 depois dos dois primeiros relatórios do Redmagic 11 Air. Nada muda no que o jogo desenha ou em como roda.

## O que os relatórios da 0.1.8.2 mostraram

- **1º relatório (save sem jogo salvo):** a rodada única acabou "viva" aos 150 s, ainda no filme de abertura de 154 s. O diagnóstico parava cedo demais. O censo de memória desse relatório descarta falta de RAM: 15 GB no total, 10 de 12 GB de swap livres, 7 GB de cache, e só 2,5 GB de memória anônima no jogo.
- **2º relatório (com um save que tem jogo):** seis rodadas, todas mortas entre 46 e 52 s com sinal 9, com qualquer opção. Em todas, o primeiro envio da cena (3.813 draws) entra na fila da GPU e o log acaba 0,1 a 0,3 s depois; no POCO esse envio leva 0,8 s para terminar. Suspeita, não provada: o lado da GPU daquele aparelho com o driver 891.14.
- **No POCO F7 Ultra**, um jogo novo com save vazio passa pelo filme e roda a primeira cena a 24 fps: não reproduz.
- **3º relatório (o dono trocou para o driver 891.11):** as mesmas seis rodadas, todas mortas, mas três passaram do primeiro envio (56, 83 e 102 s) e morreram depois, sempre logo após o jogo mapear mais memória (`alias region mapped up to +4.50` a `+5.00 GB`), com 2,8 a 3,4 GB disponíveis. O que difere do POCO nos censos: o kernel dele dá de 68 a 288 MiB de huge pages transparentes ao processo do jogo, e o do POCO dá 0. Hipótese, não provada: o host mexe na memória do jogo página a página, o que quebra huge pages, e o kernel daquele aparelho reage mal a isso.

## O que muda em relação à 0.1.8.2-beta

| Mudança | Arquivos | Testado onde |
|---|---|---|
| Rodadas por driver de GPU: o do sistema e até três instalados, cada um escolhido como a configuração do jogo escolheria (o driver é carregado na checagem de compatibilidade, antes do ambiente da rodada) | `AutoDiag.java`, `GpuDriver.java` (`runChoice`), `GameActivity.java` | POCO, app de teste: sistema (Qualcomm 208ca19915, sem placed maps, alias exportado), Turnip V39 e V39-patched carregados de fato |
| Rodada sem huge pages transparentes (`DM_THP=0`: `prctl(PR_SET_THP_DISABLE)` antes de a memória do jogo ser mapeada), a segunda do diagnóstico | `nxlaunch.c`, `AutoDiag.java`, `docs/env_switches.tsv` | **Não rodou em nenhum celular** (o POCO estava bloqueado); compila |
| O censo registra os limites de memória do cgroup do processo, quando o app pode lê-los | `dm/gfx/vk_android/vka_ctl.c` | O analisador da linha rodou fora do celular com as linhas dos dois aparelhos e entradas malformadas (`scratchpad/t_cgroup.sh`); **não rodou em celular** |
| Rodada sem desenhar nada (`DM_VK_DRAW=0`) | `AutoDiag.java` | POCO: a linha `draws and dispatches are off` no log da rodada |
| A primeira rodada dura até 420 s; as outras terminam 60 s depois do ponto em que a primeira morreu; o A é apertado até o fim | `AutoDiag.java` | POCO: jogo novo com save vazio chega à primeira cena aos ~3,5 min |
| A cópia do save é apagada inteira no fim e antes de cada diagnóstico (antes ficava, e o diagnóstico seguinte reaproveitava a cópia velha) | `AutoDiag.java` | POCO: sem `diag-save` depois do relatório |
| O relatório traz a versão do sistema, o kernel, a revisão da GPU, o driver do jogo e os instalados | `AutoDiag.java` | POCO: `kernel 6.6.118-android15-8-...`, `GPU model Adreno830v2` |
| `DM_VK_UNPLACE_MREMAP=1` entra na lista "como na 0.1.5"; sai a rodada "sem mesh" (`DM_VK_MESH=0` não desliga o fallback por compute) | `AutoDiag.java` | POCO: 13 rodadas no teste final (`output/zelda-1112/diagD-report.log`) |
| Uma rodada de diagnóstico não mostra a pergunta da checagem de compatibilidade: um driver ao qual falta algo é resultado da rodada | `GameActivity.java` | POCO (o driver do sistema passa na checagem lá; o caminho com falha não foi exercitado) |

## Artefatos

| Arquivo | SHA-256 |
|---|---|
| `android/app/out/swap2-native-0.1.8.3-beta.apk` (42.149.231 bytes) | `8bc8ab32022516852878124dda212cb07ad3a6206fc6a6e97f3959d90a878969` |
| `output/build/swap2-native-0.1.8.3-beta-source.zip` (154.918.117 bytes, 3.861 entradas) | `1654df49f88449bf6530f08d6abd04687227eb390ba2eb014e6fad096a36471e` |

- **APK:** `com.swap2native`, versionCode 14, versionName 0.1.8.3-beta. Certificado `9d6f1e98…c0f1`, o de sempre, conferido com `apksigner verify`. O dex traz as rodadas novas (`no-huge-pages`, `system-driver`, `no-draws`), o `libnxlaunch.so` a linha do `DM_THP` e o `libnxhost.so` a linha `memory census cgroup`.
- **Fontes:** `output/build/package-release-0183-source.py`. Em relação ao zip da 0.1.8.2: nove arquivos mudam (o código desta versão, a nota do jogo, `TESTING.md`, `env_switches.tsv` e o `RELEASE-SOURCE.txt`) e entra a nota do build da 0.1.8.2. O scan (`scan-release-zip.py`) só aponta os dois `.nso` sintéticos do corpus de fuzz do upstream.
- **Script de build:** `output/build/build-public-0183.ps1` (o usuário digita a senha; a primeira tentativa falhou por senha recusada, sem gerar APK).
- **Notas:** `output/build/release-notes-0.1.8.3-beta.md`.

## Testes

- POCO F7 Ultra (Adreno 830, 891.14), app de teste, save de teste: os itens da tabela, com o processo do jogo morto por mim (`kill -9`) em várias rodadas para imitar o Redmagic.
- `test_docs`: só os dois links quebrados antigos.
- Não testado no Redmagic 11 Air, em Mali nem em Xclipse.
- `regress.py`: não rodou (é do Mac; a mudança é do launcher Android).

## O que falta

- O dono do Redmagic 11 Air rodar o diagnóstico desta versão com um save que tenha jogo e mandar o relatório: ele diz se outro driver de GPU passa da cena e se o jogo vive sem desenhar.
- Saber qual driver de GPU ele usava quando a 0.1.5 rodava.
