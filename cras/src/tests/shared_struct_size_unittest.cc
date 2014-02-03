// Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>
#include <stdio.h>

namespace {

TEST(StructSizeTest, All) {
  FILE* pipe[2];
  char buf[2][256];

  pipe[0] = popen("./shared_struct_size_dump_32", "r");
  pipe[1] = popen("./shared_struct_size_dump_64", "r");
  ASSERT_TRUE(pipe[0] && pipe[1]) << "Cannot run subtests.";

  while (!feof(pipe[0]) && !feof(pipe[1])) {
    std::string out32 = fgets(buf[0], 256, pipe[0]) ? buf[0] : "<EOF>";
    std::string out64 = fgets(buf[1], 256, pipe[1]) ? buf[1] : "<EOF>";
    EXPECT_EQ(out32, out64) << "Output of 32 and 64 bit versions differs.";
  }
  pclose(pipe[0]);
  pclose(pipe[1]);
}

}  //  namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
