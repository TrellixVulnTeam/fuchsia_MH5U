// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use anyhow::Error;
use base64;
use fidl_fuchsia_media::*;
use fidl_fuchsia_virtualaudio::*;
use fuchsia_async as fasync;
use fuchsia_component as app;
use fuchsia_zircon as zx;
use futures::channel::mpsc;
use futures::lock::Mutex;
use futures::{select, StreamExt, TryFutureExt, TryStreamExt};
use parking_lot::RwLock;
use serde_json::{to_value, Value};
use std::convert::{TryFrom, TryInto};
use std::io::Write;
use std::sync::Arc;

use fuchsia_syslog::macros::*;

// Values found in:
//   zircon/system/public/zircon/device/audio.h
const AUDIO_SAMPLE_FORMAT_8BIT: u32 = 1 << 1;
const AUDIO_SAMPLE_FORMAT_16BIT: u32 = 1 << 2;
const AUDIO_SAMPLE_FORMAT_24BIT_IN32: u32 = 1 << 7;
const AUDIO_SAMPLE_FORMAT_32BIT_FLOAT: u32 = 1 << 9;

const ASF_RANGE_FLAG_FPS_CONTINUOUS: u16 = 1 << 0;

// If this changes, so too must the astro audio_core_config.
const AUDIO_OUTPUT_ID: [u8; 16] = [0x01; 16];

fn get_sample_size(format: u32) -> Result<u32, Error> {
    Ok(match format {
        // These are the currently implemented formats.
        AUDIO_SAMPLE_FORMAT_8BIT => 1,
        AUDIO_SAMPLE_FORMAT_16BIT => 2,
        AUDIO_SAMPLE_FORMAT_24BIT_IN32 => 4,
        AUDIO_SAMPLE_FORMAT_32BIT_FLOAT => 4,
        _ => return Err(format_err!("Cannot handle sample_format: {:?}", format)),
    })
}

fn get_zircon_sample_format(format: AudioSampleFormat) -> u32 {
    match format {
        AudioSampleFormat::Signed16 => AUDIO_SAMPLE_FORMAT_16BIT,
        AudioSampleFormat::Unsigned8 => AUDIO_SAMPLE_FORMAT_8BIT,
        AudioSampleFormat::Signed24In32 => AUDIO_SAMPLE_FORMAT_24BIT_IN32,
        AudioSampleFormat::Float => AUDIO_SAMPLE_FORMAT_32BIT_FLOAT,
        // No default case, these are all the audio sample formats supported right now.
    }
}

#[derive(Debug)]
enum ExtractMsg {
    Start,
    Stop { out_sender: mpsc::Sender<Vec<u8>> },
}

#[derive(Debug, Default)]
struct OutputWorker {
    va_output: Option<OutputProxy>,
    extracted_data: Vec<u8>,
    vmo: Option<zx::Vmo>,

    // Whether we should store samples when we receive notification from the VAD
    capturing: bool,

    // How much of the vmo's data we're actually using, in bytes.
    work_space: u64,

    // How often, in frames, we want to be updated on the state of the extraction ring buffer.
    frames_per_notification: u64,

    // How many bytes a frame is.
    frame_size: u64,

    // Offset into vmo where we'll start to read next, in bytes.
    next_read: u64,

    // Offset into vmo where we'll finish reading next, in bytes.
    next_read_end: u64,
}

impl OutputWorker {
    fn on_set_format(
        &mut self,
        frames_per_second: u32,
        sample_format: u32,
        num_channels: u32,
        _external_delay: i64,
    ) -> Result<(), Error> {
        let sample_size = get_sample_size(sample_format)?;
        self.frame_size = u64::from(num_channels) * u64::from(sample_size);

        let frames_per_millisecond = u64::from(frames_per_second / 1000);
        self.frames_per_notification = frames_per_millisecond * 50;

        fx_log_info!(
            "AudioFacade::OutputWorker: configuring with {:?} fps, {:?} bpf",
            frames_per_second,
            self.frame_size
        );

        Ok(())
    }

