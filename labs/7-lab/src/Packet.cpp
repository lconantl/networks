#include "Packet.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace Rdtp
{
namespace
{
constexpr std::uint32_t PacketMagic = 0x52445450;
constexpr std::uint8_t PacketVersion = 1;
constexpr std::size_t ChecksumOffset = 20;

void AssertIsPayloadSizeValid(std::size_t size)
{
	if (size > PacketPayloadSize)
	{
		throw std::runtime_error("Размер полезной нагрузки пакета превышает лимит");
	}
}

void WriteUint16(std::span<std::uint8_t> bytes, std::size_t offset, std::uint16_t value)
{
	bytes[offset] = static_cast<std::uint8_t>(value >> 8);
	bytes[offset + 1] = static_cast<std::uint8_t>(value);
}

void WriteUint32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value)
{
	for (std::size_t index = 0; index < 4; ++index)
	{
		bytes[offset + index] = static_cast<std::uint8_t>(value >> (24 - index * 8));
	}
}

std::uint16_t ReadUint16(std::span<const std::uint8_t> bytes, std::size_t offset)
{
	return static_cast<std::uint16_t>((bytes[offset] << 8) | bytes[offset + 1]);
}

std::uint32_t ReadUint32(std::span<const std::uint8_t> bytes, std::size_t offset)
{
	std::uint32_t result = 0;
	for (std::size_t index = 0; index < 4; ++index)
	{
		result = (result << 8) | bytes[offset + index];
	}
	return result;
}

std::uint32_t ComputeChecksum(std::span<const std::uint8_t> bytes)
{
	std::uint32_t checksum = 0xFFFFFFFF;
	for (std::size_t index = 0; index < bytes.size(); ++index)
	{
		std::uint8_t value = bytes[index];
		if (index >= ChecksumOffset && index < ChecksumOffset + 4)
		{
			value = 0;
		}
		checksum ^= value;
		for (int bit = 0; bit < 8; ++bit)
		{
			checksum = (checksum >> 1) ^
				((checksum & 1U) != 0 ? 0xEDB88320U : 0U);
		}
	}
	return ~checksum;
}

bool IsKnownType(std::uint8_t value)
{
	return value >= static_cast<std::uint8_t>(PacketType::Start) &&
		value <= static_cast<std::uint8_t>(PacketType::FinishAck);
}

bool IsPayloadValid(PacketType type, std::size_t length)
{
	if (type == PacketType::Start)
	{
		return length == 8;
	}
	if (type == PacketType::Data)
	{
		return length > 0 && length <= PacketPayloadSize;
	}
	return length == 0;
}
}

std::vector<std::uint8_t> SerializePacket(const Packet& packet)
{
	AssertIsPayloadSizeValid(packet.payload.size());
	std::vector<std::uint8_t> bytes(PacketHeaderSize + packet.payload.size(), 0);
	WriteUint32(bytes, 0, PacketMagic);
	bytes[4] = PacketVersion;
	bytes[5] = static_cast<std::uint8_t>(packet.type);
	WriteUint32(bytes, 8, packet.sessionId);
	WriteUint32(bytes, 12, packet.sequence);
	WriteUint16(bytes, 16, static_cast<std::uint16_t>(packet.payload.size()));
	std::copy(packet.payload.begin(), packet.payload.end(), bytes.begin() + PacketHeaderSize);
	WriteUint32(bytes, ChecksumOffset, ComputeChecksum(bytes));
	return bytes;
}

std::optional<Packet> ParsePacket(std::span<const std::uint8_t> bytes)
{
	if (bytes.size() < PacketHeaderSize || bytes.size() > PacketHeaderSize + PacketPayloadSize)
	{
		return std::nullopt;
	}
	if (ReadUint32(bytes, 0) != PacketMagic || bytes[4] != PacketVersion ||
		!IsKnownType(bytes[5]) || bytes[6] != 0 || bytes[7] != 0 ||
		bytes[18] != 0 || bytes[19] != 0)
	{
		return std::nullopt;
	}
	std::size_t payloadLength = ReadUint16(bytes, 16);
	PacketType type = static_cast<PacketType>(bytes[5]);
	if (bytes.size() != PacketHeaderSize + payloadLength ||
		!IsPayloadValid(type, payloadLength) ||
		ReadUint32(bytes, ChecksumOffset) != ComputeChecksum(bytes))
	{
		return std::nullopt;
	}
	Packet packet;
	packet.type = type;
	packet.sessionId = ReadUint32(bytes, 8);
	packet.sequence = ReadUint32(bytes, 12);
	packet.payload.assign(bytes.begin() + PacketHeaderSize, bytes.end());
	return packet;
}

std::vector<std::uint8_t> EncodeFileSize(std::uint64_t fileSize)
{
	std::vector<std::uint8_t> bytes(8);
	for (std::size_t index = 0; index < 8; ++index)
	{
		bytes[index] = static_cast<std::uint8_t>(fileSize >> (56 - index * 8));
	}
	return bytes;
}

std::optional<std::uint64_t> DecodeFileSize(std::span<const std::uint8_t> payload)
{
	if (payload.size() != 8)
	{
		return std::nullopt;
	}
	std::uint64_t fileSize = 0;
	for (std::uint8_t byte : payload)
	{
		fileSize = (fileSize << 8) | byte;
	}
	return fileSize;
}

const char* PacketTypeName(PacketType type) noexcept
{
	switch (type)
	{
	case PacketType::Start:
		return "START";
	case PacketType::Data:
		return "DATA";
	case PacketType::Ack:
		return "ACK";
	case PacketType::Finish:
		return "FIN";
	case PacketType::FinishAck:
		return "FIN-ACK";
	}
	return "UNKNOWN";
}
}
