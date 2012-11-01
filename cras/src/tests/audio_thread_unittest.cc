// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <sys/select.h>
#include <gtest/gtest.h>

extern "C" {

#include "cras_iodev.h"
#include "cras_rstream.h"
#include "cras_shm.h"
#include "cras_types.h"
#include "audio_thread.h"
#include "utlist.h"

int thread_add_stream(struct cras_iodev *iodev,
                      struct cras_rstream *stream);
int thread_remove_stream(struct cras_iodev *iodev,
                         struct cras_rstream *stream);
int possibly_fill_audio(struct audio_thread *thread,
                        struct timespec *ts);
int possibly_read_audio(struct audio_thread *thread,
                        struct timespec *ts);

static int cras_mix_add_stream_dont_fill_next;
static unsigned int cras_mix_add_stream_count;
static int cras_rstream_audio_ready_count;
static unsigned int cras_rstream_request_audio_called;
static unsigned int cras_rstream_audio_ready_called;
static int select_return_value;
static struct timeval select_timeval;
static int select_max_fd;
static fd_set select_in_fds;
static fd_set select_out_fds;
static unsigned int cras_iodev_config_params_for_streams_called;
static int cras_iodev_append_stream_ret;
static int cras_dsp_get_pipeline_called;
static int cras_dsp_get_pipeline_ret;
static int cras_dsp_put_pipeline_called;
static int cras_dsp_pipeline_get_source_buffer_called;
static int cras_dsp_pipeline_get_sink_buffer_called;
static float cras_dsp_pipeline_source_buffer[2][DSP_BUFFER_SIZE];
static float cras_dsp_pipeline_sink_buffer[2][DSP_BUFFER_SIZE];
static int cras_dsp_pipeline_run_called;
static int cras_dsp_pipeline_run_sample_count;
}

// Number of frames past target that will be added to sleep times to insure that
// all frames are ready.
static const int CAP_EXTRA_SLEEP_FRAMES = 16;

static void fill_test_data(int16_t *data, size_t size)
{
  for (size_t i = 0; i < size; i++)
    data[i] = i;
}

static void verify_processed_data(int16_t *data, size_t size)
{
  for (size_t i = 0; i < size; i++)
    EXPECT_EQ(i * 2, data[i]);  // multiplied by 2 in cras_dsp_pipeline_run()
}

//  Test the audio capture path.
class ReadStreamSuite : public testing::Test {
  protected:
    virtual void SetUp() {
      memset(&fmt_, 0, sizeof(fmt_));
      fmt_.frame_rate = 44100;
      fmt_.num_channels = 2;
      fmt_.format = SND_PCM_FORMAT_S16_LE;

      memset(&iodev_, 0, sizeof(iodev_));
      iodev_.format = &fmt_;
      iodev_.buffer_size = 16384;
      iodev_.cb_threshold = 480;
      iodev_.direction = CRAS_STREAM_INPUT;

      iodev_.frames_queued = frames_queued;
      iodev_.delay_frames = delay_frames;
      iodev_.get_buffer = get_buffer;
      iodev_.put_buffer = put_buffer;
      iodev_.is_open = is_open;

      rstream_ = (struct cras_rstream *)calloc(1, sizeof(*rstream_));
      memcpy(&rstream_->format, &fmt_, sizeof(fmt_));

      shm_ = &rstream_->shm;

      shm_->area = (struct cras_audio_shm_area *)calloc(1,
          sizeof(*shm_->area) + iodev_.cb_threshold * 8);
      cras_shm_set_frame_bytes(shm_, 4); // channels * bytes/sample
      cras_shm_set_used_size(
          shm_, iodev_.cb_threshold * cras_shm_frame_bytes(shm_));

      cras_iodev_append_stream(&iodev_, rstream_);

      cras_mix_add_stream_dont_fill_next = 0;
      cras_mix_add_stream_count = 0;

      fill_test_data((int16_t *)audio_buffer_,
                     cras_shm_used_size(shm_) / 2);

      cras_dsp_get_pipeline_called = 0;
      cras_dsp_get_pipeline_ret = 0;
      cras_dsp_put_pipeline_called = 0;
      cras_dsp_pipeline_get_source_buffer_called = 0;
      cras_dsp_pipeline_get_sink_buffer_called = 0;
      memset(&cras_dsp_pipeline_source_buffer, 0,
             sizeof(cras_dsp_pipeline_source_buffer));
      memset(&cras_dsp_pipeline_sink_buffer, 0,
             sizeof(cras_dsp_pipeline_sink_buffer));
      cras_dsp_pipeline_run_called = 0;
      cras_dsp_pipeline_run_sample_count = 0;
    }

    virtual void TearDown() {
      free(shm_->area);
      free(rstream_);
    }

    unsigned int GetCaptureSleepFrames() {
      // Account for padding the sleep interval to ensure the wake up happens
      // after the last desired frame is received.
      return iodev_.cb_threshold + 16;
    }

    // Stub functions for the iodev structure.
    static int frames_queued(const cras_iodev* iodev) {
      return frames_queued_;
    }

    static int delay_frames(const cras_iodev* iodev) {
      return delay_frames_;
    }

    static int get_buffer(cras_iodev* iodev,
                          uint8_t** buf,
                          unsigned int* num) {
      *buf = audio_buffer_;
      if (audio_buffer_size_ < *num)
	      *num = audio_buffer_size_;
      return 0;
    }

    static int put_buffer(cras_iodev* iodev,
                          unsigned int num) {
      return 0;
    }

    static int is_open(const cras_iodev* iodev) {
      return 0;
    }

  struct cras_iodev iodev_;
  static int frames_queued_;
  static int delay_frames_;
  static uint8_t audio_buffer_[8192];
  static unsigned int audio_buffer_size_;
  struct cras_rstream *rstream_;
  struct cras_audio_format fmt_;
  struct cras_audio_shm *shm_;
};

