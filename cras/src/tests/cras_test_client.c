/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <alsa/asoundlib.h>
#include <getopt.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>

#include "cras_client.h"
#include "cras_types.h"

#define CAPTURE_CB_THRESHOLD (4800*5)
#define PLAYBACK_CB_THRESHOLD (480)
#define PLAYBACK_BUFFER_SIZE (4800)

static uint8_t *file_buf;
static size_t file_buf_size;
static size_t file_buf_read_offset;
static struct timespec last_latency;
static int show_latency;
static int keep_looping = 1;
static int full_frames;
uint32_t min_cb_level;

/* Run from callback thread. */
static int got_samples(struct cras_client *client, cras_stream_id_t stream_id,
		       uint8_t *samples, size_t frames,
		       const struct timespec *sample_time, void *arg)
{
	int *fd = (int *)arg;
	int ret;

	cras_client_calc_latency(client, stream_id, sample_time, &last_latency);

	ret = write(*fd, samples, frames * 4);
	if (ret != frames * 4)
		printf("Error writing file\n");
	return frames;
}

/* Run from callback thread. */
static int put_samples(struct cras_client *client, cras_stream_id_t stream_id,
		       uint8_t *samples, size_t frames,
		       const struct timespec *sample_time, void *arg)
{
	size_t this_size;
	snd_pcm_uframes_t avail;
	uint32_t frame_bytes = cras_client_bytes_per_frame(client, stream_id);

	if (file_buf_read_offset >= file_buf_size)
		return EOF;

	if (frames < min_cb_level)
		printf("req for only %zu - %d min\n", frames, min_cb_level);
	avail = frames * frame_bytes;

	this_size = file_buf_size - file_buf_read_offset;
	if (this_size > avail)
		this_size = avail;

	if (full_frames && this_size > min_cb_level * frame_bytes)
		this_size = min_cb_level * frame_bytes;

	cras_client_calc_latency(client, stream_id, sample_time, &last_latency);

	memcpy(samples, file_buf + file_buf_read_offset, this_size);
	file_buf_read_offset += this_size;

	return this_size / frame_bytes;
}

static int stream_error(struct cras_client *client, cras_stream_id_t stream_id,
			int err)
{
	printf("Stream error %d\n", err);
	keep_looping = 0;
	return 0;
}

static void print_last_latency()
{
	printf("%u.%09u\n", (unsigned)last_latency.tv_sec,
	       (unsigned)last_latency.tv_nsec);
}

static int run_file_io_stream(struct cras_client *client,
			      int fd,
			      enum CRAS_STREAM_DIRECTION direction,
			      uint32_t buffer_frames,
			      uint32_t cb_threshold,
			      uint32_t rate,
			      int flags)
{
	struct cras_stream_params *params;
	struct cras_audio_format *aud_format;
	cras_playback_cb_t aud_cb;
	cras_stream_id_t stream_id = 0;
	int stream_playing = 0;
	int *pfd = malloc(sizeof(*pfd));
	*pfd = fd;
	fd_set poll_set;
	struct timespec sleep_ts;

	sleep_ts.tv_sec = 0;
	sleep_ts.tv_nsec = 250 * 1000000;

	if (direction == CRAS_STREAM_INPUT)
		aud_cb = got_samples;
	else
		aud_cb = put_samples;

	aud_format = cras_audio_format_create(SND_PCM_FORMAT_S16_LE, rate, 2);
	if (aud_format == NULL)
		return -ENOMEM;

	params = cras_client_stream_params_create(direction,
						  buffer_frames,
						  cb_threshold,
						  min_cb_level,
						  0,
						  0,
						  pfd,
						  aud_cb,
						  stream_error,
						  aud_format);
	if (params == NULL)
		return -ENOMEM;

	cras_client_run_thread(client);

	while (keep_looping) {
		char input;
		int nread, err;

		FD_ZERO(&poll_set);
		FD_SET(1, &poll_set);
		sleep_ts.tv_sec = 0;
		sleep_ts.tv_nsec = 750 * 1000000;
		pselect(2, &poll_set, NULL, NULL, &sleep_ts, NULL);

		if (stream_playing && show_latency)
			print_last_latency();

		if (!FD_ISSET(1, &poll_set))
			continue;

		nread = read(1, &input, 1);
		if (nread < 1) {
			fprintf(stderr, "Error reading stdin\n");
			return nread;
		}
		switch (input) {
		case 'q':
			keep_looping = 0;
			break;
		case 's':
			if (!stream_playing) {
				file_buf_read_offset = 0;
				err = cras_client_add_stream(client,
							     &stream_id,
							     params);
				if (err < 0) {
					fprintf(stderr, "adding a stream\n");
					break;
				}
				stream_playing = 1;
			}
			break;
		case 'r':
			if (stream_playing) {
				cras_client_rm_stream(client, stream_id);
				stream_playing = 0;
			}
			break;
		case '\n':
			break;
		default:
			printf("Invalid key\n");
			break;
		}
	}
	cras_client_stop(client);

	cras_audio_format_destroy(aud_format);
	cras_client_stream_params_destroy(params);
	free(pfd);

	return 0;
}

