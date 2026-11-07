#include "Receiver.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace Rdtp
{
namespace
{
using Clock = std::chrono::steady_clock;

bool IsFileSizeSupported(std::uint64_t fileSize)
{
	std::uint64_t chunks = fileSize / PacketPayloadSize +
		(fileSize % PacketPayloadSize != 0 ? 1 : 0);
	return chunks <= std::numeric_limits<std::uint32_t>::max() - 1ULL;
}

std::uint32_t CalculateChunkCount(std::uint64_t fileSize)
{
	return static_cast<std::uint32_t>(fileSize / PacketPayloadSize +
		(fileSize % PacketPayloadSize != 0 ? 1 : 0));
}
}

Receiver::Receiver(ReceiverOptions options)
	: m_options(std::move(options)),
	  m_logger(m_options.channel.debug),
	  m_socket(),
	  m_channel(m_socket, m_options.channel, m_logger)
{
}

void Receiver::Run()
{
	m_socket.Bind(m_options.port);
	m_logger.Info("Получатель: ожидание на UDP-порту " +
		std::to_string(m_options.port));
	while (!m_finished)
	{
		m_channel.Pump();
		if (auto datagram = m_socket.Receive(10))
		{
			HandleDatagram(*datagram);
		}
	}
	ReportStatistics();
	Linger();
}

void Receiver::HandleDatagram(const ReceivedDatagram& datagram)
{
	auto packet = ParsePacket(datagram.bytes);
	if (!packet)
	{
		++m_invalidPackets;
		m_logger.Debug("Пакет отклонён: формат или контрольная сумма");
		return;
	}
	if (!m_writer)
	{
		if (packet->type == PacketType::Start)
		{
			HandleStart(*packet, datagram.sender);
		}
		return;
	}
	if (!IsSameAddress(datagram.sender, m_sender) || packet->sessionId != m_sessionId)
	{
		return;
	}
	switch (packet->type)
	{
	case PacketType::Start:
		HandleStart(*packet, datagram.sender);
		break;
	case PacketType::Data:
		HandleData(*packet);
		break;
	case PacketType::Finish:
		HandleFinish(*packet);
		break;
	default:
		break;
	}
}

void Receiver::HandleStart(const Packet& packet, const SocketAddress& sender)
{
	if (packet.sequence != 0 || packet.sessionId == 0)
	{
		return;
	}
	auto fileSize = DecodeFileSize(packet.payload);
	if (!fileSize || !IsFileSizeSupported(*fileSize))
	{
		m_logger.Debug("Отклонён START: недопустимый размер файла");
		return;
	}
	if (!m_writer)
	{
		m_writer = std::make_unique<FileWriter>(m_options.outputFile);
		m_sender = sender;
		m_sessionId = packet.sessionId;
		m_fileSize = *fileSize;
		m_chunkCount = CalculateChunkCount(*fileSize);
		m_started = Clock::now();
		m_logger.Info("Получен START: " + std::to_string(m_fileSize) +
			" байт, блоков " + std::to_string(m_chunkCount));
	}
	if (*fileSize == m_fileSize && packet.sessionId == m_sessionId)
	{
		SendAcknowledgement(0);
	}
}

bool Receiver::IsExpectedDataSize(const Packet& packet) const noexcept
{
	if (m_writtenBytes >= m_fileSize)
	{
		return false;
	}
	std::uint64_t expectedSize = std::min<std::uint64_t>(PacketPayloadSize,
		m_fileSize - m_writtenBytes);
	return packet.payload.size() == expectedSize;
}

void Receiver::HandleData(const Packet& packet)
{
	m_logger.Debug("Получен DATA №" + std::to_string(packet.sequence));
	if (packet.sequence == m_expectedSequence &&
		packet.sequence <= m_chunkCount && IsExpectedDataSize(packet))
	{
		m_writer->Write(packet.payload);
		m_writtenBytes += packet.payload.size();
		++m_expectedSequence;
	}
	else
	{
		++m_outOfOrder;
		m_logger.Debug("Отклонён DATA №" + std::to_string(packet.sequence) +
			", ожидается №" + std::to_string(m_expectedSequence));
	}
	SendAcknowledgement(m_expectedSequence - 1);
}

void Receiver::HandleFinish(const Packet& packet)
{
	m_logger.Debug("Получен FIN №" + std::to_string(packet.sequence));
	if (packet.sequence != m_chunkCount + 1 ||
		m_expectedSequence != m_chunkCount + 1 || m_writtenBytes != m_fileSize)
	{
		SendAcknowledgement(m_expectedSequence - 1);
		return;
	}
	m_writer->Finish();
	m_finished = true;
	m_finishedAt = Clock::now();
	SendFinishAcknowledgement();
}

void Receiver::SendAcknowledgement(std::uint32_t sequence)
{
	m_channel.Send(Packet{PacketType::Ack, m_sessionId, sequence, {}}, m_sender);
}

void Receiver::SendFinishAcknowledgement()
{
	m_channel.Send(Packet{PacketType::FinishAck, m_sessionId, m_chunkCount + 1, {}},
		m_sender);
}

void Receiver::Linger()
{
	auto deadline = Clock::now() + std::chrono::milliseconds(m_options.lingerMs);
	while (Clock::now() < deadline)
	{
		m_channel.Pump();
		auto datagram = m_socket.Receive(10);
		if (!datagram || !IsSameAddress(datagram->sender, m_sender))
		{
			continue;
		}
		auto packet = ParsePacket(datagram->bytes);
		if (!packet || packet->sessionId != m_sessionId)
		{
			continue;
		}
		if (packet->type == PacketType::Finish && packet->sequence == m_chunkCount + 1)
		{
			m_logger.Debug("Повторно получен FIN №" + std::to_string(packet->sequence));
			SendFinishAcknowledgement();
			deadline = Clock::now() + std::chrono::milliseconds(m_options.lingerMs);
		}
		else if (packet->type == PacketType::Data)
		{
			SendAcknowledgement(m_chunkCount);
		}
	}
	m_channel.Pump();
}

void Receiver::ReportStatistics() const
{
	const ChannelStatistics& statistics = m_channel.Statistics();
	double seconds = std::max(0.000001,
		std::chrono::duration<double>(m_finishedAt - m_started).count());
	double speed = static_cast<double>(m_writtenBytes) / (1024.0 * 1024.0 * seconds);
	double lossPercent = statistics.attempts == 0 ? 0.0 :
		100.0 * static_cast<double>(statistics.simulatedLosses) / statistics.attempts;
	std::ostringstream stream;
	stream << std::fixed << std::setprecision(2);
	stream << "Файл получен: " << m_writtenBytes << " байт; " << seconds << " с; "
		<< speed << " МиБ/с; потери ACK=" << lossPercent << "% ("
		<< statistics.simulatedLosses << "/" << statistics.attempts << "); "
		<< "вне порядка/повторы=" << m_outOfOrder << "; повреждённые=" << m_invalidPackets;
	m_logger.Info(stream.str());
}
}
