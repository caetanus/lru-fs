#!/usr/bin/env python3
"""Exercise lru-fs against real mounts. Usage: tests/test.py [workdir]"""
import hashlib
import os
import random
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BIN = os.path.join(HERE, "..", "lru-fs")
MiB = 1 << 20


class Mount:
    def __init__(self, work, **opts):
        self.mnt = os.path.join(work, "mnt")
        self.ram = os.path.join(work, "ram")
        self.disk = os.path.join(work, "disk")
        os.makedirs(self.mnt, exist_ok=True)
        o = dict(ram_dir=self.ram, disk_dir=self.disk, ram_limit="8M", max_file="3M",
                 ttl="1h", scan_interval="1h", promote_window="1h")
        o.update(opts)
        self.opts = ",".join(k if v is True else f"{k}={v}" for k, v in o.items())
        self.log = open(os.path.join(work, "log"), "a")

    def __enter__(self):
        wrap = os.environ.get("LRUFS_WRAP", "").split()
        self.p = subprocess.Popen(wrap + [BIN, "-f", "-o", self.opts + ",verbose", self.mnt],
                                  stdin=subprocess.DEVNULL, stdout=self.log, stderr=self.log)
        for _ in range(50):
            if os.path.ismount(self.mnt):
                return self
            time.sleep(0.05)
        raise RuntimeError("mount failed")

    def __exit__(self, *a):
        subprocess.run(["fusermount3", "-u", "-z", self.mnt], check=True)
        self.p.wait(timeout=30)

    def tier(self, rel):
        r = os.path.exists(os.path.join(self.ram, rel))
        d = os.path.lexists(os.path.join(self.disk, rel))
        assert not (r and d), f"{rel} in both tiers"
        return "ram" if r else "disk" if d else None

    def p_(self, rel):
        return os.path.join(self.mnt, rel)


def blob(n, seed):
    return random.Random(seed).randbytes(n)


def check(cond, msg):
    if not cond:
        raise AssertionError(msg)
    print("  ok:", msg)


def t_eviction(m):
    for i in range(4):
        with open(m.p_(f"f{i}"), "wb") as f:
            f.write(blob(2 * MiB + 1, i))
    check(m.tier("f0") == "disk" and m.tier("f3") == "ram", "oldest file evicted to disk, newest in ram")
    for i in range(4):
        check(open(m.p_(f"f{i}"), "rb").read() == blob(2 * MiB + 1, i), f"f{i} content intact")


def t_promotion(m):
    # f0 is on disk; reading it (its atime is recent) brings it back
    open(m.p_("f0"), "rb").read()
    open(m.p_("f0"), "rb").read()
    check(m.tier("f0") == "ram", "recently read disk file promoted to ram")
    with open(m.p_("f1"), "r+b") as f:
        pass
    check(m.tier("f1") == "ram", "disk file opened for writing promoted")


def t_big_file(m):
    with open(m.p_("big"), "wb") as f:
        for i in range(5):
            f.write(blob(MiB, 100 + i))
            if i == 1:
                check(m.tier("big") == "ram", "growing file starts in ram")
        f.flush()
        check(m.tier("big") == "disk", "file past max_file moved to disk while open")
        f.write(b"tail")
    data = open(m.p_("big"), "rb").read()
    check(data == b"".join(blob(MiB, 100 + i) for i in range(5)) + b"tail",
          "writes before and after migration all landed")
    os.unlink(m.p_("big"))


def t_migrate_open_reader(m):
    with open(m.p_("r1"), "wb") as f:
        f.write(blob(MiB, 7))
    fd = os.open(m.p_("r1"), os.O_RDONLY)
    first = os.pread(fd, 1000, 0)
    for i in range(6):  # push r1 out of ram
        with open(m.p_(f"push{i}"), "wb") as f:
            f.write(blob(2 * MiB, 50 + i))
    check(m.tier("r1") == "disk", "open file evicted")
    os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
    check(os.pread(fd, MiB, 0) == blob(MiB, 7) and first == blob(MiB, 7)[:1000],
          "reader keeps seeing the same data after eviction")
    os.close(fd)
    for i in range(6):
        os.unlink(m.p_(f"push{i}"))


