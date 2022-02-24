// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/syslog/cpp/macros.h>

#include "garnet/lib/perfmon/events.h"
#include "intel_tests.h"

class ProgrammableCounterVerifier : public Verifier {
 public:
  static std::unique_ptr<Verifier> Create(const cpuperf::SessionResultSpec* spec) {
    return std::make_unique<ProgrammableCounterVerifier>(spec);
  }

  ProgrammableCounterVerifier(const cpuperf::SessionResultSpec* spec) : Verifier(spec) {
    const perfmon::EventDetails* details;

    bool rc __UNUSED = LookupEventByName("arch", "llc_references", &details);
    FX_DCHECK(rc);
    llc_references_id_ = details->id;

    rc = LookupEventByName("arch", "llc_misses", &details);
    FX_DCHECK(rc);
    llc_misses_id_ = details->id;

    rc = LookupEventByName("arch", "branch_instructions_retired", &details);
    FX_DCHECK(rc);
    branch_instructions_retired_id_ = details->id;

    rc = LookupEventByName("arch", "branch_misses_retired", &details);
    FX_DCHECK(rc);
    branch_misses_retired_id_ = details->id;
  }

 private:
  bool VerifyRecord(const perfmon::SampleRecord& record) override {
    if (record.header->event == llc_references_id_) {
      ++llc_references_count_;
    } else if (record.header->event == llc_misses_id_) {
      ++llc_misses_count_;
    } else if (record.header->event == branch_instructions_retired_id_) {
      ++branch_instructions_retired_count_;
    } else if (record.header->event == branch_misses_retired_id_) {
      ++branch_misses_retired_count_;
    }
    return true;
  }

  bool VerifyTrace(const RecordCounts& counts) override {
    bool pass = true;
    if (llc_references_count_ == 0) {
      FX_LOGS(ERROR) << "Missing llc_references events";
      pass = false;
    }
    if (llc_misses_count_ == 0) {
      FX_LOGS(ERROR) << "Missing llc_misses events";
      pass = false;
    }
    if (branch_instructions_retired_count_ == 0) {
      FX_LOGS(ERROR) << "Missing branch_instructions_retired events";
      pass = false;
    }
    if (branch_misses_retired_count_ == 0) {
      FX_LOGS(ERROR) << "Missing branch_misses_retired events";
      pass = false;
    }
    return pass;
  }

  // Ids of the events we should see.
  perfmon::EventId llc_references_id_;
  perfmon::EventId llc_misses_id_;
  perfmon::EventId branch_instructions_retired_id_;
  perfmon::EventId branch_misses_retired_id_;

  // Counts of the events we should see;
  perfmon::EventId llc_references_count_ = 0;
  perfmon::EventId llc_misses_count_ = 0;
  perfmon::EventId branch_instructions_retired_count_ = 0;
  perfmon::EventId branch_misses_retired_count_ = 0;
};

const TestSpec kProgrammableCounterSpec = {
    "programmable-counters",
    &ProgrammableCounterVerifier::Create,
};
