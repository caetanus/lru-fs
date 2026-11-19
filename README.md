<p align="center">
  <img src="assets/icon.svg" width="160" alt="lru-fs icon: a RAM module above a disk, arrows between them and a clock">
</p>

<h1 align="center">lru-fs</h1>

<p align="center">
  <b>Scratch space for AI agents (and builds, and you):</b><br>
  recently used files in RAM, the rest on disk, untouched files wiped.
</p>

<p align="center">
  <a href="README.pt-BR.md">Português</a> ·
  <a href="https://github.com/caetanus/lru-fs/releases">Releases</a> ·
  <a href="#install">Install</a>
</p>

---

## The problem

AI coding agents produce a lot of scratch: test clones, throwaway builds,
downloads, intermediate outputs, probe scripts. They put it in one of two
places, and both go wrong.

**`/tmp` is RAM.** On most modern distros `/tmp` is a tmpfs: every byte written
there takes memory, and without swap the kernel can't give it back until
someone deletes the file. Agents rarely clean up. Leave a dozen sessions open
for a few days and gigabytes of forgotten clones and `cc*.s` files sit in RAM
next to the agents themselves, until memory runs out and the OOM killer
starts picking processes.

**`~/.cache` is disk, forever.** Pointing agents at a disk directory fixes RAM
and creates the opposite problem: nothing ever expires, so the junk piles up
until the disk is full. And disk is slower for the churn of small files that
builds and tests produce.

Asking every agent to remember to clean up, and to pick the right place each
time, doesn't work. What was missing is a place that does the right thing on
its own:

- what is being used **now** stays in **RAM**, fast, but under a hard budget;
- everything else moves to **disk** automatically;
- what nobody touched for **5 days disappears** automatically;
- if the scratch grows past a **cap**, or the disk runs low, the least
  recently used files go first.

That's lru-fs. Mount it on `~/scratch`, tell your agents to write there, and
RAM, disk and cleanup take care of themselves.

## How it works

lru-fs is a FUSE filesystem over two ordinary directories:

| tier | default             | holds |
|------|---------------------|-------|
| RAM  | `/tmp/lru-fs`       | recently used files, up to `ram_limit` |
| disk | `~/.cache/lru-fs`   | everything else |

There is no special on-disk format. Each regular file lives whole in **one**
of the two directories, under the same relative path. With the daemon
stopped, you can still read everything directly.

- **New files** are created in RAM.
- **RAM full** → the least recently used file is copied to disk until the new
  data fits. Open files can move too: lru-fs reopens their descriptors on the
  new copy and the program never notices.
- **Files bigger than `max_file`** go to disk as soon as they cross it, so one
  huge log doesn't push everything else out.
- **Back to RAM**: a disk file returns when opened for writing, or when used
  again within `promote_window`. A one-off scan (`grep -r`) doesn't churn the
  RAM tier.
- **Wipe**: files not opened, read, written or `touch`ed for `wipe` (5 days)
  are deleted, and so are empty directories. Open files are never deleted.
- **Cap**: above `max_size` (RAM + disk), or below `min_free` free disk space,
  the least recently used files are deleted early, down to 5% under the
  limit. If nothing can be freed (everything left is open), writes get
  `ENOSPC`. `df` shows `max_size` as the filesystem size.
- **On unmount** (or shutdown) the RAM tier is copied to disk, so a reboot
  loses nothing that was still within its `wipe` window.

<a id="install"></a>
## Install

Packages install the binary and a systemd **user** unit that mounts
`~/scratch`. Each user turns it on for themselves.

**Arch Linux / Manjaro (AUR)**

```sh
yay -S lru-fs        # or: paru -S lru-fs
```

