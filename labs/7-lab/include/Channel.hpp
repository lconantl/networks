#pragma once

#include "Logger.hpp"
#include "Options.hpp"
#include "Packet.hpp"
#include "UdpSocket.hpp"

#include <chrono>
#include <cstdint>
#include <random>
#include <vector>

namespace Rdtp
{
struct ChannelStatistics
{
	std::uint64_t attempts = 0;
	std::uint64_t simulatedLosses = 0;
	std::uint64_t transmitted = 0;
	std::uint64_t delayed = 0;
};

class UnreliableChannel final
{
public:
	UnreliableChannel(UdpSocket& socket, const ChannelOptions& options, const Logger& logger);

	void Send(const Packet& packet, const SocketAddress& receiver);
	void Pump();
	const ChannelStatistics& Statistics() const noexcept;

private:
	struct PendingDatagram
	{
		std::vector<std::uint8_t> bytes;
		SocketAddress receiver;
		std::chrono::steady_clock::time_point sendAt;
		PacketType type = PacketType::Data;
		std::uint32_t sequence = 0;
	};

	UdpSocket& m_socket;
	const Logger& m_logger;
	std::mt19937 m_generator;
	std::bernoulli_distribution m_lossDistribution;
	int m_maxDelayMs = 0;
	ChannelStatistics m_statistics;
	std::vector<PendingDatagram> m_pending;
};
}
