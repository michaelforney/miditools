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
	fprintf(stderr, "usage: alsaseq [-rw] [-f rfd,wfd] [-p client:port] [cmd...]\n");
	exit(1);
}

static void *
midireader(void *arg)
{
	int fd;
	ssize_t ret;
	size_t len;
	snd_seq_event_t *evt;
	unsigned char *pos, buf[1024];

	fd = *(int *)arg;
	for (;;) {
		ret = snd_seq_event_input(seq, &evt);
		if (ret < 0) {
			fprintf(stderr, "snd_seq_event_input: %s\n", snd_strerror(ret));
			if (ret == -ENOSPC)
				continue;
			exit(1);
		}
		ret = snd_midi_event_decode(dev, buf, sizeof buf, evt);
		if (ret < 0) {
			if (ret == -ENOENT)
				continue;  /* not a midi message */
			fatal("snd_midi_event_decode: %s", snd_strerror(ret));
		}
		len = ret;
		pos = buf;
		while (len > 0) {
			ret = write(fd, pos, len);
			if (ret < 0)
				fatal("write:");
			pos += ret;
			len -= ret;
		}
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
		ret = snd_seq_drain_output(seq);
		if (ret < 0)
			fatal("snd_seq_drain_output: %s", snd_strerror(ret));
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
	int err;
	snd_seq_port_info_t *info;
	snd_seq_addr_t dest, self;
	snd_seq_port_subscribe_t *sub;
	pthread_t thread;
	char *port, *end;
	int fd[2], mode, cap;

	mode = 0;
	port = NULL;
	fd[0] = 0;
	fd[1] = 1;
	ARGBEGIN {
	case 'r':
		mode |= READ;
		break;
	case 'w':
		mode |= WRITE;
		break;
	case 'p':
		port = EARGF(usage());
		break;
	case 'f':
		parseintpair(EARGF(usage()), fd);
		break;
	default:
		usage();
	} ARGEND

	if (mode == 0)
		mode = READ | WRITE;

	if (port) {
		dest.client = strtol(port, &end, 10);
		if (*end != ':')
			usage();
		dest.port = strtol(end + 1, &end, 10);
		if (*end)
			usage();
	}

	err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
	if (err)
		fatal("snd_seq_open: %s", snd_strerror(err));
	err = snd_seq_set_client_name(seq, "alsaseq");
	if (err) {
		fprintf(stderr, "snd_seq_set_client_name: %s\n", snd_strerror(err));
		return 1;
	}
	cap = 0;
	if (mode & READ)
		cap |= SND_SEQ_PORT_CAP_WRITE;
	if (mode & WRITE)
		cap |= SND_SEQ_PORT_CAP_READ;
	if (!port)
		cap |= SND_SEQ_PORT_CAP_SUBS_READ | SND_SEQ_PORT_CAP_SUBS_WRITE;
	err = snd_seq_create_simple_port(seq, "alsaseq", cap, SND_SEQ_PORT_TYPE_MIDI_GENERIC);
	if (err) {
		fprintf(stderr, "snd_seq_create_simple_port: %s\n", snd_strerror(err));
		return 1;
	}

	err = snd_seq_port_info_malloc(&info);
	if (err) {
		fprintf(stderr, "snd_seq_port_info_malloc: %s\n", snd_strerror(err));
		return 1;
	}
	if (port) {
		err = snd_seq_get_any_port_info(seq, dest.client, dest.port, info);
		if (err) {
			fprintf(stderr, "snd_seq_get_any_port_info: %s\n", snd_strerror(err));
			return 1;
		}
		setenv("MIDIPORT", snd_seq_port_info_get_name(info), 1);
		snd_seq_port_info_free(info);

		err = snd_seq_port_subscribe_malloc(&sub);
		if (err) {
			fprintf(stderr, "snd_seq_port_subscribe_malloc: %s\n", snd_strerror(err));
			return 1;
		}
		self.client = snd_seq_client_id(seq);
		self.port = 0;
		snd_seq_port_subscribe_set_sender(sub, &self);
		snd_seq_port_subscribe_set_dest(sub, &dest);
		err = snd_seq_subscribe_port(seq, sub);
		if (err) {
			fprintf(stderr, "snd_seq_subscribe_port: %s\n", snd_strerror(err));
			return 1;
		}
		snd_seq_port_subscribe_set_sender(sub, &dest);
		snd_seq_port_subscribe_set_dest(sub, &self);
		err = snd_seq_subscribe_port(seq, sub);
		if (err) {
			fprintf(stderr, "snd_seq_subscribe_port: %s\n", snd_strerror(err));
			return 1;
		}
	}

	err = snd_midi_event_new(1024, &dev);
	if (err) {
		fatal("snd_midi_event_new: %s", snd_strerror(err));
		return 1;
	}

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
