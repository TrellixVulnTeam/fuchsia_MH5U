// Copyright 2016 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <assert.h>
#include <lib/crypto/entropy_pool.h>
#include <lib/crypto/prng.h>
#include <lib/fit/defer.h>
#include <pow2.h>
#include <string.h>
#include <zircon/compiler.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <explicit-memory/bytes.h>
#include <kernel/mutex.h>
#include <ktl/atomic.h>
#include <openssl/chacha.h>
#include <openssl/sha.h>

namespace crypto {
namespace {

const uint128_t kNonceOverflow = ((uint128_t)1ULL) << 96;

static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "must be LE");

}  // namespace

Prng::Prng(const void* data, size_t size) : Prng(data, size, NonThreadSafeTag()) {
  BecomeThreadSafe();
}

Prng::Prng(const void* data, size_t size, NonThreadSafeTag tag) : nonce_(0), accumulated_(0) {
  AddEntropy(data, size);
}

Prng::~Prng() {
  Guard<SpinLock, IrqSave> spinlock_guard(&pool_lock_);
  nonce_ = 0;
}

void Prng::AddEntropy(const void* data, size_t size) {
  DEBUG_ASSERT(data || size == 0);
  ASSERT(size <= kMaxEntropy);

  // Concurrent calls to |AddEntropy| must run sequentially.
  Guard<Mutex> add_entropy_guard(&add_entropy_lock_);
  EntropyPool pool;
  {
    Guard<SpinLock, IrqSave> pool_guard(&pool_lock_);
    pool = pool_.Clone();
  }

  pool.Add(ktl::span<const uint8_t>(reinterpret_cast<const uint8_t*>(data), size));

  {
    Guard<SpinLock, IrqSave> pool_guard(&pool_lock_);
    pool_ = std::move(pool);
  }

  // Increment how much entropy has been added, and signal if we have enough.
  size_t total_entropy = accumulated_.fetch_add(size) + size;
  if (is_thread_safe() && total_entropy >= kMinEntropy) {
    ready_->Signal();
  }
}

// AddEntropy() with NULL input effectively reseeds with hash of current key.
void Prng::SelfReseed() { AddEntropy(nullptr, 0); }

void Prng::Draw(void* out, size_t size) {
  DEBUG_ASSERT(out || size == 0);
  ASSERT(size <= kMaxDrawLen);
  // Wait if other threads should add entropy.
  if (is_thread_safe() && accumulated_.load() < kMinEntropy) {
    ready_->Wait();
  }
  // Save these on the stack, but guarantee we clean them up
  EntropyPool pool;
  uint128_t nonce;
  auto cleanup = fit::defer([&] { mandatory_memset(&nonce, 0, sizeof(nonce)); });
  {
    Guard<SpinLock, IrqSave> pool_guard(&pool_lock_);
    nonce = ++nonce_;
    pool = pool_.Clone();
  }
  ASSERT(nonce < kNonceOverflow);
  uint8_t* nonce_u8 = reinterpret_cast<uint8_t*>(&nonce);
  uint8_t* buf = reinterpret_cast<uint8_t*>(out);

  // We randomize |buf| by encrypting it with a key that is never exposed to
  // the caller, and a 96-bit nonce that changes on each call.  We don't zero
  // |buf| because the encrypted output meets the criteria of the PRNG
  // regardless of its original contents.  We reset the counter to 0 on each
  // request; it can't overflow because of the limit on the overall size.
  CRYPTO_chacha_20(buf, buf, size, pool.contents().data(), nonce_u8, 0);
}

uint64_t Prng::RandInt(uint64_t exclusive_upper_bound) {
  ASSERT(exclusive_upper_bound != 0);

  const uint log2 = static_cast<uint>(log2_ceil<uint64_t>(exclusive_upper_bound));
  const size_t mask =
      (log2 != sizeof(uint64_t) * CHAR_BIT) ? (uint64_t(1) << log2) - 1 : UINT64_MAX;
  DEBUG_ASSERT(exclusive_upper_bound - 1 <= mask);

  // This loop should terminate very fast, since the probability that the
  // drawn value is >= exclusive_upper_bound is less than 0.5.  This is the
  // classic discard out-of-range values approach.
  while (true) {
    uint64_t v;
    Draw(reinterpret_cast<uint8_t*>(&v), sizeof(uint64_t) / sizeof(uint8_t));
    v &= mask;
    if (v < exclusive_upper_bound) {
      return v;
    }
  }
}

// It is safe to call this function from PRNG's constructor provided
// |ready_| and |accumulated_| initialized.
void Prng::BecomeThreadSafe() {
  ASSERT(!is_thread_safe());
  ready_.Initialize(accumulated_.load() >= kMinEntropy);
  is_thread_safe_ = true;
}

bool Prng::is_thread_safe() const {
  // Safe to read |is_thread_safe_|; it is read-only in a threaded context.
  return is_thread_safe_;
}

}  // namespace crypto
