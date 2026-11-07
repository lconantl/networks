#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace Rdtp
{
struct ChannelOptions
{
	double lossRate = 0.0;
	int maxDelayMs = 0;
	std::uint32_t seed = 0;
	bool debug = false;
};

struct SenderOptions
{
	std::string host;
	std::uint16_t port = 0;
	std::filesystem::path inputFile;
	ChannelOptions channel;
	std::uint32_t windowSize = 32;
	int timeoutMs = 250;
	int maxRetries = 100;
};

struct ReceiverOptions
{
	std::uint16_t port = 0;
	std::filesystem::path outputFile;
	ChannelOptions channel;
	int lingerMs = 2500;
};

SenderOptions ParseSenderOptions(int argc, const char* const* argv);
ReceiverOptions ParseReceiverOptions(int argc, const char* const* argv);
}
