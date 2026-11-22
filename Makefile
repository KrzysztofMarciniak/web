CC       ?= cc
CFLAGS   ?= -O2
STD      = -std=c89 -pedantic -Wall -Wextra
PREFIX   ?= /usr/local
BINS     = web_plain web_pager

all: $(BINS)

web_plain: web_plain.c
	$(CC) $(STD) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ web_plain.c

web_pager: web_pager.c
	$(CC) $(STD) $(CFLAGS) $(CPPFLAGS) $(LDFLAGS) -o $@ web_pager.c

install: all
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 $(BINS) $(DESTDIR)$(PREFIX)/bin

uninstall:
	cd $(DESTDIR)$(PREFIX)/bin && rm -f $(BINS)

clean:
	rm -f $(BINS)

.PHONY: all install uninstall clean
