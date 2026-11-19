/*
 * lru-fs — a scratch filesystem that keeps recently used files in RAM and the
 * rest on disk, and forgets anything nobody touched for a while.
 *
 * Two plain directories hold the data:
 *   ram tier  (default /tmp/lru-fs, a tmpfs)       — hot files, capped by ram_limit
 *   disk tier (default ~/.cache/lru-fs)             — everything else
 *
 * Every regular file lives in exactly one tier, under the same relative path.
 * Directories are canonical on disk; the ram tier keeps 0700 mirror
 * directories created on demand. Symlinks and special files stay on disk.
 *
 * Locking, always taken in this order:
 *   tree_lock  — namespace changes (create/unlink/rename/mkdir/rmdir),
 *                migrations between tiers, and expiry. A file's tier only
 *                changes under it.
 *   node->io   — rwlock per file; IO and open/release take it shared, a
 *                migration takes it exclusive while it copies and swaps fds.
 *   g_lock     — the index, the LRU list and the ram accounting.
 */
#define FUSE_USE_VERSION 31
#define _GNU_SOURCE

#include <fuse.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sendfile.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

enum { T_RAM = 0, T_DISK = 1 };
static const char *tier_name[] = { "ram", "disk" };

#define TMP_PREFIX ".lru-fs.tmp."
#define TMP_PREFIX_LEN (sizeof(TMP_PREFIX) - 1)

static struct {
	char *root[2];
	size_t root_len[2];
	/* effective limits; refresh_limits() recomputes the ones given in % */
	off_t ram_limit;
	off_t max_file;
	off_t max_size;   /* ram + disk, 0 = unlimited */
	off_t min_free;   /* keep this much free on the disk tier's filesystem */
	/* as requested: an absolute size, or a percentage (pct > 0) */
	off_t ram_limit_abs, max_file_abs, max_size_abs, min_free_abs;
	double ram_limit_pct, max_size_pct, min_free_pct;
	time_t ttl;       /* 0 = never expire */
	time_t promote_window;
	time_t scan_interval;
	int noflush;
	int verbose;
} cfg;

struct node;

#define FILE_MAGIC 0x6c727566u
#define DIR_MAGIC 0x6c727564u

struct handle {
	unsigned magic;
	struct node *n;
	int fd;
	int flags;
	struct handle *next;
};