**Debian / Ubuntu** (Ubuntu 22.04+, Debian 12): download the `.deb` from
[Releases](https://github.com/caetanus/lru-fs/releases), then

```sh
sudo apt install ./lru-fs_0.1.0_amd64.deb
```

**Fedora**: download the `.rpm` from
[Releases](https://github.com/caetanus/lru-fs/releases), then

```sh
sudo dnf install ./lru-fs-0.1.0-1.fc*.x86_64.rpm
```

**From source**: needs a C compiler, `make`, `pkg-config` and libfuse 3
(`fuse3` on Arch, `libfuse3-dev` + `fuse3` on Debian/Ubuntu, `fuse3-devel` +
`fuse3` on Fedora).

```sh
make
sudo make install        # /usr/local/bin + /usr/local/lib/systemd/user
# or, without root:
make install-user        # ~/.local/bin + ~/.config/systemd/user
```

Packaging sources live in [`packaging/aur`](packaging/aur),
[`debian/`](debian) and [`packaging/rpm`](packaging/rpm).

### Turn it on

```sh
systemctl --user enable --now lru-fs      # mounts ~/scratch
df -h ~/scratch
```

Options go in `~/.config/lru-fs.conf`, read by the unit:

```sh
echo 'LRUFS_ARGS=-o wipe=5d,max_size=50G,min_free=10%,ram_limit=10%' > ~/.config/lru-fs.conf
systemctl --user restart lru-fs
```

### Point your tools at it

```sh
mkdir -p ~/scratch/myproject/tmp
export TMPDIR=~/scratch/myproject/tmp     # for anything that honors TMPDIR
```

For AI agents, put something like this in your `CLAUDE.md` / `AGENTS.md`:

```markdown
## Scratch
Put scratch files (throwaway builds, test clones, temporary downloads,
intermediate outputs) in ~/scratch/<project>/, not /tmp or ~/.cache. Set
TMPDIR=~/scratch/<project>/tmp for tools that honor it. It is disposable:
files unused for 5 days are deleted, and the least recently used go first
when it is full. Nothing that must last goes there.
If ~/scratch isn't mounted: systemctl --user start lru-fs
```

## Options

Passed as `-o name=value,...` (command line, `~/.config/lru-fs.conf`, or fstab).

| option           | default            | what it does |
|------------------|--------------------|--------------|
| `wipe`           | `5d`               | delete files unused this long; `never` turns it off (`ttl` is an alias) |
| `max_size`       | unlimited          | cap on RAM + disk; above it, least recently used go first |
| `min_free`       | `0`                | free space to keep on the filesystem holding `disk_dir` |
| `ram_limit`      | `2G`               | RAM budget; `0` = disk only |
| `max_file`       | `256M`             | larger files live on disk |
| `ram_dir`        | `/tmp/lru-fs`      | RAM tier directory (should be a tmpfs) |
| `disk_dir`       | `~/.cache/lru-fs`  | disk tier directory |
| `promote_window` | `10m`              | a disk file used again within this goes back to RAM |
| `scan_interval`  | `10m`              | how often to look for expired files |
| `noflush`        |                    | don't copy RAM to disk on unmount |
| `verbose`        |                    | log migrations, expiry and trims to stderr (`journalctl --user -u lru-fs`) |

Sizes: `512`, `64K`, `2G`, `1.5T`, `off`. Durations: `90s`, `30m`, `12h`,
`5d`, `2w`, `never`.

`max_size`, `min_free` and `ram_limit` also take a percentage (`10%`). The
base is never the raw size of the device or of RAM. It is the room scratch
actually has: free space right now plus what scratch already holds
(`MemAvailable` plus the tmpfs contents, for `ram_limit`). It is recomputed
every few seconds, so when something else fills the disk or the RAM, the
limits shrink with it and RAM files are pushed to disk right away.

## Mounting by hand, or from fstab

```sh
lru-fs ~/scratch
lru-fs -o wipe=2d,max_size=40G,min_free=10% ~/scratch
fusermount3 -u ~/scratch
```

`mount.fuse3` is supported, so fstab works too (`lru-fs` must be in root's
`PATH`):

```
lru-fs  /home/USER/scratch  fuse.lru-fs  setuid=USER,disk_dir=/home/USER/.cache/lru-fs,wipe=5d,max_size=50G,nofail  0 0
```

Use absolute paths there; `~` and `$HOME` don't expand in fstab.

## Limitations

- **Hard links** are refused (`EPERM`): files move between tiers by path, and
  an inode with two names can't survive that. `cp -al`, local `git clone` and
  friends fall back to copying on their own.
- `rename` with `RENAME_EXCHANGE` isn't supported (`EINVAL`); `RENAME_NOREPLACE` is.
- No extended attributes.
- A file deleted while open leaves the directory at once (so `rmdir` of its
  parent works) and stays readable and writable through its descriptor until
  closed. Only `fstat()` on that descriptor fails (`ESTALE`) if its
  **directory** was removed too.
- Don't write into `/tmp/lru-fs` or `~/.cache/lru-fs` directly while the
  daemon runs.
- The RAM budget covers the tmpfs. The kernel page cache may hold a second
  copy of files read through the mount, but that copy is reclaimable; tmpfs
  pages aren't (without swap).
- Metadata-heavy work is ~3× slower than on a native disk (it's FUSE);
  sequential reads and writes run close to native speed.

## Tests

```sh
make check     # = make && python3 tests/test.py
```

Mounts throwaway instances with tiny limits and checks eviction, promotion,
migration of open files, directory renames across tiers, unlink of open
files, 3000 random operations compared against a plain directory, 8
concurrent writers, flushing on unmount (even under SIGTERM), expiry,
`max_size`, `min_free`, percentage limits under simulated memory pressure,
`wipe=never` and mounting through `mount.fuse3`.

## License

[MIT](LICENSE)
