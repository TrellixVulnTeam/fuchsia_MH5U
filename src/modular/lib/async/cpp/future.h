// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MODULAR_LIB_ASYNC_CPP_FUTURE_H_
#define SRC_MODULAR_LIB_ASYNC_CPP_FUTURE_H_

#include <lib/async/cpp/task.h>
#include <lib/async/default.h>
#include <lib/fit/function.h>
#include <lib/syslog/cpp/macros.h>

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "src/lib/fxl/functional/apply.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/memory/ref_counted.h"
#include "src/lib/fxl/memory/ref_ptr.h"
#include "src/lib/fxl/memory/weak_ptr.h"

namespace modular {

// # Futures
//
// A *future* is an object representing the eventual value of an asynchronous
// operation. They are a useful complement to callback functions or lambdas
// because they are _composable_: asynchronous operations can be sequentially
// executed, and an async operation's result can be passed to another async
// operation, like a Unix pipeline.
//
// To use a future:
//
// 1. A *producer*, typically an async operation, creates a Future with
//    Future<ResultType>::Create().
// 2. The producer starts its async operation (e.g. a network request or disk
//    read).
// 3. The producer synchronously returns the Future to a *consumer*.
// 4. A consumer attaches a *callback* lambda to the Future using Then(). (The
//    callback can be attached to the future any time after the future is
//    created, before or after the async operation is finished.)
// 5. Some time later, when the producer's async operation is finished, the
//    producer *completes* the future with a *result* using Complete(result).
//    |result| is 0 to N movable or copyable values, e.g. Complete(),
//    Complete(value), Complete(value1, value2, ...).
// 6. The consumer's callback is invoked after the future is completed, with the
//    completed result passed as zero or more parameters to the callback.
//
// The following example shows a simple use case:
//
// Producer:
//
// FuturePtr<Bytes> MakeNetworkRequest(NetworkRequest& request) {
//   auto f = Future<Bytes>::Create("NetworkRequest");  // a "trace_name" that's
//                                                      // logged when things go
//                                                      // wrong
//   auto network_request_callback = [f] (Bytes bytes) {
//     f->Complete(bytes);
//   };
//   PerformAsyncNetworkRequest(request, network_request_callback);
//   return f;
// }
//
// Client:
//
// FuturePtr<Bytes> f = MakeNetworkRequest();
// f->Then([] (Bytes bytes) {
//   ProcessBytes(bytes);
// });
//
// ## Chaining and sequencing futures
//
// Futures can be composed; this is commonly called "chaining". Methods that
// attach callbacks, such as Then(), will return another Future: the returned
// Future is completed once the previous callback has finished executing. For
// example:
//
// Client:
//
// ShowProgressSpinner(true);
// FuturePtr<Bytes> f = MakeNetworkRequest(request);
// f->AsyncMap([] (Bytes zipped_bytes) {
//   // Use AsyncMap() if your callback wants to return another Future.
//   FuturePtr<Bytes> f = UnzipInBackground(zipped_bytes);
//   return f;
// })->Map([] (Bytes unzipped_bytes) {
//   // Use Map() if your callback returns a non-future: the callback's return
//   // value will be wrapped up into the returned Future.
//   JPEGImage image = DecodeImageSynchronously(unzipped_bytes);
//   return image;
// })->AsyncMap([] (JPEGImage image) {
//   FuturePtr<> f = UpdateUIAsynchronously(image);
//   return f;
// })->Then([] {
//   // Use Then() if your callback returns void. Note that Then() still returns
//   // a Future<>, which will be completed only when your callback finishes
//   // executing.
//   ShowProgressSpinner(false);
// });
//
// ## Memory Management & Ownership
//
// FuturePtr is a fxl::RefPtr, which is a smart pointer similar to
// std::shared_ptr that holds a reference count (refcount). When you call
// Future::Create(), you are expected to maintain a reference to it. When the
// Future is deleted, its result and callbacks are also deleted.
//
// Each method documents how it affects the future's refcount. To summarize:
//
// * Calling Then() on a future does not affect its refcount. This applies to
//   all methods that returns a chained future, such as AsyncMap() and Map().
// * However, calling Then() on a future will cause that future to maintain a
//   reference to the returned chained future. So, you do not need to maintain a
//   reference to the returned future.
// * Calling Complete() does not affect the future's refcount.
// * Unlike Complete(), the closure returned by Completer() _does own_ the
//   future, so you do not need to maintain a reference to the future after
//   calling Completer(). (You do need to maintain a reference to the closure,
//   however.)
// * Wait(futures) returns a future that every future in |futures| owns, so you
//   do not need to maintain a reference to the returned future. The callback
//   attached to each future in |futures| will also keep a reference to
//   themselves, so that if a future that is Wait()ed on otherwise goes out of
//   scope, the future itself is kept alive.
//
// See each method's documentation for more details on memory management.
//
// ## Use Weak*() variants to cancel callback chains
//
// "Weak" variants exist for all chain/sequence methods (WeakThen(),
// WeakConstThen(), WeakMap() and WeakAsyncMap()). These are almost identical
// to their non-weak counterparts but take an fxl::WeakPtr<T> as a first
// argument. If, at callback invocation time, the WeakPtr is invalid, execution
// will halt and no future further down the chain will be executed.
//
// Example:
//
// FuturePtr<> f = MakeFuture();
// f->WeakThen(weak_ptr_factory.GetWeakPtr(), [] {
//   FX_LOGS(INFO) << "This won't execute";
// })->Then([] {
//   FX_LOGS(INFO) << "Neither will this";
// });
// weak_ptr_factory.InvalidateWeakPtrs();
// f->Complete();
//
// ## Use Wait() to synchronize on multiple futures
//
// If multiple futures are running, use the Wait() function to create a
// Future that completes when all the futures passed to it are completed:
//
// FuturePtr<Bytes> f1 = MakeNetworkRequest(request1);
// FuturePtr<Bytes> f2 = MakeNetworkRequest(request2);
// FuturePtr<Bytes> f3 = MakeNetworkRequest(request3);
// Wait<Future<>>("Network requests", {f1, f2, f3})->Then([] {
//   AllNetworkRequestsAreComplete();
// });
//
// See the Wait() function documentation for more details.
//
// ## Use Completer() to integrate with functions requiring callbacks
//
// Use the Completer() method to integrate with existing code that uses callback
// functions. Completer() returns a fit::function<void(Result)> that, when
// called, calls Complete() on the future. Re-visiting the first example:
//
// Without Completer():
//
// FuturePtr<Bytes> MakeNetworkRequest(NetworkRequest& request) {
//   auto f = Future<Bytes>::Create("NetworkRequest");
//   auto network_request_callback = [f] (Bytes bytes) {
//     f->Complete(bytes);
//   };
//   PerformAsyncNetworkRequest(request, network_request_callback);
//   return f;
// }
//
// With Completer():
//
// FuturePtr<Bytes> MakeNetworkRequest(NetworkRequest& request) {
//   auto f = Future<Bytes>::Create("NetworkRequest");
//   PerformAsyncNetworkRequest(request, f->Completer());
//   return f;
// }
//
// ## Use error values to propagate errors back to consumers
//
// If the future can fail, use a value (or multiple values) that's capable of
// storing both the error and a successful result to propagate the return value
// back to the consumer. For example:
//
// FuturePtr<std::error_code, Bytes> f = MakeNetworkRequest();
// f->Then([] (std::error_code error, Bytes bytes) {
//   if (error) {
//     // handle error
//   } else {
//     // network request was successful
//     ProcessBytes(bytes);
//   }
// });
//
// ## fuchsia::modular::Future vs other Futures/Promises
//
// If you are familiar with Futures & Promises in other languages, this Future
// class is intentionally different from others, to better integrate with
// Fuchsia coding patterns:
//
// * NOT THREADSAFE. This will change in the future when thread safety is
//   required, but there are no use cases yet. (YAGNI!)
// * Support for multiple result values via variadic template parameters. This
//   is required for smooth integration with the fuchsia::modular::Operation
//   class.
// * Only a single callback can be set via Then(), since move semantics are used
//   so heavily in Fuchsia code. (If multiple callbacks were supported and the
//   result is moved, how does one move the result from one callback to
//   another?)
//   * Multiple callbacks can be set if the callback lambda takes the result via
//     const&. In this case, use ConstThen() to attach each callback, rather
//     than Then(). ConstThen() calls can also be chained, like Then().
// * There are no success/error callbacks and control flows: all callbacks are
//   "success callbacks".
//   * The traditional reason for error callbacks is to convert exceptions into
//     error values, but Google C++ style doesn't use exceptions, so error
//     callbacks aren't needed.
//   * There's some argument that using separate success/error control flow
//     paths is beneficial. However, in practice, a lot of client code using
//     this pattern don't attach error callbacks, only success callbacks, so
//     errors often go unchecked.
//   * If error values need to be propagated back to the client, use a dedicated
//     error type. (Note that many built-in types may have values that can be
//     interpreted as errors, e.g. nullptr, or 0 or -1.) This also forces a
//     consumer to inspect the type and check for errors.
// * No cancellation/progress support. Adding cancellation/progress:
//   * adds more complexity to the futures implementation,
//   * can be implemented on top of a core futures implementation for the few
//     cases where they're required, and
//   * typically requires extra cancel/progress callbacks, which adds more
//     control flow paths.
//   * see fxl::CancelableCallback if you need cancellation.
// * No execution contexts (yet).
//   * There needs to be a comprehensive story about runloops etc first.

template <typename... Result>
class Future;

template <typename... Result>
using FuturePtr = fxl::RefPtr<Future<Result...>>;

namespace internal {

enum class FutureStatus {
  kAwaiting,   // not completed
  kCompleted,  // value available, not yet moved into callback
  kConsumed    // value moved into callback
};

// type_traits functions, ported from C++17.
template <typename From, typename To>
constexpr bool is_convertible_v = std::is_convertible<From, To>::value;

template <class T>
constexpr bool is_void_v = std::is_void<T>::value;

template <typename... Result>
class DefaultResultsFuture {
 public:
  using type = Future<std::vector<std::tuple<Result...>>>;
};

template <typename Result>
class DefaultResultsFuture<Result> {
 public:
  using type = Future<std::vector<Result>>;
};

template <>
class DefaultResultsFuture<> {
 public:
  using type = Future<>;
};

template <typename... Result>
using DefaultResultsFuture_t = typename DefaultResultsFuture<Result...>::type;

template <typename ResultsFuture>
class ResultCollector {
 public:
  // ResultsFuture = Future<std::vector<ElementType>>
  using ElementType =
      typename std::tuple_element_t<0, typename ResultsFuture::result_tuple_type>::value_type;

