// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_PLAYBACK_MEDIAPLAYER_GRAPH_PAYLOADS_LOCAL_MEMORY_PAYLOAD_ALLOCATOR_H_
#define SRC_MEDIA_PLAYBACK_MEDIAPLAYER_GRAPH_PAYLOADS_LOCAL_MEMORY_PAYLOAD_ALLOCATOR_H_

#include <fbl/ref_counted.h>
#include <fbl/ref_ptr.h>

#include "src/media/playback/mediaplayer/graph/payloads/payload_allocator.h"

namespace media_player {

class LocalMemoryPayloadAllocator : public PayloadAllocator,
                                    public fbl::RefCounted<LocalMemoryPayloadAllocator> {
 public:
  static fbl::RefPtr<LocalMemoryPayloadAllocator> Create();

  // Dumps this |LocalMemoryPayloadAllocator|'s state to |os|.
  void Dump(std::ostream& os) const;

  // PayloadAllocator implementation.
  fbl::RefPtr<PayloadBuffer> AllocatePayloadBuffer(uint64_t size) override;
};

}  // namespace media_player

#endif  // SRC_MEDIA_PLAYBACK_MEDIAPLAYER_GRAPH_PAYLOADS_LOCAL_MEMORY_PAYLOAD_ALLOCATOR_H_