def t_unlink_open(m):
    fd = os.open(m.p_("gone"), os.O_RDWR | os.O_CREAT, 0o644)
    os.write(fd, b"hello")
    os.unlink(m.p_("gone"))
    check(not os.path.exists(m.p_("gone")) and "gone" not in os.listdir(m.mnt), "unlinked file disappears")
    os.write(fd, b" world")
    check(os.pread(fd, 100, 0) == b"hello world", "unlinked-but-open file still readable/writable")
    check(os.fstat(fd).st_size == 11, "fstat on unlinked file works")
    check(not [n for n in os.listdir(m.mnt) if n.startswith(".fuse_hidden")],
          "no .fuse_hidden entry while the unlinked file is open")
    os.close(fd)
    # an open file deleted inside a directory must not block rmdir (sqlite, logs...)
    os.makedirs(m.p_("od/sub"))
    fds = []
    for n in ("od/a", "od/sub/b"):
        fds.append(os.open(m.p_(n), os.O_RDWR | os.O_CREAT, 0o644))
        os.write(fds[-1], b"payload")
    shutil.rmtree(m.p_("od"))
    check(not os.path.exists(m.p_("od")), "rmtree succeeds while deleted files are still open")
    # (fstat here is ESTALE: libfuse's high-level API can't name a file whose
    # directory is gone, and the kernel sends no handle with getattr)
    check(all(os.pread(f, 7, 0) == b"payload" for f in fds),
          "those files stay readable through their fds")
    os.write(fds[0], b"more")
    for f in fds:
        os.close(f)
    time.sleep(0.2)
    leftovers = [os.path.join(dp, f) for d in ("ram", "disk") for dp, _, fs in os.walk(getattr(m, d))
                 for f in fs if f.startswith(".fuse_hidden") or "/od" in dp]
    check(not leftovers, f"nothing left in the backing dirs after close ({leftovers})")


def t_rename(m):
    os.makedirs(m.p_("a/b"))
    open(m.p_("a/b/x"), "w").write("x")
    open(m.p_("a/y"), "w").write("y")
    os.symlink("b/x", m.p_("a/l"))
    check(m.tier("a/b/x") == "ram", "new file created in ram")
    fd = os.open(m.p_("a/b/x"), os.O_RDWR)
    os.rename(m.p_("a"), m.p_("c"))
    check(sorted(os.listdir(m.p_("c"))) == ["b", "l", "y"], "renamed dir lists ram and disk entries")
    check(open(m.p_("c/l")).read() == "x", "symlink in renamed dir resolves")
    for i in range(5):  # evict c/b/x while open, through its new name
        with open(m.p_(f"push{i}"), "wb") as f:
            f.write(blob(2 * MiB, 60 + i))
    os.write(fd, b"!")
    os.close(fd)
    check(open(m.p_("c/b/x")).read() == "!", "write through fd opened before dir rename and eviction")
    open(m.p_("c/z"), "w").write("z")
    os.rename(m.p_("c/z"), m.p_("c/b/x"))  # ram file replaces disk file
    check(open(m.p_("c/b/x")).read() == "z" and m.tier("c/b/x") == "ram", "rename over file in other tier")
    try:
        os.rmdir(m.p_("c/b"))
        check(False, "rmdir non-empty")
    except OSError:
        check(True, "rmdir of non-empty dir refused")
    shutil.rmtree(m.p_("c"))
    check(not os.path.exists(os.path.join(m.disk, "c")) and not os.path.exists(os.path.join(m.ram, "c")),
          "rmtree clears both tiers")
    for i in range(5):
        os.unlink(m.p_(f"push{i}"))


def t_misc(m):
    p = m.p_("modes")
    open(p, "w").write("data")
    os.chmod(p, 0o600)
    os.utime(p, (1_000_000, 2_000_000))
    st = os.stat(p)
    check(st.st_mode & 0o777 == 0o600 and st.st_mtime == 2_000_000, "chmod/utime pass through")
    os.truncate(p, 100)
    check(os.path.getsize(p) == 100, "truncate grows")
    try:
        os.link(p, m.p_("hard"))
        check(False, "hard link")
    except PermissionError:
        check(True, "hard links refused with EPERM")
    check(subprocess.run(["sh", "-c", f"echo 'echo run' > {m.p_('s.sh')} && chmod +x {m.p_('s.sh')} && {m.p_('s.sh')}"],
                         capture_output=True, text=True).stdout == "run\n", "executing a script from the mount")
    os.unlink(p)
    os.unlink(m.p_("s.sh"))


