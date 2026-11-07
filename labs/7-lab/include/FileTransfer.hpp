#pragma once

#include "Packet.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <vector>

namespace Rdtp
{
class FileReader final
{
public:
	explicit FileReader(const std::filesystem::path& path);

	std::uint64_t Size() const noexcept;
	std::uint32_t ChunkCount() const noexcept;
	std::vector<std::uint8_t> ReadChunk(std::uint32_t sequence);

private:
	std::ifstream m_stream;
	std::uint64_t m_size = 0;
	std::uint32_t m_chunkCount = 0;
};

class FileWriter final
{
public:
	explicit FileWriter(const std::filesystem::path& path);

	void Write(std::span<const std::uint8_t> bytes);
	void Finish();

private:
	std::ofstream m_stream;
};
}
