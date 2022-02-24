// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::utils::{watch_to_stream, Either, WatchOrSetResult};
use fidl_fuchsia_settings::{AudioInput, AudioProxy, AudioSettings, AudioStreamSettings, Volume};
use fuchsia_syslog::fx_log_info;

pub async fn command(
    proxy: AudioProxy,
    stream: Option<fidl_fuchsia_media::AudioRenderUsage>,
    source: Option<fidl_fuchsia_settings::AudioStreamSettingSource>,
    level: Option<f32>,
    volume_muted: Option<bool>,
    input_muted: Option<bool>,
) -> WatchOrSetResult {
    let mut audio_settings = AudioSettings::EMPTY;
    let mut stream_settings = AudioStreamSettings::EMPTY;
    let mut volume = Volume::EMPTY;
    let mut input = AudioInput::EMPTY;

    volume.level = level;
    volume.muted = volume_muted;
    stream_settings.stream = stream;
    stream_settings.source = source;
    if volume != Volume::EMPTY {
        stream_settings.user_volume = Some(volume);
    }
    input.muted = input_muted;

    if stream_settings != AudioStreamSettings::EMPTY {
        audio_settings.streams = Some(vec![stream_settings]);
    }
    if input != AudioInput::EMPTY {
        audio_settings.input = Some(input);
    }

    let none_set = stream.is_none()
        && source.is_none()
        && level.is_none()
        && volume_muted.is_none()
        && input_muted.is_none();

    Ok(if none_set {
        Either::Watch(watch_to_stream(proxy, |p| p.watch()))
    } else {
        let mut output = String::new();
        fx_log_info!("Setting audio_settings {:?}", audio_settings);
        let mutate_result = proxy.set(audio_settings).await?;
        match mutate_result {
            Ok(_) => {
                if let Some(stream_val) = stream {
                    output.push_str(&format!("Successfully set stream to {:?}\n", stream_val));
                }
                if let Some(source_val) = source {
                    output.push_str(&format!("Successfully set source to {:?}\n", source_val));
                }
                if let Some(level_val) = level {
                    output.push_str(&format!("Successfully set level to {}\n", level_val));
                }
                if let Some(volume_muted_val) = volume_muted {
                    output.push_str(&format!(
                        "Successfully set volume_muted to {}\n",
                        volume_muted_val
                    ));
                }
                if let Some(input_muted_val) = input_muted {
                    output.push_str(&format!(
                        "Successfully set input_muted to {}\n",
                        input_muted_val
                    ));
                }
            }
            // TODO(fxbug.dev/67748) This should return an error rather than silently formatting
            // into the result.
            Err(err) => output.push_str(&format!("{:?}", err)),
        }
        Either::Set(output)
    })
}
