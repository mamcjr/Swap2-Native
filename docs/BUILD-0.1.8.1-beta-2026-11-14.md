# Build 0.1.8.1-beta (14/11/2026)

**Situação:** compilado, assinado e publicado como pre-release em 14/11 (https://github.com/mamcjr/Swap2-Native/releases/tag/v0.1.8.1-beta, tag `v0.1.8.1-beta` em `main`); os hashes dos três arquivos no GitHub batem com os locais. Não instalado.

Versão só de diagnóstico: nada muda no que o jogo desenha ou em como roda. Ela faz os dois problemas abertos dos logs da 0.1.8 (Redmagic 11 Air e POCO X7 Pro) dizerem mais no log.

## O que muda em relação à 0.1.8-beta

| Mudança | Arquivos | Testado onde |
|---|---|---|
| Rastro dos quatro primeiros draws do fallback de mesh por compute: uma linha quando o draw é gravado (`task/mesh fallback draw N recorded`, com a fence do envio) e outra quando a GPU termina esse envio (`... its submission completed on the GPU`); liga também oito linhas `sync sub` em volta | `dm/gfx/vk_android/vka_mesh.inc` | POCO F7 Ultra, duas rodadas até Kokiri: as oito linhas aparecem, 30 fps |
| O relatório de crash lista os últimos 24 arquivos diferentes que o jogo abriu, com a posição e o tamanho de cada um dentro do pacote e quantas vezes foi aberto | `dm/host/hle_fs.c`, `dm/host/crash.c` | POCO, com um SIGSEGV mandado ao processo do jogo; a lógica do anel também fora do celular (`scratchpad/t_recent.sh`) |

- Na primeira versão da lista (16 aberturas), um som que o jogo reabre a cada segundo ocupava 10 das 16 linhas. Por isso ela passou a guardar arquivos diferentes, e um arquivo novo toma o lugar do que foi aberto há mais tempo.
- Para que servem:
  - **Redmagic 11 Air:** a última linha do rastro diz se o processo é morto ao gravar o draw, na fila ou com o trabalho já na GPU.
  - **POCO X7 Pro:** a lista dá o nome do arquivo que o jogo acabara de ler antes do crash em `main+0x773270`, para comparar com um dump bom.

## Artefatos

| Arquivo | SHA-256 |
|---|---|
| `android/app/out/swap2-native-0.1.8.1-beta.apk` (42.126.279 bytes) | `0eed688e94902458cd996bd5869f05ad6c690eabe61ce75c70478a4a260c107b` |
| `output/build/swap2-native-0.1.8.1-beta-source.zip` (154.892.504 bytes, 3.858 entradas) | `4891a5abf25d0b7283b5b60181392725b4448a4df2f62148054b748795abd222` |

- **APK:** `com.swap2native`, versionCode 12, versionName 0.1.8.1-beta. Certificado `9d6f1e98…c0f1`, o de sempre, conferido com `apksigner verify`. O `libnxhost.so` do APK traz as linhas de log dos dois diagnósticos.
- **Fontes:** `output/build/package-release-0181-source.py`. Em relação ao zip da 0.1.8: sete arquivos mudam (os três do código, a nota do jogo, `shaders.md`, o plano e o `RELEASE-SOURCE.txt`) e entra a nota do build da 0.1.8. O zip é 2 MB menor porque foi comprimido por outro Python (3.14), não por faltar arquivo. O scan (`scan-release-zip.py`) só aponta os dois `.nso` sintéticos do corpus de fuzz do upstream.
- **Script de build:** `output/build/build-public-0181.ps1` (o usuário digita a senha).
- **Notas:** `output/build/release-notes-0.1.8.1-beta.md`.

## Testes

- POCO F7 Ultra (Adreno 830, 891.14), app de teste, save de teste: Kokiri a 30 fps nas duas rodadas, com o rastro completo e a lista no relatório.
- Não testado em Mali, Xclipse nem nos dois aparelhos que têm os problemas.
- `test_docs`: só os dois links quebrados antigos.
- `regress.py`: não rodou (é do Mac; as mudanças do host só escrevem no log e no relatório de crash).

## O que falta

- Um log do Redmagic 11 Air e um do POCO X7 Pro com esta versão.
