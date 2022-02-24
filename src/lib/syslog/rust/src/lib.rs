//! Rust fuchsia logger library.

// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#![deny(missing_docs)]

use fuchsia_zircon::{self as zx, sys::*, HandleBased};
use lazy_static::lazy_static;
use log::{Level, LevelFilter, Metadata, Record};
use std::ffi::CString;
use std::fmt::Arguments;
use std::os::raw::c_char;
use std::panic;

#[doc(inline)]
pub use fidl_fuchsia_logger::COMPONENT_NAME_PLACEHOLDER_TAG;

#[allow(non_camel_case_types)]
mod syslog;

/// Encapsulates Log Levels.
pub mod levels {
    use crate::syslog;

    /// Defines log levels for clients.
    pub type LogLevel = i32;

    /// ALL log level
    pub const ALL: LogLevel = syslog::FX_LOG_ALL;

    /// TRACE log level
    pub const TRACE: LogLevel = syslog::FX_LOG_TRACE;

    /// DEBUG log level
    pub const DEBUG: LogLevel = syslog::FX_LOG_DEBUG;

    /// INFO log level
    pub const INFO: LogLevel = syslog::FX_LOG_INFO;

    /// WARN log level
    pub const WARN: LogLevel = syslog::FX_LOG_WARN;

    /// ERROR log level
    pub const ERROR: LogLevel = syslog::FX_LOG_ERROR;

    /// FATAL log level
    pub const FATAL: LogLevel = syslog::FX_LOG_FATAL;
}

/// Convenient re-export of macros for globed imports Rust Edition 2018
pub mod macros {
    pub use crate::fx_log;
    pub use crate::fx_log_debug;
    pub use crate::fx_log_err;
    pub use crate::fx_log_info;
    pub use crate::fx_log_trace;
    pub use crate::fx_log_warn;
}

/// Maps log crate log levels to syslog severity levels.
fn get_fx_logger_severity(level: Level) -> syslog::fx_log_severity_t {
    match level {
        Level::Trace => syslog::FX_LOG_TRACE,
        Level::Debug => syslog::FX_LOG_DEBUG,
        Level::Info => syslog::FX_LOG_INFO,
        Level::Warn => syslog::FX_LOG_WARN,
        Level::Error => syslog::FX_LOG_ERROR,
    }
}

/// Maps log crate log levels to syslog severity levels.
pub fn get_fx_logger_level(level: Level) -> levels::LogLevel {
    match level {
        Level::Trace => levels::TRACE,
        Level::Debug => levels::DEBUG,
        Level::Info => levels::INFO,
        Level::Warn => levels::WARN,
        Level::Error => levels::ERROR,
    }
}

/// Maps syslog severity levels to  log crate log filters.
fn get_log_filter(level: levels::LogLevel) -> LevelFilter {
    match level {
        syslog::FX_LOG_ALL => LevelFilter::Trace, // log::LevelFilter fidelity
        syslog::FX_LOG_TRACE => LevelFilter::Trace,
        syslog::FX_LOG_DEBUG => LevelFilter::Debug,
        syslog::FX_LOG_INFO => LevelFilter::Info,
        syslog::FX_LOG_WARN => LevelFilter::Warn,
        syslog::FX_LOG_ERROR => LevelFilter::Error,
        syslog::FX_LOG_FATAL => LevelFilter::Error, // log::LevelFilter fidelity
        _ => LevelFilter::Off,                      // return for all other levels
    }
}

/// Convenience macro for logging.
///
/// Example:
///
/// ```rust
/// fx_log!(tag: "my_tag", levels::WARN, "print integer {}", 10);
/// fx_log!(levels::WARN, "print integer {}", 10);
/// ```
#[macro_export]
macro_rules! fx_log {
    (tag: $tag:expr, $lvl:expr, $($arg:tt)+) => ({
        let lvl = $lvl;
        $crate::log_helper(format_args!($($arg)+), lvl, $tag);
    });
    ($lvl:expr, $($arg:tt)+) => ({
        let lvl = $lvl;
        $crate::log_helper_null(format_args!($($arg)+), lvl);
    });
}

/// Convenience macro to log error.
///
/// Example:
///
/// ```rust
/// fx_log_err!(tag: "my_tag", "failed: {}", e);
/// fx_log_err!("failed: {}", e);
/// ```
#[macro_export]
macro_rules! fx_log_err {
    (tag: $tag:expr, $($arg:tt)*) => (
        $crate::fx_log!(tag: $tag, $crate::levels::ERROR, "[{}({})] {}",
            file!().trim_start_matches("../"), line!(), format_args!($($arg)*));
    );
    ($($arg:tt)*) => (
        $crate::fx_log!($crate::levels::ERROR, "[{}({})] {}",
            file!().trim_start_matches("../"), line!(), format_args!($($arg)*));
    )
}