    fn on_buffer_created(
        &mut self,
        ring_buffer: zx::Vmo,
        num_ring_buffer_frames: u32,
        _notifications_per_ring: u32,
    ) -> Result<(), Error> {
        let va_output = self.va_output.as_mut().ok_or(format_err!("va_output not initialized"))?;

        // Ignore AudioCore's notification cadence (_notifications_per_ring); set up our own.
        let target_notifications_per_ring =
            u64::from(num_ring_buffer_frames) / self.frames_per_notification;
        let target_notifications_per_ring = target_notifications_per_ring.try_into()?;

        va_output.set_notification_frequency(target_notifications_per_ring)?;
        fx_log_info!(
            "AudioFacade::OutputWorker: created buffer with {:?} frames, {:?} notifications",
            num_ring_buffer_frames,
            target_notifications_per_ring
        );

        self.work_space = u64::from(num_ring_buffer_frames) * self.frame_size;

        // Start reading from the beginning.
        self.next_read = 0;
        self.next_read_end = 0;

        self.vmo = Some(ring_buffer);
        Ok(())
    }

    fn on_position_notify(
        &mut self,
        _monotonic_time: i64,
        ring_position: u32,
        capturing: bool,
    ) -> Result<(), Error> {
        if capturing && self.next_read != self.next_read_end {
            let vmo = if let Some(vmo) = &self.vmo { vmo } else { return Ok(()) };

            fx_log_info!(
                "AudioFacade::OutputWorker read byte {:?} to {:?}",
                self.next_read,
                self.next_read_end
            );

            if self.next_read_end < self.next_read {
                // Wrap-around case, read through the end.
                let len = (self.work_space - self.next_read).try_into()?;
                let mut data = vec![0u8; len];
                let overwrite1 = vec![1u8; len];
                vmo.read(&mut data, self.next_read)?;
                vmo.write(&overwrite1, self.next_read)?;
                self.extracted_data.append(&mut data);

                // Read remaining data.
                let next_read_end = self.next_read_end.try_into()?;
                let mut data = vec![0u8; next_read_end];
                let overwrite2 = vec![1u8; next_read_end];
                vmo.read(&mut data, 0)?;
                vmo.write(&overwrite2, 0)?;

                self.extracted_data.append(&mut data);
            } else {
                // Normal case, just read all the bytes.
                let len = (self.next_read_end - self.next_read).try_into()?;
                let mut data = vec![0u8; len];
                let overwrite = vec![1u8; len];
                vmo.read(&mut data, self.next_read)?;
                vmo.write(&overwrite, self.next_read)?;

                self.extracted_data.append(&mut data);
            }
        }
        // We always stay 1 notification behind, since audio_core writes audio data into
        // our shared buffer based on these same notifications. This avoids audio glitches.
        self.next_read = self.next_read_end;
        self.next_read_end = ring_position.into();
        Ok(())
    }

