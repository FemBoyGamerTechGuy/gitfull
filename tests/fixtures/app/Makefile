CC ?= cc
PREFIX ?= /usr/local
all: bin/app
bin/app: src/main.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $< -lgreeting
install: all
	install -D bin/app $(DESTDIR)$(PREFIX)/bin/app
check: all
	./bin/app