/// Convenience macro to log warning.
///
/// Example:
///
/// ```rust
/// fx_log_warn!(tag: "my_tag", "print integer {}", 10);
/// fx_log_warn!("print integer {}", 10);
/// ```
#[macro_export]
macro_rules! fx_log_warn {
    (tag: $tag:expr, $($arg:tt)*) => (
        $crate::fx_log!(tag: $tag, $crate::levels::WARN, $($arg)*);
    );
    ($($arg:tt)*) => (
        $crate::fx_log!($crate::levels::WARN, $($arg)*);
    )
}

/// Convenience macro to log information.
///
/// Example:
///
/// ```rust
/// fx_log_info!(tag: "my_tag", "print integer {}", 10);
/// fx_log_info!("print integer {}", 10);
/// ```
#[macro_export]
macro_rules! fx_log_info {
    (tag: $tag:expr, $($arg:tt)*) => (
        $crate::fx_log!(tag: $tag, $crate::levels::INFO, $($arg)*);
    );
    ($($arg:tt)*) => (
        $crate::fx_log!($crate::levels::INFO, $($arg)*);
    )
}

/// Convenience macro to log debug info.
///
/// Example:
///
/// ```rust
/// fx_log_debug!(tag: "my_tag", "print integer {}", 10);
/// fx_log_debug!("print integer {}", 10);
/// ```
#[macro_export]
macro_rules! fx_log_debug {
    (tag: $tag:expr, $($arg:tt)*) => (
        $crate::fx_log!(tag: $tag, $crate::levels::DEBUG, $($arg)*);
    );
    ($($arg:tt)*) => (
        $crate::fx_log!($crate::levels::DEBUG, $($arg)*);
    )
}

/// Convenience macro to log trace info.
///
/// Example:
///
/// ```rust
/// fx_log_trace!(tag: "my_tag", "print integer {}", 10);
/// fx_log_trace!("print integer {}", 10);
/// ```
#[macro_export]
macro_rules! fx_log_trace {
    (tag: $tag:expr, $($arg:tt)*) => (
        $crate::fx_log!(tag: $tag, $crate::levels::TRACE, $($arg)*);
    );
    ($($arg:tt)*) => (
        $crate::fx_log!($crate::levels::TRACE, $($arg)*);
    )
}

/// Convenience macro to log verbose messages.
///
/// Example:
///
/// ```rust
/// fx_vlog!(tag: "my_tag", 1 /*verbosity*/, "print integer {}", 10);
/// fx_vlog!(2 /*verbosity*/, "print integer {}", 10);
/// ```
#[macro_export]
macro_rules! fx_vlog {
    (tag: $tag:expr, $verbosity:expr, $($arg:tt)*) => (
        $crate::fx_log!(tag: $tag, $crate::get_severity_from_verbosity($verbosity), $($arg)*);
    );
    ($verbosity:expr, $($arg:tt)*) => (
        $crate::fx_log!($crate::get_severity_from_verbosity($verbosity), $($arg)*);
    )
}

/// C API logger wrapper which provides wrapper for C APIs.
pub struct Logger {
    logger: *mut syslog::fx_logger_t,
}

impl Drop for Logger {
    fn drop(&mut self) {
        unsafe {
            syslog::fx_logger_destroy(self.logger);
        }
    }
}

impl Logger {
    /// Wrapper around C API `fx_logger_get_min_severity`.
    fn get_severity(&self) -> syslog::fx_log_severity_t {
        unsafe { syslog::fx_logger_get_min_severity(self.logger) }
    }

    /// Returns true if inner logger is not null and log level is enabled.
    pub fn is_enabled(&self, severity: levels::LogLevel) -> bool {
        if !self.logger.is_null() {
            return self.get_severity() <= severity;
        }
        false
    }

    /// Returns whether or not the underlying logger is valid
    pub fn is_valid(&self) -> bool {
        !self.logger.is_null()
    }

    /// Whether the log socket is valid
    pub fn is_connected(&self) -> bool {
        unsafe { syslog::fx_logger_get_connection_status(self.logger) == zx::Status::OK.into_raw() }
    }