    async fn run(
        &mut self,
        mut rx: mpsc::Receiver<ExtractMsg>,
        va_output: OutputProxy,
    ) -> Result<(), Error> {
        let mut output_events = va_output.take_event_stream();
        self.va_output = Some(va_output);

        // Monotonic timestamp returned by the most-recent OnStart/OnPositionNotify/OnStop response.
        let mut last_timestamp = zx::Time::from_nanos(0);
        // Observed monotonic time that OnStart/OnPositionNotify/OnStop messages actually arrived.
        let mut last_event_time = zx::Time::from_nanos(0);

        loop {
            select! {
                rx_msg = rx.next() => {
                    match rx_msg {
                        None => {
                            return Err(format_err!("Got None ExtractMsg Event, exiting worker"));
                        },
                        Some(ExtractMsg::Stop { mut out_sender }) => {
                            fx_log_info!("AudioFacade::OutputWorker: Stop capture");
                            self.capturing = false;
                            let mut ret_data = vec![0u8; 0];

                            ret_data.append(&mut self.extracted_data);

                            out_sender.try_send(ret_data)?;
                        }
                        Some(ExtractMsg::Start) => {
                            fx_log_info!("AudioFacade::OutputWorker: Start capture");
                            self.extracted_data.clear();
                            self.capturing = true;
                        }
                    }
                },
                output_msg = output_events.try_next() => {
                    match output_msg? {
                        None => {
                            return Err(format_err!("Got None OutputEvent Message, exiting worker"));
                        },
                        Some(OutputEvent::OnSetFormat { frames_per_second, sample_format,
                                                        num_channels, external_delay}) => {
                            self.on_set_format(frames_per_second, sample_format, num_channels,
                                               external_delay)?;
                        },
                        Some(OutputEvent::OnBufferCreated { ring_buffer, num_ring_buffer_frames,
                                                            notifications_per_ring }) => {
                            self.on_buffer_created(ring_buffer, num_ring_buffer_frames,
                                                   notifications_per_ring)?;
                        },
                        Some(OutputEvent::OnStart { start_time }) => {
                            if self.capturing && last_timestamp > zx::Time::from_nanos(0) {
                                fx_log_info!("AudioFacade::OutputWorker: Extraction OnPositionNotify received before OnStart");
                            }
                            last_timestamp = zx::Time::from_nanos(start_time);
                            last_event_time = zx::Time::get_monotonic();
                        },
                        Some(OutputEvent::OnStop { stop_time: _, ring_position: _ }) => {
                            if last_timestamp == zx::Time::from_nanos(0) {
                                fx_log_info!(
                                    "AudioFacade::OutputWorker: Extraction OnPositionNotify timestamp cleared before OnStop");
                            }
                            last_timestamp = zx::Time::from_nanos(0);
                            last_event_time = zx::Time::from_nanos(0);
                        },
                        Some(OutputEvent::OnPositionNotify { monotonic_time, ring_position }) => {
                            let monotonic_zx_time = zx::Time::from_nanos(monotonic_time);
                            let now = zx::Time::get_monotonic();

                            // To minimize logspam, log glitches only when capturing.
                            if self.capturing {
                                if last_timestamp == zx::Time::from_nanos(0) {
                                    fx_log_info!(
                                        "AudioFacade::OutputWorker: Extraction OnStart not received before OnPositionNotify");
                                }

                                // Log if our timestamps had a gap of more than 100ms. This is highly
                                // abnormal and indicates possible glitching while receiving playback
                                // audio from the system and/or extracting it for analysis.
                                let timestamp_interval = monotonic_zx_time - last_timestamp;
                                if  timestamp_interval > zx::Duration::from_millis(100) {
                                    fx_log_info!(
                                        "AudioFacade::OutputWorker: Extraction position timestamp jumped by more than 100ms ({:?}ms). Expect glitches.",
                                        timestamp_interval.into_millis());
                                }
                                if  monotonic_zx_time < last_timestamp {
                                    fx_log_info!(
                                        "AudioFacade::OutputWorker: Extraction position timestamp moved backwards ({:?}ms). Expect glitches.",
                                        timestamp_interval.into_millis());
                                }

                                // Log if there was a gap in position notification arrivals of more
                                // than 150ms. This is highly abnormal and indicates possible glitching
                                // while receiving playback audio from the system and/or extracting it
                                // for analysis.
                                let observed_interval = now - last_event_time;
                                if  observed_interval > zx::Duration::from_millis(150) {
                                    fx_log_info!(
                                        "AudioFacade::OutputWorker: Extraction position not updated for 150ms ({:?}ms). Expect glitches.",
                                        observed_interval.into_millis());
                                }
                            }

                            last_timestamp = monotonic_zx_time;
                            last_event_time = now;

                            self.on_position_notify(monotonic_time, ring_position, self.capturing)?;
                        },
                        Some(evt) => {
                            fx_log_info!("AudioFacade::OutputWorker: Got unknown OutputEvent {:?}", evt);
                        }
                    }
                },
            };
        }
    }
}

#[derive(Debug)]
struct VirtualOutput {
    extracted_data: Vec<u8>,
    capturing: Arc<Mutex<bool>>,
    // TODO(fxbug.dev/84729)
    #[allow(unused)]
    have_data: bool,

    sample_format: AudioSampleFormat,
    channels: u8,
    frames_per_second: u32,

    // TODO(fxbug.dev/84729)
    #[allow(unused)]
    output: Option<OutputProxy>,
    output_sender: Option<mpsc::Sender<ExtractMsg>>,
}

impl VirtualOutput {
    pub fn new(
        sample_format: AudioSampleFormat,
        channels: u8,
        frames_per_second: u32,
    ) -> Result<VirtualOutput, Error> {
        Ok(VirtualOutput {
            extracted_data: vec![],
            capturing: Arc::new(Mutex::new(false)),
            have_data: false,

            sample_format,
            channels,
            frames_per_second,

            output: None,
            output_sender: None,
        })
    }

