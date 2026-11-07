#include "Channel.hpp"

#include <chrono>
#include <string>
#include <utility>

namespace Rdtp
{
UnreliableChannel::UnreliableChannel(UdpSocket& socket, const ChannelOptions& options,
	const Logger& logger)
	: m_socket(socket),
	  m_logger(logger),
	  m_generator(options.seed == 0 ? std::random_device{}() : options.seed),
	  m_lossDistribution(options.lossRate),
	  m_maxDelayMs(options.maxDelayMs)
{
}

void UnreliableChannel::Send(const Packet& packet, const SocketAddress& receiver)
{
	++m_statistics.attempts;
	std::string description = std::string(PacketTypeName(packet.type)) + " №" +
		std::to_string(packet.sequence);
	m_logger.Debug("Отправка " + description);
	if (m_lossDistribution(m_generator))
	{
		++m_statistics.simulatedLosses;
		m_logger.Debug("Симулирована потеря " + description);
		return;
	}
	std::uniform_int_distribution<int> delayDistribution(0, m_maxDelayMs);
	int delayMs = delayDistribution(m_generator);
	PendingDatagram pending;
	pending.bytes = SerializePacket(packet);
	pending.receiver = receiver;
	pending.type = packet.type;
	pending.sequence = packet.sequence;
	pending.sendAt = std::chrono::steady_clock::now() +
		std::chrono::milliseconds(delayMs);
	if (delayMs > 0)
	{
		++m_statistics.delayed;
		m_logger.Debug("Задержка " + description + " на " + std::to_string(delayMs) + " мс");
	}
	m_pending.push_back(std::move(pending));
	Pump();
}

void UnreliableChannel::Pump()
{
	auto now = std::chrono::steady_clock::now();
	for (auto iterator = m_pending.begin(); iterator != m_pending.end();)
	{
		if (iterator->sendAt > now)
		{
			++iterator;
			continue;
		}
		m_socket.SendTo(iterator->bytes, iterator->receiver);
		++m_statistics.transmitted;
		iterator = m_pending.erase(iterator);
	}
}

const ChannelStatistics& UnreliableChannel::Statistics() const noexcept
{
	return m_statistics;
}
}