    /// Wrapper around C API `fx_logger_log`. Consider using `fx_log_*` macros
    /// instead of calling this function directly. Calling this function
    /// directly is almost certainly not what you want to do unless you are
    /// writing a custom logging integration.
    pub fn log_f(&self, level: levels::LogLevel, args: Arguments<'_>, tag: Option<&str>) {
        if !self.is_enabled(level) {
            return;
        }
        let s = std::fmt::format(args);
        let c_msg = CString::new(s).unwrap();
        match tag {
            Some(t) => {
                let c_tag = CString::new(t).unwrap();
                unsafe {
                    syslog::fx_logger_log(self.logger, level, c_tag.as_ptr(), c_msg.as_ptr())
                };
            }
            None => {
                unsafe {
                    syslog::fx_logger_log(self.logger, level, std::ptr::null(), c_msg.as_ptr());
                };
            }
        }
    }

    /// Set logger severity. Returns false if internal logger is null.
    pub fn set_severity(&self, severity: levels::LogLevel) -> bool {
        if !self.logger.is_null() {
            unsafe { syslog::fx_logger_set_min_severity(self.logger, severity) };
            return true;
        }
        false
    }
}

lazy_static! {
    /// Global reference to default logger object.
    pub static ref LOGGER: Logger = {
        let l = get_default();
        install_panic_hook();
        l
    };
}

/// macro helper function to convert strings and call log
pub fn log_helper(args: Arguments<'_>, lvl: i32, tag: &str) {
    LOGGER.log_f(lvl, args, Some(tag));
}

/// macro helper function to convert strings and call log with null tag
pub fn log_helper_null(args: Arguments<'_>, lvl: i32) {
    LOGGER.log_f(lvl, args, None);
}

/// Gets default logger.
fn get_default() -> Logger {
    Logger { logger: unsafe { syslog::fx_log_get_logger() } }
}

unsafe impl Send for Logger {}

unsafe impl Sync for Logger {}

impl log::Log for Logger {
    fn enabled(&self, metadata: &Metadata<'_>) -> bool {
        self.is_enabled(get_fx_logger_severity(metadata.level()))
    }

    fn log(&self, record: &Record<'_>) {
        if record.level() == Level::Error {
            fx_log!(tag:record.target(),
                get_fx_logger_severity(record.level()), "[{}({})] {}",
                record.file().unwrap_or("??").trim_start_matches("../"), record.line().unwrap_or(0), record.args());
        } else {
            fx_log!(tag:record.target(),
                get_fx_logger_severity(record.level()), "{}", record.args());
        }
    }

    fn flush(&self) {}
}

/// Initializes syslogger using default options.
pub fn init() -> Result<(), zx::Status> {
    init_with_tags_and_handle(zx::sys::ZX_HANDLE_INVALID, &[])
}

/// Initializes syslogger with tags. Max number of tags can be 4
/// and max length of each tag can be 63 characters.
pub fn init_with_tags(tags: &[&str]) -> Result<(), zx::Status> {
    init_with_tags_and_handle(zx::sys::ZX_HANDLE_INVALID, tags)
}

/// Initialize syslogger with a single tag and a log service channel socket.
pub fn init_with_socket_and_name(sink: zx::Socket, name: &str) -> Result<(), zx::Status> {
    init_with_tags_and_handle(sink.into_raw(), &[name])
}

/// Initializes syslogger with tags. Max number of tags can be 4
/// and max length of each tag can be 63 characters.
fn init_with_tags_and_handle(handle: zx_handle_t, tags: &[&str]) -> Result<(), zx::Status> {
    with_default_config_with_tags_and_handle(handle, tags, |config| -> Result<(), zx::Status> {
        let status = unsafe { syslog::fx_log_reconfigure(config) };
        if status == zx::Status::OK.into_raw() {
            log::set_logger(&*LOGGER).expect("Attempted to initialize multiple loggers");
            log::set_max_level(get_log_filter(config.severity));
        }
        zx::ok(status)
    })
}

/// Initialize and return a syslogger that uses the `sink` socket.
pub fn build_with_tags_and_socket(sink: zx::Socket, tags: &[&str]) -> Result<Logger, zx::Status> {
    with_default_config_with_tags_and_handle(
        sink.into_raw(),
        tags,
        |config| -> Result<Logger, zx::Status> {
            let logger = unsafe {
                let logger_ptr: *mut syslog::fx_logger_t = std::ptr::null_mut();
                let status = syslog::fx_logger_create(config, &logger_ptr);
                if status != zx::Status::OK.into_raw() {
                    return Err(zx::Status::from_raw(status));
                }
                logger_ptr
            };
            Ok(Logger { logger })
        },
    )
}