    pub fn start_output(&mut self) -> Result<(), Error> {
        let va_output = app::client::connect_to_protocol::<OutputMarker>()?;
        va_output.clear_format_ranges()?;
        va_output.set_fifo_depth(0)?;
        va_output.set_external_delay(0)?;
        va_output.set_unique_id(&mut AUDIO_OUTPUT_ID.clone())?;

        let sample_format = get_zircon_sample_format(self.sample_format);
        va_output.add_format_range(
            sample_format,
            self.frames_per_second,
            self.frames_per_second,
            self.channels,
            self.channels,
            ASF_RANGE_FLAG_FPS_CONTINUOUS,
        )?;

        // set buffer size to be at least 1s.
        let frames_1ms = self.frames_per_second / 1000;
        let frames_low = 1000 * frames_1ms;
        let frames_high = 2000 * frames_1ms;
        let frames_modulo = 1 * frames_1ms;
        va_output.set_ring_buffer_restrictions(frames_low, frames_high, frames_modulo)?;

        let (tx, rx) = mpsc::channel(512);
        va_output.add()?;
        fasync::Task::spawn(
            async move {
                let mut worker = OutputWorker::default();
                worker.run(rx, va_output).await?;
                Ok::<(), Error>(())
            }
            .unwrap_or_else(|e| eprintln!("Output extraction thread failed: {:?}", e)),
        )
        .detach();

        self.output_sender = Some(tx);
        Ok(())
    }

    pub fn write_header(&mut self, len: u32) -> Result<(), Error> {
        let bytes_per_sample = get_sample_size(get_zircon_sample_format(self.sample_format))?;

        // 8 Bytes
        self.extracted_data.write("RIFF".as_bytes())?;
        self.extracted_data.write(&u32::to_le_bytes(len + 8 + 28 + 8))?;

        // 28 bytes
        self.extracted_data.write("WAVE".as_bytes())?; // wave_four_cc uint32
        self.extracted_data.write("fmt ".as_bytes())?; // fmt_four_cc uint32
        self.extracted_data.write(&u32::to_le_bytes(16))?; // fmt_chunk_len
        self.extracted_data.write(&u16::to_le_bytes(1))?; // format
        self.extracted_data.write(&u16::to_le_bytes(self.channels.into()))?;
        self.extracted_data.write(&u32::to_le_bytes(self.frames_per_second))?;
        let channels: u32 = self.channels.into();
        self.extracted_data
            .write(&u32::to_le_bytes(bytes_per_sample * channels * self.frames_per_second))?; // avg_byte_rate
        self.extracted_data.write(&u16::to_le_bytes((bytes_per_sample * channels).try_into()?))?;
        self.extracted_data.write(&u16::to_le_bytes((bytes_per_sample * 8).try_into()?))?;

        // 8 bytes
        self.extracted_data.write("data".as_bytes())?;
        self.extracted_data.write(&u32::to_le_bytes(len))?;

        Ok(())
    }
}

#[derive(Debug, Default)]
struct InputWorker {
    va_input: Option<InputProxy>,
    inj_data: Vec<u8>,
    vmo: Option<zx::Vmo>,

    // How much of the vmo's data we're actually using, in bytes.
    work_space: usize,

    // How many frames ahead of the position we want to be writing to, when writing.
    target_frames: usize,

    // How often, in frames, we want to be updated on the state of the injection ring buffer.
    frames_per_notification: usize,

    // How many bytes a frame is.
    frame_size: usize,

    // Next write pointer as a byte number.
    // This is not an offset into vmo (it does not wrap around).
    write_pointer: usize,

    // Next ring buffer pointer as a byte number.
    // This is not an offset into vmo (it does not wrap around).
    ring_pointer: usize,

    // Last relative ring buffer byte offset.
    // This is an offset into vmo (it wraps around).
    last_ring_offset: usize,
}

impl InputWorker {
    fn write_to_vmo(&self, data: &[u8]) -> Result<(), Error> {
        let vmo = if let Some(vmo) = &self.vmo { vmo } else { return Ok(()) };
        let start = self.write_pointer % self.work_space;
        let end = (self.write_pointer + data.len()) % self.work_space;
        let start_u64 = start.try_into()?;

        // Write in two chunks if we've wrapped around.
        if end < start {
            let split = self.work_space - start;
            vmo.write(&data[0..split], start_u64)?;
            vmo.write(&data[split..], 0)?;
        } else {
            vmo.write(&data, start_u64)?;
        }
        Ok(())
    }

    // Events from the sl4f facade
    fn flush(&mut self) {
        self.inj_data.clear();
    }

    fn set_data(&mut self, data: Vec<u8>) -> Result<(), Error> {
        if self.inj_data.len() > 0 {
            return Err(format_err!("Cannot inject new audio without flushing old audio"));
        }
        // This is a bad assumption, wav headers can be many different sizes.
        // 8 Bytes for riff header
        // 28 bytes for wave fmt block
        // 8 bytes for data header
        self.inj_data.write(&data[44..])?;
        fx_log_info!("AudioFacade::InputWorker: Injecting {:?} bytes", self.inj_data.len());
        Ok(())
    }

