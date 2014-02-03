// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

extern "C" {
#include "cras_messages.h"
#include "cras_shm.h"
#include "cras_types.h"
}

namespace {

TEST(StructSizeTest, All) {
  // Any structure size modification should introduce a bump in either version
  EXPECT_EQ(2, CRAS_SERVER_STATE_VERSION) <<
                               "Incorrect CRAS_SERVER_STATE_VERSION.";
  EXPECT_EQ(1, CRAS_PROTO_VER) << "Incorrect CRAS_PROTO_VER.";

  /* Check structures size, generated by:
      cd cras/src/common; for header in *.h ; do
        echo "  // --- $header --- "
        sed -n 's/^struct.* \([^ ]*\) {/  EXPECT_EQ(0, sizeof(struct \1));/p' \
          "$header"
      done
    Then remove structures that are local, and fix sizes
  */

  // --- cras_audio_codec.h ---
  //   struct cras_audio_codec: local
  // --- cras_audio_format.h ---
  EXPECT_EQ(23, sizeof(struct cras_audio_format));
  // --- cras_iodev_info.h ---
  EXPECT_EQ(68, sizeof(struct cras_iodev_info));
  EXPECT_EQ(140, sizeof(struct cras_ionode_info));
  // --- cras_messages.h ---
  EXPECT_EQ(8, sizeof(struct cras_server_message));
  EXPECT_EQ(8, sizeof(struct cras_client_message));
  EXPECT_EQ(63, sizeof(struct cras_connect_message));
  EXPECT_EQ(12, sizeof(struct cras_disconnect_stream_message));
  EXPECT_EQ(16, sizeof(struct cras_switch_stream_type_iodev));
  EXPECT_EQ(12, sizeof(struct cras_set_system_volume));
  EXPECT_EQ(12, sizeof(struct cras_set_system_capture_gain));
  EXPECT_EQ(12, sizeof(struct cras_set_system_mute));
  EXPECT_EQ(24, sizeof(struct cras_set_node_attr));
  EXPECT_EQ(20, sizeof(struct cras_select_node));
  EXPECT_EQ(8, sizeof(struct cras_reload_dsp));
  EXPECT_EQ(8, sizeof(struct cras_dump_dsp_info));
  EXPECT_EQ(8, sizeof(struct cras_dump_audio_thread));
  EXPECT_EQ(16, sizeof(struct cras_client_connected));
  EXPECT_EQ(51, sizeof(struct cras_client_stream_connected));
  EXPECT_EQ(12, sizeof(struct cras_client_stream_reattach));
  EXPECT_EQ(8, sizeof(struct cras_client_audio_debug_info_ready));
  EXPECT_EQ(12, sizeof(struct audio_message));
  // --- cras_shm.h ---
  EXPECT_EQ(8, sizeof(struct cras_audio_shm_config));
  EXPECT_EQ(68, sizeof(struct cras_audio_shm_area));
  // struct cras_audio_shm: local
  // --- cras_types.h ---
  EXPECT_EQ(8, sizeof(struct cras_timespec));
  EXPECT_EQ(16, sizeof(struct cras_attached_client_info));
  EXPECT_EQ(16388, sizeof(struct audio_thread_event_log));
  EXPECT_EQ(51, sizeof(struct audio_stream_debug_info));
  EXPECT_EQ(16952, sizeof(struct audio_debug_info));
  EXPECT_EQ(25696, sizeof(struct cras_server_state));
  EXPECT_EQ(20, sizeof(struct cras_alsa_card_info));
  // --- dumper.h: local ---
  // --- rtp.h: local ---
}

}  //  namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
