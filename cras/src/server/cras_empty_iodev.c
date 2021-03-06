/* Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <pthread.h>
#include <syslog.h>

#include "cras_config.h"
#include "cras_iodev.h"
#include "cras_iodev_list.h"
#include "cras_rstream.h"
#include "cras_types.h"
#include "utlist.h"

#define EMPTY_BUFFER_SIZE (16 * 1024)
#define EMPTY_FRAME_SIZE 4
#define EMPTY_FRAMES (EMPTY_BUFFER_SIZE / EMPTY_FRAME_SIZE)

static size_t empty_supported_rates[] = {
	44100, 48000, 0
};

static size_t empty_supported_channel_counts[] = {
	1, 2, 0
};

struct empty_iodev {
	struct cras_iodev base;
	int open;
	uint8_t *audio_buffer;
	unsigned int buffer_level;
	struct timespec last_buffer_access;
};

/* Current level of the audio buffer.  This is made up based on what has been
 * read/written and how long it has been since then.  Simulates audio hardware
 * running at the given sample rate.
 */
static unsigned int current_level(const struct cras_iodev *iodev)
{
	struct empty_iodev *empty_iodev = (struct empty_iodev *)iodev;
	unsigned int frames, frames_since_last;
	struct timespec now, time_since;

	frames = empty_iodev->buffer_level;

	clock_gettime(CLOCK_MONOTONIC, &now);

	subtract_timespecs(&now, &empty_iodev->last_buffer_access, &time_since);

	frames_since_last = cras_time_to_frames(&time_since,
						iodev->format->frame_rate);

	if (iodev->direction == CRAS_STREAM_INPUT)
		return (frames + frames_since_last) % EMPTY_FRAMES;

	/* output */
	if (frames <= frames_since_last)
		return 0;
	return frames - frames_since_last;
}

/*
 * iodev callbacks.
 */

static int is_open(const struct cras_iodev *iodev)
{
	struct empty_iodev *empty_iodev = (struct empty_iodev *)iodev;

	return empty_iodev->open;
}

static int dev_running(const struct cras_iodev *iodev)
{
	return 1;
}

static int frames_queued(const struct cras_iodev *iodev)
{
	return current_level(iodev);
}

static int delay_frames(const struct cras_iodev *iodev)
{
	return 0;
}

static int close_dev(struct cras_iodev *iodev)
{
	struct empty_iodev *empty_iodev = (struct empty_iodev *)iodev;

	empty_iodev->open = 0;
	free(empty_iodev->audio_buffer);
	empty_iodev->audio_buffer = NULL;
	return 0;
}

static int open_dev(struct cras_iodev *iodev)
{
	struct empty_iodev *empty_iodev = (struct empty_iodev *)iodev;

	if (iodev->format == NULL)
		return -EINVAL;

	empty_iodev->open = 1;
	empty_iodev->audio_buffer = calloc(1, EMPTY_BUFFER_SIZE);
	empty_iodev->buffer_level = 0;

	clock_gettime(CLOCK_MONOTONIC, &empty_iodev->last_buffer_access);

	return 0;
}

static int get_buffer(struct cras_iodev *iodev, uint8_t **dst, unsigned *frames)
{
	struct empty_iodev *empty_iodev = (struct empty_iodev *)iodev;

	*dst = empty_iodev->audio_buffer;

	if (iodev->direction == CRAS_STREAM_OUTPUT)
		*frames = min(*frames, EMPTY_FRAMES - current_level(iodev));
	else
		*frames = min(*frames, current_level(iodev));

	return 0;
}

static int put_buffer(struct cras_iodev *iodev, unsigned frames)
{
	struct empty_iodev *empty_iodev = (struct empty_iodev *)iodev;

	empty_iodev->buffer_level = current_level(iodev);

	clock_gettime(CLOCK_MONOTONIC, &empty_iodev->last_buffer_access);

	if (iodev->direction == CRAS_STREAM_OUTPUT) {
		empty_iodev->buffer_level += frames;
		empty_iodev->buffer_level %= EMPTY_FRAMES;
	} else {
		/* Input */
		if (empty_iodev->buffer_level > frames)
			empty_iodev->buffer_level -= frames;
		else
			empty_iodev->buffer_level = 0;
	}

	return 0;
}

static void update_active_node(struct cras_iodev *iodev)
{
}

/*
 * Exported Interface.
 */

struct cras_iodev *empty_iodev_create(enum CRAS_STREAM_DIRECTION direction)
{
	struct empty_iodev *empty_iodev;
	struct cras_iodev *iodev;
	struct cras_ionode *node;

	if (direction != CRAS_STREAM_INPUT && direction != CRAS_STREAM_OUTPUT)
		return NULL;

	empty_iodev = calloc(1, sizeof(*empty_iodev));
	if (empty_iodev == NULL)
		return NULL;
	iodev = &empty_iodev->base;
	iodev->direction = direction;

	/* Finally add it to the appropriate iodev list. */
	if (direction == CRAS_STREAM_INPUT) {
		snprintf(iodev->info.name,
			 ARRAY_SIZE(iodev->info.name),
			 "Silent record device.");
		iodev->info.name[ARRAY_SIZE(iodev->info.name) - 1] = '\0';
		cras_iodev_list_add_input(iodev);
	} else {
		snprintf(iodev->info.name,
			 ARRAY_SIZE(iodev->info.name),
			 "Silent playback device.");
		iodev->info.name[ARRAY_SIZE(iodev->info.name) - 1] = '\0';
		cras_iodev_list_add_output(iodev);
	}

	iodev->supported_rates = empty_supported_rates;
	iodev->supported_channel_counts = empty_supported_channel_counts;
	iodev->buffer_size = EMPTY_BUFFER_SIZE;
        iodev->software_volume_scaler = 1.0;

	iodev->open_dev = open_dev;
	iodev->close_dev = close_dev;
	iodev->is_open = is_open;
	iodev->frames_queued = frames_queued;
	iodev->delay_frames = delay_frames;
	iodev->get_buffer = get_buffer;
	iodev->put_buffer = put_buffer;
	iodev->dev_running = dev_running;
	iodev->update_active_node = update_active_node;

	/* Create a dummy ionode */
	node = (struct cras_ionode *)calloc(1, sizeof(*node));
	node->dev = iodev;
	node->type = CRAS_NODE_TYPE_UNKNOWN;
	node->volume = 100;
	strcpy(node->name, "(default)");
	cras_iodev_add_node(iodev, node);
	cras_iodev_set_active_node(iodev, node);

	return iodev;
}

void empty_iodev_destroy(struct cras_iodev *iodev)
{
	struct empty_iodev *empty_iodev = (struct empty_iodev *)iodev;

	if (iodev->direction == CRAS_STREAM_INPUT)
		cras_iodev_list_rm_input(iodev);
	else {
		cras_iodev_list_rm_output(iodev);
	}
	free(iodev->active_node);
	cras_iodev_free_dsp(iodev);
	free(empty_iodev);
}
