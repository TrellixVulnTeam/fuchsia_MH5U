// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include <lib/async/cpp/task.h>
#include <lib/fpromise/bridge.h>
#include <lib/inspect/contrib/cpp/archive_reader.h>
#include <unistd.h>

#include <iostream>
#include <set>
#include <string>
#include <thread>

#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <src/lib/fsl/vmo/strings.h>
#include <src/lib/fxl/strings/join_strings.h>

namespace inspect {
namespace contrib {

constexpr char kPathName[] = "moniker";
constexpr char kContentsName[] = "payload";

// Time to delay between snapshots to find components.
// 250ms so that tests are not overly delayed. Missing the component at
// first is common since the system needs time to start it and receive
// the events.
constexpr size_t kDelayMs = 250;

void EmplaceDiagnostics(rapidjson::Document document, std::vector<DiagnosticsData>* out) {
  if (document.IsArray()) {
    for (auto& value : document.GetArray()) {
      // We need to ensure that the value is safely moved between documents, which may involve
      // copying.
      //
      // It is an error to maintain a reference to a Value in a Document after that Document is
      // destroyed, and the input |document| is destroyed immediately after this branch.
      rapidjson::Document value_document;
      rapidjson::Value temp(value.Move(), value_document.GetAllocator());
      value_document.Swap(temp);
      out->emplace_back(DiagnosticsData(std::move(value_document)));
    }
  } else {
    out->emplace_back(DiagnosticsData(std::move(document)));
  }
}

namespace {

bool AllDigits(const std::string& value) {
  for (char c : value) {
    if (!std::isdigit(c)) {
      return false;
    }
  }
  return !value.empty();
}

void SortObject(rapidjson::Value* object) {
  std::sort(object->MemberBegin(), object->MemberEnd(),
            [](const rapidjson::Value::Member& lhs, const rapidjson::Value::Member& rhs) {
              auto lhs_name = lhs.name.GetString();
              auto rhs_name = rhs.name.GetString();
              if (AllDigits(lhs_name) && AllDigits(rhs_name)) {
                uint64_t lhs_val = atoll(lhs_name);
                uint64_t rhs_val = atoll(rhs_name);
                return lhs_val < rhs_val;
              } else {
                return strcmp(lhs_name, rhs_name) < 0;
              }
            });
}

void InnerReadBatches(fuchsia::diagnostics::BatchIteratorPtr ptr,
                      fpromise::bridge<std::vector<DiagnosticsData>, std::string> done,
                      std::vector<DiagnosticsData> ret) {
  ptr->GetNext(
      [ptr = std::move(ptr), done = std::move(done), ret = std::move(ret)](auto result) mutable {
        if (result.is_err()) {
          done.completer.complete_error("Batch iterator returned error: " +
                                        std::to_string(static_cast<size_t>(result.err())));
          return;
        }

        if (result.response().batch.empty()) {
          done.completer.complete_ok(std::move(ret));
          return;
        }

        for (const auto& content : result.response().batch) {
          if (!content.is_json()) {
            done.completer.complete_error("Received an unexpected content format");
            return;
          }
          std::string json;
          if (!fsl::StringFromVmo(content.json(), &json)) {
            done.completer.complete_error("Failed to read returned VMO");
            return;
          }
          rapidjson::Document document;
          document.Parse(json);

          EmplaceDiagnostics(std::move(document), &ret);
        }

        InnerReadBatches(std::move(ptr), std::move(done), std::move(ret));
      });
}

fpromise::promise<std::vector<DiagnosticsData>, std::string> ReadBatches(
    fuchsia::diagnostics::BatchIteratorPtr ptr) {
  fpromise::bridge<std::vector<DiagnosticsData>, std::string> result;
  auto consumer = std::move(result.consumer);
  InnerReadBatches(std::move(ptr), std::move(result), {});
  return consumer.promise_or(fpromise::error("Failed to obtain consumer promise"));
}

}  // namespace

DiagnosticsData::DiagnosticsData(rapidjson::Document document) : document_(std::move(document)) {
  if (document_.HasMember(kPathName) && document_[kPathName].IsString()) {
    std::string val = document_[kPathName].GetString();

    size_t idx = val.find_last_of("/");

    if (idx != std::string::npos) {
      name_ = val.substr(idx + 1);
    } else {
      name_ = std::move(val);
    }
  }
}

const std::string& DiagnosticsData::component_name() const { return name_; }

const rapidjson::Value& DiagnosticsData::content() const {
  static rapidjson::Value default_ret;

  if (!document_.IsObject() || !document_.HasMember(kContentsName)) {
    return default_ret;
  }

  return document_[kContentsName];
}

const rapidjson::Value& DiagnosticsData::GetByPath(const std::vector<std::string>& path) const {
  static rapidjson::Value default_ret;

  const rapidjson::Value* cur = &content();
  for (size_t i = 0; i < path.size(); i++) {
    if (!cur->IsObject()) {
      return default_ret;
    }

    auto it = cur->FindMember(path[i]);
    if (it == cur->MemberEnd()) {
      return default_ret;
    }

    cur = &it->value;
  }

  return *cur;
}

std::string DiagnosticsData::PrettyJson() {
  rapidjson::StringBuffer buffer;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buffer);
  document_.Accept(writer);
  return buffer.GetString();
}

void DiagnosticsData::Sort() {
  std::vector<rapidjson::Value*> pending;
  auto& object = document_;
  pending.push_back(&object);
  while (!pending.empty()) {
    rapidjson::Value* pending_object = pending.back();
    pending.pop_back();
    SortObject(pending_object);
    for (auto m = pending_object->MemberBegin(); m != pending_object->MemberEnd(); ++m) {
      if (m->value.IsObject()) {
        pending.push_back(&m->value);
      }
    }
  }
}

ArchiveReader::ArchiveReader(fuchsia::diagnostics::ArchiveAccessorPtr archive,
                             std::vector<std::string> selectors)

