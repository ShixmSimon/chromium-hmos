// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "base/process/launch_ohos.h"

#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace base::internal {
namespace {

class OhosRendererIsolationTest : public testing::Test {
 protected:
  void SetUp() override { current_ = this; }
  void TearDown() override { current_ = nullptr; }

  static Ability_ChildProcessConfigs* Config() {
    // The production transaction treats this as opaque; no fake NDK layout.
    return reinterpret_cast<Ability_ChildProcessConfigs*>(current_);
  }
  static Ability_ChildProcessConfigs* Create() {
    current_->calls_.push_back("create");
    return current_->failure_ == "create" ? nullptr : Config();
  }
  static Ability_NativeChildProcess_ErrCode Destroy(
      Ability_ChildProcessConfigs* config) {
    EXPECT_EQ(config, Config());
    current_->calls_.push_back("destroy");
    return NCP_NO_ERROR;
  }
  static Ability_NativeChildProcess_ErrCode SetMode(
      Ability_ChildProcessConfigs* config,
      NativeChildProcess_IsolationMode mode) {
    EXPECT_EQ(config, Config());
    EXPECT_EQ(mode, NCP_ISOLATION_MODE_ISOLATED);
    current_->calls_.push_back("mode");
    return current_->failure_ == "mode" ? NCP_ERR_INVALID_PARAM : NCP_NO_ERROR;
  }
  static Ability_NativeChildProcess_ErrCode SetUid(
      Ability_ChildProcessConfigs* config,
      bool isolated) {
    EXPECT_EQ(config, Config());
    EXPECT_TRUE(isolated);
    current_->calls_.push_back("uid");
    return current_->failure_ == "uid" ? NCP_ERR_NOT_SUPPORTED : NCP_NO_ERROR;
  }
  static Ability_NativeChildProcess_ErrCode Start(
      const char* entry,
      NativeChildProcess_Args args,
      Ability_ChildProcessConfigs* config,
      int32_t* pid) {
    EXPECT_STREQ(entry, "libnweb_render.so:ChromiumNativeChildMain");
    EXPECT_EQ(config, Config());
    EXPECT_EQ(args.entryParams, current_->args_.entryParams);
    EXPECT_EQ(args.fdList.head, current_->args_.fdList.head);
    current_->calls_.push_back("start");
    if (current_->failure_ == "start") {
      return NCP_ERR_MULTI_PROCESS_DISABLED;
    }
    *pid = 123;
    return NCP_NO_ERROR;
  }

  OhosIsolatedChildApi api_{&Create, &Destroy, &SetMode, &SetUid, &Start};
  char encoded_args_[3] = "{}";
  char fd_name_[5] = "mojo";
  NativeChildProcess_Fd descriptor_{fd_name_, 10, nullptr};
  NativeChildProcess_Args args_{encoded_args_, {&descriptor_}};
  std::string failure_;
  std::vector<std::string> calls_;
  inline static OhosRendererIsolationTest* current_ = nullptr;
};

TEST_F(OhosRendererIsolationTest, RequiresEveryApiBeforeAllocating) {
  for (int missing = 0; missing != 5; ++missing) {
    SCOPED_TRACE(missing);
    auto api = api_;
    switch (missing) {
      case 0:
        api.create = nullptr;
        break;
      case 1:
        api.destroy = nullptr;
        break;
      case 2:
        api.set_mode = nullptr;
        break;
      case 3:
        api.set_uid = nullptr;
        break;
      case 4:
        api.start = nullptr;
        break;
    }
    int32_t pid = 999;
    EXPECT_EQ(StartOhosIsolatedRenderer(api, args_, &pid),
              NCP_ERR_NOT_SUPPORTED);
    EXPECT_EQ(pid, -1);
    EXPECT_TRUE(calls_.empty());
  }
}

TEST_F(OhosRendererIsolationTest, RequiresAnOutputPid) {
  EXPECT_EQ(StartOhosIsolatedRenderer(api_, args_, nullptr),
            NCP_ERR_INVALID_PARAM);
  EXPECT_TRUE(calls_.empty());
}

TEST_F(OhosRendererIsolationTest, AllocationFailureDoesNotLaunch) {
  failure_ = "create";
  int32_t pid = -1;
  EXPECT_EQ(StartOhosIsolatedRenderer(api_, args_, &pid), NCP_ERR_INTERNAL);
  EXPECT_EQ(calls_, (std::vector<std::string>{"create"}));
}

TEST_F(OhosRendererIsolationTest, SandboxFailureDoesNotLaunch) {
  failure_ = "mode";
  int32_t pid = -1;
  EXPECT_EQ(StartOhosIsolatedRenderer(api_, args_, &pid),
            NCP_ERR_INVALID_PARAM);
  EXPECT_EQ(calls_, (std::vector<std::string>{"create", "mode", "destroy"}));
}

TEST_F(OhosRendererIsolationTest, UidFailureDoesNotLaunchWithSharedUid) {
  failure_ = "uid";
  int32_t pid = -1;
  EXPECT_EQ(StartOhosIsolatedRenderer(api_, args_, &pid),
            NCP_ERR_NOT_SUPPORTED);
  EXPECT_EQ(calls_,
            (std::vector<std::string>{"create", "mode", "uid", "destroy"}));
}

TEST_F(OhosRendererIsolationTest, LaunchFailureIsReturnedWithoutRetry) {
  failure_ = "start";
  int32_t pid = -1;
  EXPECT_EQ(StartOhosIsolatedRenderer(api_, args_, &pid),
            NCP_ERR_MULTI_PROCESS_DISABLED);
  EXPECT_EQ(pid, -1);
  EXPECT_EQ(calls_, (std::vector<std::string>{"create", "mode", "uid", "start",
                                              "destroy"}));
}

TEST_F(OhosRendererIsolationTest, ConfiguresBothBoundariesBeforeLaunching) {
  int32_t pid = -1;
  EXPECT_EQ(StartOhosIsolatedRenderer(api_, args_, &pid), NCP_NO_ERROR);
  EXPECT_EQ(pid, 123);
  EXPECT_EQ(calls_, (std::vector<std::string>{"create", "mode", "uid", "start",
                                              "destroy"}));
}

}  // namespace
}  // namespace base::internal