struct node {
	char *path;
	size_t hash;
	int tier;
	int unlinked;     /* gone from the namespace, still open */
	int hidden;       /* unlinked, but still indexed under libfuse's
			     .fuse_hidden name until libfuse unlinks that */
	off_t bytes;      /* allocated bytes, counted in its tier's total; -1 = not seen yet */
	time_t last;      /* last open/read/write */
	int refs;         /* open handles + temporary pins */
	pthread_rwlock_t io;
	struct node *hnext;
	struct node *prev, *next;  /* LRU, head = most recent; only ram nodes */
	struct handle *handles;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t tree_lock = PTHREAD_MUTEX_INITIALIZER;

static struct node **tab;
static size_t tab_size, tab_count;
static struct node *lru_head, *lru_tail;
static off_t ram_used, disk_used;
static size_t ram_files;
static unsigned long tmp_counter;

static void logf_(const char *fmt, ...)
{
	if (!cfg.verbose)
		return;
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "lru-fs: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static void tpath(char *out, int tier, const char *rel)
{
	snprintf(out, PATH_MAX, "%s%s", cfg.root[tier], rel);
}

static off_t round_block(off_t v)
{
	return (v + 4095) & ~(off_t)4095;
}

static time_t now_s(void)
{
	return time(NULL);
}

static int is_tmp_name(const char *name)
{
	return strncmp(name, TMP_PREFIX, TMP_PREFIX_LEN) == 0;
}

/* ---- index ------------------------------------------------------------- */

static size_t hstr(const char *s)
{
	size_t h = 1469598103934665603ULL;
	while (*s) {
		h ^= (unsigned char)*s++;
		h *= 1099511628211ULL;
	}
	return h;
}

static void idx_grow(void)
{
	size_t nsize = tab_size ? tab_size * 2 : 4096;
	struct node **nt = calloc(nsize, sizeof(*nt));
	if (!nt)
		return;
	for (size_t i = 0; i < tab_size; i++) {
		struct node *n = tab[i], *nx;
		for (; n; n = nx) {
			nx = n->hnext;
			struct node **b = &nt[n->hash & (nsize - 1)];
			n->hnext = *b;
			*b = n;
		}
	}
	free(tab);
	tab = nt;
	tab_size = nsize;
}

static struct node *idx_find(const char *p)
{
	if (!tab_size)
		return NULL;
	size_t h = hstr(p);
	for (struct node *n = tab[h & (tab_size - 1)]; n; n = n->hnext)
		if (n->hash == h && strcmp(n->path, p) == 0)
			return n;
	return NULL;
}

static void idx_insert(struct node *n)
{
	if (tab_count + 1 > tab_size * 2)
		idx_grow();
	n->hash = hstr(n->path);
	struct node **b = &tab[n->hash & (tab_size - 1)];
	n->hnext = *b;
	*b = n;
	tab_count++;
}

static void idx_remove(struct node *n)
{
	struct node **pp = &tab[n->hash & (tab_size - 1)];
	while (*pp && *pp != n)
		pp = &(*pp)->hnext;
	if (*pp) {
		*pp = n->hnext;
		tab_count--;
	}
}

/* ---- LRU / accounting (g_lock held) ------------------------------------ */

static void lru_unlink(struct node *n)
{
	if (n->prev)
		n->prev->next = n->next;
	else
		lru_head = n->next;
	if (n->next)
		n->next->prev = n->prev;
	else
		lru_tail = n->prev;
	n->prev = n->next = NULL;
}

static void lru_push_front(struct node *n)
{
	n->prev = NULL;
	n->next = lru_head;
	if (lru_head)
		lru_head->prev = n;
	lru_head = n;
	if (!lru_tail)
		lru_tail = n;
}

static void ram_add(struct node *n, off_t bytes)
{
	n->bytes = bytes;
	ram_used += bytes;
	ram_files++;
	lru_push_front(n);
}

static void ram_del(struct node *n)
{
	ram_used -= n->bytes;
	n->bytes = 0;
	ram_files--;
	lru_unlink(n);
}

static struct node *node_new(const char *path, int tier)
{
	struct node *n = calloc(1, sizeof(*n));
	n->path = strdup(path);
	n->tier = tier;
	n->last = now_s();
	pthread_rwlockattr_t a;
	pthread_rwlockattr_init(&a);
	/* a migration must not starve behind a stream of reads */
	pthread_rwlockattr_setkind_np(&a, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
	pthread_rwlock_init(&n->io, &a);
	pthread_rwlockattr_destroy(&a);
	return n;
}

static void node_free(struct node *n)
{
	pthread_rwlock_destroy(&n->io);
	free(n->path);
	free(n);
}

/* Drop a reference. Disk nodes and unlinked nodes go away with their last
 * reference; ram nodes stay because the LRU needs them. g_lock held. */
static void node_put_locked(struct node *n)
{
	if (--n->refs > 0)
		return;
	if (n->unlinked) {
		if (n->hidden)
			idx_remove(n);
		if (n->tier == T_RAM)
			ram_del(n);
		node_free(n);
	} else if (n->tier == T_DISK) {
		idx_remove(n);
		node_free(n);
	}
}

static void node_put(struct node *n)
{
	pthread_mutex_lock(&g_lock);
	node_put_locked(n);
	pthread_mutex_unlock(&g_lock);
}

static struct node *node_get(const char *path)
{
	pthread_mutex_lock(&g_lock);
	struct node *n = idx_find(path);
	if (!n) {
		n = node_new(path, T_DISK);
		n->bytes = -1;
		idx_insert(n);
	}
	n->refs++;
	pthread_mutex_unlock(&g_lock);
	return n;
}

/* Forget a path that left the namespace (unlink, or replaced by rename).
 * g_lock held. */
static void node_forget_locked(struct node *n)
{
	idx_remove(n);
	n->unlinked = 1;
	n->hidden = 0;
	if (n->refs == 0) {
		if (n->tier == T_RAM)
			ram_del(n);
		node_free(n);
	}
}

static int tier_of(const char *path)
{
	pthread_mutex_lock(&g_lock);
	struct node *n = idx_find(path);
	int t = n ? n->tier : T_DISK;
	pthread_mutex_unlock(&g_lock);
	return t;
}

static void touch_node(struct node *n)
{
	time_t now = now_s();
	if (__atomic_load_n(&n->last, __ATOMIC_RELAXED) == now)
		return;
	pthread_mutex_lock(&g_lock);
	n->last = now;
	if (n->tier == T_RAM && !n->unlinked && lru_head != n) {
		lru_unlink(n);
		lru_push_front(n);
	}
	pthread_mutex_unlock(&g_lock);
}

/* Re-read allocated size after a change. Caller holds n->io. */
static void update_bytes(struct node *n, int fd)
{
	struct stat st;
	if (fstat(fd, &st) < 0)
		return;
	off_t b = (off_t)st.st_blocks * 512;
	pthread_mutex_lock(&g_lock);
	/* a disk file seen for the first time is already in disk_used */
	if (n->bytes >= 0)
		*(n->tier == T_RAM ? &ram_used : &disk_used) += b - n->bytes;
	n->bytes = b;
	pthread_mutex_unlock(&g_lock);
}

/* ---- tier helpers ------------------------------------------------------ */

/* mkdir -p the parents of rel inside a tier. Ram mirrors are private (0700),
 * disk parents copy the mode of the ram mirror when they had to be made. */
static int ensure_parents(int tier, const char *rel)
{
	char buf[PATH_MAX], dst[PATH_MAX];
	snprintf(buf, sizeof(buf), "%s", rel);
	for (char *s = buf + 1; (s = strchr(s, '/')); s++) {
		*s = 0;
		tpath(dst, tier, buf);
		if (mkdir(dst, tier == T_RAM ? 0700 : 0755) < 0 && errno != EEXIST)
			return -errno;
		*s = '/';
	}
	return 0;
}

/* The disk tier's directory mtime is what users see; keep it moving when the
 * entries that changed live in ram. */
static void touch_parent(const char *rel)
{
	char buf[PATH_MAX], p[PATH_MAX];
	snprintf(buf, sizeof(buf), "%s", rel);
	char *s = strrchr(buf, '/');
	if (!s)
		return;
	if (s == buf)
		s[1] = 0;
	else
		*s = 0;
	tpath(p, T_DISK, buf);
	utimensat(AT_FDCWD, p, NULL, 0);
}

static int dir_empty(const char *abs)
{
	DIR *d = opendir(abs);
	if (!d)
		return 1;
	struct dirent *de;
	int empty = 1;
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || is_tmp_name(de->d_name))
			continue;
		empty = 0;
		break;
	}
	closedir(d);
	return empty;
}

static int copy_data(int in, int out)
{
	for (;;) {
		ssize_t r = copy_file_range(in, NULL, out, NULL, 1 << 30, 0);
		if (r == 0)
			return 0;
		if (r < 0) {
			if (errno == EXDEV || errno == EINVAL || errno == ENOSYS || errno == EOPNOTSUPP)
				break;
			return -errno;
		}
	}
	for (;;) {
		ssize_t r = sendfile(out, in, NULL, 1 << 30);
		if (r == 0)
			return 0;
		if (r < 0) {
			if (errno == EINVAL || errno == ENOSYS)
				break;
			return -errno;
		}
	}
	char buf[1 << 16];
	for (;;) {
		ssize_t r = read(in, buf, sizeof(buf));
		if (r == 0)
			return 0;
		if (r < 0)
			return -errno;
		for (ssize_t w = 0; w < r;) {
			ssize_t k = write(out, buf + w, r - w);
			if (k < 0)
				return -errno;
			w += k;
		}
	}
}

static int backend_flags(int flags)
{
	return (flags & ~(O_CREAT | O_EXCL | O_TRUNC | O_NOCTTY | O_DIRECT)) | O_CLOEXEC;
}

/* Move a file to the other tier. Caller holds tree_lock and a reference.
 * Open handles are reopened on the new copy before the old one goes away. */
static int migrate(struct node *n, int to)
{
	int err = 0, in = -1, out = -1, nh = 0;
	int *fds = NULL;
	char src[PATH_MAX], dst[PATH_MAX], tmp[PATH_MAX];
	struct stat st, ost;

	pthread_rwlock_wrlock(&n->io);
	pthread_mutex_lock(&g_lock);
	int from = n->tier, dead = n->unlinked;
	char *rel = strdup(n->path);
	pthread_mutex_unlock(&g_lock);
	if (dead || from == to)
		goto out;

	tpath(src, from, rel);
	tpath(dst, to, rel);
	if ((err = ensure_parents(to, rel)))
		goto out;
	char *slash = strrchr(dst, '/');
	snprintf(tmp, sizeof(tmp), "%.*s/" TMP_PREFIX "%d.%lu",
		 (int)(slash - dst), dst, (int)getpid(), tmp_counter++);

	if ((in = open(src, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) < 0) {
		err = -errno;
		goto out;
	}
	if (fstat(in, &st) < 0 || !S_ISREG(st.st_mode)) {
		err = -EINVAL;
		goto out;
	}
	if ((out = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)) < 0) {
		err = -errno;
		goto out;
	}
	if ((err = copy_data(in, out)))
		goto out_tmp;

	for (struct handle *h = n->handles; h; h = h->next)
		nh++;
	fds = calloc(nh ? nh : 1, sizeof(int));
	int i = 0;
	for (struct handle *h = n->handles; h; h = h->next, i++) {
		/* still 0600 here, so write handles of read-only files reopen fine */
		if ((fds[i] = open(tmp, backend_flags(h->flags))) < 0) {
			err = -errno;
			while (i-- > 0)
				close(fds[i]);
			goto out_tmp;
		}
	}
	fchmod(out, st.st_mode & 07777);
	struct timespec ts[2] = { st.st_atim, st.st_mtim };
	futimens(out, ts);
	fstat(out, &ost);
	if (rename(tmp, dst) < 0) {
		err = -errno;
		for (i = 0; i < nh; i++)
			close(fds[i]);
		goto out_tmp;
	}
	if (unlink(src) < 0)
		logf_("migrate %s: unlink %s: %s", rel, src, strerror(errno));
	i = 0;
	for (struct handle *h = n->handles; h; h = h->next, i++) {
		close(h->fd);
		h->fd = fds[i];
	}

	pthread_mutex_lock(&g_lock);
	if (from == T_RAM)
		ram_del(n);
	else
		disk_used -= (off_t)st.st_blocks * 512;
	n->tier = to;
	if (to == T_RAM) {
		ram_add(n, (off_t)ost.st_blocks * 512);
	} else {
		n->bytes = (off_t)ost.st_blocks * 512;
		disk_used += n->bytes;
	}
	pthread_mutex_unlock(&g_lock);
	logf_("%s -> %s: %s (%lld bytes)", tier_name[from], tier_name[to], rel, (long long)st.st_size);
	goto out;

out_tmp:
	unlink(tmp);
out:
	if (out >= 0)
		close(out);
	if (in >= 0)
		close(in);
	free(fds);
	free(rel);
	pthread_rwlock_unlock(&n->io);
	return err;
}

/* Evict least recently used files until `need` more bytes fit in ram.
 * Caller holds tree_lock. */
static int make_room(struct node *self, off_t need)
{
	int failures = 0;
	for (;;) {
		pthread_mutex_lock(&g_lock);
		if (ram_used + need <= cfg.ram_limit) {
			pthread_mutex_unlock(&g_lock);
			return 0;
		}
		struct node *v = lru_tail;
		while (v && (v == self || v->unlinked))
			v = v->prev;
		if (!v) {
			pthread_mutex_unlock(&g_lock);
			return -ENOSPC;
		}
		v->refs++;
		pthread_mutex_unlock(&g_lock);
		int r = migrate(v, T_DISK);
		pthread_mutex_lock(&g_lock);
		if (r && v->tier == T_RAM && !v->unlinked) {
			/* can't move it now; don't pick it again right away */
			lru_unlink(v);
			lru_push_front(v);
		}
		node_put_locked(v);
		pthread_mutex_unlock(&g_lock);
		if (r && ++failures > 16)
			return -ENOSPC;
	}
}

static int check_quota(off_t growth, int tier);

/* Called before a file grows to `end` bytes: keeps ram within budget (or
 * sends the file to disk), then enforces max_size/min_free. No locks held. */
static int prep_grow(struct node *n, off_t end)
{
	pthread_mutex_lock(&g_lock);
	if (n->unlinked) {
		pthread_mutex_unlock(&g_lock);
		return 0;
	}
	off_t need = round_block(end) - (n->bytes > 0 ? n->bytes : 0);
	int tier = n->tier;
	int fits = need <= 0 || ram_used + need <= cfg.ram_limit;
	pthread_mutex_unlock(&g_lock);
	if (need <= 0)
		return 0;

	if (tier == T_RAM && (!fits || round_block(end) > cfg.max_file)) {
		pthread_mutex_lock(&tree_lock);
		if (round_block(end) > cfg.max_file || make_room(n, need))
			migrate(n, T_DISK);
		pthread_mutex_unlock(&tree_lock);
		pthread_mutex_lock(&g_lock);
		tier = n->tier;
		pthread_mutex_unlock(&g_lock);
	}
	return check_quota(need, tier);
}

/* A disk file opened for writing, or read twice within promote_window,
 * comes back to ram if it is small enough. No locks held. */
static void maybe_promote(struct node *n, int flags)
{
	char p[PATH_MAX];
	struct stat st;
	tpath(p, T_DISK, n->path);
	if (lstat(p, &st) < 0 || !S_ISREG(st.st_mode))
		return;
	int wr = (flags & O_ACCMODE) != O_RDONLY;
	int trunc = wr && (flags & O_TRUNC);
	off_t size = trunc ? 0 : st.st_size;
	if (size > cfg.max_file)
		return;
	if (!wr && now_s() - st.st_atime > cfg.promote_window)
		return;

	pthread_mutex_lock(&tree_lock);
	if (n->tier == T_DISK && !n->unlinked) {
		/* the open is about to truncate anyway; don't copy what it throws away */
		if (trunc && size == 0)
			truncate(p, 0);
		if (make_room(n, round_block(size)) == 0)
			migrate(n, T_RAM);
	}
	pthread_mutex_unlock(&tree_lock);
}

/* ---- FUSE operations --------------------------------------------------- */

struct dir_handle {
	unsigned magic;
	int fd[2]; /* per tier, -1 when the tier has no such directory */
};

/* The kernel may hand a directory's handle to setattr; only file handles
 * count. */
static struct handle *fh_of(struct fuse_file_info *fi)
{
	struct handle *h = fi ? (struct handle *)(uintptr_t)fi->fh : NULL;
	return h && h->magic == FILE_MAGIC ? h : NULL;
}

#define H(fi) ((struct handle *)(uintptr_t)(fi)->fh)

static void *lf_init(struct fuse_conn_info *conn, struct fuse_config *fc);
static void lf_destroy(void *);

static int lf_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
	char p[PATH_MAX];
	struct handle *h = fh_of(fi);
	if (h) {
		pthread_rwlock_rdlock(&h->n->io);
		int r = fstat(h->fd, st);
		pthread_rwlock_unlock(&h->n->io);
		return r < 0 ? -errno : 0;
	}
	pthread_mutex_lock(&g_lock);
	struct node *n = idx_find(path);
	if (n && n->hidden) {
		/* fstat() of an unlinked file comes here by its hidden name */
		int r = n->handles ? (fstat(n->handles->fd, st) < 0 ? -errno : 0) : -ENOENT;
		pthread_mutex_unlock(&g_lock);
		return r;
	}
	int t = n ? n->tier : T_DISK;
	pthread_mutex_unlock(&g_lock);
	tpath(p, t, path);
	if (lstat(p, st) == 0)
		return 0;
	if (errno != ENOENT)
		return -errno;
	tpath(p, !t, path);
	return lstat(p, st) < 0 ? -errno : 0;
}

