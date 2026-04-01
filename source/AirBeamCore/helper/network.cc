// Copyright (c) 2025 ChenKS12138

#include "network.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "errcode.h"
#include "helper/logger.h"

namespace AirBeamCore {
namespace helper {

std::string NetAddr::ToString() const {
  return ip_ + ":" + std::to_string(port_);
}

TCPClient::TCPClient() = default;
TCPClient::~TCPClient() { Close(); }

ErrCode TCPClient::Connect(const std::string& ip, int port) {
  if (sockfd_ != -1) Close();
  sockfd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (sockfd_ < 0) return kErrTcpSocketCreate;

  struct sockaddr_in remote_addr {};
  struct sockaddr_in local_addr {};

  memset(&remote_addr, 0, sizeof(remote_addr));
  remote_addr.sin_family = AF_INET;
  remote_addr.sin_port = htons(port);
  if (inet_pton(AF_INET, ip.c_str(), &remote_addr.sin_addr) <= 0)
    return kErrTcpAddrParse;
  // Disable Nagle's algorithm so RTSP control messages are sent immediately
  int nodelay = 1;
  setsockopt(sockfd_, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

  if (connect(sockfd_, (sockaddr*)&remote_addr, sizeof(remote_addr)) < 0) {
    ABDebugLog("connect() failed: errno=%d (%s)", errno, strerror(errno));
    return kErrTcpConnect;
  }
  socklen_t len = sizeof(local_addr);
  getsockname(sockfd_, (sockaddr*)&local_addr, &len);

  char ipaddr[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &local_addr.sin_addr, ipaddr, sizeof(ipaddr));

  remote_addr_.ip_ = ip;
  remote_addr_.port_ = ntohs(remote_addr.sin_port);

  local_addr_.ip_ = ipaddr;
  local_addr_.port_ = ntohs(local_addr.sin_port);
  return kOk;
}

ErrCode TCPClient::Write(const std::string& data) {
  if (sockfd_ < 0) return kErrTcpSocketCreate;
  ssize_t sent = send(sockfd_, data.data(), data.size(), 0);
  return sent < 0 ? kErrTcpSend : kOk;
}

ErrCode TCPClient::Read(std::string& data) {
  if (sockfd_ < 0) return kErrTcpSocketCreate;
  char buf[4096];
  ssize_t n = recv(sockfd_, buf, sizeof(buf), 0);
  if (n <= 0) return kErrTcpRecv;
  data.assign(buf, n);
  return kOk;
}

void TCPClient::Close() {
  if (sockfd_ != -1) {
    close(sockfd_);
    sockfd_ = -1;
  }
}

UDPServer::UDPServer() = default;
UDPServer::~UDPServer() { Close(); }

ErrCode UDPServer::Bind() {
  if (sockfd_ != -1) Close();
  sockfd_ = socket(AF_INET, SOCK_DGRAM, 0);
  if (sockfd_ < 0) return kErrUdpSocketCreate;
  struct sockaddr_in local_addr {};
  memset(&local_addr, 0, sizeof(local_addr));
  local_addr.sin_family = AF_INET;
  local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  local_addr.sin_port = htons(0);  // random
  if (bind(sockfd_, (sockaddr*)&local_addr, sizeof(local_addr)) < 0)
    return kErrUdpBind;

  socklen_t addr_len = sizeof(local_addr);
  if (getsockname(sockfd_, (sockaddr*)&local_addr, &addr_len) < 0)
    return kErrGetsockName;

  // Increase send buffer to handle micro-bursts without dropping packets
  int sndbuf = 256 * 1024;  // 256KB
  setsockopt(sockfd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

  // Mark packets as real-time audio (DSCP Expedited Forwarding = 0xB8)
  // so routers/switches prioritize them over bulk traffic
  int tos = 0xB8;
  setsockopt(sockfd_, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));

  char ip[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &local_addr.sin_addr, ip, sizeof(ip));

  local_addr_.ip_ = ip;
  local_addr_.port_ = ntohs(local_addr.sin_port);

  return kOk;
}

ErrCode UDPServer::Write(const NetAddr& remote_addr, const std::string& data) {
  sockaddr_in dest{};
  if (!ResolveAddr(remote_addr, dest)) return kErrUdpAddrParse;
  return WriteTo(dest, data.data(), data.size());
}

ErrCode UDPServer::WriteTo(const sockaddr_in& dest, const void* data,
                           size_t len) {
  if (sockfd_ < 0) return kErrUdpSocketCreate;
  ssize_t sent =
      sendto(sockfd_, data, len, 0, (const sockaddr*)&dest, sizeof(dest));
  return sent < 0 ? kErrUdpSend : kOk;
}

bool UDPServer::ResolveAddr(const NetAddr& addr, sockaddr_in& out) {
  memset(&out, 0, sizeof(out));
  out.sin_family = AF_INET;
  out.sin_port = htons(addr.port_);
  return inet_pton(AF_INET, addr.ip_.c_str(), &out.sin_addr) > 0;
}

ErrCode UDPServer::Read(NetAddr& remote_addr, std::string& data) {
  if (sockfd_ < 0) return kErrUdpSocketCreate;
  char buf[4096];
  sockaddr_in src{};
  socklen_t len = sizeof(src);
  ssize_t n = recvfrom(sockfd_, buf, sizeof(buf), 0, (sockaddr*)&src, &len);
  if (n <= 0) return kErrUdpRecv;
  data.assign(buf, n);
  char ipbuf[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &src.sin_addr, ipbuf, sizeof(ipbuf));
  remote_addr.ip_ = ipbuf;
  remote_addr.port_ = ntohs(src.sin_port);
  return kOk;
}

void UDPServer::Close() {
  if (sockfd_ != -1) {
    close(sockfd_);
    sockfd_ = -1;
  }
}

}  // namespace helper
}  // namespace AirBeamCore
