// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    super::DoneSignaler,
    anyhow::{bail, Error},
    fidl_fuchsia_feedback as fcrash, fuchsia_async as fasync,
    futures::StreamExt,
    std::sync::{
        atomic::{AtomicUsize, Ordering},
        Arc,
    },
    tracing::*,
};

const REPORT_PROGRAM_NAME: &str = "triage_detect";
const REGISTER_PRODUCT_NAME: &str = "FuchsiaDetect";

/// FakeCrashReportingProductRegister can be injected to capture Detect's program-name registration.
pub struct FakeCrashReportingProductRegister {
    done_signaler: DoneSignaler,
    // The program under test should call upsert_with_ack() exactly once, with predictable inputs.
    // So the correctness can be tracked with an AtomicUsize which will have the following values:
    // 0: No call to upsert_with_ack().
    // 1: A single correct call to upsert_with_ack().
    // >1: Error: Multiple calls, and/or incorrect call.
    ok_tracker: AtomicUsize,
}

fn evaluate_registration(
    program_name: String,
    product: &fcrash::CrashReportingProduct,
) -> Result<(), Error> {
    let fcrash::CrashReportingProduct { name: product_name, .. } = product;
    if product_name != &Some(REGISTER_PRODUCT_NAME.to_string()) {
        bail!(
            "Crash report program name should be {} but it was {:?}",
            REGISTER_PRODUCT_NAME,
            product_name
        );
    }
    if &program_name != REPORT_PROGRAM_NAME {
        bail!("program_name should be {} but it was {:?}", REGISTER_PRODUCT_NAME, program_name);
    }
    Ok(())
}

impl FakeCrashReportingProductRegister {
    pub fn new(done_signaler: DoneSignaler) -> Arc<FakeCrashReportingProductRegister> {
        Arc::new(FakeCrashReportingProductRegister {
            done_signaler,
            ok_tracker: AtomicUsize::new(0),
        })
    }

    fn record_correct_registration(&self) {
        self.ok_tracker.fetch_add(1, Ordering::Relaxed);
    }

    fn record_bad_registration(&self) {
        info!("Detected bad registration");
        self.ok_tracker.fetch_add(2, Ordering::Relaxed);
    }

    pub fn detected_error(&self) -> bool {
        let state = self.ok_tracker.load(Ordering::Relaxed);
        state > 1
    }

    pub fn detected_good_registration(&self) -> bool {
        let state = self.ok_tracker.load(Ordering::Relaxed);
        state == 1
    }

    async fn serve(
        self: Arc<Self>,
        mut request_stream: fcrash::CrashReportingProductRegisterRequestStream,
    ) -> Result<(), Error> {
        loop {
            match request_stream.next().await {
                Some(Ok(fcrash::CrashReportingProductRegisterRequest::Upsert { .. })) => {
                    error!("We shouldn't be calling upsert")
                }
                Some(Ok(fcrash::CrashReportingProductRegisterRequest::UpsertWithAck {
                    component_url,
                    product,
                    responder,
                    ..
                })) => {
                    match evaluate_registration(component_url, &product) {
                        Ok(()) => {
                            self.record_correct_registration();
                        }
                        Err(problem) => {
                            error!("Problem in report: {}", problem);
                            self.record_bad_registration();
                            self.done_signaler.signal_done().await;
                        }
                    }
                    match responder.send() {
                        Ok(()) => (),
                        Err(problem) => error!("Failed to send response: {}", problem),
                    }
                }
                Some(Err(e)) => {
                    info!("Registration error: {}", e);
                    self.record_bad_registration();
                    self.done_signaler.signal_done().await;
                    bail!("{}", e);
                }
                None => break,
            }
        }
        Ok(())
    }

    pub fn serve_async(
        self: Arc<Self>,
        stream: fcrash::CrashReportingProductRegisterRequestStream,
    ) {
        fasync::Task::spawn(async move {
            let result = self.serve(stream).await;
            if let Err(e) = result {
                error!("Error while serving CrashReportingProductRegister: {:?}", e);
            }
        })
        .detach();
    }
}
