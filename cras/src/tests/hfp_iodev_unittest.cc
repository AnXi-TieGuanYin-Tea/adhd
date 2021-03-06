/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <gtest/gtest.h>

extern "C" {
#include "cras_hfp_iodev.h"
#include "cras_iodev.h"
#include "cras_iodev_list.h"
#include "cras_hfp_info.h"
}

static struct cras_iodev *iodev;
static struct cras_bt_transport *fake_transport;
static struct hfp_info *fake_info;
struct cras_audio_format fake_format;
static size_t cras_iodev_list_add_output_called;
static size_t cras_iodev_list_rm_output_called;
static size_t cras_iodev_list_add_input_called;
static size_t cras_iodev_list_rm_input_called;
static size_t cras_iodev_add_node_called;
static size_t cras_iodev_rm_node_called;
static size_t cras_iodev_set_active_node_called;
static size_t cras_iodev_free_format_called;
static size_t cras_bt_transport_sco_connect_called;
static int cras_bt_transport_sco_connect_return_val;
static size_t hfp_info_add_iodev_called;
static size_t hfp_info_rm_iodev_called;
static size_t hfp_info_running_called;
static int hfp_info_running_return_val;
static size_t hfp_info_has_iodev_called;
static int hfp_info_has_iodev_return_val;
static size_t hfp_info_start_called;
static size_t hfp_info_stop_called;
static size_t hfp_buf_acquire_called;
static unsigned hfp_buf_acquire_return_val;
static size_t hfp_buf_release_called;
static unsigned hfp_buf_release_nwritten_val;

void ResetStubData() {
  cras_iodev_list_add_output_called = 0;
  cras_iodev_list_rm_output_called = 0;
  cras_iodev_list_add_input_called = 0;
  cras_iodev_list_rm_input_called = 0;
  cras_iodev_add_node_called = 0;
  cras_iodev_rm_node_called = 0;
  cras_iodev_set_active_node_called = 0;
  cras_iodev_free_format_called = 0;
  cras_bt_transport_sco_connect_called = 0;
  cras_bt_transport_sco_connect_return_val = 0;
  hfp_info_add_iodev_called = 0;
  hfp_info_rm_iodev_called = 0;
  hfp_info_running_called = 0;
  hfp_info_running_return_val = 1;
  hfp_info_has_iodev_called = 0;
  hfp_info_has_iodev_return_val = 0;
  hfp_info_start_called = 0;
  hfp_info_stop_called = 0;
  hfp_buf_acquire_called = 0;
  hfp_buf_acquire_return_val = 0;
  hfp_buf_release_called = 0;
  hfp_buf_release_nwritten_val = 0;

  fake_info = reinterpret_cast<struct hfp_info *>(0x123);
}

namespace {

TEST(HfpIodev, CreateHfpIodev) {
  iodev = hfp_iodev_create(CRAS_STREAM_OUTPUT, fake_transport,
		  	   fake_info);

  ASSERT_EQ(CRAS_STREAM_OUTPUT, iodev->direction);
  ASSERT_EQ(1, cras_iodev_list_add_output_called);
  ASSERT_EQ(1, cras_iodev_add_node_called);
  ASSERT_EQ(1, cras_iodev_set_active_node_called);

  hfp_iodev_destroy(iodev);

  ASSERT_EQ(1, cras_iodev_list_rm_output_called);
  ASSERT_EQ(1, cras_iodev_rm_node_called);
}

TEST(HfpIodev, OpenHfpIodev) {
  ResetStubData();

  iodev = hfp_iodev_create(CRAS_STREAM_OUTPUT, fake_transport,
		  	   fake_info);

  cras_iodev_set_format(iodev, &fake_format);

  /* hfp_info not start yet */
  hfp_info_running_return_val = 0;
  iodev->open_dev(iodev);

  ASSERT_EQ(1, cras_bt_transport_sco_connect_called);
  ASSERT_EQ(1, hfp_info_start_called);
  ASSERT_EQ(1, hfp_info_add_iodev_called);

  /* hfp_info is running now */
  hfp_info_running_return_val = 1;
  ASSERT_EQ(1, iodev->is_open(iodev));

  iodev->close_dev(iodev);
  ASSERT_EQ(1, hfp_info_rm_iodev_called);
  ASSERT_EQ(1, hfp_info_stop_called);
  ASSERT_EQ(1, cras_iodev_free_format_called);
}

TEST(HfpIodev, OpenIodevWithHfpInfoAlreadyRunning) {
  ResetStubData();

  iodev = hfp_iodev_create(CRAS_STREAM_INPUT, fake_transport,
		  	   fake_info);

  cras_iodev_set_format(iodev, &fake_format);

  /* hfp_info already started by another device */
  hfp_info_running_return_val = 1;
  iodev->open_dev(iodev);

  ASSERT_EQ(0, cras_bt_transport_sco_connect_called);
  ASSERT_EQ(0, hfp_info_start_called);
  ASSERT_EQ(1, hfp_info_add_iodev_called);

  ASSERT_EQ(1, iodev->is_open(iodev));

  hfp_info_has_iodev_return_val = 1;
  iodev->close_dev(iodev);
  ASSERT_EQ(1, hfp_info_rm_iodev_called);
  ASSERT_EQ(0, hfp_info_stop_called);
  ASSERT_EQ(1, cras_iodev_free_format_called);
}

TEST(HfpIodev, PutGetBuffer) {
  uint8_t *buf;
  unsigned frames;

  ResetStubData();
  iodev = hfp_iodev_create(CRAS_STREAM_OUTPUT, fake_transport,
			   fake_info);
  cras_iodev_set_format(iodev, &fake_format);
  iodev->open_dev(iodev);

  hfp_buf_acquire_return_val = 100;
  iodev->get_buffer(iodev, &buf, &frames);

  ASSERT_EQ(1, hfp_buf_acquire_called);
  ASSERT_EQ(100, frames);

  iodev->put_buffer(iodev, 40);
  ASSERT_EQ(1, hfp_buf_release_called);
  ASSERT_EQ(40, hfp_buf_release_nwritten_val);
}

} // namespace

