#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Rdtp
{
constexpr std::size_t PacketHeaderSize = 24;
constexpr std::size_t PacketPayloadSize = 1024;

enum class PacketType : std::uint8_t
{
	Start = 1,
	Data = 2,
	Ack = 3,
	Finish = 4,
	FinishAck = 5
};

struct Packet
{
	PacketType type = PacketType::Data;
	std::uint32_t sessionId = 0;
	std::uint32_t sequence = 0;
	std::vector<std::uint8_t> payload;
};

std::vector<std::uint8_t> SerializePacket(const Packet& packet);
std::optional<Packet> ParsePacket(std::span<const std::uint8_t> bytes);
std::vector<std::uint8_t> EncodeFileSize(std::uint64_t fileSize);
std::optional<std::uint64_t> DecodeFileSize(std::span<const std::uint8_t> payload);
const char* PacketTypeName(PacketType type) noexcept;
}
