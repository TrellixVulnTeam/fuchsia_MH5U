use crate::handler::base::Error;

use fuchsia_syslog::fx_log_warn;

// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/// This macro generates a mod containing the logic to process a FIDL stream for the
/// fuchsia.settings namespace.
/// Callers can spawn a handler task by invoking fidl_io::spawn.
/// Macro usages specify the interface's base name (prefix for all generated
/// classes), along with a repeated set of the following:
/// - `Switchboard` setting type.
/// - FIDL setting type.
/// - The responder type for the `HangingGetHandler`.
/// - A type of a key for change functions if custom change functions are used.
///     - If change functions aren't used, the `change_func_key` is ignored,
///       so can be anything.
///       TODO(fxb/68167): Restructure this code to avoid needing to specify
///       unneeded parameters.
/// - The handler function for requests.
#[macro_export]
macro_rules! fidl_process_full {
    ($interface:ident $(,$setting_type:expr, $fidl_settings:ty,
            $fidl_responder:ty, $change_func_key:ty, $handle_func:ident)+$(,)*) => {
        type HandleResult<'a> = ::futures::future::LocalBoxFuture<
            'a,
            Result<Option<paste::paste!{[<$interface Request>]}>, anyhow::Error>,
        >;

        pub(crate) mod fidl_io {
            paste::paste!{
                use fidl_fuchsia_settings::{[<$interface Marker>], [<$interface RequestStream>]};
            }
            use super::*;
            use $crate::fidl_processor::processor::SettingsFidlProcessor;
            use $crate::service;
            use $crate::message::base::MessengerType;
            use ::fuchsia_async as fasync;
            use ::futures::FutureExt;

            pub(crate) fn spawn (
                delegate: service::message::Delegate,
                stream: paste::paste!{[<$interface RequestStream>]}
            ) {
                fasync::Task::local(async move {
                    let service_messenger = delegate
                        .create(MessengerType::Unbound)
                        .await.expect("service messenger should be created")
                        .0;

                    let mut processor =
                        SettingsFidlProcessor::<paste::paste!{[<$interface Marker>]}>::new(
                            stream, service_messenger,
                        )
                        .await;
                    $(
                        processor
                            .register::<$fidl_settings, $fidl_responder, $change_func_key>(
                                $setting_type,
                                Box::new(move |context, req| -> HandleResult<'_> {
                                    async move { $handle_func(context, req).await }.boxed_local()
                                }),
                            )
                            .await;
                    )*
                    processor.process().await;
                }).detach();
            }
        }
    };
}

/// This macro generates a mod containing the logic to process a FIDL stream for the
/// fuchsia.settings.policy namespace.
/// Callers can spawn a handler task by invoking fidl_io::spawn.
/// Macro usages specify the interface's base name and a handler function for requests.
// TODO(fxbug.dev/61433): consider returning spawned task instead of detaching
#[macro_export]
macro_rules! fidl_process_policy {
    ($interface:ident, $handle_func:ident) => {
        type HandleResult<'a> = LocalBoxFuture<
            'a,
            Result<Option<paste::paste! {[<$interface Request>]}>, anyhow::Error>,
        >;

        pub(crate) mod fidl_io {
            paste::paste! {
                use fidl_fuchsia_settings_policy::{
                    [<$interface Marker>],
                    [<$interface RequestStream>],
                };
            }
            use super::*;
            use crate::fidl_processor::processor::PolicyFidlProcessor;
            use crate::message::base::MessengerType;
            use crate::service;
            use fuchsia_async as fasync;

            pub(crate) fn spawn(
                delegate: service::message::Delegate,
                stream: paste::paste! {[<$interface RequestStream>]},
            ) {
                fasync::Task::local(async move {
                    let service_messenger = delegate
                        .create(MessengerType::Unbound)
                        .await
                        .expect("service messenger should be created")
                        .0;

                    let mut processor =
                        PolicyFidlProcessor::<paste::paste! {[<$interface Marker>]}>::new(
                            stream,
                            service_messenger,
                        )
                        .await;
                    processor
                        .register(Box::new(move |context, req| -> HandleResult<'_> {
                            async move { $handle_func(context, req).await }.boxed_local()
                        }))
                        .await;
                    processor.process().await;
                })
                .detach();
            }
        }
    };
}

#[macro_export]
macro_rules! fidl_process {
    // Generates a fidl_io mod with a spawn for the given fidl interface,
    // setting type, and handler function. Additional handlers can be specified
    // by providing the internal setting type, fidl setting type, watch
    // responder, and handle function.
    ($interface:ident, $setting_type:expr, $handle_func:ident
            $(,$item_setting_type:expr, $fidl_settings:ty, $fidl_responder:ty,
            $item_handle_func:ident)*$(,)*) => {
        paste::paste! {
            $crate::fidl_process_full!(
                $interface,
                $setting_type,
                [<$interface Settings>],
                [<$interface WatchResponder>],
                String,
                $handle_func
                $(,$item_setting_type, $fidl_settings, $fidl_responder, String, $item_handle_func)*
            );
        }
    };

    // Generates a fidl_io mod with a spawn for the given fidl interface,
    // setting type, and handler function. Additional handlers can be specified
    // by providing the responder type and handle function.
    ($interface:ident, $setting_type:expr, $handle_func:ident
            $(, $fidl_responder:ty, $item_handle_func:ident)*$(,)*) => {
        paste::paste! {
            $crate::fidl_process_full!(
                $interface,
                $setting_type,
                [<$interface Settings>],
                [<$interface WatchResponder>],
                String,
                $handle_func
                $(, $setting_type,
                [<$interface Settings>],
                $fidl_responder,
                String,
                $item_handle_func)*
            );
        }
    };

    // Generates a fidl_io mod with a spawn for the given fidl interface,
    // setting type, fidl setting type and handler function. To be used when the
    // fidl interface and fidl setting type differ in name.
    ($interface:ident, $setting_type:expr, $fidl_settings:ident,
            $handle_func:ident) => {
        paste::paste! {
            $crate::fidl_process_full!(
                $interface,
                $setting_type,
                $fidl_settings,
                [<$interface WatchResponder>],
                String,
                $handle_func
            );
        }
    };
}

