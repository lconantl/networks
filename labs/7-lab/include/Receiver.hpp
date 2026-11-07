#pragma once

#include "Channel.hpp"
#include "FileTransfer.hpp"
#include "Logger.hpp"
#include "Options.hpp"
#include "Packet.hpp"
#include "UdpSocket.hpp"

#include <chrono>
#include <cstdint>
#include <memory>

namespace Rdtp
{
class Receiver final
{
public:
	explicit Receiver(ReceiverOptions options);

	void Run();

private:
	void HandleDatagram(const ReceivedDatagram& datagram);
	void HandleStart(const Packet& packet, const SocketAddress& sender);
	void HandleData(const Packet& packet);
	void HandleFinish(const Packet& packet);
	void SendAcknowledgement(std::uint32_t sequence);
	void SendFinishAcknowledgement();
	void Linger();
	void ReportStatistics() const;
	bool IsExpectedDataSize(const Packet& packet) const noexcept;

	ReceiverOptions m_options;
	Logger m_logger;
	UdpSocket m_socket;
	UnreliableChannel m_channel;
	std::unique_ptr<FileWriter> m_writer;
	SocketAddress m_sender;
	std::uint32_t m_sessionId = 0;
	std::uint64_t m_fileSize = 0;
	std::uint64_t m_writtenBytes = 0;
	std::uint32_t m_chunkCount = 0;
	std::uint32_t m_expectedSequence = 1;
	std::uint64_t m_outOfOrder = 0;
	std::uint64_t m_invalidPackets = 0;
	bool m_finished = false;
	std::chrono::steady_clock::time_point m_started;
	std::chrono::steady_clock::time_point m_finishedAt;
};
}
