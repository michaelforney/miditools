#define _POSIX_C_SOURCE 200809L
#include <limits.h>
#include <stdlib.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include "arg.h"
#include "fatal.h"
#include "spawn.h"

#define LEN(a) (sizeof (a) / sizeof *(a))

static snd_seq_t *seq;
static snd_midi_event_t *dev;

static void
usage(void)
{
	fprintf(stderr, "usage: alsaseqio [-rws] [-f rfd,wfd] [-n name] [-p client:port] [command...]\n"
	                "       alsaseqio -l\n");
	exit(1);
}

static int
getportmode(snd_seq_port_info_t *info)
{
	int caps, mode;

	caps = snd_seq_port_info_get_capability(info);
	mode = 0;
	if (caps & SND_SEQ_PORT_CAP_READ && caps & SND_SEQ_PORT_CAP_SUBS_READ)
		mode |= READ;
	if (caps & SND_SEQ_PORT_CAP_WRITE && caps & SND_SEQ_PORT_CAP_SUBS_WRITE)
		mode |= WRITE;
	return mode;
}

static void
listports(int mode)
{
	int err, portmode;

	snd_seq_client_info_t *clientinfo;
	snd_seq_port_info_t *portinfo;

	err = snd_seq_client_info_malloc(&clientinfo);
	if (err)
		fatal("snd_seq_client_info_malloc: %s", snd_strerror(err));
	err = snd_seq_port_info_malloc(&portinfo);
	if (err)
		fatal("snd_seq_client_info_malloc: %s", snd_strerror(err));

	snd_seq_client_info_set_client(clientinfo, -1);
	for (;;) {
		err = snd_seq_query_next_client(seq, clientinfo);
		if (err == -ENOENT)
			break;
		if (err)
			fatal("snd_seq_query_next_client: %s", snd_strerror(err));
		snd_seq_port_info_set_client(portinfo, snd_seq_client_info_get_client(clientinfo));
		snd_seq_port_info_set_port(portinfo, -1);
		for (;;) {
			err = snd_seq_query_next_port(seq, portinfo);
			if (err == -ENOENT)
				break;
			if (err)
				fatal("snd_seq_query_next_port: %s", snd_strerror(err));
			portmode = getportmode(portinfo);
			if (!(mode & portmode))
				continue;
			printf("%3d:%-3d %c%c\t%-32s %s\n",
				snd_seq_port_info_get_client(portinfo),
				snd_seq_port_info_get_port(portinfo),
				portmode & READ ? 'r' : '-',
				portmode & WRITE ? 'w' : '-',
				snd_seq_client_info_get_name(clientinfo),
				snd_seq_port_info_get_name(portinfo));
		}
	}
}

static void
writefull(int fd, const unsigned char *buf, size_t len)
{
	const unsigned char *pos;
	ssize_t ret;

	pos = buf;
	while (len > 0) {
		ret = write(fd, pos, len);
		if (ret < 0) {
			perror("write");
			exit(1);
		}
		pos += ret;
		len -= ret;
	}
}

static void *
midireader(void *arg)
{
	int fd;
	ssize_t ret;
	snd_seq_event_t *evt;
	unsigned char *pos, *end, buf[1024];

	fd = *(int *)arg;
	pos = buf;
	end = buf + sizeof buf;
	for (;;) {
		do {
			ret = snd_seq_event_input(seq, &evt);
			if (ret < 0) {
				fprintf(stderr, "snd_seq_event_input: %s\n", snd_strerror(ret));
				if (ret == -ENOSPC)
					continue;
				exit(1);
			}
		decode:
			ret = snd_midi_event_decode(dev, pos, end - pos, evt);
			if (ret < 0) {
				if (ret == -ENOENT)
					continue;  /* not a midi message */
				if (ret == -ENOMEM && pos != buf) {
					writefull(fd, buf, pos - buf);
					pos = buf;
					goto decode;
				}
				fatal("snd_midi_event_decode: %s", snd_strerror(ret));
			}
			pos += ret;
		} while (snd_seq_event_input_pending(seq, 0) && end - pos >= 3);
		writefull(fd, buf, pos - buf);
		pos = buf;
	}
	return NULL;
}

static void
inputreader(int fd)
{
	snd_seq_event_t evt;
	ssize_t ret;
	size_t len;
	unsigned char *pos, buf[1024];

	snd_seq_ev_set_source(&evt, 0);
	snd_seq_ev_set_subs(&evt);
	snd_seq_ev_set_direct(&evt);
	for (;;) {
		ret = read(fd, buf, sizeof buf);
		if (ret < 0) {
			perror("read");
			exit(1);
		}
		if (ret == 0)
			break;
		pos = buf;
		len = ret;
		while (len > 0) {
			ret = snd_midi_event_encode(dev, pos, len, &evt);
			if (ret < 0)
				fatal("snd_midi_event_encode: %s", snd_strerror(ret));
			pos += ret;
			len -= ret;
			if (evt.type != SND_SEQ_EVENT_NONE) {
				ret = snd_seq_event_output(seq, &evt);
				if (ret < 0)
					fatal("snd_seq_event_output: %s", snd_strerror(ret));
			}
		}
		do {
			ret = snd_seq_drain_output(seq);
			if (ret < 0 && ret != -EAGAIN)
				fatal("snd_seq_drain_output: %s", snd_strerror(ret));
		} while (ret != 0);
	}
}

