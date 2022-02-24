// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "codec_factory_app.h"

#include <dirent.h>
#include <fuchsia/cobalt/cpp/fidl.h>
#include <fuchsia/hardware/mediacodec/cpp/fidl.h>
#include <fuchsia/mediacodec/cpp/fidl.h>
#include <fuchsia/sysinfo/cpp/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/syslog/global.h>
#include <zircon/status.h>

#include <algorithm>
#include <random>

#include "codec_factory_impl.h"
#include "codec_isolate.h"
#include "lib/fidl/cpp/interface_request.h"
#include "lib/fidl/cpp/string.h"
#include "lib/sys/cpp/component_context.h"
#include "src/lib/fsl/io/device_watcher.h"

namespace {

constexpr char kDeviceClass[] = "/dev/class/media-codec";
constexpr char kGpuDeviceClass[] = "/dev/class/gpu";
const char* kLogTag = "CodecFactoryApp";
const char kRealmSvc[] = "fuchsia.component.Realm";

const std::string kAllSwDecoderMimeTypes[] = {
    "video/h264",  // VIDEO_ENCODING_H264
};

}  // namespace

// board_name_ initialization requires startup_context_ already initialized.
// policy_ initialization requires board_name_ already initialized.
CodecFactoryApp::CodecFactoryApp(async_dispatcher_t* dispatcher)
    : dispatcher_(dispatcher),
      startup_context_(sys::ComponentContext::Create()),
      board_name_(GetBoardName()),
      policy_(this) {
  // Don't publish service or outgoing()->ServeFromStartupInfo() until after initial discovery is
  // done, else the pumping of the loop will drop the incoming request for CodecFactory before
  // AddPublicService() below has had a chance to register for it.

  zx_status_t status =
      outgoing_codec_aux_service_directory_parent_.AddPublicService<fuchsia::cobalt::LoggerFactory>(
          [this](fidl::InterfaceRequest<fuchsia::cobalt::LoggerFactory> request) {
            ZX_DEBUG_ASSERT(startup_context_);
            FX_LOGF(INFO, kLogTag,
                    "codec_factory handling request for LoggerFactory -- handle value: %u",
                    request.channel().get());
            startup_context_->svc()->Connect(std::move(request));
          });
  outgoing_codec_aux_service_directory_ =
      outgoing_codec_aux_service_directory_parent_.GetOrCreateDirectory("svc");

  // Else codec_factory won't be able to provide what codecs expect to be able to rely on.
  ZX_ASSERT(status == ZX_OK);

  DiscoverMagmaCodecDriversAndListenForMoreAsync();
  DiscoverMediaCodecDriversAndListenForMoreAsync();
}

bool CodecFactoryApp::IsV2() {
  DIR* dir = opendir("/svc");
  if (!dir) {
    return false;
  }

  struct dirent* ent;
  std::string svc_name = std::string(kRealmSvc);
  while ((ent = readdir(dir)) != nullptr) {
    if (svc_name == ent->d_name) {
      closedir(dir);
      return true;
    }
  }
  closedir(dir);
  return false;
}

void CodecFactoryApp::PublishService() {
  // We delay doing this until we're completely ready to add services.
  // We _rely_ on the driver to either fail the channel or send OnCodecList().
  ZX_DEBUG_ASSERT(existing_devices_discovered_);
  zx_status_t status =
      startup_context_->outgoing()->AddPublicService<fuchsia::mediacodec::CodecFactory>(
          [this](fidl::InterfaceRequest<fuchsia::mediacodec::CodecFactory> request) {
            // The CodecFactoryImpl is self-owned and will self-delete when the
            // channel closes or an error occurs.
            CodecFactoryImpl::CreateSelfOwned(this, startup_context_.get(), std::move(request),
                                              this->IsV2());
          });
  // else this codec_factory is useless
  ZX_ASSERT(status == ZX_OK);
  status = startup_context_->outgoing()->ServeFromStartupInfo();
  ZX_ASSERT(status == ZX_OK);
}

// All of the current supported hardware and software decoders, randomly shuffled
// so as to avoid clients depending on the order.
// TODO(schottm): send encoders as well
std::vector<fuchsia::mediacodec::CodecDescription> CodecFactoryApp::MakeCodecList() const {
  std::vector<fuchsia::mediacodec::CodecDescription> codecs;
  for (const auto& mime_type : kAllSwDecoderMimeTypes) {
    codecs.push_back({
        .codec_type = fuchsia::mediacodec::CodecType::DECODER,
        .mime_type = mime_type,

        // TODO(schottm): can some of these be true?
        .can_stream_bytes_input = false,
        .can_find_start = false,
        .can_re_sync = false,
        .will_report_all_detected_errors = false,

        .is_hw = false,
        .split_header_handling = true,
    });
  }

  for (const auto& entry : hw_codecs_) {
    codecs.push_back(entry->description);
  }
  auto rng = std::default_random_engine();
  std::shuffle(codecs.begin(), codecs.end(), rng);

  return codecs;
}

