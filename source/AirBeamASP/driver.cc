// Copyright (c) 2025 ChenKS12138

#include "aspl/Driver.hpp"

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/CoreAudio.h>
#include <arpa/inet.h>
#include <fmt/core.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/time.h>
#include <syslog.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <thread>

#include "aspl/ControlRequestHandler.hpp"
#include "aspl/Device.hpp"
#include "aspl/IORequestHandler.hpp"
#include "aspl/Plugin.hpp"
#include "helper/logger.h"
#include "macos/bonjour_browse.h"
#include "macos/volume_observer.h"
#include "raop/codec.h"
#include "raop/fifo.h"
#include "raop/raop.h"

namespace {

void airbeam_log(const char* fmt, ...) {
  FILE* f = fopen("/tmp/airbeam.log", "a");
  if (!f) return;
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm tm;
  localtime_r(&tv.tv_sec, &tm);
  fprintf(f, "%02d:%02d:%02d.%03d ", tm.tm_hour, tm.tm_min, tm.tm_sec,
          (int)(tv.tv_usec / 1000));
  va_list args;
  va_start(args, fmt);
  vfprintf(f, fmt, args);
  va_end(args);
  fprintf(f, "\n");
  fclose(f);
}

using namespace AirBeamCore::raop;
using namespace AirBeamCore::helper;
using namespace AirBeamCore::macos;

constexpr UInt32 SampleRate = 44100;
constexpr UInt32 ChannelCount = 2;

const size_t kFiFOCapacity = 30 * 1024 * 1024;  // 30MB buffer

class RaopHandler : public aspl::ControlRequestHandler,
                    public aspl::IORequestHandler {
 public:
  explicit RaopHandler(aspl::Device& device, const std::string& ip,
                       uint32_t port)
      : raop_(std::make_shared<Raop>(ip, port)),
        fifo_(kFiFOCapacity),
        device_(device) {
    auto volume_control =
        device_.GetVolumeControlByIndex(kAudioObjectPropertyScopeOutput, 0);
    volume_control->SetScalarValue(0.5);
  }

  OSStatus OnStartIO() override {
    Prepare();

    return kAudioHardwareNoError;
  }

  void OnStopIO() override {}

  void OnWriteMixedOutput(const std::shared_ptr<aspl::Stream>& stream,
                          Float64 zeroTimestamp, Float64 timestamp,
                          const void* buff, UInt32 buffBytesSize) override {
    size_t written = fifo_.Write(reinterpret_cast<const uint8_t*>(buff),
                                 buffBytesSize, std::chrono::milliseconds(5));
    if (written < buffBytesSize) {
      syslog(LOG_WARNING,
             "[AirBeam] FIFO DROP: wrote %zu/%u bytes (FIFO full)",
             written, buffBytesSize);
    }
  }

 public:
  std::unique_ptr<VolumeObserver> volume_observer_ = nullptr;

 private:
  std::shared_ptr<Raop> raop_;
  ConcurrentByteFIFO fifo_;
  aspl::Device& device_;

  std::unique_ptr<std::thread> consumer_thread_;

  void Prepare() {
    if (consumer_thread_) {
      return;
    }

    raop_->Start();
    consumer_thread_ = std::make_unique<std::thread>([&]() {
      // Real-time constraint: declare 2ms of CPU needed within a 4ms window,
      // every 8ms period. The FIFO read blocks off-CPU (doesn't count).
      // Using computation=period starves other threads (keepalive, sync)
      // and causes disconnects.
      mach_timebase_info_data_t timebase;
      mach_timebase_info(&timebase);
      uint32_t chunk_ns = 352 * 1'000'000'000U / SampleRate;  // ~8ms
      uint32_t chunk_abs = chunk_ns * timebase.denom / timebase.numer;
      thread_time_constraint_policy_data_t policy;
      policy.period = chunk_abs;              // 8ms period
      policy.computation = chunk_abs / 4;     // 2ms CPU budget
      policy.constraint = chunk_abs / 2;      // 4ms deadline
      policy.preemptible = 1;
      thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
                        reinterpret_cast<thread_policy_t>(&policy),
                        THREAD_TIME_CONSTRAINT_POLICY_COUNT);

      while (true) {
        RtpAudioPacketChunk chunk, encoded;
        size_t read_cnt = fifo_.Read(chunk.data_, sizeof(chunk.data_));
        chunk.len_ = read_cnt;
        if (read_cnt == 0) {
          continue;
        }

        PCMCodec::Encode(chunk, encoded);
        encoded.len_ = chunk.len_;

        raop_->PrepareChunk(encoded);
        raop_->AcceptFrame();
        raop_->SendPreparedChunk();
      }
    });
    consumer_thread_->detach();

    (new std::thread([=]() {
      auto device_uid = device_.GetDeviceUID();

      AudioObjectID output_device_id = 0;
      while (true) {
        OSStatus status =
            VolumeObserver::FindAudioDeviceByUID(device_uid, output_device_id);

        if (status == noErr) {
          break;
        }
        if (status == kAudioHardwareUnknownPropertyError) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          continue;
        }
        return;
      }

      raop_->SetVolume(50);
      volume_observer_ = std::make_unique<VolumeObserver>(
          output_device_id, [=](Float32 volume) {
            uint8_t volume_percent = volume * 100;
            raop_->SetVolume(volume_percent);
          });
    }))->detach();
  }
};