  ResultCollector(size_t reserved_count) : results_(reserved_count) {}

  bool IsComplete() const { return finished_count_ == results_.size(); }

  template <typename... Result>
  void AssignResult(size_t result_index, Result&&... result) {
    results_[result_index] = std::make_unique<ElementType>(std::forward<Result>(result)...);
    finished_count_++;
  }

  void Complete(ResultsFuture* future) {
    std::vector<ElementType> final_results;
    final_results.reserve(results_.size());
    for (auto& result : results_) {
      final_results.push_back(std::move(*result));
    }
    future->Complete(std::move(final_results));
  }

 private:
  size_t finished_count_ = 0;
  // Use unique ptrs initially so that we can reserve even if |ElementType| is
  // not default-constructible.
  //
  // TODO(rosswang): Consider adding a specialization for default-constructible
  // types.
  std::vector<std::unique_ptr<ElementType>> results_;
};

template <>
class ResultCollector<Future<>> {
 public:
  ResultCollector(size_t reserved_count);
  bool IsComplete() const;

  // The template on this allows us to use Future<> collectors to swallow
  // unneeded results of futures we're waiting on.
  template <typename... Result>
  void AssignResult(size_t, Result... result) {
    finished_count_++;
  }

  void Complete(Future<>* future) const;

 private:
  size_t finished_count_ = 0;
  size_t reserved_count_;
};

}  // namespace internal

template <typename... Result>
class Future : public fxl::RefCountedThreadSafe<Future<Result...>> {
 public:
  using result_tuple_type = std::tuple<Result...>;