int ReadStreamSuite::frames_queued_ = 0;
int ReadStreamSuite::delay_frames_ = 0;
uint8_t ReadStreamSuite::audio_buffer_[8192];
unsigned int ReadStreamSuite::audio_buffer_size_ = 0;

TEST_F(ReadStreamSuite, PossiblyReadGetAvailError) {
  struct timespec ts;
  int rc;
  struct audio_thread *thread;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_read_audio, (void *)thread->audio_cb);

  frames_queued_ = -4;
  rc = possibly_read_audio(thread, &ts);
  EXPECT_EQ(-4, rc);
  EXPECT_EQ(0, ts.tv_sec);
  EXPECT_EQ(0, ts.tv_nsec);

  audio_thread_destroy(thread);
}

TEST_F(ReadStreamSuite, PossiblyReadEmpty) {
  struct timespec ts;
  int rc;
  uint64_t nsec_expected;
  struct audio_thread *thread;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_read_audio, (void *)thread->audio_cb);

  //  If no samples are present, it should sleep for cb_threshold frames.
  frames_queued_ = 0;
  nsec_expected = (GetCaptureSleepFrames() + 1) * 1000000000ULL /
                  (uint64_t)fmt_.frame_rate;
  rc = possibly_read_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(0, ts.tv_sec);
  EXPECT_EQ(0, shm_->area->write_offset[0]);
  EXPECT_GE(ts.tv_nsec, nsec_expected - 1000);
  EXPECT_LE(ts.tv_nsec, nsec_expected + 1000);
  EXPECT_EQ(1, thread->sleep_correction_frames);

  audio_thread_destroy(thread);
}

TEST_F(ReadStreamSuite, PossiblyReadHasDataDrop) {
  struct timespec ts;
  int rc;
  uint64_t nsec_expected;
  struct audio_thread *thread;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_read_audio, (void *)thread->audio_cb);

  //  A full block plus 4 frames.  No streams attached so samples are dropped.
  iodev_.streams = NULL;
  frames_queued_ = iodev_.cb_threshold + 4;
  audio_buffer_size_ = frames_queued_;

  // +1 for correction factor.
  uint64_t sleep_frames = GetCaptureSleepFrames() - 4 + 1;
  nsec_expected = (uint64_t)sleep_frames * 1000000000ULL /
                  (uint64_t)fmt_.frame_rate;
  rc = possibly_read_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(0, ts.tv_sec);
  EXPECT_GE(ts.tv_nsec, nsec_expected - 1000);
  EXPECT_LE(ts.tv_nsec, nsec_expected + 1000);

  audio_thread_destroy(thread);
}

TEST_F(ReadStreamSuite, PossiblyReadTooLittleData) {
  struct timespec ts;
  int rc;
  uint64_t nsec_expected;
  static const uint64_t num_frames_short = 40;
  struct audio_thread *thread;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_read_audio, (void *)thread->audio_cb);

  frames_queued_ = iodev_.cb_threshold - num_frames_short;
  audio_buffer_size_ = frames_queued_;
  nsec_expected = ((uint64_t)num_frames_short + CAP_EXTRA_SLEEP_FRAMES + 1) *
                  1000000000ULL / (uint64_t)fmt_.frame_rate;

  rc = possibly_read_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(0, cras_rstream_audio_ready_called);
  EXPECT_EQ(0, shm_->area->write_offset[0]);
  EXPECT_EQ(0, shm_->area->write_buf_idx);
  EXPECT_EQ(0, ts.tv_sec);
  EXPECT_GE(ts.tv_nsec, nsec_expected - 1000);
  EXPECT_LE(ts.tv_nsec, nsec_expected + 1000);

  audio_thread_destroy(thread);
}

TEST_F(ReadStreamSuite, PossiblyReadHasDataWriteStream) {
  struct timespec ts;
  int rc;
  uint64_t nsec_expected;
  struct audio_thread *thread;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_read_audio, (void *)thread->audio_cb);

  //  A full block plus 4 frames.
  frames_queued_ = iodev_.cb_threshold + 4;
  audio_buffer_size_ = frames_queued_;

  for (unsigned int i = 0; i < sizeof(audio_buffer_); i++)
	  audio_buffer_[i] = i;

  // +1 for correction factor.
  uint64_t sleep_frames = GetCaptureSleepFrames() - 4 + 1;
  nsec_expected = (uint64_t)sleep_frames * 1000000000ULL /
                  (uint64_t)fmt_.frame_rate;
  cras_rstream_audio_ready_count = 999;
  //  Give it some samples to copy.
  rc = possibly_read_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(0, ts.tv_sec);
  EXPECT_GE(ts.tv_nsec, nsec_expected - 1000);
  EXPECT_LE(ts.tv_nsec, nsec_expected + 1000);
  EXPECT_EQ(iodev_.cb_threshold, cras_rstream_audio_ready_count);
  for (size_t i = 0; i < iodev_.cb_threshold; i++)
    EXPECT_EQ(audio_buffer_[i], shm_->area->samples[i]);

  audio_thread_destroy(thread);
}