def t_random_diff(m, seed=1, ops=3000):
    """Same random operations on the mount and on a plain dir; compare."""
    rng = random.Random(seed)
    ref = tempfile.mkdtemp(dir=os.path.dirname(m.mnt))
    mnt = m.p_("rd")
    os.mkdir(mnt)
    names = [f"d{i}/f{j}" for i in range(4) for j in range(8)] + [f"f{j}" for j in range(8)]
    for root in (mnt, ref):
        for i in range(4):
            os.makedirs(os.path.join(root, f"d{i}"), exist_ok=True)
    for _ in range(ops):
        op = rng.choice(["write", "write", "append", "trunc", "unlink", "rename", "read"])
        a, b = rng.choice(names), rng.choice(names)
        off = rng.randrange(0, 600_000)
        data = rng.randbytes(rng.choice([10, 4096, 200_000, 900_000]))
        size = rng.randrange(0, 1_500_000)
        res = []
        for root in (mnt, ref):
            pa, pb = os.path.join(root, a), os.path.join(root, b)
            try:
                if op == "write":
                    fd = os.open(pa, os.O_WRONLY | os.O_CREAT, 0o644)
                    os.pwrite(fd, data, off)
                    os.close(fd)
                elif op == "append":
                    with open(pa, "ab") as f:
                        f.write(data)
                elif op == "trunc":
                    os.truncate(pa, size)
                elif op == "unlink":
                    os.unlink(pa)
                elif op == "rename":
                    os.rename(pa, pb)
                elif op == "read":
                    res.append(hashlib.sha1(open(pa, "rb").read()).hexdigest())
                    continue
                res.append("ok")
            except OSError as e:
                res.append(e.errno)
        assert res[0] == res[1], f"{op} {a} {b}: mount={res[0]} ref={res[1]}"
    for n in names:
        x, y = os.path.join(mnt, n), os.path.join(ref, n)
        assert os.path.exists(x) == os.path.exists(y), n
        if os.path.exists(y):
            assert open(x, "rb").read() == open(y, "rb").read(), n
    ram = sum(os.stat(os.path.join(dp, f)).st_blocks * 512
              for dp, _, fs in os.walk(m.ram) for f in fs)
    check(ram <= 8 * MiB + 1_500_000, f"ram tier within budget ({ram / MiB:.1f} MiB)")
    check(True, f"{ops} random ops identical to a plain directory")
    shutil.rmtree(ref)
    shutil.rmtree(mnt)


def t_concurrent(m):
    errors = []

    def worker(k):
        try:
            rng = random.Random(k)
            os.makedirs(m.p_(f"w{k}"), exist_ok=True)
            for i in range(60):
                data = rng.randbytes(rng.choice([1000, 300_000, 1_200_000]))
                p = m.p_(f"w{k}/f{i % 10}")
                with open(p, "wb") as f:
                    f.write(data)
                if open(p, "rb").read() != data:
                    errors.append(p)
        except Exception as e:
            errors.append(repr(e))

    ts = [threading.Thread(target=worker, args=(k,)) for k in range(8)]
    [t.start() for t in ts]
    [t.join() for t in ts]
    check(not errors, f"8 concurrent writers under eviction pressure ({errors[:3]})")
    for k in range(8):
        shutil.rmtree(m.p_(f"w{k}"))


