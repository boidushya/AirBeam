// Copyright (c) 2025 ChenKS12138

#include "raop.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <syslog.h>
#include <thread>

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
}  // namespace

#include "absl/strings/numbers.h"
#include "constants.h"
#include "fmt/core.h"
#include "helper/errcode.h"
#include "helper/logger.h"
#include "helper/network.h"
#include "helper/random.h"
#include "rtp.h"
#include "rtsp.h"

namespace AirBeamCore {

namespace raop {

using namespace helper;

bool Raop::Start() {
  int ret = rtsp_client_.Connect(rtsp_ip_addr_, rtsp_port_);
  if (ret != kOk) {
    ABDebugLog("rtsp_client_.Connect failed, ret=%d", ret);
    return false;
  }

  GenerateID();
  if (!Announce()) return false;
  if (!BindCtrlAndTimePort()) return false;
  if (!Setup()) return false;
  if (!Record()) return false;
  SyncStart();
  KeepAlive();
  if (!FirstSendSync()) return false;
  {
    ssrc_ =
        static_cast<uint32_t>(helper::RandomGenerator::GetInstance().GenU64());
  }
  is_started_ = true;
  return true;
}

void Raop::AcceptFrame() {
  if (!is_started_) return;

  uint64_t now_ts = NtpTime::Now().IntoTimestamp(kSampleRate44100);
  uint64_t head = status_.head_ts.load(std::memory_order_relaxed);
  uint64_t first = status_.first_ts.load(std::memory_order_relaxed);

  // Reinit head_ts on first frame to absorb the 10-20ms startup gap.
  if (head == first && now_ts > head + kPCMChunkLength) {
    status_.head_ts.store(now_ts, std::memory_order_relaxed);
    status_.first_ts.store(now_ts, std::memory_order_relaxed);
    head = now_ts;
    first = now_ts;
    airbeam_log("INIT: reinit head_ts to now (startup gap absorbed)");
  }

  uint64_t target = head + kPCMChunkLength;

  if (now_ts < target) {
    uint64_t wait_frames = target - now_ts;
    std::this_thread::sleep_for(std::chrono::nanoseconds(
        wait_frames * 1'000'000'000ULL / kSampleRate44100));
  } else {
    uint64_t late_frames = now_ts - target;
    double late_ms =
        static_cast<double>(late_frames) * 1000.0 / kSampleRate44100;
    if (late_ms > 2.0) diag_.late_count++;
    if (late_ms > diag_.max_late_ms) diag_.max_late_ms = late_ms;
  }

  diag_.frames_sent++;

  // Every ~30 seconds, log health summary
  uint64_t frames_since_start = head - first;
  if (frames_since_start > 0 &&
      frames_since_start % (kSampleRate44100 * 30) < kPCMChunkLength) {
    airbeam_log("STATS: sent=%u late=%u(>2ms) max_late=%.1fms "
               "retransmit=%u send_fail=%u crackles=%u",
               diag_.frames_sent, diag_.late_count, diag_.max_late_ms,
               diag_.retransmit_count, diag_.send_failures,
               diag_.crackle_count);
    diag_ = {};
  }
}

void Raop::PrepareChunk(const RtpAudioPacketChunk& chunk) {
  if (!is_started_) return;
  status_.seq_number += 1;
  RtpAudioPacket packet;
  packet.header.proto = 0x80;
  packet.header.type = first_pkt_ ? 0xE0 : 0x60;
  packet.header.seq = status_.seq_number;
  packet.timestamp = status_.head_ts.load(std::memory_order_relaxed);
  packet.ssrc = ssrc_;
  packet.data = chunk;
  first_pkt_ = false;
  send_buffer_.clear();
  packet.Serialize(send_buffer_);

  {
    std::lock_guard<std::mutex> lock(retransmit_mutex_);
    auto& slot = retransmit_buffer_[status_.seq_number % kRetransmitBufferSize];
    slot.seq = status_.seq_number;
    slot.data.assign(reinterpret_cast<const char*>(send_buffer_.data()),
                     send_buffer_.size());
  }
}

void Raop::SendPreparedChunk() {
  if (!is_started_) return;
  int ret = audio_server_.WriteTo(resolved_audio_addr_, send_buffer_.data(),
                                  send_buffer_.size());
  if (ret != kOk) {
    diag_.send_failures++;
  }
  status_.head_ts.fetch_add(kPCMChunkLength, std::memory_order_relaxed);
}

void Raop::SetVolume(uint8_t volume_percent) {
  status_.seq_number += 1;
  Volume volume = Volume::FromPercent(volume_percent);
  std::string body = fmt::format("volume: {}\r\n", volume.GetValue());
  std::string uri = fmt::format("rtsp://{}/{}", rtsp_ip_addr_, sid_);

  auto request =
      AirBeamCore::raop::RtspMsgBuilder<AirBeamCore::raop::RtspReqMessage>()
          .SetMethod("SET_PARAMETER")
          .SetUri(uri)
          .AddHeader("Content-Type", "text/parameters")
          .AddHeader("Content-Length", std::to_string(body.size()))
          .AddHeader("CSeq", std::to_string(status_.seq_number++))
          .AddHeader("User-Agent", "iTunes/7.6.2 (Windows; N;)")
          .AddHeader("Client-Instance", sci_)
          .AddHeader("Session", "1")
          .SetBody(body)
          .Build();
  RtspRespMessage response;
  int ret = rtsp_client_.DoRequest(request, response);
  if (ret != kOk) {
    ABDebugLog("SetVolume: rtsp_client_.DoRequest failed, ret=%d", ret);
  }
}

void Raop::GenerateID() {
  constexpr int kSidLen = 10;
  sid_ = helper::RandomGenerator::GetInstance().GenNumStr(kSidLen);
  constexpr int kSciLen = 16;
  sci_ = helper::RandomGenerator::GetInstance().GenHexStr(kSciLen);
}

bool Raop::Announce() {
  std::string uri = fmt::format("rtsp://{}/{}", rtsp_ip_addr_, sid_);

  std::vector<std::tuple<std::string, std::string>> sdp_map = {
      {"v", "0"},
      {"o", fmt::format("iTunes {} 0 IN IP4 {}", sid_,
                        rtsp_client_.GetLocalNetAddr().ip_)},
      {"s", "iTunes"},
      {"c", fmt::format("IN IP4 {}", rtsp_ip_addr_)},
      {"t", "0 0"},
      {"m", "audio 0 RTP/AVP 96"},
      {"a", "rtpmap:96 L16/44100/2"}};
  std::string sdp = JoinKVStrOrdered(sdp_map, "=", "\r\n") + "\r\n";

  auto request =
      AirBeamCore::raop::RtspMsgBuilder<AirBeamCore::raop::RtspReqMessage>()
          .SetMethod("ANNOUNCE")
          .SetUri(uri)
          .AddHeader("Content-Type", "application/sdp")
          .AddHeader("Content-Length", std::to_string(sdp.size()))
          .AddHeader("CSeq", std::to_string(status_.seq_number++))
          .AddHeader("User-Agent", "iTunes/7.6.2 (Windows; N;)")
          .AddHeader("Client-Instance", sci_)
          .SetBody(sdp)
          .Build();
  RtspRespMessage response;
  int ret = rtsp_client_.DoRequest(request, response);
  if (ret != kOk) {
    ABDebugLog("Announce: rtsp_client_.DoRequest failed, ret=%d", ret);
    return false;
  }
  return true;
}

bool Raop::BindCtrlAndTimePort() {
  int ret = 0;
  ret = ctrl_server_.Bind();
  if (ret != kOk) {
    ABDebugLog("ctrl_server_.Bind failed, ret=%d", ret);
    return false;
  }
  ret = time_server_.Bind();
  if (ret != kOk) {
    ABDebugLog("time_server_.Bind failed, ret=%d", ret);
    return false;
  }
  (new std::thread([&]() {
    helper::NetAddr remote_addr;
    uint8_t buffer[32];
    std::string data;
    while (true) {
      memset(buffer, 0, sizeof(buffer));
      int ret = time_server_.Read(remote_addr, data);
      if (ret != kOk) {
        ABDebugLog("time_server_.Read failed, ret=%d", ret);
        continue;
      }
      memcpy(buffer, data.data(), data.size());
      auto recv_pkt = RtpTimePacket::Deserialize(buffer, data.size());
      RtpTimePacket send_pkt;
      send_pkt.header.proto = recv_pkt.header.proto;
      send_pkt.header.type = 0x53 | 0x80;
      send_pkt.header.seq = recv_pkt.header.seq;
      send_pkt.dummy = 0;
      send_pkt.recv_time = NtpTime::Now();
      send_pkt.ref_time = recv_pkt.send_time;
      send_pkt.send_time = NtpTime::Now();
      memset(buffer, 0, 32);
      send_pkt.Serialize(buffer);
      data.resize(sizeof(buffer), '\0');
      memcpy(data.data(), buffer, sizeof(buffer));
      ret = time_server_.Write(remote_addr, data);
      if (ret != kOk) {
        ABDebugLog("time_server_.Write failed, ret=%d", ret);
      }
    }
  }))->detach();
  ret = audio_server_.Bind();
  if (ret != kOk) {
    ABDebugLog("audio_server_.Bind failed, ret=%d", ret);
    return false;
  }
  return true;
}

bool Raop::Setup() {
  int ret = 0;

  ABDebugLog("Raop::Setup ctrl server port =%d",
             ctrl_server_.GetLocalNetAddr().port_);

  std::string uri = fmt::format("rtsp://{}/{}", rtsp_ip_addr_, sid_);
  std::vector<std::tuple<std::string, std::string>> transport_params = {
      {"interleaved", "0-1"},
      {"mode", "record"},
      {"control_port", std::to_string(ctrl_server_.GetLocalNetAddr().port_)},
      {"timing_port", std::to_string(time_server_.GetLocalNetAddr().port_)},
  };
  std::string transport =
      "RTP/AVP/UDP;unicast;" + JoinKVStrOrdered(transport_params, "=", ";");

  auto request =
      AirBeamCore::raop::RtspMsgBuilder<AirBeamCore::raop::RtspReqMessage>()
          .SetMethod("SETUP")
          .SetUri(uri)
          .AddHeader("Transport", transport)
          .AddHeader("CSeq", std::to_string(status_.seq_number++))
          .AddHeader("User-Agent", "iTunes/7.6.2 (Windows; N;)")
          .AddHeader("Client-Instance", sci_)
          .Build();
  RtspRespMessage response;
  ret = rtsp_client_.DoRequest(request, response);
  if (ret != kOk) {
    ABDebugLog("Setup: rtsp_client_.DoRequest failed, ret=%d", ret);
    return false;
  }

  auto transport_map = ParseKVStr(response.GetHeader("Transport"), "=", ";");
  if (!absl::SimpleAtoi(transport_map["server_port"],
                        &remote_audio_addr_.port_)) {
    ABDebugLog("Setup: failed to parse server_port");
    return false;
  }
  if (!absl::SimpleAtoi(transport_map["control_port"],
                        &remote_ctrl_addr_.port_)) {
    ABDebugLog("Setup: failed to parse control_port");
    return false;
  }
  if (!absl::SimpleAtoi(transport_map["timing_port"],
                        &remote_time_addr_.port_)) {
    ABDebugLog("Setup: failed to parse timing_port");
    return false;
  }

  remote_audio_addr_.ip_ = rtsp_client_.GetRemoteNetAddr().ip_;
  remote_ctrl_addr_.ip_ = rtsp_client_.GetRemoteNetAddr().ip_;
  remote_time_addr_.ip_ = rtsp_client_.GetRemoteNetAddr().ip_;

  // Pre-resolve audio destination so SendPreparedChunk avoids inet_pton
  helper::UDPServer::ResolveAddr(remote_audio_addr_, resolved_audio_addr_);
  return true;
}

bool Raop::Record() {
  uint64_t start_seq = status_.seq_number++;
  uint64_t start_ts = NtpTime::Now().IntoTimestamp(kSampleRate44100);
  std::string uri = fmt::format("rtsp://{}/{}", rtsp_ip_addr_, sid_);
  std::string range = "npt=0-";
  std::vector<std::tuple<std::string, std::string>> rtp_info_map = {
      {"seq", std::to_string(start_seq)},
      {"rtptime", std::to_string(start_ts)},
  };
  std::string rtp_info = JoinKVStrOrdered(rtp_info_map, "=", ";");
  auto request =
      AirBeamCore::raop::RtspMsgBuilder<AirBeamCore::raop::RtspReqMessage>()
          .SetMethod("RECORD")
          .SetUri(uri)
          .AddHeader("Range", range)
          .AddHeader("RTP-Info", rtp_info)
          .AddHeader("CSeq", std::to_string(start_seq))
          .AddHeader("User-Agent", "iTunes/7.6.2 (Windows; N;)")
          .AddHeader("Client-Instance", sci_)
          .AddHeader("Session", "1")
          .Build();

  RtspRespMessage response;
  int ret = rtsp_client_.DoRequest(request, response);
  if (ret != kOk) {
    ABDebugLog("Record: rtsp_client_.DoRequest failed, ret=%d", ret);
    return false;
  }
  if (!absl::SimpleAtoi(response.GetHeader("Audio-Latency"), &latency_)) {
    ABDebugLog("Record: failed to parse Audio-Latency");
    return false;
  }
  return true;
}

void Raop::RetransmitPackets(uint16_t seq_start, uint16_t count) {
  // Copy packets out while holding the lock, then send outside it.
  // This prevents the consumer thread (PrepareChunk) from being blocked
  // during potentially slow UDP sends.
  struct PendingRetransmit {
    uint16_t seq;
    std::string data;
  };
  std::vector<PendingRetransmit> to_send;

  {
    std::lock_guard<std::mutex> lock(retransmit_mutex_);
    for (uint16_t i = 0; i < count; ++i) {
      uint16_t seq = seq_start + i;
      const auto& entry =
          retransmit_buffer_[seq % kRetransmitBufferSize];
      // Only retransmit if the buffer slot still holds the requested packet.
      // If it's been overwritten (seq mismatch), skip — sending the wrong
      // packet causes the HomePod to re-request endlessly.
      if (entry.data.empty() || entry.seq != seq) continue;
      to_send.push_back({seq, entry.data});
    }
  }

  diag_.retransmit_count += to_send.size();

  // Track retransmit rate in a rolling 1-second window.
  // When rate exceeds 20 retransmits/sec, log as a likely crackle.
  // This catches both big bursts and clusters of small requests.
  {
    uint64_t now_ntp = NtpTime::Now().IntoTimestamp(kSampleRate44100);
    if (now_ntp - diag_.retransmit_window_start > kSampleRate44100) {
      // New 1-second window
      if (diag_.retransmit_window_count > 20) {
        diag_.crackle_count++;
      }
      diag_.retransmit_window_count = 0;
      diag_.retransmit_window_start = now_ntp;
    }
    diag_.retransmit_window_count += to_send.size();
  }

  for (const auto& entry : to_send) {
    std::string retransmit_pkt;
    retransmit_pkt.resize(4 + entry.data.size());
    retransmit_pkt[0] = static_cast<char>(0x80);
    retransmit_pkt[1] = static_cast<char>(0xD6);
    retransmit_pkt[2] = static_cast<char>((entry.seq >> 8) & 0xFF);
    retransmit_pkt[3] = static_cast<char>(entry.seq & 0xFF);
    memcpy(&retransmit_pkt[4], entry.data.data(), entry.data.size());

    int ret = ctrl_server_.Write(remote_ctrl_addr_, retransmit_pkt);
    if (ret != kOk) {
      ABDebugLog("retransmit failed for seq=%u, ret=%d", entry.seq, ret);
    }
  }
}

void Raop::SyncStart() {
  (new std::thread([this]() {
    NetAddr ctrl_remote_addr;
    while (true) {
      std::string buffer;
      int ret = ctrl_server_.Read(ctrl_remote_addr, buffer);
      if (ret != kOk) {
        ABDebugLog("ctrl_server_.Read failed, ret=%d", ret);
        continue;
      }
      auto recv_pkt = RtpLostPacket::Deserialize(
          reinterpret_cast<uint8_t*>(buffer.data()), buffer.size());
      if (recv_pkt.n > 0) {
        RetransmitPackets(recv_pkt.seq_number, recv_pkt.n);
      }
    }
  }))->detach();
  (new std::thread([this]() {
    while (true) {
      auto rsp = RtpSyncPacket::Build(
          status_.head_ts.load(std::memory_order_relaxed), kSampleRate44100,
          latency_, false);
      uint8_t buffer[sizeof(RtpSyncPacket)];
      rsp.Serialize(buffer);
      std::string data;
      data.resize(sizeof(RtpSyncPacket), '\0');
      memcpy(data.data(), buffer, sizeof(RtpSyncPacket));
      int ret = ctrl_server_.Write(remote_ctrl_addr_, data);
      if (ret != kOk) {
        airbeam_log("SYNC FAILED: ret=%d", ret);
      }
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }))->detach();
}

void Raop::KeepAlive() {
  (new std::thread([this]() {
    while (true) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      auto request =
          AirBeamCore::raop::RtspMsgBuilder<AirBeamCore::raop::RtspReqMessage>()
              .SetMethod("OPTIONS")
              .SetUri("*")
              .AddHeader("CSeq", std::to_string(status_.seq_number++))
              .AddHeader("User-Agent", "iTunes/7.6.2 (Windows; N;)")
              .AddHeader("Client-Instance", sci_)
              .AddHeader("Session", "1")
              .Build();
      RtspRespMessage response;
      int ret = rtsp_client_.DoRequest(request, response);
      if (ret != kOk) {
        airbeam_log("KEEPALIVE FAILED: ret=%d", ret);
      }
    }
  }))->detach();
}

bool Raop::FirstSendSync() {
  // Initialize Mach absolute time conversion factors
  mach_timebase_info_data_t timebase;
  mach_timebase_info(&timebase);
  mach_to_ns_ = static_cast<double>(timebase.numer) / timebase.denom;
  ns_to_mach_ = static_cast<double>(timebase.denom) / timebase.numer;

  uint64_t now_ts = NtpTime::Now().IntoTimestamp(kSampleRate44100);
  status_.head_ts.store(now_ts, std::memory_order_relaxed);
  status_.first_ts.store(now_ts, std::memory_order_relaxed);
  start_mach_time_ = mach_absolute_time();
  auto pkt = RtpSyncPacket::Build(now_ts, kSampleRate44100, latency_, true);
  uint8_t buffer[sizeof(RtpSyncPacket)];
  pkt.Serialize(buffer);
  std::string data;
  data.resize(sizeof(RtpSyncPacket), '\0');
  memcpy(data.data(), buffer, sizeof(RtpSyncPacket));
  int ret = ctrl_server_.Write(remote_ctrl_addr_, data);
  ABDebugLog("ctrl_server_.Write,len=%lu", data.size());
  if (ret != kOk) {
    ABDebugLog("FirstSendSync: ctrl_server_.Write failed, ret=%d", ret);
    return false;
  }
  return true;
}
}  // namespace raop

}  // namespace AirBeamCore