    // Events from the Virtual Audio Device
    fn on_set_format(
        &mut self,
        frames_per_second: u32,
        sample_format: u32,
        num_channels: u32,
        _external_delay: i64,
    ) -> Result<(), Error> {
        let sample_size = get_sample_size(sample_format)?;
        self.frame_size = usize::try_from(num_channels)? * usize::try_from(sample_size)?;

        let frames_per_millisecond = frames_per_second / 1000;

        fx_log_info!(
            "AudioFacade::InputWorker: configuring with {:?} fps, {:?} bpf",
            frames_per_second,
            self.frame_size
        );

        // We get notified every 50ms and write up to 250ms worth of data in the future.
        // The gap between frames_per_notification and target_frames gives us slack to
        // account for scheduling delays. Audio injection proceeds as follows:
        //
        //   1. When the buffer is created, the initial ring buffer position is "0ms".
        //      At that time, the ring buffer is zeroed and we pretend to write target_frames
        //      worth of silence to the ring buffer. This puts our write pointer ~250ms
        //      ahead of the ring buffer's safe read pointer.
        //
        //   2. We receive the first notification at time 50ms + scheduling delay. At this
        //      point we write data for the range 250ms - 300ms. As long as our scheduling
        //      delay is < 250ms, our writes will stay ahead of the ring buffer's safe read
        //      pointer. We assume that 250ms is more than enough time (this test framework
        //      should never be run in a debug or sanitizer build).
        //
        //   3. This continues ad infinitum.
        //
        // The sum of 250ms + 50ms must fit within the ring buffer. We currently use a 1s
        // ring buffer: see VirtualInput.start_input.
        //
        self.target_frames = (250 * frames_per_millisecond).try_into()?;
        self.frames_per_notification = (50 * frames_per_millisecond).try_into()?;
        Ok(())
    }

    fn on_buffer_created(
        &mut self,
        ring_buffer: zx::Vmo,
        num_ring_buffer_frames: u32,
        _notifications_per_ring: u32,
    ) -> Result<(), Error> {
        let va_input = self.va_input.as_mut().ok_or(format_err!("va_input not initialized"))?;

        // Ignore AudioCore's notification cadence (_notifications_per_ring); set up our own.
        let target_notifications_per_ring =
            num_ring_buffer_frames / u32::try_from(self.frames_per_notification)?;

        va_input.set_notification_frequency(target_notifications_per_ring)?;

        // The buffer starts zeroed and our write pointer starts target_frames in the future.
        self.work_space = usize::try_from(num_ring_buffer_frames)? * self.frame_size;
        self.write_pointer = self.target_frames * self.frame_size;
        self.last_ring_offset = 0;
        self.vmo = Some(ring_buffer);

        fx_log_info!(
            "AudioFacade::InputWorker: created buffer with {:?} frames, {:?} notifications, {:?} target frames per write",
            num_ring_buffer_frames,
            target_notifications_per_ring,
            self.target_frames
        );
        Ok(())
    }

    fn on_position_notify(&mut self, _monotonic_time: i64, ring_offset: u32) -> Result<(), Error> {
        let ring_offset = ring_offset.try_into()?;
        if ring_offset < self.last_ring_offset {
            self.ring_pointer += self.work_space - self.last_ring_offset + ring_offset;
        } else {
            self.ring_pointer += ring_offset - self.last_ring_offset;
        };

        let next_write_pointer = self.ring_pointer + self.target_frames * self.frame_size;
        let bytes_to_write = next_write_pointer - self.write_pointer;

        // Next segment of inj_data.
        if self.inj_data.len() > 0 {
            let data_end = std::cmp::min(self.inj_data.len(), bytes_to_write);
            self.write_to_vmo(&self.inj_data[0..data_end])?;
            self.inj_data.drain(0..data_end);
            self.write_pointer += data_end;
        }

        // Pad with zeroes.
        if self.write_pointer < next_write_pointer {
            let zeroes = vec![0; next_write_pointer - self.write_pointer];
            self.write_to_vmo(&zeroes)?;
            self.write_pointer = next_write_pointer;
        }

        self.last_ring_offset = ring_offset;
        Ok(())
    }