  // Creates a FuturePtr<Result...>. |trace_name| is used solely for debugging
  // purposes, and is logged when something goes wrong (e.g. Complete() is
  // called twice.)
  static FuturePtr<Result...> Create(const std::string& trace_name) {
    auto f = fxl::AdoptRef(new Future<Result...>);
    f->trace_name_ = std::move(trace_name);
    return f;
  }

  // Creates a FuturePtr<Result...> that's already completed. For example:
  //
  //   FuturePtr<int> f = Future<int>::CreateCompleted("MyFuture", 5);
  //   f->Then([] (int i) {
  //     // this lambda executes immediately
  //     assert(i == 5);
  //   });
  static FuturePtr<Result...> CreateCompleted(const std::string& trace_name, Result&&... result) {
    auto f = Create(std::move(trace_name));
    f->Complete(std::forward<Result>(result)...);
    return f;
  }

  // Completes a future with |result|. This causes any callbacks registered
  // with Then(), ConstThen(), etc to be invoked with |result| passed to them
  // as a parameter.
  //
  // Calling Complete() does not affect this future's refcount. This is because:
  //
  // 1. Any callbacks that are registered are called immediately and
  //    synchronously, so the future's lifetime does not need to be extended
  //    before callbacks are invoked.
  // 2. Then() correctly handles cases where the future may be deleted by their
  //    callbacks.
  // 3. There is no danger of the future being deleted before Complete() is
  //    called, because if Complete() is called, the code that calls Complete()
  //    must have a reference to the future.
  void Complete(Result&&... result) {
    CompleteWithTuple(std::forward_as_tuple(std::forward<Result>(result)...));
  }

  // Returns a fit::function<void(Result)> that, when called, calls Complete()
  // on this future. For example:
  //
  // FuturePtr<Bytes> MakeNetworkRequest(NetworkRequest& request) {
  //   auto f = Future<Bytes>::Create();
  //   PerformAsyncNetworkRequest(request, f->Completer());
  //   return f;
  // }
  //
  // The returned closure will maintain a reference to the future, so that the
  // closure can call Complete() on it correctly later. In other words, calling
  // Completer() will increase this future's refcount, and you do not need to
  // maintain a reference to it. After the closure is called, the future's
  // refcount will drop by 1. This enables you to write code like
  //
  //   {
  //     auto f = Future<>::Create();
  //     CallAsyncMethod(f->Completer());
  //     // f will now go out of scope, but f->Completer() owns it, so it's
  //     // still kept alive.
  //   }
  fit::function<void(Result...)> Completer() {
    return [shared_this = FuturePtr<Result...>(this)](Result&&... result) {
      shared_this->Complete(std::forward<Result>(result)...);
    };
  }

  // Attaches a |callback| that is invoked when the future is completed with
  // Complete(), and returns a Future that is complete once |callback| has
  // finished executing.
  //
  // * The callback is invoked immediately (synchronously); it is not scheduled
  //   on the event loop.
  // * The callback is invoked on the same thread as the code that calls
  //   Complete().
  // * Only one callback can be attached: any callback that was previously
  //   attached with Then() is discarded.
  // * |callback| is called after callbacks attached with ConstThen().
  // * It is safe for |callback| to delete the future that Then() is invoked on.
  //   If this occurs, any chained futures returned by Then(), Map() etc will be
  //   de-referenced by this future and not be completed, even if a reference to
  //   the chained future is maintained elsewhere.
  // * It is also safe for |callback| to delete the chained future that Then()
  //   returns.
  // * The future returned by Then() will be owned by this future, so you do not
  //   need to maintain a reference to it.
  //
  // The type of this function looks complex, but is basically:
  //
  //   FuturePtr<> Then(fit::function<void(Result...)> callback);
  template <typename Callback, typename = typename std::enable_if_t<
                                   internal::is_void_v<std::invoke_result_t<Callback, Result...>>>>
  FuturePtr<> Then(Callback callback) {
    return SubfutureCreate(Future<>::Create(trace_name_ + "(Then)"),
                           SubfutureVoidCallback<Result...>(std::move(callback)),
                           SubfutureCompleter<>(), [] { return true; });
  }