def t_quota(work):
    with Mount(work, max_size="12M", ram_limit="4M", max_file="2M", scan_interval="1h") as m:
        st = os.statvfs(m.mnt)
        check(st.f_blocks * st.f_frsize == 12 * MiB, "df reports max_size as the filesystem size")
        old = time.time() - 3600
        for i in range(6):
            with open(m.p_(f"q{i}"), "wb") as f:
                f.write(blob(MiB + 1000 * i, 200 + i))
            os.utime(m.p_(f"q{i}"), (old + i, old + i))  # q0 is the least recently used
        st = os.statvfs(m.mnt)
        check(st.f_bavail * st.f_frsize < 6 * MiB, f"df free space shrinks ({st.f_bavail * st.f_frsize / MiB:.1f} MiB)")
        with open(m.p_("new"), "wb") as f:
            f.write(blob(5 * MiB, 300))
        left = sorted(n for n in os.listdir(m.mnt) if n.startswith("q"))
        check("q0" not in left and "q5" in left, f"over max_size the least recently used go first ({left})")
        check(open(m.p_("new"), "rb").read() == blob(5 * MiB, 300), "the write that needed room landed")
        fd = os.open(m.p_("q5"), os.O_RDONLY)
        try:
            with open(m.p_("huge"), "wb") as f:
                f.write(blob(20 * MiB, 301))
            check(False, "ENOSPC past max_size")
        except OSError as e:
            check(e.errno == 28, "a write that can't fit gets ENOSPC")
        check(os.path.exists(m.p_("q5")), "open files are never trimmed")
        os.close(fd)
    with Mount(work, min_free="100%", ram_limit="4M", max_file="1M") as m:
        with open(m.p_("small"), "wb") as f:
            f.write(b"x" * 1000)
        check(os.statvfs(m.mnt).f_bavail == 0, "min_free is taken off the free space df shows")
        try:
            with open(m.p_("big"), "wb") as f:
                f.write(blob(3 * MiB, 302))
            check(False, "min_free blocks disk writes")
        except OSError as e:
            check(e.errno == 28, "min_free=100% refuses disk-tier growth, ram files still work")
    for d in ("ram", "disk"):
        shutil.rmtree(os.path.join(work, d), ignore_errors=True)


def t_wipe_never(work):
    with Mount(work, wipe="never", scan_interval="1s") as m:
        open(m.p_("ancient"), "w").write("a")
        os.utime(m.p_("ancient"), (1_000_000, 1_000_000))
        time.sleep(2.5)
        check(os.path.exists(m.p_("ancient")), "wipe=never keeps old files")
        os.unlink(m.p_("ancient"))


def t_mount_helper(work):
    mnt = os.path.join(work, "mnt")
    env = dict(os.environ, PATH=os.path.dirname(os.path.abspath(BIN)) + ":" + os.environ["PATH"])
    opts = f"ram_dir={work}/ram,disk_dir={work}/disk,wipe=3d,max_size=1G"
    r = subprocess.run(["mount.fuse3", "lru-fs", mnt, "-o", opts], env=env, capture_output=True, text=True)
    check(r.returncode == 0 and os.path.ismount(mnt), f"mount.fuse3 (fstab path) mounts it {r.stderr.strip()}")
    open(os.path.join(mnt, "h"), "w").write("via helper")
    st = os.statvfs(mnt)
    check(st.f_blocks * st.f_frsize == 1 << 30, "options from mount -o are applied")
    subprocess.run(["fusermount3", "-u", mnt], check=True)
    time.sleep(0.5)
    r = subprocess.run([BIN, "-o", "wipe=5x", mnt], capture_output=True, text=True)
    check(r.returncode != 0 and "wipe" in r.stderr, f"bad values are rejected by name ({r.stderr.strip()})")


def t_percent(work):
    with Mount(work, max_size="50%", ram_limit="1%", min_free="10%") as m:
        with open(m.p_("p"), "wb") as f:
            f.write(blob(3 * MiB, 400))
        time.sleep(0.2)
        d = os.statvfs(m.disk)
        used = sum(os.stat(os.path.join(dp, f)).st_blocks * 512
                   for root in (m.ram, m.disk) for dp, _, fs in os.walk(root) for f in fs)
        room = d.f_bavail * d.f_frsize + used
        size = os.statvfs(m.mnt).f_blocks * os.statvfs(m.mnt).f_frsize
        check(abs(size - room / 2) < room * 0.02,
              f"max_size=50% is half of free+held ({size / MiB:.0f} MiB of {room / MiB:.0f} MiB), not of the device")
        dev = d.f_blocks * d.f_frsize
        check(abs(size - dev / 2) > dev * 0.01 or abs(room - dev) < dev * 0.02,
              "and differs from half the raw device size")
        os.unlink(m.p_("p"))
    log = open(os.path.join(work, "log")).read().splitlines()
    lim = [l for l in log if "ram_limit=" in l][-1]
    meminfo = dict(l.split(":", 1) for l in open("/proc/meminfo"))
    avail_mib = int(meminfo["MemAvailable"].split()[0]) // 1024
    got = int(lim.split("ram_limit=")[1].split("M")[0])
    check(abs(got - avail_mib / 100) <= max(8, avail_mib * 0.003),
          f"ram_limit=1% follows MemAvailable ({got} MiB vs {avail_mib / 100:.0f})")
    # ram_limit in % follows MemAvailable; when it drops, ram files leave on their own
    fake = os.path.join(work, "meminfo")
    open(fake, "w").write("MemAvailable:  102400 kB\n")  # 100 MiB -> 10% = 10 MiB
    os.environ["LRUFS_MEMINFO"] = fake
    try:
        with Mount(work, ram_limit="10%", max_file="2M") as m:
            for i in range(3):
                with open(m.p_(f"e{i}"), "wb") as f:
                    f.write(blob(MiB, 500 + i))
            before = sum(1 for _, _, fs in os.walk(m.ram) for _ in fs)
            open(fake, "w").write("MemAvailable:  10240 kB\n")  # 10% of (10+3) MiB = 1.3 MiB
            time.sleep(6.5)
            after = sum(1 for _, _, fs in os.walk(m.ram) for _ in fs)
            check(before == 3 and after <= 1, f"memory pressure shrinks ram_limit and evicts without writes ({before} -> {after})")
            check(open(m.p_("e0"), "rb").read() == blob(MiB, 500), "evicted file intact")
            for i in range(3):
                os.unlink(m.p_(f"e{i}"))
    finally:
        del os.environ["LRUFS_MEMINFO"]
    r = subprocess.run([BIN, "-o", "max_file=10%", os.path.join(work, "mnt")], capture_output=True, text=True)
    check(r.returncode != 0, "max_file refuses a percentage")


