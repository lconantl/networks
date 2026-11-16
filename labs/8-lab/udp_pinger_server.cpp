#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
#include <unistd.h>

namespace
{
constexpr std::uint16_t DEFAULT_PORT = 12000;
constexpr std::size_t MAX_MESSAGE_BYTES = 1024;
constexpr int LOSS_PERCENT = 30;

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

struct ReceivedDatagram
{
	std::string payload;
	sockaddr_in clientAddress{};
};

volatile sig_atomic_t g_stopRequested = 0;

void HandleSigint(int) noexcept
{
	g_stopRequested = 1;
}

void RegisterSigintHandler()
{
	struct sigaction action{};
	action.sa_handler = HandleSigint;
	sigemptyset(&action.sa_mask);
	AssertIsSucceeded(sigaction(SIGINT, &action, nullptr), "Не удалось установить обработчик SIGINT");
}

void SeedRandomGenerator() noexcept
{
	std::srand(static_cast<unsigned>(std::time(nullptr)));
}

bool ShouldDropPacket() noexcept
{
	return (std::rand() % 100) < LOSS_PERCENT;
}

std::uint16_t ParsePort(const int argc, char* const argv[]) noexcept
{
	if (argc <= 1)
	{
		return DEFAULT_PORT;
	}

	int parsedPort = 0;
	const std::string_view argument(argv[1]);
	const auto parseResult = std::from_chars(argument.data(), argument.data() + argument.size(), parsedPort);
	if (parseResult.ec != std::errc())
	{
		return DEFAULT_PORT;
	}
	return static_cast<std::uint16_t>(parsedPort);
}

sockaddr_in BuildServerAddress(const std::uint16_t port) noexcept
{
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(port);
	return address;
}

FileDescriptor CreateUdpSocket()
{
	FileDescriptor socketDescriptor(socket(AF_INET, SOCK_DGRAM, 0));
	AssertIsSucceeded(socketDescriptor.Get(), "Не удалось создать UDP-сокет");
	return socketDescriptor;
}

void BindSocket(const FileDescriptor& socketDescriptor, const sockaddr_in& address)
{
	AssertIsSucceeded(
		bind(socketDescriptor.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)),
		"Не удалось привязать сокет к порту");
}

std::optional<ReceivedDatagram> ReceiveDatagram(const FileDescriptor& socketDescriptor) noexcept
{
	char buffer[MAX_MESSAGE_BYTES];
	sockaddr_in clientAddress{};
	socklen_t clientAddressLength = sizeof(clientAddress);

	const ssize_t received = recvfrom(socketDescriptor.Get(), buffer, sizeof(buffer) - 1, 0,
	                                   reinterpret_cast<sockaddr*>(&clientAddress), &clientAddressLength);
	if (received < 0)
	{
		return std::nullopt;
	}

	buffer[received] = '\0';
	return ReceivedDatagram{std::string(buffer), clientAddress};
}

void SendDatagramBack(const FileDescriptor& socketDescriptor, const ReceivedDatagram& datagram) noexcept
{
	sendto(socketDescriptor.Get(), datagram.payload.data(), datagram.payload.size(), 0,
	       reinterpret_cast<const sockaddr*>(&datagram.clientAddress), sizeof(datagram.clientAddress));
}

void LogDroppedPacket(const std::string& payload)
{
	std::cout << "Потерян пакет: " << payload << std::endl;
}

void LogForwardedPacket(const std::string& payload)
{
	std::cout << "Отправлен ответ: " << payload << std::endl;
}

void PrintListeningMessage(const std::uint16_t port)
{
	std::cout << "UDP pinger server слушает порт " << port
	          << " (эмуляция потерь: " << LOSS_PERCENT << "%)" << std::endl;
}

void PrintShutdownMessage()
{
	std::cout << "Сервер остановлен" << std::endl;
}

void RunServerLoop(const FileDescriptor& socketDescriptor)
{
	while (!g_stopRequested)
	{
		const std::optional<ReceivedDatagram> datagram = ReceiveDatagram(socketDescriptor);
		if (!datagram)
		{
			continue;
		}

		if (ShouldDropPacket())
		{
			LogDroppedPacket(datagram->payload);
			continue;
		}

		SendDatagramBack(socketDescriptor, *datagram);
		LogForwardedPacket(datagram->payload);
	}
}
}

int main(const int argc, char* argv[])
{
	try
	{
		RegisterSigintHandler();
		SeedRandomGenerator();

		const std::uint16_t port = ParsePort(argc, argv);
		const FileDescriptor socketDescriptor = CreateUdpSocket();
		BindSocket(socketDescriptor, BuildServerAddress(port));

		PrintListeningMessage(port);
		RunServerLoop(socketDescriptor);
		PrintShutdownMessage();
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
