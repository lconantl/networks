#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace
{
constexpr std::uint16_t DEFAULT_PORT = 12000;
constexpr int PING_COUNT = 10;
constexpr int TIMEOUT_SECONDS = 1;
constexpr std::size_t MAX_MESSAGE_BYTES = 1024;

class SystemError final : public std::runtime_error
{
public:
	SystemError(const std::string_view action, const int code)
		: std::runtime_error(std::format("{}: {}", action, std::strerror(code)))
	{
	}
};

void AssertIsSucceeded(const int result, const std::string_view action)
{
	if (result < 0)
	{
		throw SystemError(action, errno);
	}
}

class FileDescriptor final
{
public:
	explicit FileDescriptor(const int descriptor = -1) noexcept
		: m_descriptor(descriptor)
	{
	}

	~FileDescriptor() noexcept
	{
		Reset();
	}

	FileDescriptor(const FileDescriptor&) = delete;
	FileDescriptor& operator=(const FileDescriptor&) = delete;

	FileDescriptor(FileDescriptor&& other) noexcept
		: m_descriptor(std::exchange(other.m_descriptor, -1))
	{
	}

	FileDescriptor& operator=(FileDescriptor&& other) noexcept
	{
		if (this != &other)
		{
			Reset();
			m_descriptor = std::exchange(other.m_descriptor, -1);
		}
		return *this;
	}

	[[nodiscard]] int Get() const noexcept
	{
		return m_descriptor;
	}

	void Reset() noexcept
	{
		if (m_descriptor >= 0)
		{
			close(m_descriptor);
			m_descriptor = -1;
		}
	}

private:
	int m_descriptor;
};

struct ClientArguments
{
	std::string serverIp;
	std::uint16_t port = DEFAULT_PORT;
};

ClientArguments ParseArguments(const int argc, char* const argv[])
{
	if (argc < 2)
	{
		throw std::invalid_argument(std::format("Использование: {} <server_ip> [port]", argv[0]));
	}

	ClientArguments arguments;
	arguments.serverIp = argv[1];

	if (argc > 2)
	{
		int parsedPort = 0;
		const std::string_view portArgument(argv[2]);
		const auto parseResult = std::from_chars(portArgument.data(), portArgument.data() + portArgument.size(), parsedPort);
		if (parseResult.ec == std::errc())
		{
			arguments.port = static_cast<std::uint16_t>(parsedPort);
		}
	}

	return arguments;
}

sockaddr_in ResolveServerAddress(const ClientArguments& arguments)
{
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(arguments.port);

	if (inet_pton(AF_INET, arguments.serverIp.c_str(), &address.sin_addr) != 1)
	{
		throw std::runtime_error(std::format("Некорректный адрес сервера: {}", arguments.serverIp));
	}
	return address;
}

FileDescriptor CreateClientSocket()
{
	FileDescriptor socketDescriptor(socket(AF_INET, SOCK_DGRAM, 0));
	AssertIsSucceeded(socketDescriptor.Get(), "Не удалось создать UDP-сокет");
	return socketDescriptor;
}

void ConfigureReceiveTimeout(const FileDescriptor& socketDescriptor)
{
	timeval timeout{};
	timeout.tv_sec = TIMEOUT_SECONDS;
	timeout.tv_usec = 0;
	AssertIsSucceeded(
		setsockopt(socketDescriptor.Get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)),
		"Не удалось установить тайм-аут приёма");
}

timeval CurrentTime() noexcept
{
	timeval time{};
	gettimeofday(&time, nullptr);
	return time;
}

long long MillisecondsSinceEpoch(const timeval& time) noexcept
{
	return static_cast<long long>(time.tv_sec) * 1000 + time.tv_usec / 1000;
}

double ElapsedSeconds(const timeval& start, const timeval& end) noexcept
{
	return (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1'000'000.0;
}

std::string BuildPingMessage(const int sequence, const timeval& sendTime)
{
	return std::format("Ping {} {}", sequence, MillisecondsSinceEpoch(sendTime));
}

bool SendPing(const FileDescriptor& socketDescriptor, const std::string& message, const sockaddr_in& serverAddress) noexcept
{
	const ssize_t sent = sendto(socketDescriptor.Get(), message.data(), message.size(), 0,
	                             reinterpret_cast<const sockaddr*>(&serverAddress), sizeof(serverAddress));
	return sent >= 0;
}

std::optional<std::string> ReceiveReply(const FileDescriptor& socketDescriptor) noexcept
{
	char buffer[MAX_MESSAGE_BYTES];
	sockaddr_in replyAddress{};
	socklen_t replyAddressLength = sizeof(replyAddress);

	const ssize_t received = recvfrom(socketDescriptor.Get(), buffer, sizeof(buffer) - 1, 0,
	                                   reinterpret_cast<sockaddr*>(&replyAddress), &replyAddressLength);
	if (received < 0)
	{
		return std::nullopt;
	}

	buffer[received] = '\0';
	return std::string(buffer);
}

void PrintReply(const int sequence, const long long sendTimestampMs, const double rtt)
{
	std::cout << std::format("Ответ от сервера: PING {} {}, RTT = {:.3f} сек", sequence, sendTimestampMs, rtt)
	          << std::endl;
}

void PrintTimeout()
{
	std::cout << "Request timed out" << std::endl;
}

void RunPingSequence(const FileDescriptor& socketDescriptor, const sockaddr_in& serverAddress)
{
	for (int sequence = 1; sequence <= PING_COUNT; ++sequence)
	{
		const timeval sendTime = CurrentTime();
		const std::string message = BuildPingMessage(sequence, sendTime);

		if (!SendPing(socketDescriptor, message, serverAddress))
		{
			PrintTimeout();
			continue;
		}

		const std::optional<std::string> reply = ReceiveReply(socketDescriptor);
		if (!reply)
		{
			PrintTimeout();
			continue;
		}

		const timeval receiveTime = CurrentTime();
		PrintReply(sequence, MillisecondsSinceEpoch(sendTime), ElapsedSeconds(sendTime, receiveTime));
	}
}
}

int main(const int argc, char* argv[])
{
	try
	{
		const ClientArguments arguments = ParseArguments(argc, argv);
		const sockaddr_in serverAddress = ResolveServerAddress(arguments);

		const FileDescriptor socketDescriptor = CreateClientSocket();
		ConfigureReceiveTimeout(socketDescriptor);

		RunPingSequence(socketDescriptor, serverAddress);
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
