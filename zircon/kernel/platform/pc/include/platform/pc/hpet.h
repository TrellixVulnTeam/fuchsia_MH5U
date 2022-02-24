// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_PLATFORM_PC_INCLUDE_PLATFORM_PC_HPET_H_
#define ZIRCON_KERNEL_PLATFORM_PC_INCLUDE_PLATFORM_PC_HPET_H_

#include <lib/affine/ratio.h>
#include <zircon/compiler.h>
#include <zircon/types.h>

__BEGIN_CDECLS

bool hpet_is_present(void);

uint64_t hpet_get_value(void);
zx_status_t hpet_set_value(uint64_t v);

void hpet_enable(void);
void hpet_disable(void);

void hpet_wait_ms(uint16_t ms);

__END_CDECLS

// Storage resides in platform/pc/timer.cpp
extern affine::Ratio hpet_ticks_to_clock_monotonic;

#endif  // ZIRCON_KERNEL_PLATFORM_PC_INCLUDE_PLATFORM_PC_HPET_H_
