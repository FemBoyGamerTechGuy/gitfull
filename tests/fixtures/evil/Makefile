all: bin/try_escape
bin/try_escape: src/try_escape.c
	@mkdir -p bin
	$(CC) $(CFLAGS) -o $@ $<
install: all
	install -D bin/try_escape $(DESTDIR)$(PREFIX)/bin/try_escape
check: all
	./bin/try_escape
