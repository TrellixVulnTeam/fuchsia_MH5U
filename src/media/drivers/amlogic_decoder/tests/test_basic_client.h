// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_DRIVERS_AMLOGIC_DECODER_TESTS_TEST_BASIC_CLIENT_H_
#define SRC_MEDIA_DRIVERS_AMLOGIC_DECODER_TESTS_TEST_BASIC_CLIENT_H_

#include "video_decoder.h"

namespace amlogic_decoder {
namespace test {

// This client can have some behavior injected for use in tests.
class TestBasicClient : public VideoDecoder::Client {
 public:
  // In actual operation, the FrameReadyNotifier must not keep a reference on
  // the frame shared_ptr<>, as that would interfere with muting calls to
  // ReturnFrame().  See comment on Vp9Decoder::Frame::frame field.
  using FrameReadyNotifier = fit::function<void(std::shared_ptr<VideoFrame>)>;

  virtual ~TestBasicClient() = default;

  void OnError() override {
    ZX_ASSERT(error_handler_);
    error_handler_();
  }
  void OnEos() override {
    ZX_ASSERT(eos_handler_);
    eos_handler_();
  }

  bool IsOutputReady() override {
    ZX_ASSERT_MSG(false, "Not implemented");
    return false;
  }
  void OnFrameReady(std::shared_ptr<VideoFrame> frame) override {
    frame_ready_notifier_(std::move(frame));
  }

  zx_status_t InitializeFrames(uint32_t min_frame_count, uint32_t max_frame_count, uint32_t width,
                               uint32_t height, uint32_t stride, uint32_t display_width,
                               uint32_t display_height, bool has_sar, uint32_t sar_width,
                               uint32_t sar_height) override {
    ZX_ASSERT_MSG(false, "Not implemented");
    return ZX_ERR_NOT_SUPPORTED;
  }
  bool IsCurrentOutputBufferCollectionUsable(uint32_t min_frame_count, uint32_t max_frame_count,
                                             uint32_t coded_width, uint32_t coded_height,
                                             uint32_t stride, uint32_t display_width,
                                             uint32_t display_height) override {
    ZX_ASSERT_MSG(false, "Not implemented");
    return false;
  }

  void SetFrameReadyNotifier(FrameReadyNotifier notifier) {
    frame_ready_notifier_ = std::move(notifier);
  }

  void SetErrorHandler(fit::closure error_handler) { error_handler_ = std::move(error_handler); }
  void SetEosHandler(fit::closure eos_handler) { eos_handler_ = std::move(eos_handler); }

  virtual const AmlogicDecoderTestHooks& __WARN_UNUSED_RESULT test_hooks() const override {
    return test_hooks_;
  }

 private:
  FrameReadyNotifier frame_ready_notifier_;
  fit::closure error_handler_;
  fit::closure eos_handler_;
  AmlogicDecoderTestHooks test_hooks_;
};

}  // namespace test
}  // namespace amlogic_decoder

#endif  // SRC_MEDIA_DRIVERS_AMLOGIC_DECODER_TESTS_TEST_BASIC_CLIENT_H_
