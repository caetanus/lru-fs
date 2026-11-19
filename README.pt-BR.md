<p align="center">
  <img src="assets/icon.svg" width="160" alt="ícone do lru-fs: um pente de RAM sobre um disco, setas entre eles e um relógio">
</p>

<h1 align="center">lru-fs</h1>

<p align="center">
  <b>Scratch para agentes de IA (e para builds, e para você):</b><br>
  arquivos usados recentemente na RAM, o resto no disco, o esquecido é apagado.
</p>

<p align="center">
  <a href="README.md">English</a> ·
  <a href="https://github.com/caetanus/lru-fs/releases">Releases</a> ·
  <a href="#instalar">Instalar</a>
</p>

---

## O problema

Minha máquina de vez em quando travava, ou ficava sem disco, e o culpado não
era nenhum programa pesado: era o scratchpad dos agentes de IA.

Agentes de IA que programam geram muito arquivo temporário: clones de teste,
builds descartáveis, downloads, saídas intermediárias, scripts de sondagem.
Eles guardam isso em um de dois lugares, e os dois dão errado.

**`/tmp` é RAM.** Na maioria das distros atuais o `/tmp` é um tmpfs: cada byte
gravado ali ocupa memória e, sem swap, o kernel não devolve essa memória até
alguém apagar o arquivo. Agentes quase nunca limpam. Deixe uma dúzia de
sessões abertas por alguns dias e gigabytes de clones esquecidos e arquivos
`cc*.s` ficam na RAM ao lado dos próprios agentes, até a memória acabar e a
máquina travar, ou o OOM killer começar a matar processos.

**`~/.cache` é disco, para sempre.** Mandar os agentes para uma pasta no disco
resolve a RAM e cria o problema oposto: nada expira, e o lixo se acumula até
encher o disco. E o disco é mais lento para o vaivém de arquivos pequenos que
builds e testes fazem.

Pedir a cada agente que lembre de limpar, e que escolha o lugar certo toda
vez, não funciona. Faltava um lugar que fizesse a coisa certa sozinho:

- o que está em uso **agora** fica na **RAM**, rápido, mas com um teto rígido;
- o resto vai para o **disco** automaticamente;
- o que ninguém toca há **5 dias some** automaticamente;
- se o scratch passar de um **teto**, ou o disco ficar apertado, os arquivos
  usados há mais tempo saem primeiro.

Isso é o lru-fs. Monte em `~/scratch`, mande seus agentes escreverem lá, e
RAM, disco e limpeza se resolvem sozinhos.

## Como funciona

O lru-fs é um sistema de arquivos FUSE sobre duas pastas comuns:

| nível | padrão              | guarda |
|-------|---------------------|--------|
| RAM   | `/tmp/lru-fs`       | arquivos usados recentemente, até `ram_limit` |
| disco | `~/.cache/lru-fs`   | todo o resto |

Não há formato próprio. Cada arquivo regular fica inteiro em **uma** das duas
pastas, no mesmo caminho relativo. Com o daemon parado, dá para ler tudo
direto.

- **Arquivo novo** nasce na RAM.
- **RAM cheia** → o arquivo usado há mais tempo é copiado para o disco até a
  escrita nova caber. Arquivos abertos também podem ser movidos: o lru-fs
  reabre os descritores na cópia nova, e o programa nem percebe.
- **Arquivo maior que `max_file`** vai para o disco assim que passa desse
  tamanho, para um log gigante não expulsar todo o resto.
- **Volta para a RAM**: um arquivo do disco volta quando é aberto para
  escrita, ou quando é usado de novo dentro de `promote_window`. Uma varredura
  única (`grep -r`) não embaralha a RAM.
- **Wipe**: o que não foi aberto, lido, escrito ou recebeu `touch` há mais de
  `wipe` (5 dias) é apagado, assim como diretórios vazios. Arquivos abertos
  nunca são apagados.
- **Teto**: acima de `max_size` (RAM + disco), ou abaixo de `min_free` de
  espaço livre no disco, os arquivos usados há mais tempo são apagados antes
  do prazo, até 5% abaixo do limite. Se nada puder ser liberado (tudo o que
  sobrou está aberto), a escrita recebe `ENOSPC`. O `df` mostra `max_size`
  como tamanho do sistema de arquivos.
- **Ao desmontar** (ou desligar), a RAM é copiada para o disco, então um
  reboot não perde nada que ainda estava dentro do prazo do `wipe`.

<a id="instalar"></a>
## Instalar

Os pacotes instalam o binário e uma unit de **usuário** do systemd que monta
`~/scratch`. Cada usuário liga para si.

**Arch Linux / Manjaro (AUR)**

```sh
yay -S lru-fs        # ou: paru -S lru-fs
```

