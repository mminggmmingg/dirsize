CC      ?= cc
CFLAGS  ?= -O2 -Wall -Wextra
PREFIX  ?= /usr/local

dirsize: dirsize.c
	$(CC) $(CFLAGS) -pthread -o $@ $< -lm

install: dirsize
	install -Dm755 dirsize $(DESTDIR)$(PREFIX)/bin/dirsize

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/dirsize

clean:
	rm -f dirsize

.PHONY: install uninstall clean
