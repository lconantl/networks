#include "FileTransfer.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>

namespace Rdtp
{
namespace
{
void AssertIsFileAccessible(bool accessible)
{
	if (!accessible)
	{
		throw std::runtime_error("Не удалось открыть файл для чтения");
	}
}

void AssertIsFileSizeValid(bool valid)
{
	if (!valid)
	{
		throw std::runtime_error("Слишком большой файл или не удалось получить его размер");
	}
}

void AssertIsSequenceValid(bool valid)
{
	if (!valid)
	{
		throw std::runtime_error("Некорректный номер блока файла");
	}
}

void AssertIsReadSuccessful(bool successful)
{
	if (!successful)
	{
		throw std::runtime_error("Не удалось прочитать блок исходного файла");
	}
}

void AssertIsOutputOpened(bool opened)
{
	if (!opened)
	{
		throw std::runtime_error("Не удалось открыть файл для записи");
	}
}

void AssertIsOutputWritten(bool written)
{
	if (!written)
	{
		throw std::runtime_error("Не удалось записать блок в результирующий файл");
	}
}
}

FileReader::FileReader(const std::filesystem::path& path)
	: m_stream(path, std::ios::binary)
{
	AssertIsFileAccessible(m_stream.is_open());
	std::error_code error;
	m_size = std::filesystem::file_size(path, error);
	AssertIsFileSizeValid(!error);
	std::uint64_t chunks = m_size / PacketPayloadSize +
		(m_size % PacketPayloadSize != 0 ? 1 : 0);
	AssertIsFileSizeValid(chunks <= std::numeric_limits<std::uint32_t>::max() - 1ULL);
	m_chunkCount = static_cast<std::uint32_t>(chunks);
}

std::uint64_t FileReader::Size() const noexcept
{
	return m_size;
}

std::uint32_t FileReader::ChunkCount() const noexcept
{
	return m_chunkCount;
}

std::vector<std::uint8_t> FileReader::ReadChunk(std::uint32_t sequence)
{
	AssertIsSequenceValid(sequence > 0 && sequence <= m_chunkCount);
	std::uint64_t offset = (static_cast<std::uint64_t>(sequence) - 1) * PacketPayloadSize;
	std::size_t count = static_cast<std::size_t>(
		std::min<std::uint64_t>(PacketPayloadSize, m_size - offset));
	std::vector<std::uint8_t> bytes(count);
	m_stream.clear();
	m_stream.seekg(static_cast<std::streamoff>(offset));
	AssertIsReadSuccessful(static_cast<bool>(m_stream));
	m_stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(count));
	AssertIsReadSuccessful(m_stream.gcount() == static_cast<std::streamsize>(count));
	return bytes;
}

FileWriter::FileWriter(const std::filesystem::path& path)
	: m_stream(path, std::ios::binary | std::ios::trunc)
{
	AssertIsOutputOpened(m_stream.is_open());
}

void FileWriter::Write(std::span<const std::uint8_t> bytes)
{
	m_stream.write(reinterpret_cast<const char*>(bytes.data()),
		static_cast<std::streamsize>(bytes.size()));
	AssertIsOutputWritten(static_cast<bool>(m_stream));
}

void FileWriter::Finish()
{
	m_stream.flush();
	AssertIsOutputWritten(static_cast<bool>(m_stream));
}
}