/// Create a default configuration that incorporates the provided handle and
/// tags. After that config is created it is passed to `build_logger_fn`.
/// Callers will likely want to construct a Logger in their `build_logger_fn`
/// implementation.
fn with_default_config_with_tags_and_handle<R>(
    handle: zx_handle_t,
    tags: &[&str],
    build_logger_fn: impl FnOnce(&syslog::fx_logger_config_t) -> R,
) -> R {
    let cstr_vec: Vec<CString> = tags
        .iter()
        .map(|x| CString::new(x.to_owned()).expect("Cannot create tag with interior null"))
        .collect();
    let c_tags: Vec<*const c_char> = cstr_vec.iter().map(|x| x.as_ptr()).collect();
    let config = syslog::fx_logger_config_t {
        severity: syslog::FX_LOG_SEVERITY_DEFAULT,
        fd: -1,
        log_sink_channel: zx::sys::ZX_HANDLE_INVALID,
        log_sink_socket: handle,
        log_service_channel: zx::sys::ZX_HANDLE_INVALID,
        tags: c_tags.as_ptr(),
        num_tags: c_tags.len(),
    };
    build_logger_fn(&config)
}

/// Installs a new panic hook to send the panic message to the log service, since v2 components
/// won't have stdout.
fn install_panic_hook() {
    let default_hook = panic::take_hook();
    panic::set_hook(Box::new(move |panic_info| {
        // Handle common cases of &'static str or String payload.
        let msg = match panic_info.payload().downcast_ref::<&'static str>() {
            Some(s) => *s,
            None => match panic_info.payload().downcast_ref::<String>() {
                Some(s) => &s[..],
                None => "<Unknown panic payload type>",
            },
        };

        fx_log!(levels::ERROR, "{}", format_args!("PANIC: {}", msg));

        default_hook(panic_info);
    }));
}

/// Set default logger severity.
pub fn set_severity(severity: levels::LogLevel) {
    if LOGGER.set_severity(severity) {
        log::set_max_level(get_log_filter(severity));
    }
}

/// Get the severity corresponding to the given verbosity. Note that
/// verbosity relative to the default severity and can be thought of
/// as incrementally "more vebose than" the baseline.
pub fn get_severity_from_verbosity(mut verbosity: i32) -> i32 {
    verbosity = std::cmp::max(0, verbosity);

    // verbosity scale sits in the interstitial space between INFO and DEBUG
    std::cmp::max(
        syslog::FX_LOG_DEBUG + 1,
        syslog::FX_LOG_INFO - (verbosity * syslog::FX_LOG_VERBOSITY_STEP_SIZE),
    )
}

/// Set default logger verbosity.
#[inline]
pub fn set_verbosity(verbosity: u16) {
    set_severity(get_severity_from_verbosity(verbosity as i32));
}

/// Checks if default logger is enabled for given log level.
pub fn is_enabled(severity: levels::LogLevel) -> bool {
    LOGGER.is_enabled(severity)
}

#[cfg(test)]
mod test {
    use super::*;
    use diagnostics_data::{assert_data_tree, Severity};
    use diagnostics_message::{self as message, LoggerMessage, MonikerWithUrl};
    use log::{debug, error, info, trace, warn};
    use std::convert::TryFrom;
    use std::fs::File;
    use std::io::Read;
    use std::os::unix::io::AsRawFd;
    use std::ptr;
    use tempfile::TempDir;

    #[test]
    /// Validate that using `build_with_tags_and_socket` results in log packets
    /// with the expected data being passed through the supplied socket.
    fn test_build_with_socket() {
        // Create the socket and logger with a couple of tags
        // and write a simple message to it.
        let (tx, rx) = zx::Socket::create(zx::SocketOpts::DATAGRAM)
            .expect("Datagram socket could not be made");
        let tags: [&str; 1] = ["[testing]"];
        let logger = build_with_tags_and_socket(rx, &tags).expect("failed to create logger");
        logger.log_f(levels::ERROR, format_args!("{}-{}", "hello", "world"), None);

        // Read out of the socket into a `Message`, but using a fake
        // component identity.
        let mut buffer: [u8; 1024] = [0; 1024];
        let read_len = tx.read(&mut buffer).expect("socket read failed");
        let src_id = MonikerWithUrl {
            moniker: "fake-test-env/test-component.cmx".to_string(),
            url: "fuchsia-pkg://fuchsia.com/testing123#test-component.cm".to_string(),
        };

        let msg = message::from_logger(
            src_id.clone(),
            LoggerMessage::try_from(&buffer[..read_len])
                .expect("couldn't decode message from buffer"),
        );

        // Check metadata and payload
        assert_eq!(msg.metadata.errors, None);
        assert_eq!(msg.metadata.component_url, Some(src_id.url.to_string()));
        assert_eq!(msg.metadata.severity, Severity::Error);
        assert_eq!(msg.metadata.tags, Some(tags.map(|e| e.to_string()).to_vec()));
        assert_data_tree!(msg.payload.as_ref().expect("message had no payload"),
            root: {
                message: {
                    value: "hello-world"
                }
            }
        );
    }