const fuchsia::mediacodec::CodecFactoryPtr* CodecFactoryApp::FindHwCodec(
    fit::function<bool(const fuchsia::mediacodec::CodecDescription&)> is_match) {
  auto iter = std::find_if(hw_codecs_.begin(), hw_codecs_.end(),
                           [&is_match](const std::unique_ptr<CodecListEntry>& entry) -> bool {
                             return is_match(entry->description);
                           });
  if (iter == hw_codecs_.end()) {
    return nullptr;
  }
  // HW codecs are connected to using factory, not by launching a component using the URL.
  if (!(*iter)->component_url.empty())
    return nullptr;
  return (*iter)->factory.get();
}

const std::optional<std::string> CodecFactoryApp::FindHwIsolate(
    fit::function<bool(const fuchsia::mediacodec::CodecDescription&)> is_match) {
  auto iter = std::find_if(hw_codecs_.begin(), hw_codecs_.end(),
                           [&is_match](const std::unique_ptr<CodecListEntry>& entry) -> bool {
                             return is_match(entry->description);
                           });
  if (iter == hw_codecs_.end()) {
    return {};
  }
  if ((*iter)->component_url.empty()) {
    return {};
  }
  return (*iter)->component_url;
}

void CodecFactoryApp::IdledCodecDiscovery() {
  ZX_ASSERT(num_codec_discoveries_in_flight_ >= 1);
  if (--num_codec_discoveries_in_flight_ == 0) {
    // The idle_callback indicates that all pre-existing devices have been
    // seen, and by the time this item reaches the front of the discovery
    // queue, all pre-existing devices have all been processed.
    device_discovery_queue_.emplace_back(std::make_unique<DeviceDiscoveryEntry>());
    PostDiscoveryQueueProcessing();
  }
}

