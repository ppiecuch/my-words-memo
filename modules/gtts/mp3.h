// Reference:
// ----------
// https://lauri.võsandi.com/2013/12/implementing-mp3-player.en.html

#include <mad.h>
#include <pulse/error.h>
#include <pulse/simple.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

struct mad_player_t {
	pa_simple *device = NULL;
	int ret = 1;
	int error;
	struct mad_stream mad_stream;
	struct mad_frame mad_frame;
	struct mad_synth mad_synth;

	void output(struct mad_header const *header, struct mad_pcm *pcm);

	int main(int argc, char **argv) {
		// Parse command-line arguments
		if (argc != 2) {
			fprintf(stderr, "Usage: %s [filename.mp3]", argv[0]);
			return 255;
		}

		// Set up PulseAudio 16-bit 44.1kHz stereo output
		static const pa_sample_spec ss = { .format = PA_SAMPLE_S16LE, .rate = 44100, .channels = 2 };
		if (!(device = pa_simple_new(NULL, "MP3 player", PA_STREAM_PLAYBACK, NULL, "playback", &ss, NULL, NULL, &error))) {
			printf("pa_simple_new() failed\n");
			return 255;
		}

		// Initialize MAD library
		mad_stream_init(&mad_stream);
		mad_synth_init(&mad_synth);
		mad_frame_init(&mad_frame);

		// Filename pointer
		char *filename = argv[1];

		// File pointer
		FILE *fp = fopen(filename, "r");
		int fd = fileno(fp);

		// Fetch file size, etc
		struct stat metadata;
		if (fstat(fd, &metadata) >= 0) {
			printf("File size %d bytes\n", (int)metadata.st_size);
		} else {
			printf("Failed to stat %s\n", filename);
			fclose(fp);
			return 254;
		}

		// Let kernel do all the dirty job of buffering etc, map file contents to memory
		char *input_stream = mmap(0, metadata.st_size, PROT_READ, MAP_SHARED, fd, 0);

		// Copy pointer and length to mad_stream struct
		mad_stream_buffer(&mad_stream, input_stream, metadata.st_size);

		// Decode frame and synthesize loop
		while (1) {
			// Decode frame from the stream
			if (mad_frame_decode(&mad_frame, &mad_stream)) {
				if (MAD_RECOVERABLE(mad_stream.error)) {
					continue;
				} else if (mad_stream.error == MAD_ERROR_BUFLEN) {
					continue;
				} else {
					break;
				}
			}
			// Synthesize PCM data of frame
			mad_synth_frame(&mad_synth, &mad_frame);
			output(&mad_frame.header, &mad_synth.pcm);
		}

		// Close
		fclose(fp);

		// Free MAD structs
		mad_synth_finish(&mad_synth);
		mad_frame_finish(&mad_frame);
		mad_stream_finish(&mad_stream);

		// Close PulseAudio output
		if (device)
			pa_simple_free(device);

		return EXIT_SUCCESS;
	}

	// Some helper functions, to be cleaned up in the future
	int scale(mad_fixed_t sample) {
		/* round */
		sample += (1L << (MAD_F_FRACBITS - 16));
		/* clip */
		if (sample >= MAD_F_ONE)
			sample = MAD_F_ONE - 1;
		else if (sample < -MAD_F_ONE)
			sample = -MAD_F_ONE;
		/* quantize */
		return sample >> (MAD_F_FRACBITS + 1 - 16);
	}
	void output(struct mad_header const *header, struct mad_pcm *pcm) {
		register int nsamples = pcm->length;
		mad_fixed_t const *left_ch = pcm->samples[0], *right_ch = pcm->samples[1];
		static char stream[1152 * 4];
		if (pcm->channels == 2) {
			while (nsamples--) {
				signed int sample;
				sample = scale(*left_ch++);
				stream[(pcm->length - nsamples) * 4] = ((sample >> 0) & 0xff);
				stream[(pcm->length - nsamples) * 4 + 1] = ((sample >> 8) & 0xff);
				sample = scale(*right_ch++);
				stream[(pcm->length - nsamples) * 4 + 2] = ((sample >> 0) & 0xff);
				stream[(pcm->length - nsamples) * 4 + 3] = ((sample >> 8) & 0xff);
			}
			if (pa_simple_write(device, stream, (size_t)1152 * 4, &error) < 0) {
				fprintf(stderr, "pa_simple_write() failed: %s\n", pa_strerror(error));
				return;
			}
		} else {
			printf("Mono not supported!");
		}
	}
};

/* minmad - a minimal mp3 player using libmad and oss */

#include <ctype.h>
#include <fcntl.h>
#include <mad.h>
#include <poll.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/soundcard.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))

static struct mad_decoder maddec;
static int afd; /* oss fd */