    async fn run(
        &mut self,
        mut rx: mpsc::Receiver<InjectMsg>,
        va_input: InputProxy,
    ) -> Result<(), Error> {
        let mut input_events = va_input.take_event_stream();
        self.va_input = Some(va_input);

        // Monotonic timestamp returned by the most-recent OnStart/OnPositionNotify/OnStop response.
        let mut last_timestamp = zx::Time::from_nanos(0);
        // Observed monotonic time that OnStart/OnPositionNotify/OnStop messages actually arrived.
        let mut last_event_time = zx::Time::from_nanos(0);

        loop {
            select! {
                rx_msg = rx.next() => {
                    match rx_msg {
                        None => {
                            return Err(format_err!("Got None InjectMsg Event, exiting worker"));
                        },
                        Some(InjectMsg::Flush) => {
                            self.flush();
                        }
                        Some(InjectMsg::Data { data }) => {
                            self.set_data(data)?;
                        }
                    }
                },
                input_msg = input_events.try_next() => {
                    match input_msg? {
                        None => {
                            return Err(format_err!("Got None InputEvent Message, exiting worker"));
                        },
                        Some(InputEvent::OnSetFormat { frames_per_second, sample_format,
                                                       num_channels, external_delay}) => {
                            self.on_set_format(frames_per_second, sample_format, num_channels,
                                               external_delay)?;
                        },
                        Some(InputEvent::OnBufferCreated { ring_buffer, num_ring_buffer_frames,
                                                           notifications_per_ring }) => {
                            self.on_buffer_created(ring_buffer, num_ring_buffer_frames,
                                                   notifications_per_ring)?;
                        },
                        Some(InputEvent::OnStart { start_time }) => {
                            if last_timestamp > zx::Time::from_nanos(0) {
                                fx_log_info!("AudioFacade::InputWorker: Injection OnPositionNotify received before OnStart");
                            }
                            last_timestamp = zx::Time::from_nanos(start_time);
                            last_event_time = zx::Time::get_monotonic();
                        },
                        Some(InputEvent::OnStop { stop_time: _, ring_position: _ }) => {
                            if last_timestamp == zx::Time::from_nanos(0) {
                                fx_log_info!("AudioFacade::InputWorker: Injection OnPositionNotify timestamp cleared before OnStop");
                            }
                            last_timestamp = zx::Time::from_nanos(0);
                            last_event_time = zx::Time::from_nanos(0);
                        },
                        Some(InputEvent::OnPositionNotify { monotonic_time, ring_position }) => {
                            let monotonic_zx_time = zx::Time::from_nanos(monotonic_time);
                            let now = zx::Time::get_monotonic();

                            // To minimize logspam, log glitches only when writing audio.
                            if self.inj_data.len() > 0 {
                                if last_timestamp == zx::Time::from_nanos(0) {
                                    fx_log_info!("AudioFacade::InputWorker: Injection OnStart not received before OnPositionNotify");
                                }

                                // Log if our timestamps had a gap of more than 100ms. This is highly
                                // abnormal and indicates possible glitching while receiving audio to
                                // be injected and/or providing it to the system.
                                let timestamp_interval = monotonic_zx_time - last_timestamp;

                                if  timestamp_interval > zx::Duration::from_millis(100) {
                                    fx_log_info!("AudioFacade::InputWorker: Injection position timestamp jumped by more than 100ms ({:?}ms). Expect glitches.",
                                        timestamp_interval.into_millis());
                                }
                                if  monotonic_zx_time < last_timestamp {
                                    fx_log_info!("AudioFacade::InputWorker: Injection position timestamp moved backwards ({:?}ms). Expect glitches.",
                                        timestamp_interval.into_millis());
                                }

                                // Log if there was a gap in position notification arrivals of more
                                // than 150ms. This is highly abnormal and indicates possible glitching
                                // while receiving audio to be injected and/or providing it to the
                                // system.
                                let observed_interval = now - last_event_time;

                                if  observed_interval > zx::Duration::from_millis(150) {
                                    fx_log_info!("AudioFacade::InputWorker: Injection position not updated for 150ms ({:?}ms). Expect glitches.",
                                        observed_interval.into_millis());
                                }
                            }

                            last_timestamp = monotonic_zx_time;
                            last_event_time = now;
                            self.on_position_notify(monotonic_time, ring_position)?;
                        },
                        Some(evt) => {
                            fx_log_info!("AudioFacade::InputWorker: Got unknown InputEvent {:?}", evt);
                        }
                    }
                },
            };
        }
    }
}

#[derive(Debug)]
struct VirtualInput {
    injection_data: Vec<Vec<u8>>,
    have_data: bool,