static int lf_readlink(const char *path, char *buf, size_t size)
{
	char p[PATH_MAX];
	tpath(p, T_DISK, path);
	ssize_t r = readlink(p, buf, size - 1);
	if (r < 0)
		return -errno;
	buf[r] = 0;
	return 0;
}

static int lf_opendir(const char *path, struct fuse_file_info *fi)
{
	char p[PATH_MAX];
	struct dir_handle *dh = calloc(1, sizeof(*dh));
	dh->magic = DIR_MAGIC;
	int err = ENOENT;
	for (int t = 0; t < 2; t++) {
		tpath(p, t, path);
		dh->fd[t] = open(p, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (dh->fd[t] < 0 && t == T_DISK)
			err = errno;
	}
	if (dh->fd[T_RAM] < 0 && dh->fd[T_DISK] < 0) {
		free(dh);
		return -err;
	}
	fi->fh = (uintptr_t)dh;
	return 0;
}

static int lf_releasedir(const char *path, struct fuse_file_info *fi)
{
	(void)path;
	struct dir_handle *dh = (struct dir_handle *)(uintptr_t)fi->fh;
	for (int t = 0; t < 2; t++)
		if (dh->fd[t] >= 0)
			close(dh->fd[t]);
	free(dh);
	return 0;
}

static DIR *dir_stream(int fd)
{
	if (fd < 0)
		return NULL;
	int d = dup(fd);
	if (d < 0)
		return NULL;
	DIR *dp = fdopendir(d);
	if (!dp) {
		close(d);
		return NULL;
	}
	rewinddir(dp);
	return dp;
}

/* Disk entries first; ram entries only when disk has no such name, which is
 * the case for files living in ram. */
static int lf_readdir(const char *path, void *buf, fuse_fill_dir_t fill, off_t off,
		      struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
	(void)path;
	(void)off;
	(void)flags;
	struct dir_handle *dh = (struct dir_handle *)(uintptr_t)fi->fh;
	DIR *d = dir_stream(dh->fd[T_DISK]);
	DIR *r = dir_stream(dh->fd[T_RAM]);
	if (!d && !r)
		return -errno;

	struct dirent *de;
	struct stat st;
	memset(&st, 0, sizeof(st));
	if (d) {
		while ((de = readdir(d))) {
			if (is_tmp_name(de->d_name))
				continue;
			st.st_mode = DTTOIF(de->d_type);
			fill(buf, de->d_name, &st, 0, 0);
		}
	} else {
		fill(buf, ".", NULL, 0, 0);
		fill(buf, "..", NULL, 0, 0);
	}
	if (r) {
		while ((de = readdir(r))) {
			struct stat x;
			if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || is_tmp_name(de->d_name))
				continue;
			if (d && fstatat(dirfd(d), de->d_name, &x, AT_SYMLINK_NOFOLLOW) == 0)
				continue;
			st.st_mode = DTTOIF(de->d_type);
			fill(buf, de->d_name, &st, 0, 0);
		}
		closedir(r);
	}
	if (d)
		closedir(d);
	return 0;
}

static int exists_anywhere(const char *path)
{
	char p[PATH_MAX];
	struct stat st;
	tpath(p, T_DISK, path);
	if (lstat(p, &st) == 0)
		return 1;
	pthread_mutex_lock(&g_lock);
	int e = idx_find(path) != NULL;
	pthread_mutex_unlock(&g_lock);
	return e;
}

static int lf_mkdir(const char *path, mode_t mode)
{
	char p[PATH_MAX];
	int r = 0;
	pthread_mutex_lock(&tree_lock);
	pthread_mutex_lock(&g_lock);
	struct node *n = idx_find(path);
	int busy = n && n->tier == T_RAM;
	pthread_mutex_unlock(&g_lock);
	tpath(p, T_DISK, path);
	if (busy)
		r = -EEXIST;
	else if (mkdir(p, mode) < 0)
		r = -errno;
	pthread_mutex_unlock(&tree_lock);
	return r;
}

static int lf_rmdir(const char *path)
{
	char p[PATH_MAX];
	int r = 0;
	pthread_mutex_lock(&tree_lock);
	tpath(p, T_RAM, path);
	int had_ram = rmdir(p) == 0;
	if (!had_ram && errno != ENOENT) {
		r = -errno;
		goto out;
	}
	tpath(p, T_DISK, path);
	if (rmdir(p) < 0 && !(errno == ENOENT && had_ram))
		r = -errno;
out:
	pthread_mutex_unlock(&tree_lock);
	return r;
}

static int lf_unlink(const char *path)
{
	char p[PATH_MAX];
	int r = 0;
	struct stat st;
	pthread_mutex_lock(&tree_lock);
	pthread_mutex_lock(&g_lock);
	struct node *hn = idx_find(path);
	if (hn && hn->hidden) {
		/* libfuse removing its hidden name: the backing file is long gone */
		node_forget_locked(hn);
		pthread_mutex_unlock(&g_lock);
		pthread_mutex_unlock(&tree_lock);
		return 0;
	}
	pthread_mutex_unlock(&g_lock);
	int t = tier_of(path);
	tpath(p, t, path);
	if (lstat(p, &st) < 0) {
		t = !t;
		tpath(p, t, path);
	}
	if (lstat(p, &st) < 0 || unlink(p) < 0)
		r = -errno;
	if (r == 0) {
		pthread_mutex_lock(&g_lock);
		if (t == T_DISK && S_ISREG(st.st_mode))
			disk_used -= (off_t)st.st_blocks * 512;
		struct node *n = idx_find(path);
		if (n)
			node_forget_locked(n);
		pthread_mutex_unlock(&g_lock);
		touch_parent(path);
	}
	pthread_mutex_unlock(&tree_lock);
	return r;
}

static int lf_symlink(const char *target, const char *path)
{
	char p[PATH_MAX];
	int r = 0;
	pthread_mutex_lock(&tree_lock);
	tpath(p, T_DISK, path);
	if (exists_anywhere(path))
		r = -EEXIST;
	else if (symlink(target, p) < 0)
		r = -errno;
	pthread_mutex_unlock(&tree_lock);
	return r;
}

static int lf_mknod(const char *path, mode_t mode, dev_t dev)
{
	char p[PATH_MAX];
	int r = 0;
	pthread_mutex_lock(&tree_lock);
	tpath(p, T_DISK, path);
	if (exists_anywhere(path))
		r = -EEXIST;
	else if (mknod(p, mode, dev) < 0)
		r = -errno;
	pthread_mutex_unlock(&tree_lock);
	return r;
}