static char filename[128];
static char *ossdsp; /* oss device */
static int mfd; /* input file descriptor */
static long msize; /* file size */
static unsigned char mbuf[1 << 16];
static long mpos; /* the position of mbuf[] */
static long mlen; /* data in mbuf[] */
static long moff; /* offset into mbuf[] */
static long mark[256]; /* mark positions */
static int frame_sz; /* frame size */
static int frame_ms; /* frame duration in milliseconds */
static int played; /* playing time in milliseconds */
static int rate; /* current oss sample rate */
static int topause; /* planned pause (compared with played) */

static int exited;
static int paused;
static int domark;
static int dojump;
static int doseek;
static int count;

static int oss_open(void) {
	afd = open(ossdsp ? ossdsp : "/dev/dsp", O_WRONLY);
	return afd < 0;
}

static void oss_close(void) {
	if (afd > 0) /* zero fd is used for input */
		close(afd);
	afd = 0;
	rate = 0;
}

static void oss_conf(int rate, int ch, int bits) {
	int frag = 0x0003000b; /* 0xmmmmssss: 2^m fragments of size 2^s each */
	ioctl(afd, SOUND_PCM_WRITE_CHANNELS, &ch);
	ioctl(afd, SOUND_PCM_WRITE_BITS, &bits);
	ioctl(afd, SOUND_PCM_WRITE_RATE, &rate);
	ioctl(afd, SOUND_PCM_SETFRAGMENT, &frag);
}

static int cmdread(void) {
	char b;
	if (read(0, &b, 1) <= 0)
		return -1;
	return (unsigned char)b;
}

static void cmdwait(void) {
	struct pollfd ufds[1];
	ufds[0].fd = 0;
	ufds[0].events = POLLIN;
	poll(ufds, 1, -1);
}

static long muldiv64(long num, long mul, long div) {
	return (long long)num * mul / div;
}

static void cmdinfo(void) {
	int per = muldiv64(mpos + moff, 1000, msize);
	int loc = muldiv64(mpos + moff, frame_ms, frame_sz * 1000);
	printf("%c %02d.%d%%  (%d:%02d:%02d - %04d.%ds)   [%s]\r",
			paused ? (afd < 0 ? '*' : ' ') : '>',
			per / 10, per % 10,
			loc / 3600, (loc % 3600) / 60, loc % 60,
			played / 1000, (played / 100) % 10,
			filename);
	fflush(stdout);
}

static int cmdcount(int def) {
	int result = count ? count : def;
	count = 0;
	return result;
}

static void seek(long pos) {
	mark['\''] = mpos + moff;
	mpos = MAX(0, MIN(msize, pos));
	doseek = 1;
}

static void cmdseekrel(int n) {
	int diff = muldiv64(n, frame_sz * 1000, frame_ms ? frame_ms : 40);
	seek(mpos + moff + diff);
}

static void cmdseek100(int n) {
	if (n <= 100)
		seek(muldiv64(msize, n, 100));
}

static void cmdseek(int n) {
	long pos = muldiv64(n * 60, frame_sz * 1000, frame_ms ? frame_ms : 40);
	seek(pos);
}

static int cmdpause(int pause) {
	if (!pause && paused) {
		if (oss_open())
			return 1;
		paused = 0;
	}
	if (pause && !paused) {
		oss_close();
		paused = 1;
	}
	return 0;
}

static int cmdexec(void) {
	int c;
	if (topause > 0 && topause <= played) {
		topause = 0;
		return !cmdpause(1);
	}
	while ((c = cmdread()) >= 0) {
		if (domark) {
			domark = 0;
			mark[c] = mpos + moff;
			return 0;
		}
		if (dojump) {
			dojump = 0;
			if (mark[c] > 0)
				seek(mark[c]);
			return mark[c] > 0;
		}
		switch (c) {
			case 'J':
				cmdseekrel(+600 * cmdcount(1));
				return 1;
			case 'K':
				cmdseekrel(-600 * cmdcount(1));
				return 1;
			case 'j':
				cmdseekrel(+60 * cmdcount(1));
				return 1;
			case 'k':
				cmdseekrel(-60 * cmdcount(1));
				return 1;
			case 'l':
				cmdseekrel(+10 * cmdcount(1));
				return 1;
			case 'h':
				cmdseekrel(-10 * cmdcount(1));
				return 1;
			case '%':
				cmdseek100(cmdcount(0));
				return 1;
			case 'G':
				cmdseek(cmdcount(0));
				return 1;
			case 'i':
				cmdinfo();
				break;
			case 'm':
				domark = 1;
				break;
			case '\'':
				dojump = 1;
				break;
			case 'p':
			case ' ':
				if (cmdpause(!paused))
					break;
				return 1;
			case 'P':
				topause = count ? played + cmdcount(0) * 60000 : 0;
				break;
			case 'q':
				exited = 1;
				return 1;
			case 27:
				count = 0;
				break;
			default:
				if (isdigit(c))
					count = count * 10 + c - '0';
		}
	}
	return 0;
}