static int run_capture(struct cras_client *client,
		       const char *file,
		       uint32_t buffer_frames,
		       uint32_t cb_threshold,
		       uint32_t rate,
		       int flags)
{
	int fd = open(file, O_CREAT | O_RDWR, 0666);

	run_file_io_stream(client, fd, CRAS_STREAM_INPUT, buffer_frames,
			   cb_threshold, rate, flags);

	close(fd);
	return 0;
}

static int run_playback(struct cras_client *client,
			const char *file,
			uint32_t buffer_frames,
			uint32_t cb_threshold,
			uint32_t rate,
			int flags)
{
	int fd;

	file_buf = malloc(1024*1024*4);
	if (!file_buf) {
		perror("allocating file_buf");
		return -ENOMEM;
	}

	fd = open(file, O_RDONLY);
	file_buf_size = read(fd, file_buf, 1024*1024*4);

	run_file_io_stream(client, fd, CRAS_STREAM_OUTPUT, buffer_frames,
			   cb_threshold, rate, flags);

	close(fd);
	return 0;
}

static struct option long_options[] = {
	{"show_latency",	no_argument, &show_latency, 1},
	{"write_full_frames",	no_argument, &full_frames, 1},
	{"rate",		required_argument,	0, 'r'},
	{"iodev_index",		required_argument,	0, 'o'},
	{"stream_type",		required_argument,	0, 's'},
	{"capture_file",	required_argument,	0, 'c'},
	{"playback_file",	required_argument,	0, 'p'},
	{"callback_threshold",	required_argument,	0, 't'},
	{"min_cb_level",	required_argument,	0, 'm'},
	{"buffer_frames",	required_argument,	0, 'b'},
	{0, 0, 0, 0}
};

int main(int argc, char **argv)
{
	struct cras_client *client;
	int c, err, option_index;
	int32_t buffer_size = -1;
	int32_t cb_threshold = -1;
	int32_t rate = -1;
	int32_t iodev_index = -1;
	enum CRAS_STREAM_TYPE stream_type = CRAS_STREAM_TYPE_DEFAULT;
	const char *capture_file = NULL;
	const char *playback_file = NULL;

	option_index = 0;

	while (1) {
		c = getopt_long(argc, argv, "o:s:",
				long_options, &option_index);
		if (c == -1)
			break;
		switch (c) {
		case 'c':
			capture_file = optarg;
			break;
		case 'p':
			playback_file = optarg;
			break;
		case 't':
			cb_threshold = atoi(optarg);
			break;
		case 'm':
			min_cb_level = atoi(optarg);
			break;
		case 'b':
			buffer_size = atoi(optarg);
			break;
		case 'r':
			rate = atoi(optarg);
			break;
		case 'o':
			iodev_index = atoi(optarg);
			break;
		case 's':
			stream_type = (enum CRAS_STREAM_TYPE)atoi(optarg);
			break;
		default:
			break;
		}
	}

	err = cras_client_create(&client);
	if (err < 0) {
		fprintf(stderr, "Couldn't create client.\n");
		return err;
	}

	err = cras_client_connect(client);
	if (err)
		return err;

	if (rate == -1)
		rate = 48000;

	if (iodev_index >= 0)
		cras_client_switch_iodev(client, stream_type, iodev_index);

	if (capture_file != NULL) {
		if (buffer_size == -1)
			buffer_size = CAPTURE_CB_THRESHOLD;
		run_capture(client, capture_file, buffer_size, 0, rate, 0);
	}

	if (playback_file != NULL) {
		if (cb_threshold == -1)
			cb_threshold = PLAYBACK_CB_THRESHOLD;
		if (buffer_size == -1)
			buffer_size = PLAYBACK_BUFFER_SIZE;
		if (min_cb_level == -1)
			min_cb_level = buffer_size/2;
		run_playback(client, playback_file, buffer_size, cb_threshold,
			     rate, 0);
	}

	cras_client_destroy(client);

	return 0;
}