// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_SYS_FUZZING_COMMON_CONTROLLER_H_
#define SRC_SYS_FUZZING_COMMON_CONTROLLER_H_

#include <fuchsia/fuzzer/cpp/fidl.h>
#include <lib/fidl/cpp/binding.h>
#include <lib/fidl/cpp/interface_request.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/synchronization/thread_annotations.h"
#include "src/sys/fuzzing/common/binding.h"
#include "src/sys/fuzzing/common/options.h"
#include "src/sys/fuzzing/common/response.h"
#include "src/sys/fuzzing/common/run-once.h"
#include "src/sys/fuzzing/common/runner.h"
#include "src/sys/fuzzing/common/transceiver.h"

namespace fuzzing {

using ::fuchsia::fuzzer::Controller;
using ::fuchsia::fuzzer::CorpusReader;
using ::fuchsia::fuzzer::CorpusReaderSyncPtr;
using ::fuchsia::fuzzer::Monitor;
using ::fuchsia::fuzzer::TargetAdapter;

using CorpusType = ::fuchsia::fuzzer::Corpus;
using FidlInput = ::fuchsia::fuzzer::Input;

class ControllerImpl : public Controller {
 public:
  ControllerImpl();
  ~ControllerImpl() override;

  // Sets the runner used to perform tasks.
  void SetRunner(std::unique_ptr<Runner> runner);

  // Binds the FIDL interface request to this object.
  void Bind(fidl::InterfaceRequest<Controller> request);

  // FIDL methods.
  void Configure(Options options, ConfigureCallback callback) override;
  void GetOptions(GetOptionsCallback callback) override;
  void AddToCorpus(CorpusType corpus, FidlInput input, AddToCorpusCallback callback) override;
  void ReadCorpus(CorpusType corpus, fidl::InterfaceHandle<CorpusReader> reader,
                  ReadCorpusCallback callback) override FXL_LOCKS_EXCLUDED(mutex_);
  void WriteDictionary(FidlInput dictionary, WriteDictionaryCallback callback) override;
  void ReadDictionary(ReadDictionaryCallback callback) override;
  void AddMonitor(fidl::InterfaceHandle<Monitor> monitor, AddMonitorCallback callback) override;
  void GetStatus(GetStatusCallback callback) override;
  void GetResults(GetResultsCallback callback) override;

  void Execute(FidlInput fidl_input, ExecuteCallback callback) override;
  void Minimize(FidlInput fidl_input, MinimizeCallback callback) override;
  void Cleanse(FidlInput fidl_input, CleanseCallback callback) override;
  void Fuzz(FuzzCallback callback) override;
  void Merge(MergeCallback callback) override;

  // Stages of stopping: close sources of new tasks, interrupt the current task, and join it.
  void Close() { close_.Run(); }
  void Interrupt() { interrupt_.Run(); }
  void Join() { join_.Run(); }

 private:
  // Adds defaults for unset options.
  void AddDefaults();

  // Factory method for making FIDL responses.
  template <typename Callback>
  Response NewResponse(Callback callback) {
    Response response;
    response.set_dispatcher(dispatcher_);
    response.set_transceiver(transceiver_);
    response.set_callback(std::move(callback));
    return response;
  }

  // Asynchronously receives a |fidl_input| via the transceiver before invoking the |callback| and
  // using it to send the |response|.
  void ReceiveAndThen(FidlInput fidl_input, Response response,
                      fit::function<void(Input, Response)> callback);

  // Thread body for the corpus reader client.
  void ReadCorpusLoop() FXL_LOCKS_EXCLUDED(mutex_);

  // Stop-related methods.
  void CloseImpl();
  void InterruptImpl();
  void JoinImpl();

  Binding<Controller> binding_;
  std::unique_ptr<Runner> runner_;

  // These pointers are instantiated by the controller and shared with other objects.
  std::shared_ptr<Dispatcher> dispatcher_;
  std::shared_ptr<Options> options_;
  std::shared_ptr<Transceiver> transceiver_;

  // CorpusReader requests are handled by a designated thread to avoid blocking the FIDL dispatcher.
  using CorpusReaderRequest = std::pair<CorpusType, CorpusReaderSyncPtr>;
  std::mutex mutex_;
  std::thread reader_;
  std::deque<CorpusReaderRequest> readers_ FXL_GUARDED_BY(mutex_);
  bool reading_ FXL_GUARDED_BY(mutex_) = true;
  SyncWait pending_readers_;

  RunOnce close_;
  RunOnce interrupt_;
  RunOnce join_;

  FXL_DISALLOW_COPY_ASSIGN_AND_MOVE(ControllerImpl);
};

}  // namespace fuzzing

#endif  // SRC_SYS_FUZZING_COMMON_CONTROLLER_H_