static int lf_link(const char *from, const char *to)
{
	(void)from;
	(void)to;
	/* a hard link would pin one inode to two paths; files move between
	 * tiers by path, so links can't be kept consistent */
	return -EPERM;
}

/* stat a path as the union sees it */
static int union_lstat(const char *path, struct stat *st, int *tier)
{
	char p[PATH_MAX];
	int t = tier_of(path);
	tpath(p, t, path);
	if (lstat(p, st) == 0) {
		*tier = t;
		return 0;
	}
	tpath(p, !t, path);
	if (lstat(p, st) == 0) {
		*tier = !t;
		return 0;
	}
	return -ENOENT;
}

static void rekey_prefix(const char *from, const char *to)
{
	size_t fl = strlen(from);
	struct node **moved = NULL;
	size_t nm = 0, cap = 0;
	for (size_t i = 0; i < tab_size; i++)
		for (struct node *n = tab[i]; n; n = n->hnext)
			if (strncmp(n->path, from, fl) == 0 && n->path[fl] == '/') {
				if (nm == cap) {
					cap = cap ? cap * 2 : 64;
					moved = realloc(moved, cap * sizeof(*moved));
				}
				moved[nm++] = n;
			}
	for (size_t i = 0; i < nm; i++) {
		struct node *n = moved[i];
		idx_remove(n);
		char *np = malloc(strlen(to) + strlen(n->path + fl) + 1);
		strcpy(np, to);
		strcat(np, n->path + fl);
		free(n->path);
		n->path = np;
		idx_insert(n);
	}
	free(moved);
}

/* libfuse renames a file that is unlinked while open to .fuse_hidden* and
 * unlinks that name on the last close. Left in place, the hidden name keeps
 * the directory non-empty (rmdir fails, readdir shows it). Instead, unlink
 * the backing file now — the open fds keep the data alive — and keep the
 * node under the hidden name only so fstat() and libfuse's final unlink
 * still find it. */
static int hide_open_file(const char *from, const char *to)
{
	char p[PATH_MAX];
	struct stat st;
	int r = 0;
	pthread_mutex_lock(&tree_lock);
	int t = tier_of(from);
	tpath(p, t, from);
	if (lstat(p, &st) < 0) {
		t = !t;
		tpath(p, t, from);
	}
	if (lstat(p, &st) < 0 || unlink(p) < 0) {
		r = -errno;
		goto out;
	}
	pthread_mutex_lock(&g_lock);
	if (t == T_DISK && S_ISREG(st.st_mode))
		disk_used -= (off_t)st.st_blocks * 512;
	struct node *n = idx_find(from);
	if (n) {
		idx_remove(n);
		free(n->path);
		n->path = strdup(to);
		n->unlinked = 1;
		n->hidden = 1;
		idx_insert(n);
	}
	pthread_mutex_unlock(&g_lock);
	touch_parent(from);
out:
	pthread_mutex_unlock(&tree_lock);
	return r;
}

static int lf_rename(const char *from, const char *to, unsigned int flags)
{
	char a[PATH_MAX], b[PATH_MAX];
	struct stat sst, tst;
	int st_tier, tt_tier, r = 0;

	if (flags & RENAME_EXCHANGE)
		return -EINVAL;
	if (strcmp(from, to) == 0)
		return 0;
	const char *base = strrchr(to, '/');
	if (base && strncmp(base + 1, ".fuse_hidden", 12) == 0)
		return hide_open_file(from, to);
	pthread_mutex_lock(&tree_lock);
	if ((r = union_lstat(from, &sst, &st_tier)))
		goto out;
	int texists = union_lstat(to, &tst, &tt_tier) == 0;
	if (texists && (flags & RENAME_NOREPLACE)) {
		r = -EEXIST;
		goto out;
	}

	if (S_ISDIR(sst.st_mode)) {
		if (texists) {
			if (!S_ISDIR(tst.st_mode)) {
				r = -ENOTDIR;
				goto out;
			}
			tpath(a, T_DISK, to);
			tpath(b, T_RAM, to);
			if (!dir_empty(a) || !dir_empty(b)) {
				r = -ENOTEMPTY;
				goto out;
			}
		}
		tpath(a, T_DISK, from);
		tpath(b, T_DISK, to);
		if (rename(a, b) < 0 && errno != ENOENT) {
			r = -errno;
			goto out;
		}
		tpath(a, T_RAM, from);
		tpath(b, T_RAM, to);
		struct stat x;
		if (lstat(a, &x) == 0) {
			ensure_parents(T_RAM, to);
			rmdir(b);
			if (rename(a, b) < 0)
				logf_("rename %s -> %s in ram: %s", from, to, strerror(errno));
		} else {
			rmdir(b);
		}
		pthread_mutex_lock(&g_lock);
		rekey_prefix(from, to);
		pthread_mutex_unlock(&g_lock);
	} else {
		if (texists && S_ISDIR(tst.st_mode)) {
			r = -EISDIR;
			goto out;
		}
		if ((r = ensure_parents(st_tier, to)))
			goto out;
		tpath(a, st_tier, from);
		tpath(b, st_tier, to);
		if (rename(a, b) < 0) {
			r = -errno;
			goto out;
		}
		if (texists && tt_tier != st_tier) {
			tpath(b, tt_tier, to);
			unlink(b);
		}
		pthread_mutex_lock(&g_lock);
		if (texists && tt_tier == T_DISK && S_ISREG(tst.st_mode))
			disk_used -= (off_t)tst.st_blocks * 512;
		struct node *tn = idx_find(to);
		if (tn)
			node_forget_locked(tn);
		struct node *sn = idx_find(from);
		if (sn) {
			idx_remove(sn);
			free(sn->path);
			sn->path = strdup(to);
			idx_insert(sn);
		}
		pthread_mutex_unlock(&g_lock);
	}
	touch_parent(from);
	touch_parent(to);
out:
	pthread_mutex_unlock(&tree_lock);
	return r;
}

static int lf_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	char p[PATH_MAX];
	struct handle *h = fh_of(fi);
	if (h) {
		pthread_rwlock_rdlock(&h->n->io);
		int r = fchmod(h->fd, mode);
		pthread_rwlock_unlock(&h->n->io);
		return r < 0 ? -errno : 0;
	}
	int t = tier_of(path);
	tpath(p, t, path);
	if (chmod(p, mode) == 0)
		return 0;
	if (errno != ENOENT)
		return -errno;
	tpath(p, !t, path);
	return chmod(p, mode) < 0 ? -errno : 0;
}

static int lf_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
	char p[PATH_MAX];
	struct handle *h = fh_of(fi);
	if (h) {
		pthread_rwlock_rdlock(&h->n->io);
		int r = fchown(h->fd, uid, gid);
		pthread_rwlock_unlock(&h->n->io);
		return r < 0 ? -errno : 0;
	}
	int t = tier_of(path);
	tpath(p, t, path);
	if (lchown(p, uid, gid) == 0)
		return 0;
	if (errno != ENOENT)
		return -errno;
	tpath(p, !t, path);
	return lchown(p, uid, gid) < 0 ? -errno : 0;
}

static int lf_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi)
{
	char p[PATH_MAX];
	struct handle *h = fh_of(fi);
	if (h) {
		pthread_rwlock_rdlock(&h->n->io);
		int r = futimens(h->fd, tv);
		pthread_rwlock_unlock(&h->n->io);
		return r < 0 ? -errno : 0;
	}
	int t = tier_of(path);
	tpath(p, t, path);
	if (utimensat(AT_FDCWD, p, tv, AT_SYMLINK_NOFOLLOW) == 0)
		return 0;
	if (errno != ENOENT)
		return -errno;
	tpath(p, !t, path);
	return utimensat(AT_FDCWD, p, tv, AT_SYMLINK_NOFOLLOW) < 0 ? -errno : 0;
}

static int lf_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
	char p[PATH_MAX];
	int r;
	struct handle *h = fh_of(fi);
	if (h) {
		if ((r = prep_grow(h->n, size)))
			return r;
		pthread_rwlock_rdlock(&h->n->io);
		r = ftruncate(h->fd, size) < 0 ? -errno : 0;
		if (!r)
			update_bytes(h->n, h->fd);
		pthread_rwlock_unlock(&h->n->io);
		touch_node(h->n);
		return r;
	}
	struct node *n = node_get(path);
	struct stat st;
	tpath(p, n->tier, path);
	pthread_mutex_lock(&g_lock);
	if (n->bytes < 0 && lstat(p, &st) == 0)
		n->bytes = (off_t)st.st_blocks * 512;
	pthread_mutex_unlock(&g_lock);
	if ((r = prep_grow(n, size))) {
		node_put(n);
		return r;
	}
	pthread_rwlock_rdlock(&n->io);
	tpath(p, n->tier, path);
	int fd = open(p, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
	if (fd < 0) {
		r = -errno;
	} else {
		r = ftruncate(fd, size) < 0 ? -errno : 0;
		if (!r)
			update_bytes(n, fd);
		close(fd);
	}
	pthread_rwlock_unlock(&n->io);
	node_put(n);
	return r;
}

