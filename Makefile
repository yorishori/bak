CC ?= cc
CFLAGS ?= -O2 -std=c99 -Wall -Wextra -Wpedantic
PREFIX ?= /usr/local

bak: bak.c
	$(CC) $(CFLAGS) -o $@ bak.c

install: bak
	install -Dm755 bak $(DESTDIR)$(PREFIX)/bin/bak

clean:
	rm -f bak

.PHONY: install clean