def main():
    work = sys.argv[1] if len(sys.argv) > 1 else tempfile.mkdtemp()
    for d in ("ram", "disk", "mnt"):
        shutil.rmtree(os.path.join(work, d), ignore_errors=True)

    with Mount(work) as m:
        for t in (t_eviction, t_promotion, t_big_file, t_migrate_open_reader, t_unlink_open,
                  t_rename, t_misc, t_random_diff, t_concurrent):
            print(t.__name__)
            t(m)
        open(m.p_("keep"), "w").write("persist")

    print("stop under systemd-like signals")
    with Mount(work, ram_limit="64M", max_file="16M") as m:
        for i in range(6):
            with open(m.p_(f"s{i}"), "wb") as f:
                f.write(blob(8 * MiB, 600 + i))
        m.__exit__ = None
        subprocess.run(["fusermount3", "-u", "-z", m.mnt], check=True)
        # what systemd does after ExecStop: SIGTERM, while the flush runs
        while m.p.poll() is None:
            try:
                m.p.terminate()
            except ProcessLookupError:
                break
            time.sleep(0.001)
        left = [f for _, _, fs in os.walk(m.ram) for f in fs]
        check(not left, f"SIGTERM during the unmount flush doesn't cut it short ({len(left)} left in ram)")
        subprocess.run([BIN, "-o", f"ram_dir={m.ram},disk_dir={m.disk}", m.mnt])  # remount for __exit__
        time.sleep(0.3)
        for i in range(6):
            assert open(m.p_(f"s{i}"), "rb").read() == blob(8 * MiB, 600 + i)
            os.unlink(m.p_(f"s{i}"))
        check(True, "flushed files intact")

    print("remount")
    check(os.path.exists(os.path.join(work, "disk", "keep")), "unmount flushed ram files to disk")
    with Mount(work) as m:
        files = [f for _, _, fs in os.walk(m.ram) for f in fs]
        check(not files, f"ram tier starts empty after flush ({files})")
        check(open(m.p_("keep")).read() == "persist", "contents survive remount")

    for f in (t_quota, t_percent, t_wipe_never, t_mount_helper):
        print(f.__name__)
        f(work)

    print("expiry")
    with Mount(work, ttl="3s", scan_interval="1s") as m:
        open(m.p_("old"), "w").write("o")
        os.makedirs(m.p_("olddir/sub"))
        open(m.p_("fresh"), "w").write("f")
        fd = os.open(m.p_("held"), os.O_RDWR | os.O_CREAT)
        for _ in range(6):
            os.utime(m.p_("fresh"))
            time.sleep(1)
        names = sorted(os.listdir(m.mnt))
        check(names == ["fresh", "held"], f"untouched files and empty dirs expire, touched/open stay ({names})")
        os.close(fd)
    print("all passed")


if __name__ == "__main__":
    main()