static int open_node(struct node *n, const char *path, struct fuse_file_info *fi, int create_mode)
{
	char p[PATH_MAX];
	pthread_rwlock_rdlock(&n->io);
	tpath(p, n->tier, path);
	int fd = create_mode >= 0
		? open(p, (fi->flags & ~O_DIRECT) | O_CREAT | O_CLOEXEC, create_mode)
		: open(p, (fi->flags & ~(O_CREAT | O_EXCL | O_DIRECT)) | O_CLOEXEC);
	if (fd < 0) {
		int e = errno;
		pthread_rwlock_unlock(&n->io);
		return -e;
	}
	struct handle *h = calloc(1, sizeof(*h));
	h->magic = FILE_MAGIC;
	h->n = n;
	h->fd = fd;
	h->flags = fi->flags;
	pthread_mutex_lock(&g_lock);
	h->next = n->handles;
	n->handles = h;
	pthread_mutex_unlock(&g_lock);
	update_bytes(n, fd);
	pthread_rwlock_unlock(&n->io);
	touch_node(n);
	fi->fh = (uintptr_t)h;
	return 0;
}

static int lf_open(const char *path, struct fuse_file_info *fi)
{
	struct node *n = node_get(path);
	if (n->tier == T_DISK)
		maybe_promote(n, fi->flags);
	int r = open_node(n, path, fi, -1);
	if (r)
		node_put(n);
	return r;
}

static int lf_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
	char p[PATH_MAX];
	pthread_mutex_lock(&tree_lock);
	if (exists_anywhere(path)) {
		pthread_mutex_unlock(&tree_lock);
		if (fi->flags & O_EXCL)
			return -EEXIST;
		return lf_open(path, fi);
	}
	tpath(p, T_DISK, path);
	char *s = strrchr(p, '/');
	*s = 0;
	struct stat pst;
	int r = lstat(p, &pst) < 0 ? -errno : 0;
	if (!r)
		r = ensure_parents(T_RAM, path);
	if (r) {
		pthread_mutex_unlock(&tree_lock);
		return r;
	}
	struct node *n = node_new(path, T_RAM);
	pthread_mutex_lock(&g_lock);
	idx_insert(n);
	ram_add(n, 0);
	n->refs = 1;
	pthread_mutex_unlock(&g_lock);
	r = open_node(n, path, fi, mode);
	if (r) {
		pthread_mutex_lock(&g_lock);
		node_forget_locked(n); /* refs still 1: just marks it */
		node_put_locked(n);
		pthread_mutex_unlock(&g_lock);
	} else {
		touch_parent(path);
	}
	pthread_mutex_unlock(&tree_lock);
	return r;
}

static int lf_read(const char *path, char *buf, size_t size, off_t off, struct fuse_file_info *fi)
{
	(void)path;
	struct handle *h = H(fi);
	pthread_rwlock_rdlock(&h->n->io);
	ssize_t r = pread(h->fd, buf, size, off);
	int e = errno;
	pthread_rwlock_unlock(&h->n->io);
	touch_node(h->n);
	return r < 0 ? -e : (int)r;
}

static int lf_write(const char *path, const char *buf, size_t size, off_t off,
		    struct fuse_file_info *fi)
{
	(void)path;
	struct handle *h = H(fi);
	struct node *n = h->n;
	off_t end = off + (off_t)size;
	if (h->flags & O_APPEND) {
		struct stat st;
		if (fstat(h->fd, &st) == 0)
			end = st.st_size + (off_t)size;
	}
	int q = prep_grow(n, end);
	if (q)
		return q;
	pthread_rwlock_rdlock(&n->io);
	ssize_t r = pwrite(h->fd, buf, size, off);
	int e = errno;
	if (r > 0)
		update_bytes(n, h->fd);
	pthread_rwlock_unlock(&n->io);
	touch_node(n);
	return r < 0 ? -e : (int)r;
}

static int lf_fallocate(const char *path, int mode, off_t off, off_t len, struct fuse_file_info *fi)
{
	(void)path;
	struct handle *h = H(fi);
	int r = prep_grow(h->n, off + len);
	if (r)
		return r;
	pthread_rwlock_rdlock(&h->n->io);
	r = fallocate(h->fd, mode, off, len) < 0 ? -errno : 0;
	if (!r)
		update_bytes(h->n, h->fd);
	pthread_rwlock_unlock(&h->n->io);
	return r;
}

static off_t lf_lseek(const char *path, off_t off, int whence, struct fuse_file_info *fi)
{
	(void)path;
	struct handle *h = H(fi);
	pthread_rwlock_rdlock(&h->n->io);
	off_t r = lseek(h->fd, off, whence);
	int e = errno;
	pthread_rwlock_unlock(&h->n->io);
	return r < 0 ? -e : r;
}

static int lf_statfs(const char *path, struct statvfs *st)
{
	(void)path;
	if (statvfs(cfg.root[T_DISK], st) < 0)
		return -errno;
	off_t bs = st->f_frsize ? (off_t)st->f_frsize : 4096;
	fsblkcnt_t reserve = cfg.min_free / bs;
	st->f_bavail = st->f_bavail > reserve ? st->f_bavail - reserve : 0;
	if (cfg.max_size) {
		/* df shows the quota as the size of the filesystem */
		pthread_mutex_lock(&g_lock);
		off_t used = ram_used + disk_used;
		pthread_mutex_unlock(&g_lock);
		fsblkcnt_t room = used < cfg.max_size ? (cfg.max_size - used) / bs : 0;
		st->f_blocks = cfg.max_size / bs;
		st->f_bfree = room;
		if (st->f_bavail > room)
			st->f_bavail = room;
	}
	return 0;
}

static int lf_flush(const char *path, struct fuse_file_info *fi)
{
	(void)path;
	(void)fi;
	return 0;
}

static int lf_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
	(void)path;
	struct handle *h = H(fi);
	pthread_rwlock_rdlock(&h->n->io);
	int r = (datasync ? fdatasync(h->fd) : fsync(h->fd)) < 0 ? -errno : 0;
	pthread_rwlock_unlock(&h->n->io);
	return r;
}

static int lf_release(const char *path, struct fuse_file_info *fi)
{
	(void)path;
	struct handle *h = H(fi);
	struct node *n = h->n;
	pthread_rwlock_rdlock(&n->io);
	/* the access time is what keeps a file alive across restarts */
	struct timespec ts[2] = { { 0, UTIME_NOW }, { 0, UTIME_OMIT } };
	futimens(h->fd, ts);
	close(h->fd);
	pthread_mutex_lock(&g_lock);
	struct handle **pp = &n->handles;
	while (*pp != h)
		pp = &(*pp)->next;
	*pp = h->next;
	pthread_mutex_unlock(&g_lock);
	pthread_rwlock_unlock(&n->io);
	free(h);
	touch_node(n);
	node_put(n);
	return 0;
}

static const struct fuse_operations ops = {
	.init = lf_init,
	.destroy = lf_destroy,
	.getattr = lf_getattr,
	.readlink = lf_readlink,
	.opendir = lf_opendir,
	.readdir = lf_readdir,
	.releasedir = lf_releasedir,
	.mkdir = lf_mkdir,
	.rmdir = lf_rmdir,
	.unlink = lf_unlink,
	.symlink = lf_symlink,
	.mknod = lf_mknod,
	.link = lf_link,
	.rename = lf_rename,
	.chmod = lf_chmod,
	.chown = lf_chown,
	.utimens = lf_utimens,
	.truncate = lf_truncate,
	.open = lf_open,
	.create = lf_create,
	.read = lf_read,
	.write = lf_write,
	.fallocate = lf_fallocate,
	.lseek = lf_lseek,
	.statfs = lf_statfs,
	.flush = lf_flush,
	.fsync = lf_fsync,
	.release = lf_release,
};

/* ---- expiry ------------------------------------------------------------ */

static time_t last_use(const struct stat *st)
{
	return st->st_atime > st->st_mtime ? st->st_atime : st->st_mtime;
}

/* Delete a file unless it is open, moved to the other tier, or was used after
 * `before`. Returns the bytes freed, or -1 if the file was kept. */
static off_t reap(const char *abs, const char *rel, int tier, time_t before)
{
	off_t freed = -1;
	struct stat cur;
	pthread_mutex_lock(&tree_lock);
	pthread_mutex_lock(&g_lock);
	struct node *n = idx_find(rel);
	int keep = n && (n->refs > 0 || n->tier != tier || n->last > before);
	if (!keep && lstat(abs, &cur) == 0 && last_use(&cur) <= before && unlink(abs) == 0) {
		freed = S_ISREG(cur.st_mode) ? (off_t)cur.st_blocks * 512 : 0;
		if (tier == T_DISK)
			disk_used -= freed;
		if (n)
			node_forget_locked(n);
	}
	pthread_mutex_unlock(&g_lock);
	pthread_mutex_unlock(&tree_lock);
	return freed;
}

