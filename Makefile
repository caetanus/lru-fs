PREFIX ?= $(HOME)/.local
CFLAGS ?= -O2 -g -Wall -Wextra
FUSE_CFLAGS := $(shell pkg-config --cflags fuse3)
FUSE_LIBS := $(shell pkg-config --libs fuse3)

lru-fs: lru-fs.c
	$(CC) $(CFLAGS) $(FUSE_CFLAGS) -o $@ $< $(FUSE_LIBS) -lpthread

install: install-bin
	install -Dm644 lru-fs.service $(HOME)/.config/systemd/user/lru-fs.service

# only the binary; for fstab use: sudo make install-bin PREFIX=/usr/local
install-bin: lru-fs
	install -Dm755 lru-fs $(PREFIX)/bin/lru-fs

clean:
	rm -f lru-fs

.PHONY: install install-bin clean