    sample_format: AudioSampleFormat,
    channels: u8,
    frames_per_second: u32,

    input_sender: Option<mpsc::Sender<InjectMsg>>,
}

impl VirtualInput {
    fn new(sample_format: AudioSampleFormat, channels: u8, frames_per_second: u32) -> Self {
        VirtualInput {
            injection_data: vec![],
            have_data: false,

            sample_format,
            channels,
            frames_per_second,

            input_sender: None,
        }
    }

    pub fn start_input(&mut self) -> Result<(), Error> {
        let va_input = app::client::connect_to_protocol::<InputMarker>()?;
        va_input.clear_format_ranges()?;
        va_input.set_fifo_depth(0)?;
        va_input.set_external_delay(0)?;

        let sample_format = get_zircon_sample_format(self.sample_format);
        va_input.add_format_range(
            sample_format,
            self.frames_per_second,
            self.frames_per_second,
            self.channels,
            self.channels,
            ASF_RANGE_FLAG_FPS_CONTINUOUS,
        )?;

        // set buffer size to be at least 1s.
        let frames_1ms = self.frames_per_second / 1000;
        let frames_low = 1000 * frames_1ms;
        let frames_high = 2000 * frames_1ms;
        let frames_modulo = 1 * frames_1ms;
        va_input.set_ring_buffer_restrictions(frames_low, frames_high, frames_modulo)?;

        va_input.add()?;
        let (tx, rx) = mpsc::channel(512);
        fasync::Task::spawn(
            async move {
                let mut worker = InputWorker::default();
                worker.run(rx, va_input).await?;
                Ok::<(), Error>(())
            }
            .unwrap_or_else(|e| eprintln!("Input injection thread failed: {:?}", e)),
        )
        .detach();

        self.input_sender = Some(tx);
        Ok(())
    }

    pub fn play(&mut self, index: usize) -> Result<(), Error> {
        let sender =
            self.input_sender.as_mut().ok_or(format_err!("input_sender not initialized"))?;
        sender.try_send(InjectMsg::Flush)?;
        sender.try_send(InjectMsg::Data { data: self.injection_data[index].clone() })?;
        Ok(())
    }

    pub fn stop(&mut self) -> Result<(), Error> {
        let sender =
            self.input_sender.as_mut().ok_or(format_err!("input_sender not initialized"))?;
        sender.try_send(InjectMsg::Flush)?;
        Ok(())
    }
}

#[derive(Debug)]
enum InjectMsg {
    Flush,
    Data { data: Vec<u8> },
}

#[derive(Debug)]
struct VirtualAudio {
    // Output is from the AudioCore side, so it's what we'll be capturing and extracting
    output_sample_format: AudioSampleFormat,
    output_channels: u8,
    output_frames_per_second: u32,
    // TODO(fxbug.dev/84729)
    #[allow(unused)]
    output: Option<OutputProxy>,

    // Input is from the AudioCore side, so it's the audio we'll be injecting
    input_sample_format: AudioSampleFormat,
    input_channels: u8,
    input_frames_per_second: u32,

    controller: ControlProxy,
}

impl VirtualAudio {
    fn new() -> Result<VirtualAudio, Error> {
        let va_control = app::client::connect_to_protocol::<ControlMarker>()?;
        Ok(VirtualAudio {
            output_sample_format: AudioSampleFormat::Signed16,
            output_channels: 2,
            output_frames_per_second: 48000,
            output: None,

            input_sample_format: AudioSampleFormat::Signed16,
            input_channels: 2,
            input_frames_per_second: 16000,

            controller: va_control,
        })
    }
}

/// Perform Audio operations.
///
/// Note this object is shared among all threads created by server.
#[derive(Debug)]
pub struct AudioFacade {
    // TODO(perley): This will be needed after migrating to using virtual audio devices rather than
    //               renderer+capturer in the facade.
    vad_control: RwLock<VirtualAudio>,
    audio_output: RwLock<VirtualOutput>,
    audio_input: RwLock<VirtualInput>,
    initialized: Mutex<bool>,
}

impl AudioFacade {
    async fn ensure_initialized(&self) -> Result<(), Error> {
        let mut initialized = self.initialized.lock().await;
        if !*(initialized) {
            let controller = self.vad_control.read().controller.clone();
            controller.disable().await?;
            controller.enable().await?;
            self.audio_input.write().start_input()?;
            self.audio_output.write().start_output()?;
            *(initialized) = true;
        }
        Ok(())
    }

