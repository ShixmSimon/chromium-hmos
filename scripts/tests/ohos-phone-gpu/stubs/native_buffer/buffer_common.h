// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef OHOS_PHONE_GPU_TEST_BUFFER_COMMON_H_
#define OHOS_PHONE_GPU_TEST_BUFFER_COMMON_H_
// Host-test subset of the public native_buffer/buffer_common.h enum. Keep the
// distinct SDK numbers: collapsing aliases here would hide the phone bug.
// Production builds use the SDK header; this directory is test-only.
typedef enum OH_NativeBuffer_ColorSpace {
  OH_COLORSPACE_NONE = 0,
  OH_COLORSPACE_BT709_FULL = 3,
  OH_COLORSPACE_BT2020_HLG_FULL = 4,
  OH_COLORSPACE_BT2020_PQ_FULL = 5,
  OH_COLORSPACE_BT2020_PQ_LIMIT = 10,
  OH_COLORSPACE_SRGB_FULL = 11,
  OH_COLORSPACE_P3_FULL = 12,
  OH_COLORSPACE_SRGB_LIMIT = 16,
  OH_COLORSPACE_LINEAR_SRGB = 21,
  OH_COLORSPACE_DISPLAY_SRGB = 25,
  OH_COLORSPACE_DISPLAY_P3_SRGB = 26,
  OH_COLORSPACE_DISPLAY_BT2020_SRGB = 29,
  OH_COLORSPACE_DISPLAY_BT2020_HLG = 30,
  OH_COLORSPACE_DISPLAY_BT2020_PQ = 31,
} OH_NativeBuffer_ColorSpace;
#endif