extern "C" {
void cras_iodev_free_format(struct cras_iodev *iodev)
{
  cras_iodev_free_format_called++;
}

int cras_iodev_set_format(struct cras_iodev *iodev,
			  struct cras_audio_format *fmt)
{
  fmt->format = SND_PCM_FORMAT_S16_LE;
  fmt->num_channels = 1;
  fmt->frame_rate = 8000;
  iodev->format = fmt;
  return 0;
}

void cras_iodev_add_node(struct cras_iodev *iodev, struct cras_ionode *node)
{
  cras_iodev_add_node_called++;
  iodev->nodes = node;
}

void cras_iodev_rm_node(struct cras_iodev *iodev, struct cras_ionode *node)
{
  cras_iodev_rm_node_called++;
  iodev->nodes = NULL;
}

void cras_iodev_set_active_node(struct cras_iodev *iodev,
				struct cras_ionode *node)
{
  cras_iodev_set_active_node_called++;
  iodev->active_node = node;
}

//  From iodev list.
int cras_iodev_list_add_output(struct cras_iodev *output)
{
  cras_iodev_list_add_output_called++;
  return 0;
}

int cras_iodev_list_rm_output(struct cras_iodev *dev)
{
  cras_iodev_list_rm_output_called++;
  return 0;
}

int cras_iodev_list_add_input(struct cras_iodev *output)
{
  cras_iodev_list_add_input_called++;
  return 0;
}

int cras_iodev_list_rm_input(struct cras_iodev *dev)
{
  cras_iodev_list_rm_input_called++;
  return 0;
}

// From bt transport
const char *cras_bt_transport_object_path(
		const struct cras_bt_transport *transport)
{
  return NULL;
}

int cras_bt_transport_sco_connect(struct cras_bt_transport *transport)
{
  cras_bt_transport_sco_connect_called++;
  return cras_bt_transport_sco_connect_return_val;
}

// From cras_hfp_info
int hfp_info_add_iodev(struct hfp_info *info, struct cras_iodev *dev)
{
  hfp_info_add_iodev_called++;
  return 0;
}

int hfp_info_rm_iodev(struct hfp_info *info, struct cras_iodev *dev)
{
  hfp_info_rm_iodev_called++;
  return 0;
}

int hfp_info_has_iodev(struct hfp_info *info)
{
  hfp_info_has_iodev_called++;
  return hfp_info_has_iodev_return_val;
}

int hfp_info_running(struct hfp_info *info)
{
  hfp_info_running_called++;
  return hfp_info_running_return_val;
}

int hfp_info_start(int fd, struct hfp_info *info)
{
  hfp_info_start_called++;
  return 0;
}

int hfp_info_stop(struct hfp_info *info)
{
  hfp_info_stop_called++;
  return 0;
}

int hfp_buf_queued(struct hfp_info *info, const struct cras_iodev *dev)
{
  return 0;
}

int hfp_buf_size(struct hfp_info *info, struct cras_iodev *dev)
{
  /* 1008 / 2 */
  return 504;
}

void hfp_buf_acquire(struct hfp_info *info,  struct cras_iodev *dev,
		     uint8_t **buf, unsigned *count)
{
  hfp_buf_acquire_called++;
  *count = hfp_buf_acquire_return_val;
}

void hfp_buf_release(struct hfp_info *info, struct cras_iodev *dev,
		     unsigned written_bytes)
{
  hfp_buf_release_called++;
  hfp_buf_release_nwritten_val = written_bytes;
}

} // extern "C"

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