  // Equivalent to Then(), but guards execution of |callback| with a WeakPtr.
  // If, at the time |callback| is to be executed, |weak_ptr| has been
  // invalidated, |callback| is not run, nor is the next Future in the chain
  // completed.
  template <typename Callback, typename T,
            typename = typename std::enable_if_t<
                internal::is_void_v<std::invoke_result_t<Callback, Result...>>>>
  FuturePtr<> WeakThen(fxl::WeakPtr<T> weak_ptr, Callback callback) {
    return SubfutureCreate(Future<>::Create(trace_name_ + "(WeakThen)"),
                           SubfutureVoidCallback<Result...>(std::move(callback)),
                           SubfutureCompleter<>(), [weak_ptr] { return !!weak_ptr; });
  }

  // Similar to Then(), except that:
  //
  // * |const_callback| must take in the completed result via a const&,
  // * multiple callbacks can be attached,
  // * |const_callback| is called _before_ the Then() callback.
  FuturePtr<> ConstThen(fit::function<void(const Result&...)> const_callback) {
    FuturePtr<> subfuture = Future<>::Create(trace_name_ + "(ConstThen)");
    AddConstCallback(SubfutureCallback<const Result&...>(
        subfuture, SubfutureVoidCallback<const Result&...>(std::move(const_callback)),
        SubfutureCompleter<>(), [] { return true; }));
    return subfuture;
  }

  // See WeakThen().
  template <typename T>
  FuturePtr<> WeakConstThen(fxl::WeakPtr<T> weak_ptr,
                            fit::function<void(const Result&...)> const_callback) {
    FuturePtr<> subfuture = Future<>::Create(trace_name_ + "(WeakConstThen)");
    AddConstCallback(SubfutureCallback<const Result&...>(
        subfuture, SubfutureVoidCallback<const Result&...>(std::move(const_callback)),
        SubfutureCompleter<>(), [weak_ptr] { return !!weak_ptr; }));
    return subfuture;
  }

  // Attaches a |callback| that is invoked when this future is completed with
  // Complete(). |callback| must return another future: when the returned future
  // completes, the future returned by AsyncMap() will complete. For example:
  //
  // ShowProgressSpinner(true);
  // FuturePtr<Bytes> f = MakeNetworkRequest(request);
  // f->AsyncMap([] (Bytes zipped_bytes) {
  //   FuturePtr<Bytes> f = UnzipInBackground(zipped_bytes);
  //   return f;
  // })->AsyncMap([] (Bytes unzipped_bytes) {
  //   FuturePtr<JPEGImage> f = DecodeImageInBackground(unzipped_bytes);
  //   return f;
  // })->AsyncMap([] (JPEGImage image) {
  //   FuturePtr<> f = UpdateUIAsynchronously(image);
  //   return f;
  // })->Then([] {
  //   ShowProgressSpinner(false);
  // });
  //
  // The type of this method looks terrifying, but is basically:
  //
  //   FuturePtr<CallbackResult>
  //     AsyncMap(fit::function<FuturePtr<CallbackResult>(Result...)> callback);
  template <typename Callback, typename AsyncMapResult = std::invoke_result_t<Callback, Result...>,
            typename MapResult = typename AsyncMapResult::element_type::result_tuple_type,
            typename = typename std::enable_if_t<
                internal::is_convertible_v<FuturePtr<MapResult>, AsyncMapResult>>>
  AsyncMapResult AsyncMap(Callback callback) {
    return SubfutureCreate(AsyncMapResult::element_type::Create(trace_name_ + "(AsyncMap)"),
                           std::move(callback), SubfutureAsyncMapCompleter<AsyncMapResult>(),
                           [] { return true; });
  }

  template <typename Callback, typename T,
            typename AsyncMapResult = std::invoke_result_t<Callback, Result...>,
            typename MapResult = typename AsyncMapResult::element_type::result_tuple_type,
            typename = typename std::enable_if_t<
                internal::is_convertible_v<FuturePtr<MapResult>, AsyncMapResult>>>
  AsyncMapResult WeakAsyncMap(fxl::WeakPtr<T> weak_ptr, Callback callback) {
    return SubfutureCreate(AsyncMapResult::element_type::Create(trace_name_ + "(WeakAsyncMap)"),
                           std::move(callback), SubfutureAsyncMapCompleter<AsyncMapResult>(),
                           [weak_ptr] { return !!weak_ptr; });
  }