TEST_F(ReadStreamSuite, PossiblyReadWriteTwoBuffers) {
  struct timespec ts;
  int rc;
  struct audio_thread *thread;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_read_audio, (void *)thread->audio_cb);

  //  A full block plus 4 frames.
  frames_queued_ = iodev_.cb_threshold + 4;
  audio_buffer_size_ = frames_queued_;

  cras_rstream_audio_ready_count = 999;

  //  Give it some samples to copy.
  rc = possibly_read_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(0, cras_shm_num_overruns(shm_));
  EXPECT_EQ(iodev_.cb_threshold, cras_rstream_audio_ready_count);
  for (size_t i = 0; i < iodev_.cb_threshold; i++)
    EXPECT_EQ(audio_buffer_[i], shm_->area->samples[i]);

  cras_rstream_audio_ready_count = 999;
  rc = possibly_read_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(0, cras_shm_num_overruns(shm_));
  EXPECT_EQ(iodev_.cb_threshold, cras_rstream_audio_ready_count);
  for (size_t i = 0; i < iodev_.cb_threshold; i++)
    EXPECT_EQ(audio_buffer_[i],
        shm_->area->samples[i + cras_shm_used_size(shm_)]);

  audio_thread_destroy(thread);
}

TEST_F(ReadStreamSuite, PossiblyReadWriteThreeBuffers) {
  struct timespec ts;
  int rc;
  struct audio_thread *thread;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_read_audio, (void *)thread->audio_cb);

  //  A full block plus 4 frames.
  frames_queued_ = iodev_.cb_threshold + 4;
  audio_buffer_size_ = frames_queued_;

  //  Give it some samples to copy.
  rc = possibly_read_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(0, cras_shm_num_overruns(shm_));
  EXPECT_EQ(iodev_.cb_threshold, cras_rstream_audio_ready_count);
  for (size_t i = 0; i < iodev_.cb_threshold; i++)
    EXPECT_EQ(audio_buffer_[i], shm_->area->samples[i]);

  cras_rstream_audio_ready_count = 999;
  rc = possibly_read_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(0, cras_shm_num_overruns(shm_));
  EXPECT_EQ(iodev_.cb_threshold, cras_rstream_audio_ready_count);
  for (size_t i = 0; i < iodev_.cb_threshold; i++)
    EXPECT_EQ(audio_buffer_[i],
        shm_->area->samples[i + cras_shm_used_size(shm_)]);

  cras_rstream_audio_ready_count = 999;
  rc = possibly_read_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(1, cras_shm_num_overruns(shm_));  //  Should have overrun.
  EXPECT_EQ(iodev_.cb_threshold, cras_rstream_audio_ready_count);
  for (size_t i = 0; i < iodev_.cb_threshold; i++)
    EXPECT_EQ(audio_buffer_[i], shm_->area->samples[i]);

  audio_thread_destroy(thread);
}

TEST_F(ReadStreamSuite, PossiblyReadWithoutPipeline) {
  struct timespec ts;
  int rc;
  struct audio_thread *thread;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_read_audio, (void *)thread->audio_cb);

  //  A full block plus 4 frames.
  frames_queued_ = iodev_.cb_threshold + 4;
  audio_buffer_size_ = frames_queued_;
  iodev_.dsp_context = reinterpret_cast<cras_dsp_context *>(0x5);

  rc = possibly_read_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(1, cras_dsp_get_pipeline_called);
  EXPECT_EQ(0, cras_dsp_put_pipeline_called);
  EXPECT_EQ(0, cras_dsp_pipeline_get_source_buffer_called);
  EXPECT_EQ(0, cras_dsp_pipeline_get_sink_buffer_called);
  EXPECT_EQ(0, cras_dsp_pipeline_run_called);

  audio_thread_destroy(thread);
}

TEST_F(ReadStreamSuite, PossiblyReadWithPipeline) {
  struct timespec ts;
  int rc;
  struct audio_thread *thread;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_read_audio, (void *)thread->audio_cb);

  //  A full block plus 4 frames.
  frames_queued_ = iodev_.cb_threshold + 4;
  audio_buffer_size_ = frames_queued_;
  iodev_.dsp_context = reinterpret_cast<cras_dsp_context *>(0x5);
  cras_dsp_get_pipeline_ret = 0x6;

  rc = possibly_read_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(1, cras_dsp_get_pipeline_called);
  EXPECT_EQ(1, cras_dsp_put_pipeline_called);
  EXPECT_EQ(2, cras_dsp_pipeline_get_source_buffer_called);
  EXPECT_EQ(2, cras_dsp_pipeline_get_sink_buffer_called);
  EXPECT_EQ(1, cras_dsp_pipeline_run_called);
  EXPECT_EQ(iodev_.cb_threshold, cras_dsp_pipeline_run_sample_count);

  /* The data move from the buffer to source buffer to sink buffer to shm. */
  verify_processed_data((int16_t *)shm_->area->samples,
                        cras_dsp_pipeline_run_sample_count);

  audio_thread_destroy(thread);
}

//  Test the audio playback path.
class WriteStreamSuite : public testing::Test {
  protected:
    virtual void SetUp() {
      memset(&fmt_, 0, sizeof(fmt_));
      fmt_.frame_rate = 44100;
      fmt_.num_channels = 2;
      fmt_.format = SND_PCM_FORMAT_S16_LE;

      memset(&iodev_, 0, sizeof(iodev_));
      iodev_.format = &fmt_;
      iodev_.buffer_size = 16384;
      iodev_.used_size = 480;
      iodev_.cb_threshold = 96;
      iodev_.direction = CRAS_STREAM_OUTPUT;

      iodev_.frames_queued = frames_queued;
      iodev_.delay_frames = delay_frames;
      iodev_.get_buffer = get_buffer;
      iodev_.put_buffer = put_buffer;
      iodev_.dev_running = dev_running;
      iodev_.is_open = is_open;

      SetupRstream(&rstream_, 1);
      shm_ = &rstream_->shm;
      SetupRstream(&rstream2_, 2);
      shm2_ = &rstream2_->shm;

      cras_iodev_append_stream(&iodev_, rstream_);

      cras_mix_add_stream_dont_fill_next = 0;
      cras_mix_add_stream_count = 0;
      select_max_fd = -1;
      cras_rstream_request_audio_called = 0;
      cras_dsp_get_pipeline_called = 0;

      cras_dsp_get_pipeline_ret = 0;
      cras_dsp_put_pipeline_called = 0;
      cras_dsp_pipeline_get_source_buffer_called = 0;
      cras_dsp_pipeline_get_sink_buffer_called = 0;
      memset(&cras_dsp_pipeline_source_buffer, 0,
             sizeof(cras_dsp_pipeline_source_buffer));
      memset(&cras_dsp_pipeline_sink_buffer, 0,
             sizeof(cras_dsp_pipeline_sink_buffer));
      cras_dsp_pipeline_run_called = 0;
      cras_dsp_pipeline_run_sample_count = 0;
    }

