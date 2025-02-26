.POSIX:

PREFIX=/usr/local
BINDIR=$(PREFIX)/bin
MANDIR=$(PREFIX)/share/man

-include config.mk

ALSA?=y
ALSA_CFLAGS?=$$(pkg-config --cflags alsa)
ALSA_LDFLAGS?=$$(pkg-config --libs-only-L --libs-only-other alsa)
ALSA_LDLIBS?=$$(pkg-config --libs-only-l alsa)

COREMIDI?=n

BIN=$(BIN-y)
BIN-$(ALSA)+=alsarawio alsaseqio
BIN-$(COREMIDI)+=coremidiio

TARGET=$(BIN)

all: $(TARGET)

alsarawio: alsarawio.o
	$(CC) $(LDFLAGS) -o $@ alsarawio.o

alsaseqio.o: alsaseqio.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ALSA_CFLAGS) -c -o $@ alsaseqio.c

ALSASEQIO_OBJ=alsaseqio.o fatal.o
alsaseqio: $(ALSASEQIO_OBJ)
	$(CC) $(LDFLAGS) $(ALSA_LDFLAGS) -o $@ $(ALSASEQIO_OBJ) $(ALSA_LDLIBS) -l pthread

COREMIDIIO_OBJ=coremidiio.o fatal.o
coremidiio: $(COREMIDIIO_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(COREMIDIIO_OBJ) -framework CoreMIDI -framework CoreFoundation

.PHONY: install
install: $(BIN)
	mkdir -p $(DESTDIR)$(BINDIR)
	cp $(BIN) $(DESTDIR)$(BINDIR)/
	mkdir -p $(DESTDIR)$(MANDIR)/man1

.PHONY: clean
clean:
	rm -f alsarawio alsarawio.o\
		alsaseqio alsaseqio.o\
		coremidiio coremidiio.o
