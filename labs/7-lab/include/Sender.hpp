#pragma once

#include "Channel.hpp"
#include "FileTransfer.hpp"
#include "Logger.hpp"
#include "Options.hpp"
#include "Packet.hpp"
#include "UdpSocket.hpp"

#include <chrono>
#include <cstdint>

namespace Rdtp
{
class Sender final
{
public:
	explicit Sender(SenderOptions options);

	void Run();

private:
	void ConfirmPacket(const Packet& packet, PacketType responseType);
	void TransferData();
	void SendWindow(std::uint32_t& nextSequence, std::uint32_t base);
	void HandleAcknowledgement(const ReceivedDatagram& datagram, std::uint32_t& base,
		std::uint32_t nextSequence, std::chrono::steady_clock::time_point& lastProgress);
	void RetransmitWindow(std::uint32_t base, std::uint32_t nextSequence);
	void ReportProgress(std::uint32_t base);
	void ReportStatistics() const;
	Packet MakeDataPacket(std::uint32_t sequence);
	bool IsValidResponse(const ReceivedDatagram& datagram, PacketType type,
		std::uint32_t sequence) const;

	SenderOptions m_options;
	Logger m_logger;
	UdpSocket m_socket;
	UnreliableChannel m_channel;
	FileReader m_file;
	SocketAddress m_receiver;
	std::uint32_t m_sessionId = 0;
	std::uint64_t m_retransmissions = 0;
	std::uint64_t m_timeouts = 0;
	std::uint64_t m_acknowledgements = 0;
	int m_lastProgressStep = -1;
	std::chrono::steady_clock::time_point m_started;
};
}