static enum mad_flow madinput(void *data, struct mad_stream *stream) {
	int nread = stream->next_frame ? stream->next_frame - mbuf : moff;
	int nleft = mlen - nread;
	int nr = 0;
	if (doseek) {
		doseek = 0;
		nleft = 0;
		nread = 0;
		lseek(mfd, mpos, 0);
	}
	memmove(mbuf, mbuf + nread, nleft);
	if (nleft < sizeof(mbuf)) {
		if ((nr = read(mfd, mbuf + nleft, sizeof(mbuf) - nleft)) <= 0) {
			exited = 1;
			return MAD_FLOW_STOP;
		}
	}
	mlen = nleft + nr;
	mad_stream_buffer(stream, mbuf, mlen);
	mpos += nread;
	moff = 0;
	return MAD_FLOW_CONTINUE;
}

static signed int madscale(mad_fixed_t sample) {
	sample += (1l << (MAD_F_FRACBITS - 16)); /* round */
	if (sample >= MAD_F_ONE) /* clip */
		sample = MAD_F_ONE - 1;
	if (sample < -MAD_F_ONE)
		sample = -MAD_F_ONE;
	return sample >> (MAD_F_FRACBITS + 1 - 16); /* quantize */
}

static void madupdate(void) {
	int sz, ms;
	if (maddec.sync) {
		moff = maddec.sync->stream.this_frame - mbuf;
		sz = maddec.sync->stream.next_frame -
				maddec.sync->stream.this_frame;
		ms = mad_timer_count(maddec.sync->frame.header.duration,
				MAD_UNITS_MILLISECONDS);
		frame_ms = frame_ms ? ((frame_ms << 5) - frame_ms + ms) >> 5 : ms;
		frame_sz = frame_sz ? ((frame_sz << 5) - frame_sz + sz) >> 5 : sz;
	}
}

static char mixed[1 << 18];
static enum mad_flow madoutput(void *data, struct mad_header const *header, struct mad_pcm *pcm) {
	int c1 = 0;
	int c2 = pcm->channels > 1 ? 1 : 0;
	played += mad_timer_count(maddec.sync->frame.header.duration, MAD_UNITS_MILLISECONDS);
	for (int i = 0; i < pcm->length; i++) {
		mixed[i * 4 + 0] = madscale(pcm->samples[c1][i]) & 0xff;
		mixed[i * 4 + 1] = (madscale(pcm->samples[c1][i]) >> 8) & 0xff;
		mixed[i * 4 + 2] = madscale(pcm->samples[c2][i]) & 0xff;
		mixed[i * 4 + 3] = (madscale(pcm->samples[c2][i]) >> 8) & 0xff;
	}
	if (header->samplerate != rate) {
		rate = header->samplerate;
		oss_conf(rate, 2, 16);
	}
	write(afd, mixed, pcm->length * 4);
	madupdate();
	return cmdexec() ? MAD_FLOW_STOP : MAD_FLOW_CONTINUE;
}

static enum mad_flow maderror(void *data, struct mad_stream *stream, struct mad_frame *frame) {
	return MAD_FLOW_CONTINUE;
}

static void maddecode(void) {
	mad_decoder_init(&maddec, NULL, madinput, 0, 0, madoutput, maderror, 0);
	while (!exited) {
		if (paused) {
			cmdwait();
			cmdexec();
		} else {
			mad_decoder_run(&maddec, MAD_DECODER_MODE_SYNC);
		}
	}
	mad_decoder_finish(&maddec);
}

static void term_init(struct termios *termios) {
	struct termios newtermios;
	tcgetattr(0, termios);
	newtermios = *termios;
	newtermios.c_lflag &= ~ICANON;
	newtermios.c_lflag &= ~ECHO;
	tcsetattr(0, TCSAFLUSH, &newtermios);
	fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
}

static void term_done(struct termios *termios) {
	tcsetattr(0, 0, termios);
}

int main(int argc, char *argv[]) {
	struct stat stat;
	struct termios termios;
	char *path = argc >= 2 ? argv[1] : NULL;
	if (!path)
		return 1;
	if (strchr(path, '/'))
		path = strrchr(path, '/') + 1;
	snprintf(filename, 30, "%s", path);
	mfd = open(argv[1], O_RDONLY);
	if (fstat(mfd, &stat) == -1 || stat.st_size == 0)
		return 1;
	msize = stat.st_size;
	ossdsp = getenv("OSSDSP");
	if (oss_open()) {
		fprintf(stderr, "minmad: /dev/dsp busy?\n");
		return 1;
	}
	term_init(&termios);
	maddecode();
	oss_close();
	term_done(&termios);
	close(mfd);
	printf("\n");
	return 0;
}
