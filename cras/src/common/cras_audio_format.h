/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef CRAS_AUDIO_FORMAT_H_
#define CRAS_AUDIO_FORMAT_H_

#include <alsa/asoundlib.h>

/* Identifiers for each channel in audio stream. */
enum CRAS_CHANNEL {
	/* First nine channels matches the
	 * snd_mixer_selem_channel_id_t values.
	 */
	CRAS_CH_FL,
	CRAS_CH_FR,
	CRAS_CH_RL,
	CRAS_CH_RR,
	CRAS_CH_FC,
	CRAS_CH_LFE,
	CRAS_CH_SL,
	CRAS_CH_SR,
	CRAS_CH_RC,
	/* Channels defined both in channel_layout.h and
	 * alsa channel mapping API. */
	CRAS_CH_FLC,
	CRAS_CH_FRC,
	/* Must be the last one */
	CRAS_CH_MAX,
};

/* Audio format. */
struct cras_audio_format {
	snd_pcm_format_t format;
	size_t frame_rate; /* Hz */

	// TODO(hychao): use channel_layout to replace num_channels
	size_t num_channels;

	/* Channel layout whose value represents the index of each
	 * CRAS_CHANNEL in the layout. Value -1 means the channel is
	 * not used. For example: 0,1,2,3,4,5,-1,-1,-1,-1,-1 means the
	 * channel order is FL,FR,RL,RR,FC.
	 */
	int8_t channel_layout[CRAS_CH_MAX];
};

/* Returns the number of bytes per sample.
 * This is bits per smaple / 8 * num_channels.
 */
static inline size_t cras_get_format_bytes(const struct cras_audio_format *fmt)
{
	const int bytes = snd_pcm_format_physical_width(fmt->format) / 8;
	return (size_t)bytes * fmt->num_channels;
}

/* Create an audio format structure. */
struct cras_audio_format *cras_audio_format_create(snd_pcm_format_t format,
						   size_t frame_rate,
						   size_t num_channels);

/* Destroy an audio format struct created with cras_audio_format_crate. */
void cras_audio_format_destroy(struct cras_audio_format *fmt);

/* Sets the channel layout for given format.
 *    format - The format structure to carry channel layout info
 *    layout - An integer array representing the position of each
 *        channel in enum CRAS_CHANNEL
 */
int cras_audio_format_set_channel_layout(struct cras_audio_format *format,
					 int8_t layout[CRAS_CH_MAX]);

/* Allocates an empty channel conversion matrix of given size. */
float** cras_channel_conv_matrix_alloc(size_t in_ch, size_t out_ch);

/* Destroys the channel conversion matrix. */
void cras_channel_conv_matrix_destroy(float **mtx, size_t out_ch);

/* Creates channel conversion matrix for given input and output format.
 * Returns NULL if the conversion is not supported between the channel
 * layouts specified in input/ouput formats.
 */
float **cras_channel_conv_matrix_create(const struct cras_audio_format *in,
					const struct cras_audio_format *out);

#endif /* CRAS_AUDIO_FORMAT_H_ */
