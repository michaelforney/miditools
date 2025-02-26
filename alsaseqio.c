#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#include <alsa/asoundlib.h>
#include "arg.h"
#include "fatal.h"

#define LEN(a) (sizeof (a) / sizeof *(a))

static snd_seq_t *seq;
static snd_midi_event_t *dev;

static void
usage(void)
{
	fprintf(stderr, "usage: alsaseq client:port cmd [arg...]\n");
	exit(1);
}

static void *
midiread(void *arg)
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
		if (ret < 0)
			fatal("snd_midi_event_decode: %s", snd_strerror(ret));
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

int
main(int argc, char *argv[])
{
	int err;
	ssize_t ret;
	size_t len;
	snd_seq_port_info_t *info;
	snd_seq_addr_t dest, self;
	snd_seq_port_subscribe_t *sub;
	snd_seq_event_t evt;
	pid_t pid;
	pthread_t thread;
	char *end;
	int rfd[2], wfd[2];
	unsigned char *pos, buf[1024];

	ARGBEGIN {
	default:
		usage();
	} ARGEND

	if (argc < 2)
		usage();

	dest.client = strtol(argv[0], &end, 10);
	if (*end != ':')
		usage();
	dest.port = strtol(end + 1, &end, 10);
	if (*end)
		usage();

	err = snd_seq_open(&seq, "default", SND_SEQ_OPEN_DUPLEX, 0);
	if (err)
		fatal("snd_seq_open: %s", snd_strerror(err));
	err = snd_seq_set_client_name(seq, "alsaseq");
	if (err)
		fatal("snd_seq_set_client_name: %s", snd_strerror(err));
	err = snd_seq_create_simple_port(seq, "alsaseq", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_WRITE, SND_SEQ_PORT_TYPE_MIDI_GENERIC);
	if (err)
		fatal("snd_seq_create_simple_port: %s", snd_strerror(err));

	err = snd_seq_port_info_malloc(&info);
	if (err)
		fatal("snd_seq_port_info_malloc: %s", snd_strerror(err));
	err = snd_seq_get_any_port_info(seq, dest.client, dest.port, info);
	if (err)
		fatal("snd_seq_get_any_port_info: %s", snd_strerror(err));
	setenv("MIDIPORT", snd_seq_port_info_get_name(info), 1);
	snd_seq_port_info_free(info);

	err = snd_seq_port_subscribe_malloc(&sub);
	if (err)
		fatal("snd_seq_port_subscribe_malloc: %s", snd_strerror(err));
	self.client = snd_seq_client_id(seq);
	self.port = 0;
	snd_seq_port_subscribe_set_sender(sub, &self);
	snd_seq_port_subscribe_set_dest(sub, &dest);
	err = snd_seq_subscribe_port(seq, sub);
	if (err)
		fatal("snd_seq_subscribe_port: %s", snd_strerror(err));
	snd_seq_port_subscribe_set_sender(sub, &dest);
	snd_seq_port_subscribe_set_dest(sub, &self);
	err = snd_seq_subscribe_port(seq, sub);
	if (err)
		fatal("snd_seq_subscribe_port: %s", snd_strerror(err));

	err = snd_midi_event_new(1024, &dev);
	if (err)
		fatal("snd_midi_event_new: %s", snd_strerror(err));

	if (pipe(wfd) != 0) {
		perror("pipe2");
		return 1;
	}
	if (pipe(rfd) != 0) {
		perror("pipe2");
		return 1;
	}
	pid = fork();
	switch (pid) {
	case -1:
		perror("fork");
		return 1;
	case 0:
		close(rfd[1]);
		close(wfd[0]);
		break;
	default:
		close(wfd[1]);
		close(rfd[0]);
		if (dup2(wfd[0], 6) < 0 || dup2(rfd[1], 7) < 0)
			fatal("dup2:");
		execvp(argv[1], argv + 1);
		fatal("execvp %s:", argv[1]);
	}

	err = pthread_create(&thread, NULL, midiread, &wfd[1]);
	if (err)
		fatal("pthread_create: %s", strerror(err));

	snd_seq_ev_set_source(&evt, 0);
	snd_seq_ev_set_subs(&evt);
	snd_seq_ev_set_direct(&evt);
	for (;;) {
		ret = read(rfd[0], buf, sizeof buf);
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