    virtual void TearDown() {
      free(shm_->area);
      free(rstream_);
      free(shm2_->area);
      free(rstream2_);
    }

    void SetupRstream(struct cras_rstream **rstream,
                      int fd) {
      struct cras_audio_shm *shm;

      *rstream = (struct cras_rstream *)calloc(1, sizeof(**rstream));
      memcpy(&(*rstream)->format, &fmt_, sizeof(fmt_));
      (*rstream)->fd = fd;

      shm = &(*rstream)->shm;
      shm->area = (struct cras_audio_shm_area *)calloc(1,
          sizeof(*shm->area) + iodev_.used_size * 8);
      cras_shm_set_frame_bytes(shm, 4);
      cras_shm_set_used_size(
          shm, iodev_.used_size * cras_shm_frame_bytes(shm));
      fill_test_data((int16_t *)shm->area->samples,
                     cras_shm_used_size(shm) / 2);
    }

    uint64_t GetCaptureSleepFrames() {
      // Account for padding the sleep interval to ensure the wake up happens
      // after the last desired frame is received.
      return iodev_.cb_threshold + CAP_EXTRA_SLEEP_FRAMES;
    }

    // Stub functions for the iodev structure.
    static int frames_queued(const cras_iodev* iodev) {
      return frames_queued_;
    }

    static int delay_frames(const cras_iodev* iodev) {
      return delay_frames_;
    }

    static int get_buffer(cras_iodev* iodev,
                          uint8_t** buf,
                          unsigned int* num) {
      *buf = audio_buffer_;
      if (audio_buffer_size_ < *num)
	      *num = audio_buffer_size_;
      return 0;
    }

    static int put_buffer(cras_iodev* iodev,
                          unsigned int num) {
      return 0;
    }

    static int dev_running(const cras_iodev* iodev) {
      return dev_running_;
    }

    static int is_open(const cras_iodev* iodev) {
      return 0;
    }

  struct cras_iodev iodev_;
  static int frames_queued_;
  static int delay_frames_;
  static uint8_t audio_buffer_[8192];
  static unsigned int audio_buffer_size_;
  static int dev_running_;
  struct cras_audio_format fmt_;
  struct cras_rstream* rstream_;
  struct cras_rstream* rstream2_;
  struct cras_audio_shm* shm_;
  struct cras_audio_shm* shm2_;
};

int WriteStreamSuite::frames_queued_ = 0;
int WriteStreamSuite::delay_frames_ = 0;
uint8_t WriteStreamSuite::audio_buffer_[8192];
unsigned int WriteStreamSuite::audio_buffer_size_ = 0;
int WriteStreamSuite::dev_running_ = 0;

TEST_F(WriteStreamSuite, PossiblyFillGetAvailError) {
  struct timespec ts;
  int rc;
  struct audio_thread *thread;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_fill_audio, (void *)thread->audio_cb);

  frames_queued_ = -4;
  rc = possibly_fill_audio(thread, &ts);
  EXPECT_EQ(-4, rc);
  EXPECT_EQ(0, ts.tv_sec);
  EXPECT_EQ(0, ts.tv_nsec);

  audio_thread_destroy(thread);
}

TEST_F(WriteStreamSuite, PossiblyFillEarlyWake) {
  struct timespec ts;
  int rc;
  uint64_t nsec_expected;
  struct audio_thread *thread;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_fill_audio, (void *)thread->audio_cb);

  //  If woken and still have tons of data to play, go back to sleep.
  frames_queued_ = iodev_.cb_threshold * 2;
  audio_buffer_size_ = iodev_.used_size - frames_queued_;

  // Add one to threshold due to correction_frames being incremented.
  nsec_expected = (iodev_.cb_threshold + 1) * 1000000000ULL /
                  (uint64_t)fmt_.frame_rate;
  iodev_.direction = CRAS_STREAM_OUTPUT;
  rc = possibly_fill_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(0, ts.tv_sec);
  EXPECT_GE(ts.tv_nsec, nsec_expected - 1000);
  EXPECT_LE(ts.tv_nsec, nsec_expected + 1000);

  audio_thread_destroy(thread);
}

TEST_F(WriteStreamSuite, PossiblyFillGetFromStreamFull) {
  struct timespec ts;
  int rc;
  uint64_t nsec_expected;
  struct audio_thread *thread;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_fill_audio, (void *)thread->audio_cb);

  //  Have cb_threshold samples left.
  frames_queued_ = iodev_.cb_threshold;
  audio_buffer_size_ = iodev_.used_size - frames_queued_;
  nsec_expected = ((uint64_t)iodev_.used_size - (uint64_t)iodev_.cb_threshold) *
      1000000000ULL / (uint64_t)fmt_.frame_rate;

  //  shm has plenty of data in it.
  shm_->area->write_offset[0] = cras_shm_used_size(shm_);

  FD_ZERO(&select_out_fds);
  FD_SET(rstream_->fd, &select_out_fds);
  select_return_value = 1;

  rc = possibly_fill_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(0, ts.tv_sec);
  EXPECT_GE(ts.tv_nsec, nsec_expected - 1000);
  EXPECT_LE(ts.tv_nsec, nsec_expected + 1000);
  EXPECT_EQ(iodev_.used_size - iodev_.cb_threshold, cras_mix_add_stream_count);
  EXPECT_EQ(0, cras_rstream_request_audio_called);
  EXPECT_EQ(-1, select_max_fd);

  audio_thread_destroy(thread);
}

