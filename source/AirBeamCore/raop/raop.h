// Copyright (c) 2025 ChenKS12138

#pragma once
#include <arpa/inet.h>
#include <mach/mach_time.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <array>
#include <atomic>
#include <mutex>
#include <string>

#include "helper/network.h"
#include "helper/random.h"
#include "raop/rtp.h"
#include "raop/rtsp_client.h"

namespace AirBeamCore {
namespace raop {
struct RaopStatus {
 public:
  std::atomic<uint16_t> seq_number =
      std::atomic<uint16_t>(helper::RandomGenerator::GetInstance().GenU64());
  std::atomic<uint64_t> head_ts = 0;
  std::atomic<uint64_t> first_ts = 0;
};

class Raop {
 public:
  Raop(const std::string rtsp_ip_addr, uint32_t rtsp_port)
      : rtsp_ip_addr_(rtsp_ip_addr), rtsp_port_(rtsp_port) {}

 private:
  raop::RTSPClient rtsp_client_;

  helper::UDPServer ctrl_server_;
  helper::UDPServer time_server_;
  helper::UDPServer audio_server_;

  helper::NetAddr remote_audio_addr_;
  helper::NetAddr remote_time_addr_;
  helper::NetAddr remote_ctrl_addr_;

  std::string sid_;
  std::string sci_;

  uint64_t latency_ = 0;

  bool first_pkt_ = true;

  uint32_t ssrc_ = 0;

  RaopStatus status_;

  bool is_started_ = false;

  const std::string rtsp_ip_addr_;
  const uint32_t rtsp_port_;

  uint64_t start_mach_time_ = 0;
  double mach_to_ns_ = 1.0;
  double ns_to_mach_ = 1.0;

  std::vector<uint8_t> send_buffer_;
  std::string send_data_;

  static constexpr size_t kRetransmitBufferSize = 512;
  std::array<std::string, kRetransmitBufferSize> retransmit_buffer_;
  std::mutex retransmit_mutex_;

 public:
  bool Start();
  void AcceptFrame();
  void PrepareChunk(const RtpAudioPacketChunk& chunk);
  void SendPreparedChunk();
  void SetVolume(uint8_t volume);

 private:
  void GenerateID();
  bool Announce();
  bool BindCtrlAndTimePort();
  bool Setup();
  bool Record();
  void SyncStart();
  void KeepAlive();
  bool FirstSendSync();
  void RetransmitPackets(uint16_t seq_start, uint16_t count);
};
}  // namespace raop
}  // namespace AirBeamCore