class DriverHelper {
 protected:
  struct DeviceInfo {
    std::shared_ptr<aspl::Device> device_;
    BonjourBrowse::ServiceInfo service_info_;
  };

 public:
  DriverHelper()
      : context_(std::make_shared<aspl::Context>()),
        plugin_(std::make_shared<aspl::Plugin>(context_)),
        driver_(std::make_shared<aspl::Driver>(context_, plugin_)),
        devices_mapping_() {
    static const std::string kServiceType = "_raop._tcp";
    using namespace std::placeholders;
    ABDebugLog("DriverHelper startBrowse %s", kServiceType.c_str());
    browse_.startBrowse(
        kServiceType,
        std::bind(&DriverHelper::HandleBrowseCallback, this, _1, _2));
  }

  std::shared_ptr<aspl::Driver> GetDriver() { return driver_; }

 private:
  void HandleBrowseCallback(BonjourBrowse::EventType event_type,
                            const BonjourBrowse::ServiceInfo& service_info) {
    using namespace std::chrono;

    std::lock_guard<std::mutex> guard(device_mapping_mutex_);

    if (event_type == BonjourBrowse::EventType::kServiceOnline) {
      ABDebugLog("service online %s %s %s %lu", service_info.fullname.c_str(),
                 service_info.name.c_str(), service_info.ip.c_str(),
                 service_info.port);
      // Cancel any pending removal — the service came back before the
      // grace period expired. This is the common case for WiFi flaps.
      auto pending = pending_removals_.find(service_info.fullname);
      if (pending != pending_removals_.end()) {
        pending_removals_.erase(pending);
        airbeam_log("FLAP ABSORBED: %s came back online", service_info.fullname.c_str());
      }
      auto it = devices_mapping_.find(service_info.fullname);
      if (it != devices_mapping_.end()) {
        if (it->second.service_info_ == service_info) {
          ABDebugLog("device already exists %s", service_info.fullname.c_str());
          return;
        }
        ABDebugLog("device exists service changed %s",
                   service_info.fullname.c_str());
        RemoveDevice(it->second.service_info_);
      }

      AddDevice(service_info);
      ABDebugLog("device %s added", service_info.fullname.c_str());
      return;
    }

    if (event_type == BonjourBrowse::EventType::kServiceOffline) {
      ABDebugLog("kServiceOffline %s", service_info.fullname.c_str());
      // Don't remove immediately — Bonjour flaps on WiFi when mDNS packets
      // are lost. Wait 10 seconds; if the service doesn't come back online,
      // then remove it. This prevents the music app from seeing the device
      // vanish during brief network hiccups.
      auto fullname = service_info.fullname;
      pending_removals_[fullname] = std::chrono::steady_clock::now();
      (new std::thread([this, fullname, service_info]() {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        std::lock_guard<std::mutex> guard(device_mapping_mutex_);
        auto it = pending_removals_.find(fullname);
        if (it != pending_removals_.end()) {
          // Still pending after 10s — actually remove
          pending_removals_.erase(it);
          if (devices_mapping_.find(fullname) != devices_mapping_.end()) {
            RemoveDevice(service_info);
            airbeam_log("DEVICE REMOVED: %s (offline >10s)", fullname.c_str());
          }
        }
      }))->detach();
      return;
    }
  }

  void AddDevice(const BonjourBrowse::ServiceInfo& service_info) {
    aspl::DeviceParameters deviceParams;
    deviceParams.Name = service_info.name;
    deviceParams.SampleRate = SampleRate;
    deviceParams.ChannelCount = ChannelCount;
    deviceParams.EnableMixing = true;

    auto device = std::make_shared<aspl::Device>(context_, deviceParams);
    device->AddStreamWithControlsAsync(aspl::Direction::Output);
    auto raop_handler = std::make_shared<RaopHandler>(*device, service_info.ip,
                                                      service_info.port);

    device->SetControlHandler(raop_handler);
    device->SetIOHandler(raop_handler);
    plugin_->AddDevice(device);
    devices_mapping_[service_info.fullname] = {device, service_info};
  }

  void RemoveDevice(const BonjourBrowse::ServiceInfo& service_info) {
    auto it = devices_mapping_.find(service_info.fullname);
    plugin_->RemoveDevice(it->second.device_);
    devices_mapping_.erase(it);
  }

 private:
  std::shared_ptr<aspl::Context> context_;
  std::shared_ptr<aspl::Plugin> plugin_;
  std::shared_ptr<aspl::Driver> driver_;
  std::map<std::string, DeviceInfo> devices_mapping_;
  std::map<std::string, std::chrono::steady_clock::time_point> pending_removals_;
  std::mutex device_mapping_mutex_;
  BonjourBrowse browse_;
};

std::shared_ptr<aspl::Driver> CreateRaopDriver() {
  static DriverHelper helper;
  return helper.GetDriver();
}

}  // namespace

extern "C" void* AirBeamASPEntryPoint(CFAllocatorRef allocator,
                                      CFUUIDRef typeUUID) {
  if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) {
    return nullptr;
  }

  static std::shared_ptr<aspl::Driver> driver = CreateRaopDriver();

  return driver->GetReference();
}
