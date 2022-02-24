// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_EXAMPLES_SHADERTOY_SERVICE_VIEW_SHADERTOY_H_
#define SRC_UI_EXAMPLES_SHADERTOY_SERVICE_VIEW_SHADERTOY_H_

#include "src/ui/examples/shadertoy/service/shadertoy_state.h"

namespace shadertoy {

// Subclass of ShadertoyState that displays content in a View, which responds
// directly to touch input.  This is the easiest, but least flexible way to
// use the Shadertoy API.
class ShadertoyStateForView : public ShadertoyState {
 public:
  ShadertoyStateForView(App* app, zx::eventpair view_token, bool handle_input_events);

 private:
  void OnSetResolution() override;
};

}  // namespace shadertoy

#endif  // SRC_UI_EXAMPLES_SHADERTOY_SERVICE_VIEW_SHADERTOY_H_