  // Attaches a |callback| that is invoked when this future is completed with
  // Complete(). The returned future is completed with |callback|'s return
  // value, when |callback| finishes executing. Returned tuples are flattened
  // into variadic futures.
  //
  // To return a future that produces a tuple (uncommon), wrap the map result
  // in another tuple (or use |AsyncMap|).
  //
  // That is:
  // Callback return type              | Returned future
  // ----------------------------------+----------------
  // T                                 | FuturePtr<T>
  // std::tuple<T, U, ...>             | FuturePtr<T, U, ...>
  // std::tuple<std::tuple<T, U, ...>> | FuturePtr<std::tuple<T, U, ...>>
  template <typename Callback, typename MapResult = std::invoke_result_t<Callback, Result...>>
  auto Map(Callback callback) {
    return Map(std::move(callback), Tag<MapResult>{});
  }

  template <typename Callback, typename T,
            typename MapResult = std::invoke_result_t<Callback, Result...>>
  auto WeakMap(fxl::WeakPtr<T> weak_ptr, Callback callback) {
    return WeakMap(weak_ptr, std::move(callback), Tag<MapResult>{});
  }

  const std::string& trace_name() const { return trace_name_; }

 private:
  template <typename... Args>
  friend class Future;

  // This is a utility class used as a template parameter to determine function
  // overloading; see
  // <https://www.boost.org/community/generic_programming.html#tag_dispatching>
  // for more info.
  //
  // It is used in the |Map| overloads to "specialize" for |std::tuple| and
  // capture its parameter pack.
  template <typename T>
  class Tag {};

  template <typename ResultsFuture, typename... Args>
  friend fxl::RefPtr<ResultsFuture> Wait(const std::string& trace_name,
                                         const std::vector<FuturePtr<Args...>>& futures);

  template <typename ResultsFuture, typename TimeoutCallback, typename... Args>
  friend fxl::RefPtr<ResultsFuture> WaitWithTimeout(const std::string& trace_name,
                                                    async_dispatcher_t* dispatcher,
                                                    zx::duration timeout,
                                                    TimeoutCallback on_timeout,
                                                    const std::vector<FuturePtr<Args...>>& futures);

  FRIEND_REF_COUNTED_THREAD_SAFE(Future);

  Future() : result_{}, weak_factory_(this) {}

  void SetCallback(fit::function<void(Result...)> callback) {
    callback_ = std::move(callback);

    MaybeInvokeCallbacks();
  }

  void SetCallbackWithTuple(fit::function<void(std::tuple<Result...>)> callback) {
    SetCallback([callback = std::move(callback)](Result&&... result) {
      callback(std::forward_as_tuple(std::forward<Result>(result)...));
    });
  }

  void AddConstCallback(fit::function<void(const Result&...)> callback) {
    if (!callback) {
      return;
    }

    // It's impossible to add a const callback after a future is completed
    // *and* it has a callback: the completed value will be moved into the
    // callback and won't be available for a ConstThen().
    if (status_ == internal::FutureStatus::kConsumed) {
      FX_LOGS(FATAL) << "Future@" << static_cast<void*>(this)
                     << (trace_name_.length() ? "(" + trace_name_ + ")" : "")
                     << ": Cannot add a const callback after completed result is "
                        "already moved into Then() callback.";
    }

    const_callbacks_.emplace_back(std::move(callback));

    MaybeInvokeCallbacks();
  }

  void CompleteWithTuple(std::tuple<Result...>&& result) {
    FX_DCHECK(status_ == internal::FutureStatus::kAwaiting)
        << "Future@" << static_cast<void*>(this)
        << (trace_name_.length() ? "(" + trace_name_ + ")" : "") << ": Complete() called twice.";

    result_ = std::forward<std::tuple<Result...>>(result);
    status_ = internal::FutureStatus::kCompleted;

    MaybeInvokeCallbacks();
  }

  void MaybeInvokeCallbacks() {
    if (status_ == internal::FutureStatus::kAwaiting) {
      return;
    }

    if (const_callbacks_.size()) {
      // Move |const_callbacks_| to a local variable. MaybeInvokeCallbacks()
      // can be called multiple times if the client only uses ConstThen() or
      // WeakConstThen() to fetch the completed values. This prevents calling
      // these callbacks multiple times by moving them out of the members
      // scope.
      auto local_const_callbacks = std::move(const_callbacks_);
      for (auto& const_callback : local_const_callbacks) {
        fxl::Apply(const_callback, result_);
      }
    }

    if (callback_) {
      auto callback = std::move(callback_);
      status_ = internal::FutureStatus::kConsumed;
      fxl::Apply(callback, std::move(result_));
    }
  }

  // The "subfuture" methods below are private helper functions designed to be
  // used with the futures that are returned by the public API (Then(), Map(),
  // etc); those returned futures are named "subfutures" in the code, which is
  // why the methods are named likewise.

  // A convenience method to call this->SetCallback() with the lambda returned
  // by SubfutureCallback().
  template <typename Subfuture, typename SubfutureCompleter, typename Callback, typename Guard>
  Subfuture SubfutureCreate(Subfuture subfuture, Callback&& callback,
                            SubfutureCompleter&& subfuture_completer, Guard&& guard) {
    SetCallback(SubfutureCallback<Result...>(subfuture, callback, subfuture_completer, guard));
    return subfuture;
  }