TEST_F(WriteStreamSuite, PossiblyFillGetFromStreamFullDoesntMix) {
  struct timespec ts;
  int rc;
  struct audio_thread *thread;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_fill_audio, (void *)thread->audio_cb);

  //  Have cb_threshold samples left.
  frames_queued_ = iodev_.cb_threshold;
  audio_buffer_size_ = iodev_.used_size - frames_queued_;

  //  shm has plenty of data in it.
  shm_->area->write_offset[0] = cras_shm_used_size(shm_);

  //  Test that nothing breaks if there is an empty stream.
  cras_mix_add_stream_dont_fill_next = 1;

  FD_ZERO(&select_out_fds);
  FD_SET(rstream_->fd, &select_out_fds);
  select_return_value = 1;

  rc = possibly_fill_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(0, cras_rstream_request_audio_called);
  EXPECT_EQ(-1, select_max_fd);
  EXPECT_EQ(0, shm_->area->read_offset[0]);
  EXPECT_EQ(0, shm_->area->read_offset[1]);
  EXPECT_EQ(cras_shm_used_size(shm_), shm_->area->write_offset[0]);
  EXPECT_EQ(0, shm_->area->write_offset[1]);

  audio_thread_destroy(thread);
}

TEST_F(WriteStreamSuite, PossiblyFillGetFromStreamNeedFill) {
  struct timespec ts;
  int rc;
  struct audio_thread *thread;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_fill_audio, (void *)thread->audio_cb);

  //  Have cb_threshold samples left.
  frames_queued_ = iodev_.cb_threshold;
  audio_buffer_size_ = iodev_.used_size - frames_queued_;

  //  shm is out of data.
  shm_->area->write_offset[0] = 0;

  FD_ZERO(&select_out_fds);
  FD_SET(rstream_->fd, &select_out_fds);
  select_return_value = 1;

  rc = possibly_fill_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(0, ts.tv_sec);
  EXPECT_EQ(0, ts.tv_nsec);
  EXPECT_EQ(iodev_.used_size - iodev_.cb_threshold,
            cras_mix_add_stream_count);
  EXPECT_EQ(1, cras_rstream_request_audio_called);
  EXPECT_NE(-1, select_max_fd);
  EXPECT_EQ(0, memcmp(&select_out_fds, &select_in_fds, sizeof(select_in_fds)));
  EXPECT_EQ(0, shm_->area->read_offset[0]);
  EXPECT_EQ(0, shm_->area->write_offset[0]);

  audio_thread_destroy(thread);
}

TEST_F(WriteStreamSuite, PossiblyFillGetFromTwoStreamsFull) {
  struct timespec ts;
  int rc;
  uint64_t nsec_expected;
  struct audio_thread *thread;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_fill_audio, (void *)thread->audio_cb);

  //  Have cb_threshold samples left.
  frames_queued_ = iodev_.cb_threshold;
  audio_buffer_size_ = iodev_.used_size - frames_queued_;
  nsec_expected = ((uint64_t)iodev_.used_size - (uint64_t)iodev_.cb_threshold) *
      1000000000ULL / (uint64_t)fmt_.frame_rate;

  //  shm has plenty of data in it.
  shm_->area->write_offset[0] = cras_shm_used_size(shm_);
  shm2_->area->write_offset[0] = cras_shm_used_size(shm2_);

  cras_iodev_append_stream(&iodev_, rstream2_);

  rc = possibly_fill_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(0, ts.tv_sec);
  EXPECT_GE(ts.tv_nsec, nsec_expected - 1000);
  EXPECT_LE(ts.tv_nsec, nsec_expected + 1000);
  EXPECT_EQ(iodev_.used_size - iodev_.cb_threshold,
            cras_mix_add_stream_count);
  EXPECT_EQ(0, cras_rstream_request_audio_called);
  EXPECT_EQ(-1, select_max_fd);

  audio_thread_destroy(thread);
}

TEST_F(WriteStreamSuite, PossiblyFillGetFromTwoStreamsFullOneMixes) {
  struct timespec ts;
  int rc;
  size_t written_expected;
  struct audio_thread *thread;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_fill_audio, (void *)thread->audio_cb);

  //  Have cb_threshold samples left.
  frames_queued_ = iodev_.cb_threshold;
  audio_buffer_size_ = iodev_.used_size - frames_queued_;
  written_expected = (iodev_.used_size - iodev_.cb_threshold);

  //  shm has plenty of data in it.
  shm_->area->write_offset[0] = cras_shm_used_size(shm_);
  shm2_->area->write_offset[0] = cras_shm_used_size(shm2_);

  cras_iodev_append_stream(&iodev_, rstream2_);

  //  Test that nothing breaks if one stream doesn't fill.
  cras_mix_add_stream_dont_fill_next = 1;

  rc = possibly_fill_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(0, cras_rstream_request_audio_called);
  EXPECT_EQ(0, shm_->area->read_offset[0]);  //  No write from first stream.
  EXPECT_EQ(written_expected * 4, shm2_->area->read_offset[0]);

  audio_thread_destroy(thread);
}