    pub fn new() -> Result<AudioFacade, Error> {
        let vad_control = RwLock::new(VirtualAudio::new()?);
        let audio_output = RwLock::new(VirtualOutput::new(
            vad_control.read().output_sample_format,
            vad_control.read().output_channels,
            vad_control.read().output_frames_per_second,
        )?);
        let audio_input = RwLock::new(VirtualInput::new(
            vad_control.read().input_sample_format,
            vad_control.read().input_channels,
            vad_control.read().input_frames_per_second,
        ));
        let initialized = Mutex::new(false);

        Ok(AudioFacade { vad_control, audio_output, audio_input, initialized })
    }

    pub async fn start_output_save(&self) -> Result<Value, Error> {
        self.ensure_initialized().await?;
        let capturing = self.audio_output.read().capturing.clone();
        let mut capturing = capturing.lock().await;
        if !*capturing {
            let mut write = self.audio_output.write();
            let sender = write
                .output_sender
                .as_mut()
                .ok_or(format_err!("Failed unwrapping output sender"))?;
            sender.try_send(ExtractMsg::Start)?;
            *(capturing) = true;

            Ok(to_value(true)?)
        } else {
            return Err(format_err!("Cannot StartOutputSave, already started."));
        }
    }

    pub async fn stop_output_save(&self) -> Result<Value, Error> {
        self.ensure_initialized().await?;
        let capturing = self.audio_output.read().capturing.clone();
        let mut capturing = capturing.lock().await;
        if *capturing {
            let mut write = self.audio_output.write();
            write.extracted_data.clear();

            let sender = write
                .output_sender
                .as_mut()
                .ok_or(format_err!("Failed unwrapping output sender"))?;

            let (tx, mut rx) = mpsc::channel(512);
            sender.try_send(ExtractMsg::Stop { out_sender: tx })?;

            let mut saved_audio = rx
                .next()
                .await
                .ok_or_else(|| format_err!("StopOutputSave failed, could not retrieve data."))?;
            let len = saved_audio.len().try_into()?;

            write.write_header(len)?;

            write.extracted_data.append(&mut saved_audio);
            *(capturing) = false;
            Ok(to_value(true)?)
        } else {
            return Err(format_err!("Cannot StopOutputSave, not started."));
        }
    }

    pub async fn get_output_audio(&self) -> Result<Value, Error> {
        self.ensure_initialized().await?;
        let capturing = self.audio_output.read().capturing.clone();
        let capturing = capturing.lock().await;
        if !*capturing {
            Ok(to_value(base64::encode(&self.audio_output.read().extracted_data))?)
        } else {
            return Err(format_err!("GetOutputAudio failed, still saving."));
        }
    }

    pub async fn put_input_audio(&self, args: Value) -> Result<Value, Error> {
        self.ensure_initialized().await?;
        let data = args.get("data").ok_or(format_err!("PutInputAudio failed, no data"))?;
        let data = data.as_str().ok_or(format_err!("PutInputAudio failed, data not string"))?;

        let mut wave_data_vec = base64::decode(data)?;
        let sample_index = args["index"].as_u64().ok_or(format_err!("index not a number"))?;
        let sample_index = sample_index.try_into()?;

        // TODO(perley): check wave format for correct bits per sample and float/int.
        let byte_cnt = wave_data_vec.len();
        {
            let mut write = self.audio_input.write();
            // Make sure we have somewhere to store the wav data.
            if write.injection_data.len() <= sample_index {
                write.injection_data.resize(sample_index + 1, vec![]);
            }

            write.injection_data[sample_index].clear();
            write.injection_data[sample_index].append(&mut wave_data_vec);
            write.have_data = true;
        }
        Ok(to_value(byte_cnt)?)
    }

    pub async fn start_input_injection(&self, args: Value) -> Result<Value, Error> {
        self.ensure_initialized().await?;
        {
            let sample_index = args["index"].as_u64().ok_or(format_err!("index not a number"))?;
            let sample_index = sample_index.try_into()?;

            if !self.audio_input.read().have_data {
                return Err(format_err!("StartInputInjection failed, no Audio data to inject."));
            }
            self.audio_input.write().play(sample_index)?;
        }
        Ok(to_value(true)?)
    }

    pub async fn stop_input_injection(&self) -> Result<Value, Error> {
        self.ensure_initialized().await?;
        self.audio_input.write().stop()?;
        Ok(to_value(true)?)
    }
}