    #[test]
    fn test() {
        let tmp_dir = TempDir::new().expect("should have created tempdir");
        let file_path = tmp_dir.path().join("tmp_file");
        let tmp_file = File::create(&file_path).expect("should have created file");
        let config = syslog::fx_logger_config_t {
            severity: levels::INFO,
            fd: tmp_file.as_raw_fd(),
            log_sink_channel: zx::sys::ZX_HANDLE_INVALID,
            log_sink_socket: zx::sys::ZX_HANDLE_INVALID,
            log_service_channel: zx::sys::ZX_HANDLE_INVALID,
            tags: ptr::null(),
            num_tags: 0,
        };
        let status = unsafe { syslog::fx_log_reconfigure(&config) };
        assert_eq!(status, zx::Status::OK.into_raw());

        fx_log_info!("info msg {}", 10);
        let mut expected: Vec<String> = vec![String::from("[] INFO: info msg 10")];

        fx_log_warn!("warn msg {}", 10);
        expected.push(String::from("[] WARNING: warn msg 10"));

        fx_log_err!("err msg {}", 10);
        let line = line!() - 1;
        expected.push(format!(
            "[] ERROR: [{}({})] err msg 10",
            file!().trim_start_matches("../"),
            line
        ));

        fx_log_info!(tag:"info_tag", "info msg {}", 10);
        expected.push(String::from("[info_tag] INFO: info msg 10"));

        fx_log_warn!(tag:"warn_tag", "warn msg {}", 10);
        expected.push(String::from("[warn_tag] WARNING: warn msg 10"));

        fx_log_err!(tag:"err_tag", "err msg {}", 10);
        let line = line!() - 1;
        expected.push(format!(
            "[err_tag] ERROR: [{}({})] err msg 10",
            file!().trim_start_matches("../"),
            line
        ));

        //test verbosity
        fx_vlog!(1, "verbose msg {}", 10); // will not log
        fx_vlog!(tag:"v_tag", 1, "verbose msg {}", 10); // will not log

        set_verbosity(1);
        fx_vlog!(1, "verbose2 msg {}", 10);
        expected.push(String::from("[] VLOG(1): verbose2 msg 10"));

        fx_vlog!(tag:"v_tag", 1, "verbose2 msg {}", 10);
        expected.push(String::from("[v_tag] VLOG(1): verbose2 msg 10"));

        // test log crate
        log::set_logger(&*LOGGER).expect("Attempted to initialize multiple loggers");

        set_severity(levels::DEBUG);
        info!("log info: {}", 10);
        let tag = "fuchsia_syslog_lib_test::test";
        expected.push(format!("[{}] INFO: log info: 10", tag));

        warn!("log warn: {}", 10);
        expected.push(format!("[{}] WARNING: log warn: 10", tag));

        error!("log err: {}", 10);
        let line = line!() - 1;
        expected.push(format!(
            "[{}] ERROR: [{}({})] log err: 10",
            tag,
            file!().trim_start_matches("../"),
            line
        ));

        debug!("log debug: {}", 10);
        expected.push(format!("[{}] DEBUG: log debug: 10", tag));

        trace!("log trace: {}", 10); // will not log

        set_severity(levels::TRACE);
        trace!("log trace2: {}", 10);
        expected.push(format!("[{}] TRACE: log trace2: 10", tag));

        // test set_severity
        set_severity(levels::WARN);
        info!("log info, will not log: {}", 10);
        warn!("log warn, will log: {}", 10);
        expected.push(format!("[{}] WARNING: log warn, will log: 10", tag));

        let mut tmp_file = File::open(&file_path).expect("should have opened the file");
        let mut content = String::new();
        tmp_file.read_to_string(&mut content).expect("something went wrong reading the file");
        let msgs = content.split("\n");
        let mut i = 0;
        for msg in msgs {
            if msg == "" {
                // last line - blank message
                continue;
            }
            if expected.len() <= i {
                panic!("Got extra line in msg \"{}\", full content\n{}", msg, content);
            } else if !msg.ends_with(&expected[i]) {
                panic!(
                    "expected msg:\n\"{}\"\nto end with\n\"{}\"\nfull content\n{}",
                    msg, expected[i], content
                );
            }
            i = i + 1;
        }
        if expected.len() != i {
            panic!("expected msgs:\n{:?}\nfull content\n{}", expected, content);
        }
    }
}