TEST_F(WriteStreamSuite, PossiblyFillGetFromTwoStreamsNeedFill) {
  struct timespec ts;
  int rc;
  struct audio_thread *thread;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_fill_audio, (void *)thread->audio_cb);

  //  Have cb_threshold samples left.
  frames_queued_ = iodev_.cb_threshold;
  audio_buffer_size_ = iodev_.used_size - frames_queued_;

  //  shm has nothing left.
  shm_->area->write_offset[0] = 0;
  shm2_->area->write_offset[0] = 0;

  cras_iodev_append_stream(&iodev_, rstream2_);

  FD_ZERO(&select_out_fds);
  FD_SET(rstream_->fd, &select_out_fds);
  FD_SET(rstream2_->fd, &select_out_fds);
  select_return_value = 2;

  rc = possibly_fill_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(0, ts.tv_sec);
  EXPECT_EQ(0, ts.tv_nsec);
  EXPECT_EQ(iodev_.used_size - iodev_.cb_threshold,
            cras_mix_add_stream_count);
  EXPECT_EQ(2, cras_rstream_request_audio_called);
  EXPECT_NE(-1, select_max_fd);

  audio_thread_destroy(thread);
}

TEST_F(WriteStreamSuite, PossiblyFillGetFromTwoStreamsFillOne) {
  struct timespec ts;
  int rc;
  uint64_t nsec_expected;
  struct audio_thread *thread;
  static const unsigned int smaller_frames = 40;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_fill_audio, (void *)thread->audio_cb);

  //  Have cb_threshold samples left.
  frames_queued_ = iodev_.cb_threshold;
  audio_buffer_size_ = iodev_.used_size - frames_queued_;
  nsec_expected = (uint64_t)smaller_frames / 4 *
                  (1000000000ULL / (uint64_t)fmt_.frame_rate);

  //  One has too little the other is full.
  shm_->area->write_offset[0] = smaller_frames;
  shm_->area->write_buf_idx = 1;
  shm2_->area->write_offset[0] = cras_shm_used_size(shm2_);
  shm2_->area->write_buf_idx = 1;

  cras_iodev_append_stream(&iodev_, rstream2_);

  FD_ZERO(&select_out_fds);
  FD_SET(rstream_->fd, &select_out_fds);
  select_return_value = 1;

  rc = possibly_fill_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(0, ts.tv_sec);
  EXPECT_GE(ts.tv_nsec, nsec_expected - 1000);
  EXPECT_LE(ts.tv_nsec, nsec_expected + 1000);
  EXPECT_EQ(iodev_.used_size - iodev_.cb_threshold,
            cras_mix_add_stream_count);
  EXPECT_EQ(1, cras_rstream_request_audio_called);
  EXPECT_NE(-1, select_max_fd);

  audio_thread_destroy(thread);
}

TEST_F(WriteStreamSuite, PossiblyFillWithoutPipeline) {
  struct timespec ts;
  int rc;
  struct audio_thread *thread;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_fill_audio, (void *)thread->audio_cb);

  frames_queued_ = iodev_.cb_threshold;
  audio_buffer_size_ = iodev_.used_size - frames_queued_;
  iodev_.dsp_context = reinterpret_cast<cras_dsp_context *>(0x5);

  //  shm has plenty of data in it.
  shm_->area->write_offset[0] = cras_shm_used_size(shm_);

  FD_ZERO(&select_out_fds);
  FD_SET(rstream_->fd, &select_out_fds);
  select_return_value = 1;

  rc = possibly_fill_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(iodev_.used_size - iodev_.cb_threshold,
            cras_mix_add_stream_count);
  EXPECT_EQ(1, cras_dsp_get_pipeline_called);
  EXPECT_EQ(0, cras_dsp_put_pipeline_called);
  EXPECT_EQ(0, cras_dsp_pipeline_get_source_buffer_called);
  EXPECT_EQ(0, cras_dsp_pipeline_get_sink_buffer_called);
  EXPECT_EQ(0, cras_dsp_pipeline_run_called);

  audio_thread_destroy(thread);
}

TEST_F(WriteStreamSuite, PossiblyFillWithPipeline) {
  struct timespec ts;
  int rc;
  struct audio_thread *thread;

  thread = audio_thread_create(&iodev_);
  ASSERT_TRUE(thread);
  EXPECT_EQ((void *)possibly_fill_audio, (void *)thread->audio_cb);

  //  Have cb_threshold samples left.
  frames_queued_ = iodev_.cb_threshold;
  audio_buffer_size_ = iodev_.used_size - frames_queued_;
  iodev_.dsp_context = reinterpret_cast<cras_dsp_context *>(0x5);
  cras_dsp_get_pipeline_ret = 0x6;

  //  shm has plenty of data in it.
  shm_->area->write_offset[0] = cras_shm_used_size(shm_);

  FD_ZERO(&select_out_fds);
  FD_SET(rstream_->fd, &select_out_fds);
  select_return_value = 1;

  rc = possibly_fill_audio(thread, &ts);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(iodev_.used_size - iodev_.cb_threshold,
            cras_mix_add_stream_count);
  EXPECT_EQ(1, cras_dsp_get_pipeline_called);
  EXPECT_EQ(1, cras_dsp_put_pipeline_called);
  EXPECT_EQ(2, cras_dsp_pipeline_get_source_buffer_called);
  EXPECT_EQ(2, cras_dsp_pipeline_get_sink_buffer_called);
  EXPECT_EQ(1, cras_dsp_pipeline_run_called);
  EXPECT_EQ(iodev_.used_size - iodev_.cb_threshold,
            cras_dsp_pipeline_run_sample_count);

  /* The data move from shm to source buffer to sink buffer to mmap buffer. */
  verify_processed_data((int16_t *)audio_buffer_,
                        cras_dsp_pipeline_run_sample_count);

  audio_thread_destroy(thread);
}


