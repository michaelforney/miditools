.POSIX:

PREFIX=/usr/local
BINDIR=$(PREFIX)/bin
MANDIR=$(PREFIX)/share/man

-include config.mk

OS!=uname
OS?=$(shell uname)
OS-$(OS)=y
ALSA?=$(OS-Linux)
ALSA_CFLAGS?=$$(pkg-config --cflags alsa)
ALSA_LDFLAGS?=$$(pkg-config --libs-only-L --libs-only-other alsa)
ALSA_LDLIBS?=$$(pkg-config --libs-only-l alsa)

COREMIDI?=$(OS-Darwin)
COREMIDI_LDLIBS?=-framework CoreMIDI -framework CoreFoundation

BIN=$(BIN-y)
BIN-$(ALSA)+=alsarawio alsaseqio
BIN-$(COREMIDI)+=coremidiio

MAN=$(MAN-y)
MAN-$(ALSA)+=alsaseqio.1

TARGET=$(BIN)

all: $(TARGET)

alsarawio: alsarawio.o
	$(CC) $(LDFLAGS) -o $@ alsarawio.o

alsaseqio.o: alsaseqio.c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(ALSA_CFLAGS) -c -o $@ alsaseqio.c

ALSASEQIO_OBJ=alsaseqio.o fatal.o spawn.o
alsaseqio: $(ALSASEQIO_OBJ)
	$(CC) $(LDFLAGS) $(ALSA_LDFLAGS) -o $@ $(ALSASEQIO_OBJ) $(ALSA_LDLIBS) -l pthread

COREMIDIIO_OBJ=coremidiio.o fatal.o spawn.o
coremidiio: $(COREMIDIIO_OBJ)
	$(CC) $(LDFLAGS) -o $@ $(COREMIDIIO_OBJ) $(COREMIDI_LDLIBS)

.PHONY: install
install: $(BIN)
	mkdir -p $(DESTDIR)$(BINDIR)
	cp $(BIN) $(DESTDIR)$(BINDIR)/
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	cp $(MAN) $(DESTDIR)$(MANDIR)/man1

.PHONY: clean
clean:
	rm -f alsarawio alsarawio.o\
		alsaseqio alsaseqio.o\
		coremidiio coremidiio.o\
		fatal.o spawn.o