  // Returns a lambda that:
  //
  // 1. calls |guard| before calling |callback|, and only calls |callback| if
  //    |guard| returns true;
  // 2. will not call |subfuture_completer| if either |this| or |subfuture| are
  //    destroyed by |callback|.
  template <typename... CoercedResult, typename Subfuture, typename SubfutureCompleter,
            typename Callback, typename Guard>
  auto SubfutureCallback(Subfuture subfuture, Callback&& callback,
                         SubfutureCompleter&& subfuture_completer, Guard&& guard) {
    return [this, subfuture, callback = std::move(callback), subfuture_completer,
            guard](CoercedResult&&... result) mutable {
      if (!guard())
        return;

      auto weak_future = weak_factory_.GetWeakPtr();
      auto weak_subfuture = subfuture->weak_factory_.GetWeakPtr();
      auto subfuture_result = callback(std::forward<CoercedResult>(result)...);

      // |callback| above may delete this future or the returned subfuture when
      // it finishes executing, so check if |weak_future| and |weak_subfuture|
      // are still valid before attempting to complete the subfuture.
      if (weak_future && weak_subfuture)
        subfuture_completer(subfuture, std::move(subfuture_result));
    };
  }

  // Returns a lambda that calls |callback|, and returns an empty tuple. The
  // consistent return type enables generic programming techniques to be
  // applied to |callback| since the return type is consistent (it's always a
  // |std::tuple<T...>|).
  template <typename... CoercedResult, typename Callback>
  auto SubfutureVoidCallback(Callback&& callback) {
    return [callback = std::move(callback)](CoercedResult&&... result) mutable {
      callback(std::forward<CoercedResult>(result)...);
      return std::make_tuple();
    };
  }

  // Returns a lambda that, when called with a subfuture and a std::tuple, will
  // complete the subfuture with the values from the tuple elements. This method
  // is designed to be used with SubfutureAsyncMapCompleter(), which will do
  // the same thing but can be passed Futures for the std::tuple values.
  // Together, this enables generic programming techniques to be applied to the
  // returned lambda, since the lambda presents a consistent API for callers.
  template <typename... SubfutureResult>
  auto SubfutureCompleter() {
    return [](FuturePtr<SubfutureResult...> subfuture,
              std::tuple<SubfutureResult...> subfuture_result) {
      subfuture->CompleteWithTuple(std::move(subfuture_result));
    };
  }

  // See the documentation for SubfutureCompleter() above.
  template <typename AsyncMapResult>
  auto SubfutureAsyncMapCompleter() {
    return [](AsyncMapResult subfuture, std::tuple<AsyncMapResult> subfuture_result) {
      std::get<0>(subfuture_result)
          ->SetCallbackWithTuple(
              [subfuture](std::tuple<typename AsyncMapResult::element_type::result_tuple_type>
                              transformed_result) {
                subfuture->CompleteWithTuple(std::move(std::get<0>(transformed_result)));
              });
    };
  }

  // The following overloads enable |Map| and |WeakMap| to flatten out functions that map to
  // |std::tuple|.

  template <typename Callback, typename MapResult>
  FuturePtr<MapResult> Map(Callback callback, Tag<MapResult>) {
    // Directly passing |callback| like this ends up relying on an implicit
    // |std::tuple| memberwise constructor, which should be fine. It will
    // convert from |MapResult| to |std::tuple<MapResult>| implicitly.
    return Map(std::move(callback), Tag<std::tuple<MapResult>>{});
  }

  template <typename Callback, typename... MapResult>
  FuturePtr<MapResult...> Map(Callback callback, Tag<std::tuple<MapResult...>>) {
    return SubfutureCreate(Future<MapResult...>::Create(trace_name_ + "(Map)"), std::move(callback),
                           SubfutureCompleter<MapResult...>(), [] { return true; });
  }

  template <typename Callback, typename T, typename MapResult>
  FuturePtr<MapResult> WeakMap(fxl::WeakPtr<T> weak_ptr, Callback callback, Tag<MapResult>) {
    // Directly passing |callback| like this ends up relying on an implicit
    // |std::tuple| memberwise constructor, which should be fine. It will
    // convert from |MapResult| to |std::tuple<MapResult>| implicitly.
    return WeakMap(weak_ptr, std::move(callback), Tag<std::tuple<MapResult>>{});
  }

  template <typename Callback, typename T, typename... MapResult>
  FuturePtr<MapResult...> WeakMap(fxl::WeakPtr<T> weak_ptr, Callback callback,
                                  Tag<std::tuple<MapResult...>>) {
    return SubfutureCreate(Future<MapResult...>::Create(trace_name_ + "(WeakMap)"),
                           std::move(callback), SubfutureCompleter<MapResult...>(),
                           [weak_ptr] { return !!weak_ptr; });
  }

  std::string trace_name_;

  internal::FutureStatus status_ = internal::FutureStatus::kAwaiting;
  std::tuple<Result...> result_;

  // The callback attached to this future.
  fit::function<void(Result...)> callback_;

  // Callbacks that have attached with the Const*() methods, such as
  // ConstThen().
  std::vector<fit::function<void(const Result&...)>> const_callbacks_;

  // Keep this last in the list of members. (See WeakPtrFactory documentation
  // for more info.)
  fxl::WeakPtrFactory<Future<Result...>> weak_factory_;

  FXL_DISALLOW_COPY_ASSIGN_AND_MOVE(Future);

  // For unit tests only.
  friend class FutureTest;

