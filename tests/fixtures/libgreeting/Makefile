CC ?= cc
PREFIX ?= /usr/local
all: libgreeting.a
libgreeting.a: greeting.o
	ar rcs $@ $<
greeting.o: src/greeting.c
	$(CC) $(CFLAGS) -c -o $@ $<
install: all
	install -D greeting.o $(DESTDIR)$(PREFIX)/lib/greeting.o
	install -D src/greeting.h $(DESTDIR)$(PREFIX)/include/greeting.h
	install -D libgreeting.a $(DESTDIR)$(PREFIX)/lib/libgreeting.a
check: all
	@true