**Debian / Ubuntu** (Ubuntu 22.04+, Debian 12): baixe o `.deb` em
[Releases](https://github.com/caetanus/lru-fs/releases) e rode

```sh
sudo apt install ./lru-fs_0.1.0_amd64.deb
```

**Fedora**: baixe o `.rpm` em
[Releases](https://github.com/caetanus/lru-fs/releases) e rode

```sh
sudo dnf install ./lru-fs-0.1.0-1.fc*.x86_64.rpm
```

**Do código-fonte**: precisa de compilador C, `make`, `pkg-config` e libfuse 3
(`fuse3` no Arch, `libfuse3-dev` + `fuse3` no Debian/Ubuntu, `fuse3-devel` +
`fuse3` no Fedora).

```sh
make
sudo make install        # /usr/local/bin + /usr/local/lib/systemd/user
# ou, sem root:
make install-user        # ~/.local/bin + ~/.config/systemd/user
```

As receitas dos pacotes ficam em [`packaging/aur`](packaging/aur),
[`debian/`](debian) e [`packaging/rpm`](packaging/rpm).

### Ligar

```sh
systemctl --user enable --now lru-fs      # monta ~/scratch
df -h ~/scratch
```

As opções ficam em `~/.config/lru-fs.conf`, lido pela unit:

```sh
echo 'LRUFS_ARGS=-o wipe=5d,max_size=50G,min_free=10%,ram_limit=10%' > ~/.config/lru-fs.conf
systemctl --user restart lru-fs
```

### Apontar as ferramentas para lá

```sh
mkdir -p ~/scratch/meuprojeto/tmp
export TMPDIR=~/scratch/meuprojeto/tmp    # para o que respeita TMPDIR
```

Para agentes de IA, coloque algo assim no `CLAUDE.md` / `AGENTS.md`:

```markdown
## Scratch
Arquivos temporários (builds descartáveis, clones de teste, downloads
temporários, saídas intermediárias) vão em ~/scratch/<projeto>/, não em /tmp
nem em ~/.cache. Use TMPDIR=~/scratch/<projeto>/tmp nas ferramentas que o
respeitam. É descartável: o que fica 5 dias sem uso é apagado, e os menos
usados saem primeiro quando enche. Nada que precise durar vai para lá.
Se ~/scratch não estiver montado: systemctl --user start lru-fs
```

## Opções

Passadas com `-o nome=valor,...` (linha de comando, `~/.config/lru-fs.conf` ou fstab).

| opção            | padrão             | o que faz |
|------------------|--------------------|-----------|
| `wipe`           | `5d`               | apaga o que ficou esse tempo sem uso; `never` desliga (`ttl` é sinônimo) |
| `max_size`       | ilimitado          | teto de RAM + disco; acima dele, os menos usados saem primeiro |
| `min_free`       | `0`                | espaço livre a manter no disco que guarda `disk_dir` |
| `ram_limit`      | `2G`               | quanto de RAM usar; `0` = só disco |
| `max_file`       | `256M`             | arquivos maiores ficam no disco |
| `ram_dir`        | `/tmp/lru-fs`      | pasta do nível RAM (deve ser um tmpfs) |
| `disk_dir`       | `~/.cache/lru-fs`  | pasta do nível disco |
| `promote_window` | `10m`              | arquivo do disco usado de novo dentro disso volta para a RAM |
| `scan_interval`  | `10m`              | de quanto em quanto tempo procura o que expirou |
| `noflush`        |                    | não copia a RAM para o disco ao desmontar |
| `verbose`        |                    | registra migrações, expirações e cortes no stderr (`journalctl --user -u lru-fs`) |

Tamanhos: `512`, `64K`, `2G`, `1.5T`, `off`. Durações: `90s`, `30m`, `12h`,
`5d`, `2w`, `never`.

`max_size`, `min_free` e `ram_limit` aceitam também porcentagem (`10%`). A base
nunca é o tamanho bruto do disco ou da RAM: é o espaço que o scratch de fato
tem, o que está livre agora mais o que o próprio scratch já ocupa (para
`ram_limit`, o `MemAvailable` mais o conteúdo do tmpfs). É recalculada a cada
poucos segundos: quando outro programa enche o disco ou a RAM, os limites
encolhem junto e os arquivos da RAM descem para o disco na hora.

## Montar na mão, ou pelo fstab

```sh
lru-fs ~/scratch
lru-fs -o wipe=2d,max_size=40G,min_free=10% ~/scratch
fusermount3 -u ~/scratch
```

O `mount.fuse3` é suportado, então o fstab também funciona (o `lru-fs`
precisa estar no `PATH` do root):

```
lru-fs  /home/USUARIO/scratch  fuse.lru-fs  setuid=USUARIO,disk_dir=/home/USUARIO/.cache/lru-fs,wipe=5d,max_size=50G,nofail  0 0
```

Use caminhos absolutos ali; `~` e `$HOME` não valem no fstab.

## Limitações

- **Hard links** são recusados (`EPERM`): os arquivos mudam de nível pelo
  caminho, e um inode com dois nomes não sobrevive a isso. `cp -al`,
  `git clone` local e afins caem para cópia sozinhos.
- `rename` com `RENAME_EXCHANGE` não é suportado (`EINVAL`); `RENAME_NOREPLACE` é.
- Sem atributos estendidos (xattrs).
- Arquivo apagado enquanto aberto some do diretório na hora (o `rmdir` do pai
  funciona) e continua legível e gravável pelo descritor até fechar. Só o
  `fstat()` nesse descritor falha (`ESTALE`) se o **diretório** dele também
  tiver sido removido.
- Não escreva direto em `/tmp/lru-fs` ou `~/.cache/lru-fs` com o daemon rodando.
- O teto de RAM vale para o tmpfs. O cache de páginas do kernel pode guardar
  uma segunda cópia do que é lido pelo mount, mas essa cópia o kernel libera;
  a do tmpfs ele não libera (sem swap).
- Operações de metadados ficam ~3× mais lentas que no disco nativo (é FUSE);
  leitura e escrita sequenciais ficam perto da velocidade nativa.

## Testes

```sh
make check     # = make && python3 tests/test.py
```

Monta instâncias descartáveis com limites pequenos e verifica despejo,
promoção, migração de arquivos abertos, rename de diretório entre níveis,
unlink de arquivo aberto, 3000 operações aleatórias comparadas com uma pasta
comum, 8 escritores concorrentes, flush ao desmontar (mesmo sob SIGTERM),
expiração, `max_size`, `min_free`, limites em porcentagem com pressão de
memória simulada, `wipe=never` e montagem via `mount.fuse3`.

## Licença

[MIT](LICENSE)