  // For unit tests only.
  const std::tuple<Result...>& get() const {
    FX_DCHECK(status_ != internal::FutureStatus::kAwaiting)
        << trace_name_ << ": get() called on unset future";

    return result_;
  }
};

// Returns a Future that completes when every future in |futures| is complete.
// The future returned by Wait() will be kept alive until every future in
// |futures| either completes or is destroyed. If any future in |futures| is
// destroyed prior to completing, the returned future will never complete. The
// order of the results corresponds to the order of the given futures,
// regardless of their completion order.
//
// The default type of the resulting future depends on the types of the
// component futures. Void futures produce a void future. Monadic futures
// produce a future of a flat vector. Polyadic futures produce a future of a
// vector of tuples. That is:
//
// Components         | Result
// -------------------+--------------------------------------------
// FuturePtr<>        | FuturePtr<>
// FuturePtr<T>       | FuturePtr<std::vector<T>>
// FuturePtr<T, U...> | FuturePtr<std::vector<std::tuple<T, U...>>>
//
// These defaults may be overridden by specifying the template argument for
// Wait as the type of future desired. All cases may produce a Future<> or a
// Future<std::vector<std::tuple<...>>>.
//
// Example usage:
//
// FuturePtr<Bytes> f1 = MakeNetworkRequest(request1);
// FuturePtr<Bytes> f2 = MakeNetworkRequest(request2);
// FuturePtr<Bytes> f3 = MakeNetworkRequest(request3);
// std::vector<FuturePtr<Bytes>> requests{f1, f2, f3};
// Wait("NetworkRequests", requests)->Then([](
//     std::vector<Bytes> bytes_vector) {
//   Bytes f1_bytes = bytes_vector[0];
//   Bytes f2_bytes = bytes_vector[1];
//   Bytes f3_bytes = bytes_vector[2];
// });
//
// This is similar to Promise.All() in JavaScript, or Join() in Rust.
template <typename ResultsFuture, typename... Result>
fxl::RefPtr<ResultsFuture> Wait(const std::string& trace_name,
                                const std::vector<FuturePtr<Result...>>& futures) {
  if (futures.empty()) {
    auto immediate = ResultsFuture::Create(trace_name + "(Completed)");
    immediate->CompleteWithTuple({});
    return immediate;
  }

  auto results = std::make_shared<internal::ResultCollector<ResultsFuture>>(futures.size());

  fxl::RefPtr<ResultsFuture> all_futures_completed =
      ResultsFuture::Create(trace_name + "(WillWait)");

  for (size_t i = 0; i < futures.size(); i++) {
    const auto& future = futures[i];
    future->SetCallback([i, all_futures_completed, results](Result&&... result) {
      results->AssignResult(i, std::forward<Result>(result)...);

      if (results->IsComplete()) {
        results->Complete(all_futures_completed.get());
      }
    });
  }

  return all_futures_completed;
}

// Like |Wait|, but gives up after a timeout. After the timeout, |on_timeout| is
// invoked with a diagnostic error string containing the trace names of the
// futures that have not completed.
//
// This maintains a reference to the returned |Future| until all component
// futures have been completed or destroyed, or until the timeout has elapsed,
// whichever happens first. However, |on_timeout| will be invoked on timeout if
// any future has not completed even if any or all futures have been destroyed.
template <typename ResultsFuture, typename TimeoutCallback, typename... Result>
fxl::RefPtr<ResultsFuture> WaitWithTimeout(
    const std::string& trace_name, async_dispatcher_t* dispatcher, zx::duration timeout,
    TimeoutCallback on_timeout /* void (const std::string&) */,
    const std::vector<FuturePtr<Result...>>& futures) {
  auto all_futures_completed = Wait<ResultsFuture>(trace_name, futures);

  if (all_futures_completed->status_ != internal::FutureStatus::kAwaiting) {
    return all_futures_completed;
  }

  auto all_trace_names = std::make_shared<std::vector<std::unique_ptr<std::string>>>();
  all_trace_names->reserve(futures.size());

  for (const auto& future : futures) {
    // There's no point in waiting on completed futures. Furthermore if we tried
    // that we'd have to put this before |Wait| since |Wait| consumes the
    // results, but then if all futures are already completed this is all just
    // wasted effort.
    if (future->status_ == internal::FutureStatus::kAwaiting) {
      size_t i = all_trace_names->size();
      all_trace_names->push_back(std::make_unique<std::string>(future->trace_name_));
      future->AddConstCallback([i, all_trace_names](const Result&...) {
        if (!all_trace_names->empty()) {
          (*all_trace_names)[i] = nullptr;
        }
      });
    }
  }

  // Return a proxy so that we can cancel result forwarding in the case of a
  // timeout. This could with more difficulty be done within |Wait|, but this
  // way allows us to reuse the logic more easily.
  fxl::RefPtr<ResultsFuture> all_proxy =
      ResultsFuture::Create(trace_name + "(WillWaitWithTimeout)");
  all_futures_completed->SetCallback(all_proxy->Completer());

  // TODO(rosswang): Factor this into dump and cancel functions that can be
  // called at other times.
  async::PostDelayedTask(
      dispatcher,
      [all_trace_names = std::move(all_trace_names), on_timeout = std::move(on_timeout),
       all_futures_completed = all_futures_completed->weak_factory_.GetWeakPtr()] {
        std::ostringstream msg;
        for (const auto& trace_name : *all_trace_names) {
          if (trace_name) {
            msg << "\n\t" << *trace_name;
          }
        }
        if (!msg.str().empty()) {
          on_timeout("Wait timed out. Still waiting for futures:" + msg.str());
          if (all_futures_completed) {
            // cancel results forwarding (possibly releasing all_proxy)
            all_futures_completed->SetCallback(nullptr);
          }
          // Possibly release the component futures. Once this task completes
          // and goes out of scope, the last reference to the Wait future (also
          // holding onto the component futures) should be released as well.
          all_trace_names->clear();
        }
      },
      timeout);

  return all_proxy;
}

// |WaitWithTimeout| on the thread defaut dispatcher.
template <typename ResultsFuture, typename TimeoutCallback, typename... Result>
fxl::RefPtr<ResultsFuture> WaitWithTimeout(
    const std::string& trace_name, zx::duration timeout,
    TimeoutCallback on_timeout /* void (const std::string&) */,
    const std::vector<FuturePtr<Result...>>& futures) {
  return WaitWithTimeout<ResultsFuture>(trace_name, async_get_default_dispatcher(), timeout,
                                        std::move(on_timeout), futures);
}

// These overloads allow us to effectively default the first template parameter,
// |ResultsFuture| (since the others are intended to be inferred).
// TODO(rosswang): If the overload combinatoric explosion gets too heavy, we can
// use a dummy struct parameter instead to encapsulate that template parameter.

template <typename... Result>
auto Wait(const std::string& trace_name, const std::vector<FuturePtr<Result...>>& futures) {
  return Wait<internal::DefaultResultsFuture_t<Result...>>(trace_name, futures);
}

template <typename TimeoutCallback, typename... Result>
auto WaitWithTimeout(const std::string& trace_name, async_dispatcher_t* dispatcher,
                     zx::duration timeout, TimeoutCallback on_timeout,
                     const std::vector<FuturePtr<Result...>>& futures) {
  return WaitWithTimeout<internal::DefaultResultsFuture_t<Result...>>(
      trace_name, dispatcher, timeout, std::move(on_timeout), futures);
}

template <typename TimeoutCallback, typename... Result>
auto WaitWithTimeout(const std::string& trace_name, zx::duration timeout,
                     TimeoutCallback on_timeout /* void (const std::string&) */,
                     const std::vector<FuturePtr<Result...>>& futures) {
  return WaitWithTimeout<internal::DefaultResultsFuture_t<Result...>>(
      trace_name, async_get_default_dispatcher(), timeout, std::move(on_timeout), futures);
}

// We need to provide the initializer list overloads or template deduction fails
// for the above overloads if given an initializer list.
// TODO(rosswang): Add a potentially heterogeneous variadic template instead,
// and prefer it over initializer lists outside of tests.
template <typename ResultsFuture, typename... Result>
auto Wait(const std::string& trace_name, std::initializer_list<FuturePtr<Result...>> futures) {
  return Wait<ResultsFuture>(trace_name, std::vector<FuturePtr<Result...>>(futures));
}

template <typename ResultsFuture, typename TimeoutCallback, typename... Result>
fxl::RefPtr<ResultsFuture> WaitWithTimeout(const std::string& trace_name,
                                           async_dispatcher_t* dispatcher, zx::duration timeout,
                                           TimeoutCallback on_timeout,
                                           std::initializer_list<FuturePtr<Result...>> futures) {
  return WaitWithTimeout<ResultsFuture>(trace_name, dispatcher, timeout, std::move(on_timeout),
                                        std::vector<FuturePtr<Result...>>(futures));
}

template <typename ResultsFuture, typename TimeoutCallback, typename... Result>
fxl::RefPtr<ResultsFuture> WaitWithTimeout(
    const std::string& trace_name, zx::duration timeout,
    TimeoutCallback on_timeout /* void (const std::string&) */,
    std::initializer_list<FuturePtr<Result...>> futures) {
  return WaitWithTimeout<ResultsFuture>(trace_name, async_get_default_dispatcher(), timeout,
                                        std::move(on_timeout),
                                        std::vector<FuturePtr<Result...>>(futures));
}

template <typename... Result>
auto Wait(const std::string& trace_name, std::initializer_list<FuturePtr<Result...>> futures) {
  return Wait(trace_name, std::vector<FuturePtr<Result...>>(futures));
}

template <typename TimeoutCallback, typename... Result>
auto WaitWithTimeout(const std::string& trace_name, async_dispatcher_t* dispatcher,
                     zx::duration timeout, TimeoutCallback on_timeout,
                     std::initializer_list<FuturePtr<Result...>> futures) {
  return WaitWithTimeout(trace_name, dispatcher, timeout, std::move(on_timeout),
                         std::vector<FuturePtr<Result...>>(futures));
}

template <typename TimeoutCallback, typename... Result>
auto WaitWithTimeout(const std::string& trace_name, zx::duration timeout,
                     TimeoutCallback on_timeout /* void (const std::string&) */,
                     std::initializer_list<FuturePtr<Result...>> futures) {
  return WaitWithTimeout(trace_name, async_get_default_dispatcher(), timeout, std::move(on_timeout),
                         std::vector<FuturePtr<Result...>>(futures));
}

}  // namespace modular

#endif  // SRC_MODULAR_LIB_ASYNC_CPP_FUTURE_H_