    : archive_(std::move(archive)),
      executor_(archive_.dispatcher()),
      selectors_(std::move(selectors)) {
  ZX_ASSERT(archive_.dispatcher() != nullptr);
}

fpromise::promise<std::vector<DiagnosticsData>, std::string> ArchiveReader::GetInspectSnapshot() {
  return fpromise::make_promise([this] {
           std::vector<fuchsia::diagnostics::SelectorArgument> selector_args;
           for (const auto& selector : selectors_) {
             fuchsia::diagnostics::SelectorArgument arg;
             arg.set_raw_selector(selector);
             selector_args.emplace_back(std::move(arg));
           }

           fuchsia::diagnostics::StreamParameters params;
           params.set_data_type(fuchsia::diagnostics::DataType::INSPECT);
           params.set_stream_mode(fuchsia::diagnostics::StreamMode::SNAPSHOT);
           params.set_format(fuchsia::diagnostics::Format::JSON);

           fuchsia::diagnostics::ClientSelectorConfiguration client_selector_config;
           if (!selector_args.empty()) {
             client_selector_config.set_selectors(std::move(selector_args));
           } else {
             client_selector_config.set_select_all(true);
           }

           params.set_client_selector_configuration(std::move(client_selector_config));

           fuchsia::diagnostics::BatchIteratorPtr iterator;
           archive_->StreamDiagnostics(std::move(params),
                                       iterator.NewRequest(archive_.dispatcher()));
           return ReadBatches(std::move(iterator));
         })
      .wrap_with(scope_);
}

fpromise::promise<std::vector<DiagnosticsData>, std::string>
ArchiveReader::SnapshotInspectUntilPresent(std::vector<std::string> component_names) {
  fpromise::bridge<std::vector<DiagnosticsData>, std::string> bridge;

  InnerSnapshotInspectUntilPresent(std::move(bridge.completer), std::move(component_names));

  return bridge.consumer.promise_or(fpromise::error("Failed to create bridge promise"));
}

void ArchiveReader::InnerSnapshotInspectUntilPresent(
    fpromise::completer<std::vector<DiagnosticsData>, std::string> completer,
    std::vector<std::string> component_names) {
  executor_.schedule_task(
      GetInspectSnapshot()
          .then([this, component_names = std::move(component_names),
                 completer = std::move(completer)](
                    fpromise::result<std::vector<DiagnosticsData>, std::string>& result) mutable {
            if (result.is_error()) {
              completer.complete_error(result.take_error());
              return;
            }

            auto value = result.take_value();
            std::set<std::string> remaining(component_names.begin(), component_names.end());
            for (const auto& val : value) {
              remaining.erase(val.component_name());
            }

            if (remaining.empty()) {
              completer.complete_ok(std::move(value));
            } else {
              fpromise::bridge<> timeout;
              async::PostDelayedTask(
                  executor_.dispatcher(),
                  [completer = std::move(timeout.completer)]() mutable { completer.complete_ok(); },
                  zx::msec(kDelayMs));
              executor_.schedule_task(timeout.consumer.promise_or(fpromise::error())
                                          .then([this, completer = std::move(completer),
                                                 component_names = std::move(component_names)](
                                                    fpromise::result<>& res) mutable {
                                            InnerSnapshotInspectUntilPresent(
                                                std::move(completer), std::move(component_names));
                                          })
                                          .wrap_with(scope_));
            }
          })
          .wrap_with(scope_));
}

}  // namespace contrib
}  // namespace inspect