/* The janitor thread is the only caller of sweep and trim, so nftw callbacks
 * share state through these. */
static int sweep_tier;
static size_t sweep_removed;
static off_t sweep_disk_bytes;

static int sweep_cb(const char *abs, const struct stat *st, int type, struct FTW *ftw)
{
	if (ftw->level == 0)
		return 0;
	const char *rel = abs + cfg.root_len[sweep_tier];
	time_t now = now_s();

	if (is_tmp_name(abs + ftw->base)) {
		/* leftovers of a migration that died; live ones are seconds old */
		if (type != FTW_DP && now - st->st_mtime > 3600)
			unlink(abs);
		return 0;
	}

	if (type == FTW_F || type == FTW_SL) {
		if (sweep_tier == T_DISK && type == FTW_F)
			sweep_disk_bytes += (off_t)st->st_blocks * 512;
		if (!cfg.ttl || now - last_use(st) <= cfg.ttl)
			return 0;
		off_t f = reap(abs, rel, sweep_tier, now - cfg.ttl);
		if (f >= 0) {
			if (sweep_tier == T_DISK)
				sweep_disk_bytes -= f;
			sweep_removed++;
			logf_("expired %s (%s)", rel, tier_name[sweep_tier]);
		}
	} else if (type == FTW_DP) {
		pthread_mutex_lock(&tree_lock);
		if (sweep_tier == T_RAM) {
			/* ram mirrors are disposable once empty */
			rmdir(abs);
		} else if (cfg.ttl && now - st->st_mtime > cfg.ttl) {
			char p[PATH_MAX];
			struct stat x;
			tpath(p, T_RAM, rel);
			if (lstat(p, &x) < 0 && rmdir(abs) == 0)
				logf_("expired dir %s", rel);
		}
		pthread_mutex_unlock(&tree_lock);
	}
	return 0;
}

static void sweep(void)
{
	sweep_removed = 0;
	sweep_disk_bytes = 0;
	/* ram first, so emptied mirrors are gone before disk dirs are judged */
	for (int t = T_RAM; t <= T_DISK; t++) {
		sweep_tier = t;
		nftw(cfg.root[t], sweep_cb, 32, FTW_PHYS | FTW_DEPTH | FTW_MOUNT);
	}
	pthread_mutex_lock(&g_lock);
	/* the walk just counted every disk file: drop any drift in the tally */
	disk_used = sweep_disk_bytes;
	logf_("sweep: %zu expired; ram %lld/%lld MiB in %zu files, disk %lld MiB", sweep_removed,
	      (long long)(ram_used >> 20), (long long)(cfg.ram_limit >> 20), ram_files,
	      (long long)(disk_used >> 20));
	pthread_mutex_unlock(&g_lock);
}

/* ---- quota ------------------------------------------------------------- */

static off_t avail_cache;
static time_t avail_at;

static off_t disk_avail(int fresh)
{
	time_t now = now_s();
	if (fresh || __atomic_load_n(&avail_at, __ATOMIC_RELAXED) != now) {
		struct statvfs v;
		if (statvfs(cfg.root[T_DISK], &v) == 0)
			__atomic_store_n(&avail_cache, (off_t)v.f_bavail * v.f_frsize, __ATOMIC_RELAXED);
		__atomic_store_n(&avail_at, now, __ATOMIC_RELAXED);
	}
	return __atomic_load_n(&avail_cache, __ATOMIC_RELAXED);
}

/* How far past each limit we'd be with `slack` more bytes (<= 0: within). */
static void quota_excess(off_t slack_size, off_t slack_free, int fresh, off_t *ex_size, off_t *ex_free)
{
	*ex_size = *ex_free = 0;
	if (cfg.max_size) {
		pthread_mutex_lock(&g_lock);
		*ex_size = ram_used + disk_used + slack_size - cfg.max_size;
		pthread_mutex_unlock(&g_lock);
	}
	if (cfg.min_free)
		*ex_free = cfg.min_free + slack_free - disk_avail(fresh);
}

/* ---- limits ------------------------------------------------------------ */

static int has_pct(void)
{
	return cfg.ram_limit_pct > 0 || cfg.max_size_pct > 0 || cfg.min_free_pct > 0;
}

static off_t mem_available(void)
{
	/* LRUFS_MEMINFO lets the tests fake memory pressure */
	const char *src = getenv("LRUFS_MEMINFO");
	FILE *f = fopen(src ? src : "/proc/meminfo", "r");
	char line[256];
	long long kb = 0;
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, "MemAvailable: %lld kB", &kb) == 1)
			break;
	fclose(f);
	return (off_t)kb * 1024;
}

/* Percentages are of the room scratch actually has: what is free right now
 * plus what scratch itself already holds — never of the device's raw size.
 * Writing to scratch moves bytes from "free" to "held", so the base only
 * moves when something else fills or frees the disk (or RAM). */
static void refresh_limits(void)
{
	pthread_mutex_lock(&g_lock);
	off_t ru = ram_used, du = disk_used;
	pthread_mutex_unlock(&g_lock);
	off_t disk_room = disk_avail(1) + du;

	off_t max_size = cfg.max_size_pct > 0 ? (off_t)(cfg.max_size_pct / 100 * (disk_room + ru))
					      : cfg.max_size_abs;
	off_t min_free = cfg.min_free_pct > 0 ? (off_t)(cfg.min_free_pct / 100 * disk_room)
					      : cfg.min_free_abs;
	off_t ram_limit = cfg.ram_limit_pct > 0 ? (off_t)(cfg.ram_limit_pct / 100 * (mem_available() + ru))
						: cfg.ram_limit_abs;
	if (max_size && ram_limit > max_size)
		ram_limit = max_size;
	off_t max_file = cfg.max_file_abs < ram_limit ? cfg.max_file_abs : ram_limit;

	__atomic_store_n(&cfg.max_size, max_size, __ATOMIC_RELAXED);
	__atomic_store_n(&cfg.min_free, min_free, __ATOMIC_RELAXED);
	__atomic_store_n(&cfg.ram_limit, ram_limit, __ATOMIC_RELAXED);
	__atomic_store_n(&cfg.max_file, max_file, __ATOMIC_RELAXED);
}

static void log_limits(void)
{
	logf_("ram_limit=%lldM max_file=%lldM max_size=%lldM min_free=%lldM wipe=%lds",
	      (long long)(cfg.ram_limit >> 20), (long long)(cfg.max_file >> 20),
	      (long long)(cfg.max_size >> 20), (long long)(cfg.min_free >> 20), (long)cfg.ttl);
}

struct victim {
	char *abs;
	int tier;
	time_t last;
};
static struct victim *vic;
static size_t nvic, vic_cap;

static int trim_cb(const char *abs, const struct stat *st, int type, struct FTW *ftw)
{
	if (type != FTW_F || ftw->level == 0 || is_tmp_name(abs + ftw->base))
		return 0;
	if (nvic == vic_cap) {
		vic_cap = vic_cap ? vic_cap * 2 : 4096;
		vic = realloc(vic, vic_cap * sizeof(*vic));
	}
	vic[nvic++] = (struct victim){ strdup(abs), sweep_tier, last_use(st) };
	return 0;
}

static int victim_cmp(const void *a, const void *b)
{
	time_t x = ((const struct victim *)a)->last, y = ((const struct victim *)b)->last;
	return (x > y) - (x < y);
}

/* Over max_size or under min_free: delete the least recently used files,
 * whatever their age, until `need` more bytes fit with 5% to spare.
 * Returns files removed. */
static size_t trim(off_t need)
{
	off_t ex_size, ex_free;
	if (has_pct())
		refresh_limits();
	quota_excess(need + cfg.max_size / 20, need + cfg.min_free / 20, 1, &ex_size, &ex_free);
	if (ex_size <= 0 && ex_free <= 0)
		return 0;

	nvic = 0;
	for (int t = T_RAM; t <= T_DISK; t++) {
		sweep_tier = t;
		nftw(cfg.root[t], trim_cb, 32, FTW_PHYS | FTW_MOUNT);
	}
	pthread_mutex_lock(&g_lock);
	/* ram files carry a fresher clock than their atime */
	for (size_t i = 0; i < nvic; i++) {
		if (vic[i].tier != T_RAM)
			continue;
		struct node *n = idx_find(vic[i].abs + cfg.root_len[T_RAM]);
		if (n && n->last > vic[i].last)
			vic[i].last = n->last;
	}
	pthread_mutex_unlock(&g_lock);
	qsort(vic, nvic, sizeof(*vic), victim_cmp);

	size_t removed = 0;
	off_t freed_total = 0;
	for (size_t i = 0; i < nvic && (ex_size > 0 || ex_free > 0); i++) {
		struct victim *v = &vic[i];
		/* deleting from ram doesn't free disk space */
		if (v->tier == T_RAM && ex_size <= 0)
			continue;
		off_t f = reap(v->abs, v->abs + cfg.root_len[v->tier], v->tier, v->last);
		if (f < 0)
			continue;
		removed++;
		freed_total += f;
		ex_size -= f;
		if (v->tier == T_DISK)
			ex_free -= f;
		logf_("trimmed %s (%s, unused for %lds)", v->abs + cfg.root_len[v->tier],
		      tier_name[v->tier], (long)(now_s() - v->last));
	}
	for (size_t i = 0; i < nvic; i++)
		free(vic[i].abs);
	disk_avail(1);
	logf_("trim: %zu files, %lld MiB freed", removed, (long long)(freed_total >> 20));
	return removed;
}