//  Test adding and removing streams.
class AddStreamSuite : public testing::Test {
  protected:
    virtual void SetUp() {
      memset(&fmt_, 0, sizeof(fmt_));
      fmt_.frame_rate = 44100;
      fmt_.num_channels = 2;
      fmt_.format = SND_PCM_FORMAT_S16_LE;

      memset(&iodev_, 0, sizeof(iodev_));
      iodev_.format = &fmt_;
      iodev_.buffer_size = 16384;
      iodev_.used_size = 480;
      iodev_.cb_threshold = 96;
      iodev_.direction = CRAS_STREAM_OUTPUT;

      iodev_.is_open = is_open;
      iodev_.open_dev = open_dev;
      iodev_.close_dev = close_dev;

      is_open_ = 0;
      is_open_called_ = 0;
      open_dev_called_ = 0;
      close_dev_called_ = 0;

      cras_iodev_config_params_for_streams_called = 0;
      cras_iodev_append_stream_ret = 0;
    }

    virtual void TearDown() {
    }

    unsigned int GetCaptureSleepFrames() {
      // Account for padding the sleep interval to ensure the wake up happens
      // after the last desired frame is received.
      return iodev_.cb_threshold + 16;
    }

    // Stub functions for the iodev structure.
    static int is_open(const cras_iodev* iodev) {
      is_open_called_++;
      return is_open_;
    }

    static int open_dev(cras_iodev* iodev) {
      open_dev_called_++;
      return 0;
    }

    static int close_dev(cras_iodev* iodev) {
      close_dev_called_++;
      return 0;
    }

  struct cras_iodev iodev_;
  static int is_open_;
  static int is_open_called_;
  static int open_dev_called_;
  static int close_dev_called_;
  struct cras_audio_format fmt_;
};

int AddStreamSuite::is_open_ = 0;
int AddStreamSuite::is_open_called_ = 0;
int AddStreamSuite::open_dev_called_ = 0;
int AddStreamSuite::close_dev_called_ = 0;

TEST_F(AddStreamSuite, SimpleAddOutputStream) {
  int rc;
  cras_rstream* new_stream;
  struct audio_thread thread;

  iodev_.format = &fmt_;
  iodev_.thread = &thread;
  new_stream = (struct cras_rstream *)calloc(1, sizeof(*new_stream));
  new_stream->fd = 55;
  new_stream->buffer_frames = 65;
  new_stream->cb_threshold = 80;
  memcpy(&new_stream->format, &fmt_, sizeof(fmt_));

  rc = thread_add_stream(&iodev_, new_stream);
  ASSERT_EQ(0, rc);
  EXPECT_EQ(1, is_open_called_);
  EXPECT_EQ(1, open_dev_called_);
  EXPECT_EQ(1, cras_iodev_config_params_for_streams_called);

  //  remove the stream.
  rc = thread_remove_stream(&iodev_, new_stream);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(1, close_dev_called_);

  free(new_stream);
}

TEST_F(AddStreamSuite, AddRmTwoOutputStreams) {
  int rc;
  struct cras_rstream *new_stream, *second_stream;
  struct cras_audio_format *fmt;
  struct audio_thread thread;

  fmt = (struct cras_audio_format *)malloc(sizeof(*fmt));
  memcpy(fmt, &fmt_, sizeof(fmt_));
  iodev_.format = fmt;
  iodev_.thread = &thread;
  new_stream = (struct cras_rstream *)calloc(1, sizeof(*new_stream));
  new_stream->fd = 55;
  new_stream->buffer_frames = 65;
  new_stream->cb_threshold = 80;
  memcpy(&new_stream->format, fmt, sizeof(*fmt));

  rc = thread_add_stream(&iodev_, new_stream);
  ASSERT_EQ(0, rc);
  EXPECT_EQ(1, is_open_called_);
  EXPECT_EQ(1, open_dev_called_);
  EXPECT_EQ(1, cras_iodev_config_params_for_streams_called);

  is_open_ = 1;

  second_stream = (struct cras_rstream *)calloc(1, sizeof(*second_stream));
  second_stream->fd = 56;
  second_stream->buffer_frames = 25;
  second_stream->cb_threshold = 12;
  memcpy(&second_stream->format, fmt, sizeof(*fmt));
  rc = thread_add_stream(&iodev_, second_stream);
  ASSERT_EQ(0, rc);
  EXPECT_EQ(2, is_open_called_);
  EXPECT_EQ(1, open_dev_called_);
  EXPECT_EQ(2, cras_iodev_config_params_for_streams_called);

  //  Remove the streams.
  rc = thread_remove_stream(&iodev_, second_stream);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(3, cras_iodev_config_params_for_streams_called);
  EXPECT_EQ(0, close_dev_called_);

  rc = thread_remove_stream(&iodev_, new_stream);
  EXPECT_EQ(0, rc);
  EXPECT_EQ(1, close_dev_called_);
  EXPECT_EQ(3, cras_iodev_config_params_for_streams_called);

  free(fmt);
  free(new_stream);
  free(second_stream);
}

TEST_F(AddStreamSuite, AppendStreamErrorPropogated) {
  int rc;
  struct cras_rstream *new_stream;
  cras_iodev_append_stream_ret = -10;
  new_stream = (struct cras_rstream *)calloc(1, sizeof(*new_stream));
  rc = thread_add_stream(&iodev_, new_stream);
  EXPECT_EQ(-10, rc);
  free(new_stream);
}

TEST_F(AddStreamSuite, OneInputStreamPerDevice) {
  int rc;

  iodev_.direction = CRAS_STREAM_INPUT;
  iodev_.streams = reinterpret_cast<cras_io_stream*>(0x01);
  rc = thread_add_stream(&iodev_, reinterpret_cast<cras_rstream*>(0x44));
  EXPECT_EQ(-EBUSY, rc);
}