static void
parseintpair(const char *arg, int num[static 2])
{
	char *end;
	long n;

	num[0] = -1;
	if (*arg != ',') {
		n = strtol(arg, &end, 10);
		if (end == arg || n < 0 || n > INT_MAX)
			usage();
		num[0] = (int)n;
		if (!*end) {
			num[1] = num[0];
			return;
		}
		if (*end != ',')
			usage();
		arg = end + 1;
	}
	num[1] = -1;
	if (*arg) {
		n = strtol(arg, &end, 10);
		if (end == arg || *end || n < 0 || n > INT_MAX)
			usage();
		num[1] = (int)n;
	}
}

int
main(int argc, char *argv[])
{
	int err, lflag, sflag;
	snd_seq_port_info_t *info;
	snd_seq_addr_t dest, self;
	snd_seq_port_subscribe_t *sub;
	pthread_t thread;
	char *port, *name;
	int fd[2], mode, cap;

	mode = 0;
	lflag = 0;
	sflag = 0;
	name = "alsaseqio";
	port = NULL;
	fd[0] = 0;
	fd[1] = 1;
	ARGBEGIN {
	case 'l':
		lflag = 1;
		break;
	case 'r':
		mode |= READ;
		break;
	case 'w':
		mode |= WRITE;
		break;
	case 'n':
		name = EARGF(usage());
		break;
	case 'p':
		port = EARGF(usage());
		break;
	case 's':
		sflag = 1;
		break;
	case 'f':
		parseintpair(EARGF(usage()), fd);
		break;
	default:
		usage();
	} ARGEND

	if (mode == 0)
		mode = READ | WRITE;

	err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
	if (err)
		fatal("snd_seq_open: %s", snd_strerror(err));
	if (lflag) {
		listports(mode);
		return 0;
	}
	err = snd_seq_set_client_name(seq, name);
	if (err)
		fatal("snd_seq_set_client_name: %s", snd_strerror(err));
	if (port) {
		err = snd_seq_parse_address(seq, &dest, port);
		if (err)
			fatal("snd_seq_parse_address '%s': %s", port, snd_strerror(err));
	}
	cap = 0;
	if (mode & READ)
		cap |= SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE;
	if (mode & WRITE)
		cap |= SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
	if (port && !sflag)
		cap &= ~(SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_SUBS_WRITE);
	err = snd_seq_create_simple_port(seq, name, cap, SND_SEQ_PORT_TYPE_MIDI_GENERIC);
	if (err)
		fatal("snd_seq_create_simple_port: %s", snd_strerror(err));

	err = snd_seq_port_info_malloc(&info);
	if (err)
		fatal("snd_seq_port_info_malloc: %s", snd_strerror(err));
	self.client = snd_seq_client_id(seq);
	self.port = 0;
	if (!port || sflag)
		fprintf(stderr, "using port %d:%d\n", self.client, self.port);
	if (port) {
		err = snd_seq_get_any_port_info(seq, dest.client, dest.port, info);
		if (err)
			fatal("snd_seq_get_any_port_info: %s", snd_strerror(err));
		setenv("MIDIPORT", snd_seq_port_info_get_name(info), 1);
		snd_seq_port_info_free(info);

		mode &= getportmode(info);
		if (!mode)
			fatal("port '%s' does not have any matching I/O capabilities");
		err = snd_seq_port_subscribe_malloc(&sub);
		if (err)
			fatal("snd_seq_port_subscribe_malloc: %s", snd_strerror(err));
		if (mode & READ) {
			snd_seq_port_subscribe_set_sender(sub, &dest);
			snd_seq_port_subscribe_set_dest(sub, &self);
			err = snd_seq_subscribe_port(seq, sub);
			if (err)
				fatal("snd_seq_subscribe_port: %s", snd_strerror(err));
		}
		if (mode & WRITE) {
			snd_seq_port_subscribe_set_sender(sub, &self);
			snd_seq_port_subscribe_set_dest(sub, &dest);
			err = snd_seq_subscribe_port(seq, sub);
			if (err)
				fatal("snd_seq_subscribe_port: %s", snd_strerror(err));
		}
	}

	err = snd_midi_event_new(1024, &dev);
	if (err)
		fatal("snd_midi_event_new: %s", snd_strerror(err));

	if (argc)
		spawn(argv[0], argv, mode, fd);

	if (mode & READ) {
		if (mode & WRITE) {
			err = pthread_create(&thread, NULL, midireader, &fd[1]);
			if (err)
				fatal("pthread_create: %s", strerror(err));
		} else {
			midireader(&fd[1]);
		}
	}
	if (mode & WRITE)
		inputreader(fd[0]);
}
