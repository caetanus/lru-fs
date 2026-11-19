# lru-fs

Scratch para IA (e qualquer outra coisa) que não come toda a RAM nem todo o disco.

Um sistema de arquivos FUSE com duas pastas comuns por baixo:

| nível | pasta padrão        | o que guarda                                   |
|-------|---------------------|------------------------------------------------|
| ram   | `/tmp/lru-fs`       | arquivos usados recentemente, até `ram_limit` (2G) |
| disco | `~/.cache/lru-fs`   | todo o resto                                   |

Não há formato próprio: cada arquivo regular fica inteiro em **uma** das duas
pastas, no mesmo caminho relativo. Com o daemon parado, dá para ler tudo direto
(`cat ~/.cache/lru-fs/foo`).

## Regras

- **Arquivo novo** nasce na RAM.
- **RAM cheia** (`ram_limit`) → o arquivo usado há mais tempo (LRU) é copiado
  para o disco, até caber. Arquivos abertos também podem ser movidos: o driver
  reabre os descritores na cópia nova, e o programa nem percebe.
- **Arquivo maior que `max_file`** vai para o disco assim que passa desse
  tamanho, para um log gigante não expulsar todo o resto.
- **Voltar para a RAM**: um arquivo do disco volta quando é aberto para escrita,
  ou quando é usado de novo dentro de `promote_window` desde o último uso. Uma
  varredura única (`grep -r`) não embaralha a RAM.
- **Wipe**: o que não foi usado (aberto, lido, escrito ou recebeu `touch`) há
  mais de `wipe` é apagado. Diretórios vazios e sem mudanças nesse prazo também.
  Arquivos abertos nunca são apagados.
- **Teto de armazenamento**: passando de `max_size` (RAM + disco), ou se o
  disco ficar com menos de `min_free` livre, os arquivos usados há mais tempo
  são apagados antes do prazo, até sobrar 5% de folga. Se nem assim couber
  (tudo que sobrou está aberto), a escrita recebe `ENOSPC` — "disco cheio".
  O `df` mostra `max_size` como tamanho do sistema de arquivos.
- **Ao desmontar**, o que está na RAM é copiado para o disco (`noflush` desliga
  isso), então reiniciar a máquina não perde nada que ainda estava no prazo.

## Opções

Passadas com `-o nome=valor,...` (linha de comando, unit do systemd ou fstab).

| opção            | padrão             | o que faz |
|------------------|--------------------|-----------|
| `wipe`           | `5d`               | apaga o que ficou esse tempo sem uso; `never` desliga (`ttl` é sinônimo) |
| `max_size`       | ilimitado          | teto de RAM + disco; acima dele, apaga os menos usados |
| `min_free`       | `0`                | espaço livre mínimo no disco que guarda `disk_dir` |
| `ram_limit`      | `2G`               | quanto da RAM usar; `0` = só disco |
| `max_file`       | `256M`             | arquivos maiores ficam no disco |
| `ram_dir`        | `/tmp/lru-fs`      | pasta do nível RAM (precisa ser tmpfs para fazer sentido) |
| `disk_dir`       | `~/.cache/lru-fs`  | pasta do nível disco |
| `promote_window` | `10m`              | arquivo do disco usado de novo dentro disso volta para a RAM |
| `scan_interval`  | `10m`              | de quanto em quanto tempo procura o que expirou |
| `noflush`        |                    | não copia a RAM para o disco ao desmontar |
| `verbose`        |                    | registra migrações, expirações e cortes no stderr |

Tamanhos: `512`, `64K`, `2G`, `1.5T`, `off`. Durações: `90s`, `30m`, `12h`,
`5d`, `2w`, `never`.

`max_size`, `min_free` e `ram_limit` aceitam também porcentagem (`10%`). A base
nunca é o tamanho bruto do disco ou da RAM: é o espaço que o scratch de fato
tem — o que está livre agora mais o que o próprio scratch já ocupa (para
`ram_limit`, o `MemAvailable` mais o que está no tmpfs). É recalculada a cada
poucos segundos, então se outro programa encher o disco, os limites encolhem
junto. Como escrever no scratch só move bytes de "livre" para "ocupado", a base
não muda por causa do próprio scratch.

## Montar

**Na mão:**

```sh
lru-fs ~/scratch                                  # padrões
lru-fs -o wipe=2d,max_size=40G,min_free=10% ~/scratch
fusermount3 -u ~/scratch                          # desmontar
```

**Como serviço do usuário (recomendado):**

```sh
make install                     # ~/.local/bin/lru-fs + unit do systemd
echo 'LRUFS_ARGS=-o wipe=3d,max_size=50G,min_free=10%' > ~/.config/lru-fs.conf
systemctl --user enable --now lru-fs
systemctl --user restart lru-fs  # depois de mudar as opções
```

Monta em `~/scratch`. Logs: acrescente `verbose` às opções e veja
`journalctl --user -u lru-fs -f`.

**Pelo fstab** (o `mount.fuse3` chama `lru-fs`, então ele precisa estar no
`PATH` do root: `sudo make install-bin PREFIX=/usr/local`):

```
# montado no boot, rodando como seu usuário
lru-fs  /home/USER/scratch  fuse.lru-fs  setuid=USER,disk_dir=/home/USER/.cache/lru-fs,wipe=5d,max_size=50G,nofail  0 0
```

Use caminhos absolutos no fstab (`~` e `$HOME` não valem ali). Com `user,noauto`
no lugar de `setuid=...`, você monta quando quiser com `mount ~/scratch`.
A forma `mount.fuse3 lru-fs <dir> -o ...` está coberta pelos testes; a linha do
fstab montada como root no boot não foi testada aqui.

Para mandar as ferramentas para lá: `export TMPDIR=~/scratch/tmp` (crie o
diretório), ou aponte o scratchpad/cache da ferramenta para `~/scratch/...`.

## Limitações

- **Hard links** são recusados (`EPERM`). Os arquivos mudam de nível pelo
  caminho, e um inode com dois nomes não sobrevive a isso. `git clone` local,
  `cp -al` e afins caem para cópia sozinhos.
- `rename` com `RENAME_EXCHANGE` não é suportado (`EINVAL`). `RENAME_NOREPLACE` é.
- Sem xattrs.
- Arquivo apagado enquanto aberto some do diretório na hora (o `rmdir` do pai
  funciona) e continua legível/gravável pelo fd até fechar. Só o `fstat()`
  nesse fd falha (`ESTALE`) se o **diretório** dele também tiver sido removido.
- Não escreva direto em `/tmp/lru-fs` ou `~/.cache/lru-fs` com o daemon rodando.
- O teto de RAM vale para o tmpfs. O cache de páginas do kernel pode guardar
  uma segunda cópia do que é lido pelo mount, mas essa memória o kernel libera
  sob pressão; a do tmpfs ele não libera.
- Operações de metadados ficam ~3× mais lentas que no disco nativo
  (é FUSE); leitura e escrita sequenciais ficam perto da velocidade nativa.

## Testes

```sh
make && python3 tests/test.py
```

Monta instâncias temporárias com limites pequenos e verifica despejo, promoção,
migração com arquivo aberto, rename de diretório entre níveis, unlink com
arquivo aberto, 3000 operações aleatórias comparadas com uma pasta comum,
8 escritores concorrentes, persistência ao remontar, expiração, `max_size`,
`min_free`, `wipe=never` e montagem via `mount.fuse3`.
