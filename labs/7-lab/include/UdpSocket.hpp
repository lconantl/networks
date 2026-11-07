#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <sys/socket.h>

namespace Rdtp
{
struct SocketAddress
{
	sockaddr_storage storage{};
	socklen_t length = 0;
};

struct ReceivedDatagram
{
	std::vector<std::uint8_t> bytes;
	SocketAddress sender;
};

SocketAddress ResolveAddress(const std::string& host, std::uint16_t port);
bool IsSameAddress(const SocketAddress& left, const SocketAddress& right) noexcept;

class UdpSocket final
{
public:
	UdpSocket();
	~UdpSocket();

	UdpSocket(const UdpSocket&) = delete;
	UdpSocket& operator=(const UdpSocket&) = delete;

	void Bind(std::uint16_t port);
	void SendTo(std::span<const std::uint8_t> bytes, const SocketAddress& receiver) const;
	std::optional<ReceivedDatagram> Receive(int timeoutMs) const;

private:
	int m_fd = -1;
};
}
