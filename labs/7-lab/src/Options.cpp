#include "Options.hpp"

#include <charconv>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace Rdtp
{
namespace
{
struct ParsedArguments
{
	std::vector<std::string> positional;
	ChannelOptions channel;
	std::uint32_t windowSize = 32;
	int timeoutMs = 250;
	int maxRetries = 100;
	int lingerMs = 2500;
};

void AssertIsIntegerValid(bool valid)
{
	if (!valid)
	{
		throw std::runtime_error("Некорректное целочисленное значение аргумента");
	}
}

void AssertIsProbabilityValid(bool valid)
{
	if (!valid)
	{
		throw std::runtime_error("Вероятность потери должна быть от 0 до 1");
	}
}

void AssertIsPortValid(bool valid)
{
	if (!valid)
	{
		throw std::runtime_error("Порт должен быть целым числом от 1 до 65535");
	}
}

void AssertIsOptionKnown(bool known)
{
	if (!known)
	{
		throw std::runtime_error("Неизвестный параметр командной строки");
	}
}

void AssertIsValuePresent(bool present)
{
	if (!present)
	{
		throw std::runtime_error("Не указано значение параметра командной строки");
	}
}

void AssertIsArgumentCountValid(bool valid, bool sender)
{
	if (!valid)
	{
		throw std::runtime_error(sender ?
			"Использование: rdt_sender <хост> <порт> <файл> [-d] [--loss N] "
			"[--delay-ms N] [--window N] [--timeout-ms N] [--retries N] [--seed N]" :
			"Использование: rdt_receiver <порт> <файл> [-d] [--loss N] "
			"[--delay-ms N] [--linger-ms N] [--seed N]");
	}
}

std::uint32_t ParseUnsigned(std::string_view value)
{
	std::uint32_t result = 0;
	auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
	AssertIsIntegerValid(!value.empty() && error == std::errc{} &&
		end == value.data() + value.size());
	return result;
}

int ParseBoundedInt(std::string_view value, int minimum, int maximum)
{
	std::uint32_t parsed = ParseUnsigned(value);
	AssertIsIntegerValid(parsed >= static_cast<std::uint32_t>(minimum) &&
		parsed <= static_cast<std::uint32_t>(maximum));
	return static_cast<int>(parsed);
}

double ParseProbability(const std::string& value)
{
	char* end = nullptr;
	errno = 0;
	double result = std::strtod(value.c_str(), &end);
	AssertIsProbabilityValid(errno == 0 && end == value.c_str() + value.size() &&
		result >= 0.0 && result <= 1.0);
	return result;
}

std::string ReadValue(const std::string& argument, int& index, int argc,
	const char* const* argv, std::string& name)
{
	std::size_t separator = argument.find('=');
	name = argument.substr(0, separator);
	if (separator != std::string::npos)
	{
		std::string value = argument.substr(separator + 1);
		AssertIsValuePresent(!value.empty());
		return value;
	}
	AssertIsValuePresent(index + 1 < argc);
	return argv[++index];
}

void ApplyOption(ParsedArguments& parsed, const std::string& name,
	const std::string& value, bool sender)
{
	if (name == "--loss")
	{
		parsed.channel.lossRate = ParseProbability(value);
	}
	else if (name == "--delay-ms")
	{
		parsed.channel.maxDelayMs = ParseBoundedInt(value, 0, 60000);
	}
	else if (name == "--seed")
	{
		parsed.channel.seed = ParseUnsigned(value);
	}
	else if (name == "--window" && sender)
	{
		parsed.windowSize = static_cast<std::uint32_t>(ParseBoundedInt(value, 1, 1024));
	}
	else if (name == "--timeout-ms" && sender)
	{
		parsed.timeoutMs = ParseBoundedInt(value, 10, 60000);
	}
	else if (name == "--retries" && sender)
	{
		parsed.maxRetries = ParseBoundedInt(value, 1, 100000);
	}
	else if (name == "--linger-ms" && !sender)
	{
		parsed.lingerMs = ParseBoundedInt(value, 100, 120000);
	}
	else
	{
		AssertIsOptionKnown(false);
	}
}

ParsedArguments ParseArguments(int argc, const char* const* argv, bool sender)
{
	ParsedArguments parsed;
	for (int index = 1; index < argc; ++index)
	{
		std::string argument = argv[index];
		if (argument == "-d")
		{
			parsed.channel.debug = true;
		}
		else if (argument.starts_with("--"))
		{
			std::string name;
			std::string value = ReadValue(argument, index, argc, argv, name);
			ApplyOption(parsed, name, value, sender);
		}
		else
		{
			parsed.positional.push_back(argument);
		}
	}
	AssertIsArgumentCountValid(parsed.positional.size() == (sender ? 3U : 2U), sender);
	return parsed;
}

std::uint16_t ParsePort(const std::string& value)
{
	std::uint32_t port = ParseUnsigned(value);
	AssertIsPortValid(port >= 1 && port <= 65535);
	return static_cast<std::uint16_t>(port);
}
}

SenderOptions ParseSenderOptions(int argc, const char* const* argv)
{
	ParsedArguments parsed = ParseArguments(argc, argv, true);
	SenderOptions options;
	options.host = parsed.positional[0];
	options.port = ParsePort(parsed.positional[1]);
	options.inputFile = parsed.positional[2];
	options.channel = parsed.channel;
	options.windowSize = parsed.windowSize;
	options.timeoutMs = parsed.timeoutMs;
	options.maxRetries = parsed.maxRetries;
	return options;
}

ReceiverOptions ParseReceiverOptions(int argc, const char* const* argv)
{
	ParsedArguments parsed = ParseArguments(argc, argv, false);
	ReceiverOptions options;
	options.port = ParsePort(parsed.positional[0]);
	options.outputFile = parsed.positional[1];
	options.channel = parsed.channel;
	options.lingerMs = parsed.lingerMs;
	return options;
}
}