void CodecFactoryApp::DiscoverMediaCodecDriversAndListenForMoreAsync() {
  num_codec_discoveries_in_flight_++;
  // We use fsl::DeviceWatcher::CreateWithIdleCallback() instead of fsl::DeviceWatcher::Create()
  // because the CodecFactory service is started on demand, and we don't want to start serving
  // CodecFactory until we've discovered and processed all existing media-codec devices.  That way,
  // the first time a client requests a HW-backed codec, we robustly consider all codecs provided by
  // pre-existing devices.  The request for a HW-backed Codec will have a much higher probability of
  // succeeding vs. if we just discovered pre-existing devices async.  This doesn't prevent the
  // possiblity that the device might not exist at the moment the CodecFactory is started, but as
  // long as the device does exist by then, this will ensure the device's codecs are considered,
  // including for the first client request.
  device_watcher_ = fsl::DeviceWatcher::CreateWithIdleCallback(
      kDeviceClass,
      [this](int dir_fd, std::string filename) {
        std::string device_path = std::string(kDeviceClass) + "/" + filename;
        zx::channel device_channel, device_remote;
        zx_status_t status = zx::channel::create(0, &device_channel, &device_remote);
        if (status != ZX_OK) {
          FX_LOGS(ERROR) << "Failed to create channel - status: " << status;
          return;
        }
        zx::channel client_factory_channel, client_factory_remote;
        status = zx::channel::create(0, &client_factory_channel, &client_factory_remote);
        if (status != ZX_OK) {
          FX_LOGS(ERROR) << "Failed to create channel - status: " << status;
          return;
        }

        status = fdio_service_connect(device_path.c_str(), device_remote.release());
        if (status != ZX_OK) {
          FX_LOGS(ERROR) << "Failed to connect to device by filename -"
                         << " status: " << status << " device_path: " << device_path;
          return;
        }

        fuchsia::hardware::mediacodec::DevicePtr device_interface;
        status = device_interface.Bind(std::move(device_channel));
        if (status != ZX_OK) {
          FX_LOGS(ERROR) << "Failed to bind to interface -"
                         << " status: " << status << " device_path: " << device_path;
          return;
        }

        fidl::InterfaceHandle<fuchsia::io::Directory> aux_service_directory;
        status = outgoing_codec_aux_service_directory_->Serve(
            fuchsia::io::OPEN_RIGHT_READABLE | fuchsia::io::OPEN_RIGHT_WRITABLE |
                fuchsia::io::OPEN_FLAG_DIRECTORY,
            aux_service_directory.NewRequest().TakeChannel(), dispatcher_);
        if (status != ZX_OK) {
          FX_LOGS(ERROR) << "outgoing_codec_aux_service_directory_.Serve() failed - status: "
                         << status;
          return;
        }

        // It's ok for a codec that doesn't need the aux service directory to just close the client
        // handle to it, so there's no need to attempt to detect a codec closing the aux service
        // directory client end.
        //
        // TODO(dustingreen): Combine these two calls into "Connect" and use FIDL table with the
        // needed fields.
        device_interface->SetAuxServiceDirectory(std::move(aux_service_directory));
        device_interface->GetCodecFactory(std::move(client_factory_remote));

        // From here on in the current lambda, we're doing stuff that can't fail
        // here locally (at least, not without exiting the whole process).  The
        // error handler will handle channel error async.

        auto discovery_entry = std::make_unique<DeviceDiscoveryEntry>();
        discovery_entry->device_path = device_path;

        discovery_entry->codec_factory = std::make_shared<fuchsia::mediacodec::CodecFactoryPtr>();
        discovery_entry->codec_factory->set_error_handler(
            [this, device_path,
             factory = discovery_entry->codec_factory.get()](zx_status_t status) {
              // Any given factory won't be in both lists, but will be in one or
              // the other by the time this error handler runs.
              device_discovery_queue_.remove_if(
                  [factory](const std::unique_ptr<DeviceDiscoveryEntry>& entry) {
                    return factory == entry->codec_factory.get();
                  });
              // Perhaps the removed discovery item was the first item in the
              // list; maybe now the new first item in the list can be
              // processed.
              PostDiscoveryQueueProcessing();

              hw_codecs_.remove_if([factory](const std::unique_ptr<CodecListEntry>& entry) {
                return factory == entry->factory.get();
              });
            });

        discovery_entry->codec_factory->events().OnCodecList =
            [this, discovery_entry = discovery_entry.get()](
                std::vector<fuchsia::mediacodec::CodecDescription> codec_list) {
              discovery_entry->driver_codec_list = fidl::VectorPtr(codec_list);

              // We're no longer interested in OnCodecList events from the
              // driver's CodecFactory, should the driver send any more. Sending
              // more is not legal, but disconnect this event just in case,
              // since we don't want the old lambda that touches
              // driver_codec_list (this lambda).
              discovery_entry->codec_factory->events().OnCodecList = nullptr;
              // In case discovery_entry is the first item which is now ready to
              // process, process the discovery queue.
              PostDiscoveryQueueProcessing();
            };

        discovery_entry->codec_factory->Bind(std::move(client_factory_channel), dispatcher());

        device_discovery_queue_.emplace_back(std::move(discovery_entry));
      },
      [this] { IdledCodecDiscovery(); });
}

void CodecFactoryApp::TeardownMagmaCodec(
    std::shared_ptr<fuchsia::gpu::magma::DevicePtr> magma_device) {
  // Any given magma device won't be in both lists, but will be in one or
  // the other by the time this error handler runs.
  device_discovery_queue_.remove_if(
      [magma_device](const std::unique_ptr<DeviceDiscoveryEntry>& entry) {
        return magma_device.get() == entry->magma_device.get();
      });

  hw_codecs_.remove_if([magma_device](const std::unique_ptr<CodecListEntry>& entry) {
    return magma_device.get() == entry->magma_device.get();
  });

  // Perhaps the removed discovery item was the first item in the
  // list; maybe now the new first item in the list can be
  // processed.
  PostDiscoveryQueueProcessing();
}