static pthread_t janitor_thread;
static pthread_mutex_t janitor_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t janitor_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t trim_cv = PTHREAD_COND_INITIALIZER;
static int janitor_stop;
static unsigned long trim_req, trim_done;
static off_t trim_need;
static time_t trim_fruitless_at;

/* Ask the janitor for a trim and wait for it. A writer that just saw a trim
 * free nothing gets ENOSPC right away instead of another full walk. */
static void request_trim(off_t need)
{
	pthread_mutex_lock(&janitor_mx);
	if (trim_fruitless_at != now_s()) {
		unsigned long mine = ++trim_req;
		if (need > trim_need)
			trim_need = need;
		pthread_cond_signal(&janitor_cv);
		while (trim_done < mine && !janitor_stop)
			pthread_cond_wait(&trim_cv, &janitor_mx);
	}
	pthread_mutex_unlock(&janitor_mx);
}

/* Called before `growth` more bytes land in `tier`. No locks held. */
static int check_quota(off_t growth, int tier)
{
	if (growth <= 0 || (!cfg.max_size && !cfg.min_free))
		return 0;
	for (int attempt = 0;; attempt++) {
		off_t ex_size, ex_free;
		quota_excess(growth, growth, attempt > 0, &ex_size, &ex_free);
		if (ex_size <= 0 && (tier == T_RAM || ex_free <= 0))
			return 0;
		if (attempt)
			return -ENOSPC;
		request_trim(growth);
	}
}

static void *janitor(void *arg)
{
	(void)arg;
	time_t next_sweep = 0, next_refresh = 0;
	pthread_mutex_lock(&janitor_mx);
	while (!janitor_stop) {
		if (has_pct() && now_s() >= next_refresh) {
			pthread_mutex_unlock(&janitor_mx);
			refresh_limits();
			/* a limit that shrank must push ram files out now, not at the
			 * next write */
			pthread_mutex_lock(&tree_lock);
			make_room(NULL, 0);
			pthread_mutex_unlock(&tree_lock);
			pthread_mutex_lock(&janitor_mx);
			next_refresh = now_s() + 5;
			continue;
		}
		if (now_s() >= next_sweep) {
			pthread_mutex_unlock(&janitor_mx);
			sweep();
			trim(0);
			pthread_mutex_lock(&janitor_mx);
			next_sweep = now_s() + cfg.scan_interval;
			continue;
		}
		if (trim_req != trim_done) {
			unsigned long upto = trim_req;
			off_t need = trim_need;
			trim_need = 0;
			pthread_mutex_unlock(&janitor_mx);
			size_t k = trim(need);
			pthread_mutex_lock(&janitor_mx);
			trim_done = upto;
			if (!k)
				trim_fruitless_at = now_s();
			pthread_cond_broadcast(&trim_cv);
			continue;
		}
		struct timespec ts;
		clock_gettime(CLOCK_REALTIME, &ts);
		time_t wake = has_pct() && next_refresh < next_sweep ? next_refresh : next_sweep;
		ts.tv_sec += wake - now_s();
		pthread_cond_timedwait(&janitor_cv, &janitor_mx, &ts);
	}
	pthread_cond_broadcast(&trim_cv);
	pthread_mutex_unlock(&janitor_mx);
	return NULL;
}

static void *lf_init(struct fuse_conn_info *conn, struct fuse_config *fc)
{
	(void)conn;
	fc->use_ino = 0;
	fc->nullpath_ok = 1;
	/* hard_remove would break fstat() on unlinked-but-open files (the
	 * kernel's getattr carries no handle); let libfuse hide them instead */
	fc->hard_remove = 0;
	pthread_create(&janitor_thread, NULL, janitor, NULL);
	return NULL;
}

static void flush_ram(void)
{
	int failures = 0;
	pthread_mutex_lock(&tree_lock);
	for (;;) {
		pthread_mutex_lock(&g_lock);
		struct node *n = lru_tail;
		while (n && n->unlinked)
			n = n->prev;
		if (!n) {
			pthread_mutex_unlock(&g_lock);
			break;
		}
		n->refs++;
		pthread_mutex_unlock(&g_lock);
		int r = migrate(n, T_DISK);
		pthread_mutex_lock(&g_lock);
		if (r && n->tier == T_RAM) {
			lru_unlink(n);
			lru_push_front(n);
		}
		node_put_locked(n);
		pthread_mutex_unlock(&g_lock);
		if (r && ++failures > 16)
			break;
	}
	pthread_mutex_unlock(&tree_lock);
}