extern "C" {
int cras_iodev_get_thread_poll_fd(const struct cras_iodev *iodev) {
  return 0;
}

int cras_iodev_read_thread_command(struct cras_iodev *iodev,
				   uint8_t *buf,
				   size_t max_len) {
  return 0;
}

int cras_iodev_send_command_response(struct cras_iodev *iodev, int rc) {
  return 0;
}

int cras_iodev_append_stream(struct cras_iodev *dev,
			     struct cras_rstream *stream) {
  struct cras_io_stream* out;

  if (cras_iodev_append_stream_ret)
    return cras_iodev_append_stream_ret;

  /* New stream, allocate a container and add it to the list. */
  out = (struct cras_io_stream* )calloc(1, sizeof(*out));
  if (out == NULL)
    return -ENOMEM;
  out->stream = stream;
  out->shm = cras_rstream_get_shm(stream);
  out->fd = cras_rstream_get_audio_fd(stream);
  DL_APPEND(dev->streams, out);

  return 0;
}

int cras_iodev_delete_stream(struct cras_iodev *dev,
			     struct cras_rstream *stream) {
  struct cras_io_stream *out;

  /* Find stream, and if found, delete it. */
  DL_SEARCH_SCALAR(dev->streams, out, stream, stream);
  if (out == NULL)
    return -EINVAL;
  DL_DELETE(dev->streams, out);
  free(out);

  return 0;
}

void cras_iodev_fill_time_from_frames(size_t frames,
                                      size_t frame_rate,
                                      struct timespec *ts) {
	uint64_t to_play_usec;

	ts->tv_sec = 0;
	/* adjust sleep time to target our callback threshold */
	to_play_usec = (uint64_t)frames * 1000000L / (uint64_t)frame_rate;

	while (to_play_usec > 1000000) {
		ts->tv_sec++;
		to_play_usec -= 1000000;
	}
	ts->tv_nsec = to_play_usec * 1000;
}

void cras_iodev_set_playback_timestamp(size_t frame_rate,
                                       size_t frames,
                                       struct timespec *ts) {
}

void cras_iodev_set_capture_timestamp(size_t frame_rate,
                                      size_t frames,
                                      struct timespec *ts) {
}

void cras_iodev_config_params_for_streams(struct cras_iodev *iodev) {
  cras_iodev_config_params_for_streams_called++;
}

//  From mixer.
size_t cras_mix_add_stream(struct cras_audio_shm *shm,
                           size_t num_channels,
                           uint8_t *dst,
                           size_t *count,
                           size_t *index) {
  int16_t *src;
  int16_t *target = (int16_t *)dst;
  size_t fr_written, fr_in_buf;
  size_t num_samples;
  size_t frames = 0;

  if (cras_mix_add_stream_dont_fill_next) {
    cras_mix_add_stream_dont_fill_next = 0;
    return 0;
  }
  cras_mix_add_stream_count = *count;

  /* We only copy the data from shm to dst, not actually mix them. */
  fr_in_buf = cras_shm_get_frames(shm);
  if (fr_in_buf == 0)
    return 0;
  if (fr_in_buf < *count)
    *count = fr_in_buf;

  fr_written = 0;
  while (fr_written < *count) {
    src = cras_shm_get_readable_frames(shm, fr_written,
                                       &frames);
    if (frames > *count - fr_written)
      frames = *count - fr_written;
    num_samples = frames * num_channels;
    memcpy(target, src, num_samples * 2);
    fr_written += frames;
    target += num_samples;
  }

  *index = *index + 1;
  return *count;
}

//  From util.
int cras_set_rt_scheduling(int rt_lim) {
  return 0;
}

int cras_set_thread_priority(int priority) {
  return 0;
}

//  From rstream.
int cras_rstream_request_audio(const struct cras_rstream *stream, size_t count)
{
  cras_rstream_request_audio_called++;
  return 0;
}

int cras_rstream_get_audio_request_reply(const struct cras_rstream *stream) {
  return 0;
}

int cras_rstream_audio_ready(const struct cras_rstream *stream, size_t count) {
  cras_rstream_audio_ready_called++;
  cras_rstream_audio_ready_count = count;
  return 0;
}

struct pipeline *cras_dsp_get_pipeline(struct cras_dsp_context *ctx)
{
  cras_dsp_get_pipeline_called++;
  return reinterpret_cast<struct pipeline *>(cras_dsp_get_pipeline_ret);
}

void cras_dsp_put_pipeline(struct cras_dsp_context *ctx)
{
  cras_dsp_put_pipeline_called++;
}

float *cras_dsp_pipeline_get_source_buffer(struct pipeline *pipeline,
					   int index)
{
  cras_dsp_pipeline_get_source_buffer_called++;
  return cras_dsp_pipeline_source_buffer[index];
}

float *cras_dsp_pipeline_get_sink_buffer(struct pipeline *pipeline, int index)
{
  cras_dsp_pipeline_get_sink_buffer_called++;
  return cras_dsp_pipeline_sink_buffer[index];
}

void cras_dsp_pipeline_run(struct pipeline *pipeline, int sample_count)
{
  cras_dsp_pipeline_run_called++;
  cras_dsp_pipeline_run_sample_count = sample_count;

  /* sink = source * 2 */
  for (int i = 0; i < 2; i++)
    for (int j = 0; j < sample_count; j++)
      cras_dsp_pipeline_sink_buffer[i][j] =
          cras_dsp_pipeline_source_buffer[i][j] * 2;
}

//  Override select so it can be stubbed.
int select(int nfds,
           fd_set *readfds,
           fd_set *writefds,
           fd_set *exceptfds,
           struct timeval *timeout) {
  select_max_fd = nfds;
  select_timeval.tv_sec = timeout->tv_sec;
  select_timeval.tv_usec = timeout->tv_usec;
  select_in_fds = *readfds;
  *readfds = select_out_fds;
  return select_return_value;
}

}  // extern "C"

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}