void CodecFactoryApp::DiscoverMagmaCodecDriversAndListenForMoreAsync() {
  if (!IsV2()) {
    // Magma codec components can only be launched as V2.
    return;
  }
  num_codec_discoveries_in_flight_++;
  gpu_device_watcher_ = fsl::DeviceWatcher::CreateWithIdleCallback(
      kGpuDeviceClass,
      [this](int dir_fd, std::string filename) {
        std::string device_path = std::string(kGpuDeviceClass) + "/" + filename;
        auto magma_device = std::make_shared<fuchsia::gpu::magma::DevicePtr>();
        zx_status_t status = fdio_service_connect(
            device_path.c_str(), magma_device->NewRequest().TakeChannel().release());
        if (status != ZX_OK) {
          FX_LOGS(ERROR) << "Failed to connect to device by filename -"
                         << " status: " << status << " device_path: " << device_path;
          return;
        }
        auto discovery_entry = std::make_unique<DeviceDiscoveryEntry>();
        discovery_entry->device_path = device_path;
        discovery_entry->magma_device = magma_device;
        discovery_entry->magma_device->set_error_handler(
            [this, device_path, magma_device](zx_status_t status) {
              TeardownMagmaCodec(magma_device);
            });
        (*magma_device)
            ->GetIcdList([this, discovery_entry = discovery_entry.get(), magma_device,
                          device_path](std::vector<fuchsia::gpu::magma::IcdInfo> icd_infos) {
              bool found_media_icd = false;
              for (auto& icd_entry : icd_infos) {
                if (!icd_entry.has_flags() || !icd_entry.has_component_url())
                  continue;
                if (!(icd_entry.flags() &
                      fuchsia::gpu::magma::IcdFlags::SUPPORTS_MEDIA_CODEC_FACTORY)) {
                  continue;
                }
                discovery_entry->component_url = icd_entry.component_url();
                ForwardToIsolate(
                    icd_entry.component_url(), true, IsolateType::kMagma, startup_context_.get(),
                    [this, magma_device, device_path](fuchsia::mediacodec::CodecFactoryPtr ptr) {
                      auto it = std::find_if(
                          device_discovery_queue_.begin(), device_discovery_queue_.end(),
                          [magma_device](const std::unique_ptr<DeviceDiscoveryEntry>& entry) {
                            return magma_device.get() == entry->magma_device.get();
                          });
                      if (it == device_discovery_queue_.end()) {
                        // Device was removed from the queue due to the magma error handler running.
                        return;
                      }
                      auto discovery_entry = it->get();

                      discovery_entry->codec_factory =
                          std::make_shared<fuchsia::mediacodec::CodecFactoryPtr>();
                      discovery_entry->codec_factory->Bind(ptr.Unbind());
                      discovery_entry->codec_factory->set_error_handler(
                          [this, device_path, magma_device](zx_status_t status) {
                            TeardownMagmaCodec(magma_device);
                          });
                      discovery_entry->codec_factory->events().OnCodecList =
                          [this, discovery_entry](
                              std::vector<fuchsia::mediacodec::CodecDescription> codec_list) {
                            discovery_entry->driver_codec_list = fidl::VectorPtr(codec_list);

                            // We're no longer interested in OnCodecList events from the
                            // driver's CodecFactory, should the driver send any more. Sending
                            // more is not legal, but disconnect this event just in case,
                            // since we don't want the old lambda that touches
                            // driver_codec_list (this lambda).
                            discovery_entry->codec_factory->events().OnCodecList = nullptr;
                            // In case discovery_entry is the first item which is now ready to
                            // process, process the discovery queue.
                            PostDiscoveryQueueProcessing();
                          };
                    },
                    [this, magma_device]() { TeardownMagmaCodec(magma_device); });
                found_media_icd = true;
                // Only support a single codec factory per magma device.
                break;
              }
              if (!found_media_icd) {
                TeardownMagmaCodec(magma_device);
              }
            });
        device_discovery_queue_.emplace_back(std::move(discovery_entry));
      },
      [this] { IdledCodecDiscovery(); });
}

void CodecFactoryApp::PostDiscoveryQueueProcessing() {
  async::PostTask(dispatcher_, fit::bind_member(this, &CodecFactoryApp::ProcessDiscoveryQueue));
}