static void lf_destroy(void *priv)
{
	(void)priv;
	/* By now libfuse has dropped its signal handlers, and systemd sends
	 * SIGTERM right after ExecStop unmounts: without this the flush below
	 * dies halfway and whatever is left in ram is lost at the next reboot. */
	signal(SIGTERM, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	signal(SIGHUP, SIG_IGN);
	pthread_mutex_lock(&janitor_mx);
	janitor_stop = 1;
	pthread_cond_signal(&janitor_cv);
	pthread_mutex_unlock(&janitor_mx);
	pthread_join(janitor_thread, NULL);
	if (!cfg.noflush) {
		logf_("flushing %zu ram files to disk", ram_files);
		flush_ram();
	}
}

/* ---- startup ----------------------------------------------------------- */

struct seed {
	struct node *n;
	time_t last;
};
static struct seed *seeds;
static size_t nseeds, seeds_cap;

static int scan_cb(const char *abs, const struct stat *st, int type, struct FTW *ftw)
{
	if (ftw->level == 0)
		return 0;
	if (is_tmp_name(abs + ftw->base)) {
		unlink(abs);
		return 0;
	}
	if (sweep_tier == T_DISK) {
		if (type == FTW_F)
			disk_used += (off_t)st->st_blocks * 512;
		return 0;
	}
	const char *rel = abs + cfg.root_len[T_RAM];
	char p[PATH_MAX];
	struct stat ds;
	tpath(p, T_DISK, rel);
	if (type == FTW_D) {
		/* every ram directory has its canonical twin on disk */
		mkdir(p, 0755);
		return 0;
	}
	if (type != FTW_F) {
		logf_("ignoring non-regular %s in ram tier", abs);
		return 0;
	}
	if (lstat(p, &ds) == 0) {
		/* a migration was interrupted between rename and unlink */
		if (ds.st_mtime > st->st_mtime) {
			unlink(abs);
			return 0;
		}
		if (unlink(p) == 0)
			disk_used -= (off_t)ds.st_blocks * 512;
	}
	struct node *n = node_new(rel, T_RAM);
	n->bytes = (off_t)st->st_blocks * 512;
	n->last = st->st_atime > st->st_mtime ? st->st_atime : st->st_mtime;
	if (nseeds == seeds_cap) {
		seeds_cap = seeds_cap ? seeds_cap * 2 : 1024;
		seeds = realloc(seeds, seeds_cap * sizeof(*seeds));
	}
	seeds[nseeds++] = (struct seed){ n, n->last };
	return 0;
}

static int seed_cmp(const void *a, const void *b)
{
	time_t x = ((const struct seed *)a)->last, y = ((const struct seed *)b)->last;
	return (x > y) - (x < y);
}

static int mkdirs(const char *path)
{
	char buf[PATH_MAX];
	snprintf(buf, sizeof(buf), "%s", path);
	for (char *s = buf + 1; ; s++) {
		s = strchr(s, '/');
		if (s)
			*s = 0;
		if (mkdir(buf, 0700) < 0 && errno != EEXIST)
			return -errno;
		if (!s)
			return 0;
		*s = '/';
	}
}

static void load_tiers(void)
{
	sweep_tier = T_DISK;
	nftw(cfg.root[T_DISK], scan_cb, 32, FTW_PHYS | FTW_MOUNT);
	sweep_tier = T_RAM;
	nftw(cfg.root[T_RAM], scan_cb, 32, FTW_PHYS | FTW_MOUNT);
	qsort(seeds, nseeds, sizeof(*seeds), seed_cmp);
	for (size_t i = 0; i < nseeds; i++) {
		struct node *n = seeds[i].n;
		off_t b = n->bytes;
		idx_insert(n);
		ram_add(n, b);
	}
	free(seeds);
	refresh_limits();
	pthread_mutex_lock(&tree_lock);
	make_room(NULL, 0);
	pthread_mutex_unlock(&tree_lock);
	logf_("ram tier: %zu files, %lld MiB; disk tier: %lld MiB", ram_files,
	      (long long)(ram_used >> 20), (long long)(disk_used >> 20));
}

/* ---- options ----------------------------------------------------------- */

struct raw_opts {
	char *ram_dir, *disk_dir, *ram_limit, *max_file, *max_size, *min_free, *ttl,
		*promote_window, *scan_interval;
	int noflush, verbose, help;
};

#define STR(t, f) { t, offsetof(struct raw_opts, f), 0 }
#define FLAG(t, f) { t, offsetof(struct raw_opts, f), 1 }
static const struct fuse_opt opt_spec[] = {
	STR("ram_dir=%s", ram_dir),
	STR("disk_dir=%s", disk_dir),
	STR("ram_limit=%s", ram_limit),
	STR("max_file=%s", max_file),
	STR("max_size=%s", max_size),
	STR("min_free=%s", min_free),
	STR("wipe=%s", ttl),
	STR("ttl=%s", ttl),
	STR("promote_window=%s", promote_window),
	STR("scan_interval=%s", scan_interval),
	FLAG("noflush", noflush),
	FLAG("verbose", verbose),
	FLAG("-h", help),
	FLAG("--help", help),
	FUSE_OPT_END
};

/* `lru-fs MOUNTPOINT` from the shell, `lru-fs SOURCE MOUNTPOINT` from
 * mount(8)/fstab via mount.fuse3; the source is ignored. */
static char *nonopt[2];
static int n_nonopt;

static int opt_proc(void *data, const char *arg, int key, struct fuse_args *out)
{
	(void)data;
	(void)out;
	if (key != FUSE_OPT_KEY_NONOPT)
		return 1;
	if (n_nonopt == 2) {
		fprintf(stderr, "lru-fs: unexpected argument '%s'\n", arg);
		return -1;
	}
	nonopt[n_nonopt++] = strdup(arg);
	return 0;
}

static int is_off(const char *s)
{
	return !strcmp(s, "off") || !strcmp(s, "never") || !strcmp(s, "unlimited") || !strcmp(s, "none");
}

/* SIZE: 512, 64K, 2G, 1.5T, or N% (only where `pct` is given) */
static int parse_size(const char *s, off_t *out, double *pct)
{
	if (is_off(s)) {
		*out = 0;
		return 0;
	}
	char *end;
	double v = strtod(s, &end);
	double m = 1;
	switch (*end) {
	case 'k': case 'K': m = 1024.0; end++; break;
	case 'm': case 'M': m = 1024.0 * 1024; end++; break;
	case 'g': case 'G': m = 1024.0 * 1024 * 1024; end++; break;
	case 't': case 'T': m = 1024.0 * 1024 * 1024 * 1024; end++; break;
	case '%':
		if (!pct || end[1] || v <= 0 || v > 100)
			return -1;
		*pct = v;
		*out = 0;
		return 0;
	}
	if (end == s || *end || v < 0)
		return -1;
	*out = (off_t)(v * m);
	return 0;
}

/* DURATION: 90s, 30m, 12h, 5d, 2w; "never"/"off"/0 = no limit */
static int parse_dur(const char *s, time_t *out)
{
	if (is_off(s)) {
		*out = 0;
		return 0;
	}
	char *end;
	double v = strtod(s, &end);
	double m = 1;
	switch (*end) {
	case 's': end++; break;
	case 'm': m = 60; end++; break;
	case 'h': m = 3600; end++; break;
	case 'd': m = 86400; end++; break;
	case 'w': m = 7 * 86400; end++; break;
	}
	if (end == s || *end || v < 0)
		return -1;
	*out = (time_t)(v * m);
	return 0;
}

static char *absdir(const char *p)
{
	char buf[PATH_MAX];
	if (p[0] == '~' && p[1] == '/')
		snprintf(buf, sizeof(buf), "%s%s", getenv("HOME"), p + 1);
	else
		snprintf(buf, sizeof(buf), "%s", p);
	size_t l = strlen(buf);
	while (l > 1 && buf[l - 1] == '/')
		buf[--l] = 0;
	return strdup(buf);
}

static void usage(const char *prog)
{
	printf("usage: %s [options] <mountpoint>\n"
	       "       mount -t fuse.lru-fs lru-fs <mountpoint> -o <options>\n\n"
	       "lru-fs options (-o name=value,...):\n"
	       "    wipe=DURATION        delete files untouched for this long (default 5d; never = keep)\n"
	       "    max_size=SIZE        cap on everything stored, ram + disk; least recently\n"
	       "                         used files are deleted early to stay under (default unlimited)\n"
	       "    min_free=SIZE        same, to keep this much free on the disk tier's\n"
	       "                         filesystem (default 0)\n"
	       "    ram_limit=SIZE       ram budget (default 2G; 0 = disk only)\n"
	       "  These three also take N%%: a share of the room scratch really has, i.e.\n"
	       "  free space (MemAvailable for ram_limit) plus what scratch already holds,\n"
	       "  re-evaluated every few seconds. Never a share of the raw device size.\n"
	       "    max_file=SIZE        bigger files live on disk (default 256M)\n"
	       "    ram_dir=DIR          ram tier  (default /tmp/lru-fs)\n"
	       "    disk_dir=DIR         disk tier (default ~/.cache/lru-fs)\n"
	       "    promote_window=DUR   a disk file used again within this goes back to ram (default 10m)\n"
	       "    scan_interval=DUR    how often to look for expired files (default 10m)\n"
	       "    noflush              don't copy ram files to disk on unmount\n"
	       "    verbose              log migrations, expiry and trims to stderr\n"
	       "SIZE: 512, 64K, 2G, 1.5T. DURATION: 90s, 30m, 12h, 5d, 2w. ttl= is an alias of wipe=.\n\n",
	       prog);
}

static int bad(const char *name, const char *val)
{
	fprintf(stderr, "lru-fs: bad value for %s: '%s' (see lru-fs -h)\n", name, val);
	return 1;
}

int main(int argc, char *argv[])
{
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	struct raw_opts o = { 0 };
	if (fuse_opt_parse(&args, &o, opt_spec, opt_proc) < 0)
		return 1;
	if (o.help) {
		usage(argv[0]);
		fuse_opt_add_arg(&args, "-ho");
		return fuse_main(args.argc, args.argv, &ops, NULL);
	}
	if (n_nonopt == 0) {
		fprintf(stderr, "lru-fs: no mountpoint (see lru-fs -h)\n");
		return 1;
	}

	char def_disk[PATH_MAX];
	snprintf(def_disk, sizeof(def_disk), "%s/.cache/lru-fs", getenv("HOME") ? getenv("HOME") : "");
	cfg.root[T_RAM] = absdir(o.ram_dir ? o.ram_dir : "/tmp/lru-fs");
	cfg.root[T_DISK] = absdir(o.disk_dir ? o.disk_dir : def_disk);
	for (int t = 0; t < 2; t++) {
		if (mkdirs(cfg.root[t]) < 0) {
			fprintf(stderr, "lru-fs: cannot create %s: %s\n", cfg.root[t], strerror(errno));
			return 1;
		}
		cfg.root_len[t] = strlen(cfg.root[t]);
	}
	if (!strcmp(cfg.root[0], cfg.root[1])) {
		fprintf(stderr, "lru-fs: ram_dir and disk_dir must differ\n");
		return 1;
	}

	cfg.ram_limit_abs = 2LL << 30;
	cfg.max_file_abs = 256LL << 20;
	cfg.ttl = 5 * 86400;
	cfg.promote_window = 600;
	cfg.scan_interval = 600;
	cfg.noflush = o.noflush;
	cfg.verbose = o.verbose;
	if (o.ram_limit && parse_size(o.ram_limit, &cfg.ram_limit_abs, &cfg.ram_limit_pct))
		return bad("ram_limit", o.ram_limit);
	if (o.max_file && parse_size(o.max_file, &cfg.max_file_abs, NULL))
		return bad("max_file", o.max_file);
	if (o.max_size && parse_size(o.max_size, &cfg.max_size_abs, &cfg.max_size_pct))
		return bad("max_size", o.max_size);
	if (o.min_free && parse_size(o.min_free, &cfg.min_free_abs, &cfg.min_free_pct))
		return bad("min_free", o.min_free);
	if (o.ttl && parse_dur(o.ttl, &cfg.ttl))
		return bad("wipe", o.ttl);
	if (o.promote_window && parse_dur(o.promote_window, &cfg.promote_window))
		return bad("promote_window", o.promote_window);
	if (o.scan_interval && parse_dur(o.scan_interval, &cfg.scan_interval))
		return bad("scan_interval", o.scan_interval);
	if (cfg.scan_interval < 1)
		cfg.scan_interval = 1;

	load_tiers();
	log_limits();

	fuse_opt_add_arg(&args, nonopt[n_nonopt - 1]);
	fuse_opt_add_arg(&args, "-odefault_permissions,fsname=lru-fs,subtype=lru-fs");
	int r = fuse_main(args.argc, args.argv, &ops, NULL);
	fuse_opt_free_args(&args);
	return r;
}
