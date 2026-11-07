#include "Sender.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace Rdtp
{
namespace
{
using Clock = std::chrono::steady_clock;

void AssertIsHandshakeConfirmed(bool confirmed)
{
	if (!confirmed)
	{
		throw std::runtime_error("Не удалось получить подтверждение управляющего пакета");
	}
}

void AssertIsTransferHealthy(bool healthy)
{
	if (!healthy)
	{
		throw std::runtime_error("Превышено число повторных передач: получатель недоступен");
	}
}

std::uint32_t GenerateSessionId()
{
	std::uint32_t sessionId = std::random_device{}();
	return sessionId == 0 ? 1 : sessionId;
}

std::string FormatStatistics(const ChannelStatistics& statistics, std::uint64_t bytes,
	std::chrono::steady_clock::duration duration, std::uint64_t retries,
	std::uint64_t timeouts, std::uint64_t acknowledgements)
{
	double seconds = std::max(0.000001, std::chrono::duration<double>(duration).count());
	double mibPerSecond = static_cast<double>(bytes) / (1024.0 * 1024.0 * seconds);
	double lossPercent = statistics.attempts == 0 ? 0.0 :
		100.0 * static_cast<double>(statistics.simulatedLosses) / statistics.attempts;
	std::ostringstream stream;
	stream << std::fixed << std::setprecision(2);
	stream << "Итог: " << bytes << " байт; " << seconds << " с; "
		<< mibPerSecond << " МиБ/с; потери=" << lossPercent << "% ("
		<< statistics.simulatedLosses << "/" << statistics.attempts << "); "
		<< "повторы=" << retries << "; тайм-ауты=" << timeouts
		<< "; ACK=" << acknowledgements;
	return stream.str();
}
}

Sender::Sender(SenderOptions options)
	: m_options(std::move(options)),
	  m_logger(m_options.channel.debug),
	  m_socket(),
	  m_channel(m_socket, m_options.channel, m_logger),
	  m_file(m_options.inputFile),
	  m_receiver(ResolveAddress(m_options.host, m_options.port)),
	  m_sessionId(GenerateSessionId())
{
}

void Sender::Run()
{
	m_started = Clock::now();
	m_logger.Info("Отправитель: " + m_options.inputFile.string() + ", размер " +
		std::to_string(m_file.Size()) + " байт, пакетов " +
		std::to_string(m_file.ChunkCount()));
	Packet start{PacketType::Start, m_sessionId, 0, EncodeFileSize(m_file.Size())};
	ConfirmPacket(start, PacketType::Ack);
	TransferData();
	Packet finish{PacketType::Finish, m_sessionId, m_file.ChunkCount() + 1, {}};
	ConfirmPacket(finish, PacketType::FinishAck);
	ReportStatistics();
}

void Sender::ConfirmPacket(const Packet& packet, PacketType responseType)
{
	bool confirmed = false;
	for (int attempt = 0; attempt < m_options.maxRetries && !confirmed; ++attempt)
	{
		m_channel.Send(packet, m_receiver);
		auto sentAt = Clock::now();
		while (Clock::now() - sentAt < std::chrono::milliseconds(m_options.timeoutMs))
		{
			m_channel.Pump();
			auto received = m_socket.Receive(10);
			if (received && IsValidResponse(*received, responseType, packet.sequence))
			{
				confirmed = true;
				m_logger.Debug("Получен " + std::string(PacketTypeName(responseType)) +
					" №" + std::to_string(packet.sequence));
				break;
			}
		}
		if (!confirmed)
		{
			++m_timeouts;
			m_logger.Debug("Тайм-аут на пакете №" + std::to_string(packet.sequence) +
				" (" + PacketTypeName(packet.type) + ")");
		}
	}
	AssertIsHandshakeConfirmed(confirmed);
}

bool Sender::IsValidResponse(const ReceivedDatagram& datagram, PacketType type,
	std::uint32_t sequence) const
{
	if (!IsSameAddress(datagram.sender, m_receiver))
	{
		return false;
	}
	auto packet = ParsePacket(datagram.bytes);
	return packet && packet->sessionId == m_sessionId && packet->type == type &&
		packet->sequence == sequence;
}

Packet Sender::MakeDataPacket(std::uint32_t sequence)
{
	return Packet{PacketType::Data, m_sessionId, sequence, m_file.ReadChunk(sequence)};
}

void Sender::SendWindow(std::uint32_t& nextSequence, std::uint32_t base)
{
	while (nextSequence <= m_file.ChunkCount() &&
		static_cast<std::uint64_t>(nextSequence) <
		static_cast<std::uint64_t>(base) + m_options.windowSize)
	{
		m_channel.Send(MakeDataPacket(nextSequence), m_receiver);
		++nextSequence;
	}
}

void Sender::HandleAcknowledgement(const ReceivedDatagram& datagram, std::uint32_t& base,
	std::uint32_t nextSequence, Clock::time_point& lastProgress)
{
	if (!IsSameAddress(datagram.sender, m_receiver))
	{
		return;
	}
	auto packet = ParsePacket(datagram.bytes);
	if (!packet || packet->sessionId != m_sessionId || packet->type != PacketType::Ack)
	{
		return;
	}
	++m_acknowledgements;
	m_logger.Debug("Получен ACK №" + std::to_string(packet->sequence));
	if (packet->sequence < base || packet->sequence >= nextSequence)
	{
		return;
	}
	base = packet->sequence + 1;
	lastProgress = Clock::now();
	ReportProgress(base);
}

void Sender::RetransmitWindow(std::uint32_t base, std::uint32_t nextSequence)
{
	for (std::uint32_t sequence = base; sequence < nextSequence; ++sequence)
	{
		m_logger.Debug("Повторная передача DATA №" + std::to_string(sequence));
		m_channel.Send(MakeDataPacket(sequence), m_receiver);
		++m_retransmissions;
	}
}

void Sender::TransferData()
{
	std::uint32_t base = 1;
	std::uint32_t nextSequence = 1;
	int consecutiveTimeouts = 0;
	auto lastProgress = Clock::now();
	if (m_file.ChunkCount() == 0)
	{
		ReportProgress(1);
	}
	while (base <= m_file.ChunkCount())
	{
		SendWindow(nextSequence, base);
		m_channel.Pump();
		if (auto received = m_socket.Receive(10))
		{
			std::uint32_t previousBase = base;
			HandleAcknowledgement(*received, base, nextSequence, lastProgress);
			if (base != previousBase)
			{
				consecutiveTimeouts = 0;
			}
		}
		if (base < nextSequence && Clock::now() - lastProgress >=
			std::chrono::milliseconds(m_options.timeoutMs))
		{
			++consecutiveTimeouts;
			++m_timeouts;
			AssertIsTransferHealthy(consecutiveTimeouts <= m_options.maxRetries);
			m_logger.Debug("Тайм-аут на пакете №" + std::to_string(base));
			RetransmitWindow(base, nextSequence);
			lastProgress = Clock::now();
		}
	}
}

void Sender::ReportProgress(std::uint32_t base)
{
	std::uint64_t acknowledgedBytes = std::min<std::uint64_t>(m_file.Size(),
		(static_cast<std::uint64_t>(base) - 1) * PacketPayloadSize);
	int percent = m_file.Size() == 0 ? 100 :
		static_cast<int>(100 * acknowledgedBytes / m_file.Size());
	int step = percent / 10;
	if (step != m_lastProgressStep || percent == 100)
	{
		m_lastProgressStep = step;
		m_logger.Info("Прогресс: " + std::to_string(percent) + "% (" +
			std::to_string(acknowledgedBytes) + "/" +
			std::to_string(m_file.Size()) + " байт)");
	}
}

void Sender::ReportStatistics() const
{
	m_logger.Info(FormatStatistics(m_channel.Statistics(), m_file.Size(),
		Clock::now() - m_started, m_retransmissions, m_timeouts, m_acknowledgements));
}
}
