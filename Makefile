VERSION := 0.1.0

PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
USERUNITDIR ?= $(PREFIX)/lib/systemd/user
CFLAGS ?= -O2 -g -Wall -Wextra
CPPFLAGS += -DLRUFS_VERSION=\"$(VERSION)\"
FUSE_CFLAGS := $(shell pkg-config --cflags fuse3)
FUSE_LIBS := $(shell pkg-config --libs fuse3)

lru-fs: lru-fs.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FUSE_CFLAGS) -o $@ $< $(LDFLAGS) $(FUSE_LIBS) -lpthread

# System-wide, as packages do: sudo make install (PREFIX=/usr/local by default;
# packages use PREFIX=/usr DESTDIR=...). Installs the binary and the systemd
# *user* unit; each user then runs: systemctl --user enable --now lru-fs
install: lru-fs
	install -Dm755 lru-fs $(DESTDIR)$(BINDIR)/lru-fs
	install -d $(DESTDIR)$(USERUNITDIR)
	sed 's|@BINDIR@|$(BINDIR)|g' lru-fs.service.in > $(DESTDIR)$(USERUNITDIR)/lru-fs.service
	chmod 644 $(DESTDIR)$(USERUNITDIR)/lru-fs.service

# Just for you, no root: ~/.local/bin + ~/.config/systemd/user
install-user:
	$(MAKE) install PREFIX=$(HOME)/.local USERUNITDIR=$(HOME)/.config/systemd/user

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/lru-fs $(DESTDIR)$(USERUNITDIR)/lru-fs.service

check: lru-fs
	python3 tests/test.py

dist:
	git archive --format=tar.gz --prefix=lru-fs-$(VERSION)/ -o lru-fs-$(VERSION).tar.gz HEAD

clean:
	rm -f lru-fs lru-fs-*.tar.gz

.PHONY: install install-user uninstall check dist clean
