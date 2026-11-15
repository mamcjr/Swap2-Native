# Build 0.1.8.6-beta (14/11/2026)

**Situação:** compilado, assinado e publicado como pre-release em 14/11 (https://github.com/mamcjr/Swap2-Native/releases/tag/v0.1.8.6-beta, tag `v0.1.8.6-beta` em `main`); os hashes dos três arquivos no GitHub batem com os locais. Instalado no POCO F7 Ultra do usuário a pedido dele, por cima da 0.1.7.1-beta (packs e saves mantidos; o app não foi aberto por mim). **Não commitado.**

A metade que faltava da correção de mapas de memória do Banana Native v0.3.24 (pedido do usuário em 14/11: "Sim, portar", depois "Sim, abrir o build").

## O que muda em relação à 0.1.8.5-beta

| Mudança | Arquivos | Testado onde |
|---|---|---|
| Toda faixa que o host devolve dentro do bloco do guest é remapeada com as flags da reserva (`MAP_PRIVATE \| MAP_ANONYMOUS \| MAP_NORESERVE`), então volta a se fundir com as vizinhas em vez de ficar como um mapa próprio até o fim do processo | `dm/host/plat/plat_guestmap.h` (novo, do upstream), `dm/host/hle_svc.c` (`guest_release`); a macro também em `plat_android.c` e `vka_mem.c`, sem mudar o comportamento ali | `t_android_guestmap` no WSL (Linux 6.6.114): 2 mapas no fim com as flags novas, 193 com as antigas. POCO F7 Ultra, 891.14, Zelda até Kokiri no mesmo ponto da carga: 20.246 mapas do bloco do guest antes (39.422 no total), 14.069 no alias (33.281) e 13.928 no mapeamento direto (33.014), 31,5 fps nos dois |
| `maps_red` devolve o `errno` como a chamada que falhou deixou (a contagem de mapas lê `/proc` no Android) | `dm/host/hle_svc.c` | `t_heap_size` compilado para Android arm64 com o NDK e rodado no POCO pelo `adb shell`: passa; compilado contra o `hle_svc.c` da 0.1.8.5, para no assert das flags |
| Teste novo e alvo `guestmap`; o assert do `t_heap_size` com as flags novas | `dm/host/tests/t_android_guestmap.c` (do upstream), `t_heap_size.c`, `Makefile` | os dois acima |

- **O que isto não é:** não é a causa da morte do Redmagic 11 Air no mapeamento direto (lá o processo morre com 19 a 35 mil mapas, longe do limite de 65.530); essa causa continua aberta. Também não foi testado em nenhum aparelho que tenha chegado ao limite de mapas.
- **A contagem ainda sobe** com os pools que o jogo carrega; sai só a parte que `guest_release` deixava para trás (cerca de 6.000 mapas em Kokiri).
- **O upstream deixou como estavam** três outros pontos que criam mapas anônimos fixos sem a flag (`vka_fileback.h`, `vka_replay.c`, `hle_nvn.c`); aqui também.
- **Interface:** nada muda.

## Artefatos

| Arquivo | SHA-256 |
|---|---|
| `android/app/out/swap2-native-0.1.8.6-beta.apk` (42.168.603 bytes) | `1e5b065cd825f2362cc8e329334d3559aa60d956b9910c7f7c273d7b0e55d493` |
| `output/build/swap2-native-0.1.8.6-beta-source.zip` (154.942.940 bytes, 3.868 entradas) | `12d73c9309b0e62c4eab1caf48d7fb0157b169eaa3649c3d20eeb6f3ffac60db` |

- **APK:** `com.swap2native`, versionCode 17, versionName 0.1.8.6-beta. Certificado `9d6f1e98…c0f1`, o de sempre, conferido com `apksigner verify`. Em relação ao APK da 0.1.8.5 mudam só o manifesto, o dex (a versão), o `libnxhost.so` e o pacote de ferramentas embutido (nele, só a nota do jogo). O `libnxhost.so` traz o `mmap` com as flags `0x4032` (`MAP_FIXED` + as da reserva), que o da 0.1.8.5 não tem.
- **Fontes:** `output/build/package-release-0186-source.py`. Em relação ao zip da 0.1.8.5: 10 arquivos mudam (o código acima e a documentação) e entram `plat_guestmap.h`, `t_android_guestmap.c` e a nota do build da 0.1.8.5. O scan (`scan-release-zip.py`) só aponta os dois `.nso` sintéticos do corpus de fuzz do upstream.
- **Script de build:** `output/build/build-public-0186.ps1` (o usuário digita a senha).
- **Notas:** `output/build/release-notes-0.1.8.6-beta.md`.

## Testes

- POCO F7 Ultra, app de teste, save de teste: as contagens da tabela (`output/zelda-1112/prefer1.log` antes; `gm-alias.log`, `gm-placed.log` depois) e mais uma partida com o APK de teste da árvore final (`gm-final.log`: 14.092 mapas do bloco do guest, 33.324 no total, 31,6 fps).
- `t_android_guestmap` no WSL; `t_heap_size` no POCO (arm64). `test_docs`: só os dois links quebrados antigos.
- **Não rodou no Mac:** o `t_heap_size` pelo Makefile e o `regress.py` (a nota para uma sessão no Mac está em `output/mac/NOTA-MAC-t_heap_size-2026-11-14.md`).
- Não testado no Redmagic 11 Air, em Mali nem em Xclipse.