void CodecFactoryApp::ProcessDiscoveryQueue() {
  // Both startup and steady-state use this processing loop.
  //
  // In startup, we care about ordering of the discovery queue because we want
  // to allow serving of CodecFactory as soon as all pre-existing devices are
  // done processing.  We care that pre-existing devices are before
  // newly-discovered devices in the queue.  As far as startup is concerned,
  // there are other ways we could track this without using a queue, but a queue
  // works, and using the queue allows startup to share code with steady-state.
  //
  // In steady-state, we care (a little) about ordering of the discovery queue
  // because we want (to a limited degree, for now) to prefer a
  // more-recently-discovered device over a less-recently-discovered device (for
  // now at least), so to make that robust, we preserve the device discovery
  // order through the codec discovery sequence, to account for the possibility
  // that an previously discovered device may have only just recently sent
  // OnCodecList before failing; without the device_discovery_queue_ that
  // previously-discovered device's OnCodecList could re-order vs. the
  // replacement device's OnCodecList.
  //
  // The device_discovery_queue_ marginally increases the odds of a client
  // request picking up a replacement devhost instead of an old devhost that
  // failed quickly and which we haven't yet noticed is gone.  This devhost
  // replacement case is the main motivation for caring about the device
  // discovery order in the first place (at least for now), since it should be
  // robustly the case that discovery of the old devhost happens before
  // discovery of the replacement devhost.
  //
  // The ordering of the hw_codec_ list is the main way in which
  // more-recently-discovered codecs are prefered over less-recently-discovered
  // codecs.  The device_discovery_queue_ just makes the hw_codec_ ordering
  // exactly correspond to the device discovery order (reversed) even when
  // devices are discovered near each other in time.
  //
  // None of this changes the fact that a replacement devhost's arrival can race
  // with a client's request, so if a devhost fails and is replaced, it's quite
  // possible the client will see the Codec interface just fail.  Even if this
  // were mitigated, it wouldn't change the fact that a devhost failure later
  // would result in Codec interface failure at that time, so failures near the
  // start aren't really much different than async failures later. It can make
  // sense for a client to retry a low number of times (if the client wants to
  // work despite a devhost not always fully working), even if the Codec failure
  // happens quite early.
  while (!device_discovery_queue_.empty()) {
    std::unique_ptr<DeviceDiscoveryEntry>& front = device_discovery_queue_.front();
    if (!front->codec_factory && !front->magma_device) {
      // All pre-existing devices have been processed.
      //
      // Now the CodecFactory can begin serving (shortly).
      if (!existing_devices_discovered_) {
        existing_devices_discovered_ = true;
        PublishService();
      }
      // The marker has done its job, so remove the marker.
      device_discovery_queue_.pop_front();
      return;
    }

    if (!front->driver_codec_list) {
      // The first item is not yet ready.  The current method will get re-posted
      // when the first item is potentially ready.
      return;
    }
    if (!front->component_url.empty()) {
      // If there's a component URL then a new instance will be launched for every codec, so
      // codec_factory won't be used anymore.
      front->codec_factory = {};
    }
    FX_DCHECK(front->driver_codec_list.has_value());

    for (auto& codec_description : front->driver_codec_list.value()) {
      FX_LOGS(INFO) << "Registering "
                    << (codec_description.codec_type == fuchsia::mediacodec::CodecType::DECODER
                            ? "decoder"
                            : "encoder")
                    << ", mime_type: " << codec_description.mime_type
                    << ", device_path: " << front->device_path
                    << ", component url: " << front->component_url;
      hw_codecs_.emplace_front(std::make_unique<CodecListEntry>(CodecListEntry{
          .description = std::move(codec_description),
          .component_url = front->component_url,
          // shared_ptr<>
          .factory = front->codec_factory,
          .magma_device = front->magma_device,
      }));
    }

    device_discovery_queue_.pop_front();
  }
}

// This is called during field initialization portion of the constructor, so needs to avoid reading
// any fields that are not yet initialized.
std::string CodecFactoryApp::GetBoardName() {
  fuchsia::sysinfo::SysInfoSyncPtr sysinfo;
  zx_status_t status =
      startup_context_->svc()->Connect<fuchsia::sysinfo::SysInfo>(sysinfo.NewRequest());
  // CodecFactoryApp's process can't necessarily work correctly without the board name.
  ZX_ASSERT(status == ZX_OK);
  fidl::StringPtr board_name;
  zx_status_t fidl_status = sysinfo->GetBoardName(&status, &board_name);
  if (fidl_status != ZX_OK || status != ZX_OK) {
    // This path is only taken if CodecFactory can't contact fuchsia.sysinfo.SysInfo.  Most often
    // this happens in tests that don't grant access to fuchsia.sysinfo.SysInfo (yet).  Tests which
    // print this out should be updated to include these in their .cmx file:
    //
    // "facets": {
    //     "fuchsia.test": {
    //         "system-services": [
    //             "fuchsia.sysinfo.SysInfo"
    //         ]
    //     }
    // },
    // "sandbox": {
    //     "services": [
    //         "fuchsia.sysinfo.SysInfo"
    //     ]
    // }
    FX_LOGS(WARNING) << "#############################";
    FX_LOGS(WARNING) << "sysinfo->GetBoardName() failed.  "
                        "CodecFactoryApp needs access to fuchsia.sysinfo.SysInfo.  fidl_status: "
                     << fidl_status << " status: " << status;
    FX_LOGS(WARNING) << "#############################";
    return "<UNKNOWN>";
  }
  ZX_ASSERT(fidl_status == ZX_OK && status == ZX_OK);
  return board_name.value();
}
