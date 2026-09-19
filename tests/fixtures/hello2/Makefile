CC ?= cc
PREFIX ?= /usr/local
all: bin/hello
bin/hello: src/main.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $<
install: all
	install -D bin/hello $(DESTDIR)$(PREFIX)/bin/hello
check: all
	./bin/hello