// Fully custom named inputs for when there are multiple simultaneous interfaces
// that do not have one specific pattern to conform to.
// TODO(fxbug.dev/65686): Remove once clients are migrated to input2.
#[macro_export]
macro_rules! fidl_process_custom {
    ($interface:ident, $setting_type:expr, $fidl_responder:ty, $fidl_settings:ident, $handle_func:ident
            $(, $item_setting_type:expr, $item_fidl_responder:ty, $item_fidl_settings:ident, $item_handle_func:ident)*$(,)*) => {
        paste::paste! {
            $crate::fidl_process_full!(
                $interface,
                $setting_type,
                $fidl_settings,
                $fidl_responder,
                String,
                $handle_func
                $(, $item_setting_type,
                $item_fidl_settings,
                $item_fidl_responder,
                String,
                $item_handle_func)*
            );
        }
    };
}

pub(crate) fn convert_to_epitaph(error: &anyhow::Error) -> fuchsia_zircon::Status {
    match error.root_cause().downcast_ref::<Error>() {
        Some(Error::UnhandledType(_)) => fuchsia_zircon::Status::UNAVAILABLE,
        _ => fuchsia_zircon::Status::INTERNAL,
    }
}

/// Shuts down the given fidl `responder` using a zircon epitaph generated from
/// the given `error`.
#[macro_export]
macro_rules! shutdown_responder_with_error {
    ($responder:expr, $error:ident) => {
        // Extra pair of braces is needed to limit the scope of the import.
        {
            use ::fidl::prelude::*;
            $responder
                .control_handle()
                .shutdown_with_epitaph(crate::fidl_common::convert_to_epitaph($error))
        }
    };
}

/// Implements the Sender trait for the given FIDL responder(s) that send typed data.
#[macro_export]
macro_rules! fidl_hanging_get_responder {
    ($marker_type:ty $(, $setting_type:ty, $responder_type:ty)+$(,)*) => {

        $(impl $crate::hanging_get_handler::Sender<$setting_type> for $responder_type {
            fn send_response(self, data: $setting_type) {
                use $crate::fidl_common::FidlResponseErrorLogger;

                self.send(data).log_fidl_response_error(
                    <$marker_type as ::fidl::endpoints::ProtocolMarker>::DEBUG_NAME);
            }

            fn on_error(self, error: &anyhow::Error) {
                ::fuchsia_syslog::fx_log_err!(
                    "error occurred watching for service: {:?}. Error is: {:?}",
                    <$marker_type as ::fidl::endpoints::ProtocolMarker>::DEBUG_NAME,
                    error
                );
                crate::shutdown_responder_with_error!(self, error);
            }
        })+
    };
}

/// Implements the Sender trait for the given FIDL responder(s) that send a result type.
#[macro_export]
macro_rules! fidl_result_sender_for_responder {
    ($marker_type:ty $(, $result_type:ty, $responder_type:ty)+$(,)*) => {
        $(impl $crate::hanging_get_handler::Sender<$result_type> for $responder_type {
            fn send_response(self, mut result: $result_type) {
                use $crate::fidl_common::FidlResponseErrorLogger;

                self.send(&mut result).log_fidl_response_error(
                <$marker_type as ::fidl::endpoints::ProtocolMarker>::DEBUG_NAME);
            }

            fn on_error(self, error:&anyhow::Error) {
                ::fuchsia_syslog::fx_log_err!(
                    "error occurred watching for service: {:?}. Error is: {:?}",
                    <$marker_type as ::fidl::endpoints::ProtocolMarker>::DEBUG_NAME,
                    error
                );
                crate::shutdown_responder_with_error!(self, error);
            }
        })+
    };
}

/// Custom trait used to handle results from responding to FIDL calls.
pub trait FidlResponseErrorLogger {
    fn log_fidl_response_error(&self, client_name: &str);
}

/// In order to not crash when a client dies, logs but doesn't crash for the specific case of
/// being unable to write to the client. Crashes if other types of errors occur.
impl FidlResponseErrorLogger for Result<(), fidl::Error> {
    fn log_fidl_response_error(&self, client_name: &str) {
        if let Some(error) = self.as_ref().err() {
            match error {
                fidl::Error::ServerResponseWrite(_) => {
                    fx_log_warn!("Failed to respond to client {:?} : {:?}", client_name, error);
                }
                _ => {
                    panic!(
                        "Unexpected client response error from client {:?} : {:?}",
                        client_name, error
                    );
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use fuchsia_zircon as zx;

    use super::*;

    // Should succeed either when responding was successful or there was an error on the client
    // side.
    #[test]
    fn test_error_logger_succeeds() {
        let result = Err(fidl::Error::ServerResponseWrite(zx::Status::PEER_CLOSED));
        result.log_fidl_response_error("");

        let result = Ok(());
        result.log_fidl_response_error("");
    }

    // Should fail at all other times.
    #[should_panic]
    #[test]
    fn test_error_logger_fails() {
        let result = Err(fidl::Error::ServerRequestRead(zx::Status::PEER_CLOSED));
        result.log_fidl_response_error("");
    }
}
