CC ?= cc
CFLAGS ?= -O2
PREFIX ?= /usr/local
WARN = -std=c99 -Wall -Wextra -Wpedantic

bak: bak.c
	$(CC) $(WARN) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ bak.c

install: bak
	install -Dm755 bak $(DESTDIR)$(PREFIX)/bin/bak
	install -Dm644 README.md $(DESTDIR)$(PREFIX)/share/doc/bak/README.md

clean:
	rm -f bak

.PHONY: install clean
