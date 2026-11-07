#include "UdpSocket.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <memory>
#include <netdb.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <arpa/inet.h>

namespace Rdtp
{
namespace
{
void AssertIsSocketCreated(int descriptor)
{
	if (descriptor < 0)
	{
		throw std::runtime_error("Не удалось создать UDP-сокет, код " + std::to_string(errno));
	}
}

void AssertIsAddressResolved(int status)
{
	if (status != 0)
	{
		throw std::runtime_error("Не удалось разрешить адрес получателя, код " +
			std::to_string(status));
	}
}

void AssertIsAddressFound(const addrinfo* address)
{
	if (address == nullptr)
	{
		throw std::runtime_error("Не найден IPv4-адрес получателя");
	}
}

void AssertIsBound(int result)
{
	if (result < 0)
	{
		throw std::runtime_error("Не удалось привязать UDP-сокет к порту, код " +
			std::to_string(errno));
	}
}

void AssertIsDatagramSent(ssize_t sent, std::size_t expected)
{
	if (sent < 0 || static_cast<std::size_t>(sent) != expected)
	{
		throw std::runtime_error("Ошибка отправки UDP-пакета, код " +
			std::to_string(errno));
	}
}

void AssertIsPollSuccessful(int result)
{
	if (result < 0)
	{
		throw std::runtime_error("Ошибка ожидания UDP-пакета, код " +
			std::to_string(errno));
	}
}

void AssertIsSocketReadable(bool readable)
{
	if (!readable)
	{
		throw std::runtime_error("UDP-сокет недоступен для чтения");
	}
}

void AssertIsDatagramReceived(ssize_t received)
{
	if (received < 0)
	{
		throw std::runtime_error("Ошибка получения UDP-пакета, код " +
			std::to_string(errno));
	}
}
}

SocketAddress ResolveAddress(const std::string& host, std::uint16_t port)
{
	addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	addrinfo* rawResults = nullptr;
	int status = ::getaddrinfo(host.c_str(), std::to_string(port).c_str(),
		&hints, &rawResults);
	AssertIsAddressResolved(status);
	std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> results(rawResults, ::freeaddrinfo);
	AssertIsAddressFound(results.get());
	SocketAddress address;
	std::memcpy(&address.storage, results->ai_addr, results->ai_addrlen);
	address.length = static_cast<socklen_t>(results->ai_addrlen);
	return address;
}

bool IsSameAddress(const SocketAddress& left, const SocketAddress& right) noexcept
{
	if (left.storage.ss_family != AF_INET || right.storage.ss_family != AF_INET)
	{
		return false;
	}
	const auto* first = reinterpret_cast<const sockaddr_in*>(&left.storage);
	const auto* second = reinterpret_cast<const sockaddr_in*>(&right.storage);
	return first->sin_port == second->sin_port &&
		first->sin_addr.s_addr == second->sin_addr.s_addr;
}

UdpSocket::UdpSocket()
	: m_fd(::socket(AF_INET, SOCK_DGRAM, 0))
{
	AssertIsSocketCreated(m_fd);
}

UdpSocket::~UdpSocket()
{
	if (m_fd >= 0)
	{
		::close(m_fd);
	}
}

void UdpSocket::Bind(std::uint16_t port)
{
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(port);
	int result = ::bind(m_fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
	AssertIsBound(result);
}

void UdpSocket::SendTo(std::span<const std::uint8_t> bytes,
	const SocketAddress& receiver) const
{
	ssize_t sent = ::sendto(m_fd, bytes.data(), bytes.size(), 0,
		reinterpret_cast<const sockaddr*>(&receiver.storage), receiver.length);
	AssertIsDatagramSent(sent, bytes.size());
}

std::optional<ReceivedDatagram> UdpSocket::Receive(int timeoutMs) const
{
	pollfd descriptor{};
	descriptor.fd = m_fd;
	descriptor.events = POLLIN;
	int result = ::poll(&descriptor, 1, timeoutMs);
	if (result < 0 && errno == EINTR)
	{
		return std::nullopt;
	}
	AssertIsPollSuccessful(result);
	if (result == 0)
	{
		return std::nullopt;
	}
	AssertIsSocketReadable((descriptor.revents & POLLIN) != 0);
	std::array<std::uint8_t, 2048> buffer{};
	ReceivedDatagram datagram;
	datagram.sender.length = sizeof(datagram.sender.storage);
	ssize_t received = ::recvfrom(m_fd, buffer.data(), buffer.size(), 0,
		reinterpret_cast<sockaddr*>(&datagram.sender.storage), &datagram.sender.length);
	if (received < 0 && errno == EINTR)
	{
		return std::nullopt;
	}
	AssertIsDatagramReceived(received);
	datagram.bytes.assign(buffer.begin(), buffer.begin() + received);
	return datagram;
}
}
