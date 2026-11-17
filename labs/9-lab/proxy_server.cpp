#include <cerrno>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
constexpr std::uint16_t DEFAULT_PORT = 8888;
constexpr std::uint16_t DEFAULT_ORIGIN_PORT = 80;
constexpr int LISTEN_BACKLOG = 16;
constexpr int CLIENT_TIMEOUT_SECONDS = 5;
constexpr int ORIGIN_TIMEOUT_SECONDS = 5;
constexpr std::size_t MAX_REQUEST_BYTES = 8192;
constexpr std::size_t RECV_CHUNK_BYTES = 4096;
constexpr std::string_view CACHE_DIR = "cache";
constexpr std::string_view ABSOLUTE_TARGET_PREFIX = "http://";

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

class AddressInfo final
{
public:
	explicit AddressInfo(addrinfo* info = nullptr) noexcept
		: m_info(info)
	{
	}

	~AddressInfo() noexcept
	{
		if (m_info != nullptr)
		{
			freeaddrinfo(m_info);
		}
	}

	AddressInfo(const AddressInfo&) = delete;
	AddressInfo& operator=(const AddressInfo&) = delete;

	AddressInfo(AddressInfo&& other) noexcept
		: m_info(std::exchange(other.m_info, nullptr))
	{
	}

	[[nodiscard]] const addrinfo* Get() const noexcept
	{
		return m_info;
	}

private:
	addrinfo* m_info;
};

struct ProxyStatistics
{
	long requests = 0;
	long hits = 0;
	long misses = 0;
	long errors = 0;
};

struct ParsedRequest
{
	std::string method;
	std::string host;
	std::uint16_t port = DEFAULT_ORIGIN_PORT;
	std::string path;
};

struct RequestLine
{
	std::string method;
	std::string target;
	std::string version;
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

void CreateCacheDirectory()
{
	std::filesystem::create_directories(CACHE_DIR);
}

std::uint16_t ParsePortNumber(const std::string_view text, const std::uint16_t fallback) noexcept
{
	int parsedPort = 0;
	const auto parseResult = std::from_chars(text.data(), text.data() + text.size(), parsedPort);
	if (parseResult.ec != std::errc())
	{
		return fallback;
	}
	return static_cast<std::uint16_t>(parsedPort);
}

std::uint16_t ParsePort(const int argc, char* const argv[]) noexcept
{
	if (argc <= 1)
	{
		return DEFAULT_PORT;
	}
	return ParsePortNumber(argv[1], DEFAULT_PORT);
}

sockaddr_in BuildListenAddress(const std::uint16_t port) noexcept
{
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(port);
	return address;
}

FileDescriptor CreateListeningSocket()
{
	FileDescriptor socketDescriptor(socket(AF_INET, SOCK_STREAM, 0));
	AssertIsSucceeded(socketDescriptor.Get(), "Не удалось создать TCP-сокет");
	return socketDescriptor;
}

void EnableAddressReuse(const FileDescriptor& socketDescriptor)
{
	const int enabled = 1;
	AssertIsSucceeded(
		setsockopt(socketDescriptor.Get(), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)),
		"Не удалось включить SO_REUSEADDR");
}

void BindListeningSocket(const FileDescriptor& socketDescriptor, const sockaddr_in& address)
{
	AssertIsSucceeded(
		bind(socketDescriptor.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)),
		"Не удалось привязать сокет к порту");
}

void StartListening(const FileDescriptor& socketDescriptor)
{
	AssertIsSucceeded(listen(socketDescriptor.Get(), LISTEN_BACKLOG), "Не удалось начать прослушивание");
}

void SetReceiveTimeout(const FileDescriptor& socketDescriptor, const int seconds)
{
	timeval timeout{};
	timeout.tv_sec = seconds;
	timeout.tv_usec = 0;
	AssertIsSucceeded(
		setsockopt(socketDescriptor.Get(), SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)),
		"Не удалось установить тайм-аут приёма");
}

bool SendAll(const FileDescriptor& socketDescriptor, const std::string& data) noexcept
{
	std::size_t sent = 0;
	while (sent < data.size())
	{
		const ssize_t result = send(socketDescriptor.Get(), data.data() + sent, data.size() - sent, 0);
		if (result <= 0)
		{
			return false;
		}
		sent += static_cast<std::size_t>(result);
	}
	return true;
}

std::string ReadHttpRequestHeader(const FileDescriptor& socketDescriptor) noexcept
{
	std::string header;
	char chunk[RECV_CHUNK_BYTES];

	while (header.find("\r\n\r\n") == std::string::npos && header.size() < MAX_REQUEST_BYTES)
	{
		const ssize_t received = recv(socketDescriptor.Get(), chunk, sizeof(chunk), 0);
		if (received <= 0)
		{
			break;
		}
		header.append(chunk, static_cast<std::size_t>(received));
	}
	return header;
}

std::optional<RequestLine> ParseRequestLine(const std::string& header)
{
	std::istringstream stream(header);
	RequestLine line;
	if (!(stream >> line.method >> line.target >> line.version))
	{
		return std::nullopt;
	}
	return line;
}

std::optional<ParsedRequest> ParseAbsoluteTarget(const std::string& target)
{
	const std::string rest = target.substr(ABSOLUTE_TARGET_PREFIX.size());
	const std::size_t slashPosition = rest.find('/');
	const std::string hostAndPort = slashPosition == std::string::npos ? rest : rest.substr(0, slashPosition);

	ParsedRequest request;
	request.path = slashPosition == std::string::npos ? "/" : rest.substr(slashPosition);

	const std::size_t colonPosition = hostAndPort.find(':');
	if (colonPosition == std::string::npos)
	{
		request.host = hostAndPort;
	}
	else
	{
		request.host = hostAndPort.substr(0, colonPosition);
		request.port = ParsePortNumber(hostAndPort.substr(colonPosition + 1), DEFAULT_ORIGIN_PORT);
	}

	if (request.host.empty())
	{
		return std::nullopt;
	}
	return request;
}

std::optional<ParsedRequest> ParseOriginStyleTarget(const std::string& target)
{
	const std::string rest = target.substr(1);
	const std::size_t slashPosition = rest.find('/');

	ParsedRequest request;
	request.host = slashPosition == std::string::npos ? rest : rest.substr(0, slashPosition);
	request.path = slashPosition == std::string::npos ? "/" : rest.substr(slashPosition);

	if (request.host.empty())
	{
		return std::nullopt;
	}
	return request;
}

std::optional<ParsedRequest> ParseRequest(const std::string& header)
{
	const std::optional<RequestLine> line = ParseRequestLine(header);
	if (!line)
	{
		return std::nullopt;
	}

	std::optional<ParsedRequest> request;
	if (line->target.rfind(ABSOLUTE_TARGET_PREFIX, 0) == 0)
	{
		request = ParseAbsoluteTarget(line->target);
	}
	else if (!line->target.empty() && line->target.front() == '/')
	{
		request = ParseOriginStyleTarget(line->target);
	}

	if (!request)
	{
		return std::nullopt;
	}

	request->method = line->method;
	return request;
}

std::string BuildRequestUrl(const ParsedRequest& request)
{
	return std::format("http://{}:{}{}", request.host, request.port, request.path);
}

std::string CachePathFor(const ParsedRequest& request)
{
	const std::string key = std::format("{}:{}{}", request.host, request.port, request.path);
	const std::size_t hash = std::hash<std::string>{}(key);
	return std::format("{}/{:016x}.cache", CACHE_DIR, hash);
}

std::optional<std::string> ReadCacheFile(const std::string& path)
{
	std::error_code errorCode;
	const auto size = std::filesystem::file_size(path, errorCode);
	if (errorCode)
	{
		return std::nullopt;
	}

	std::ifstream file(path, std::ios::binary);
	if (!file)
	{
		return std::nullopt;
	}

	std::string content(static_cast<std::size_t>(size), '\0');
	file.read(content.data(), static_cast<std::streamsize>(size));
	return content;
}

void WriteCacheFile(const std::string& path, const std::string& response)
{
	std::ofstream file(path, std::ios::binary | std::ios::trunc);
	file.write(response.data(), static_cast<std::streamsize>(response.size()));
}

bool IsCacheableResponse(const std::string& response) noexcept
{
	constexpr std::string_view OK_STATUS_10 = "HTTP/1.0 200";
	constexpr std::string_view OK_STATUS_11 = "HTTP/1.1 200";
	return response.rfind(OK_STATUS_10, 0) == 0 || response.rfind(OK_STATUS_11, 0) == 0;
}

std::string BuildErrorResponse(const int code, const std::string_view reason, const std::string_view body)
{
	return std::format(
		"HTTP/1.0 {} {}\r\nContent-Type: text/plain\r\nContent-Length: {}\r\nConnection: close\r\n\r\n{}",
		code, reason, body.size(), body);
}

std::string BuildOriginRequest(const ParsedRequest& request)
{
	return std::format("GET {} HTTP/1.0\r\nHost: {}\r\nConnection: close\r\n\r\n", request.path, request.host);
}

std::optional<AddressInfo> ResolveOriginAddress(const ParsedRequest& request) noexcept
{
	addrinfo hints{};
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	addrinfo* resolved = nullptr;
	const std::string portText = std::to_string(request.port);
	if (getaddrinfo(request.host.c_str(), portText.c_str(), &hints, &resolved) != 0 || resolved == nullptr)
	{
		return std::nullopt;
	}
	return AddressInfo(resolved);
}

std::optional<FileDescriptor> ConnectToOrigin(const AddressInfo& addressInfo)
{
	const addrinfo* info = addressInfo.Get();
	FileDescriptor socketDescriptor(socket(info->ai_family, info->ai_socktype, info->ai_protocol));
	if (socketDescriptor.Get() < 0)
	{
		return std::nullopt;
	}

	SetReceiveTimeout(socketDescriptor, ORIGIN_TIMEOUT_SECONDS);

	if (connect(socketDescriptor.Get(), info->ai_addr, info->ai_addrlen) < 0)
	{
		return std::nullopt;
	}
	return socketDescriptor;
}

std::string ReceiveWholeResponse(const FileDescriptor& socketDescriptor) noexcept
{
	std::string response;
	char chunk[RECV_CHUNK_BYTES];

	while (true)
	{
		const ssize_t received = recv(socketDescriptor.Get(), chunk, sizeof(chunk), 0);
		if (received <= 0)
		{
			break;
		}
		response.append(chunk, static_cast<std::size_t>(received));
	}
	return response;
}

std::optional<std::string> FetchFromOrigin(const ParsedRequest& request)
{
	const std::optional<AddressInfo> addressInfo = ResolveOriginAddress(request);
	if (!addressInfo)
	{
		return std::nullopt;
	}

	std::optional<FileDescriptor> originSocket = ConnectToOrigin(*addressInfo);
	if (!originSocket)
	{
		return std::nullopt;
	}

	if (!SendAll(*originSocket, BuildOriginRequest(request)))
	{
		return std::nullopt;
	}

	std::string response = ReceiveWholeResponse(*originSocket);
	if (response.empty())
	{
		return std::nullopt;
	}
	return response;
}

void LogRequest(const std::string& method, const std::string& url)
{
	std::cout << "[LOG] Запрос: " << method << " " << url << std::endl;
}

void LogCacheHit(const std::string& url)
{
	std::cout << "[LOG] Кэш-попадание: " << url << std::endl;
}

void LogCacheMiss(const ParsedRequest& request)
{
	std::cout << "[LOG] Кэш-промах, запрос к " << request.host << ":" << request.port << std::endl;
}

void LogOriginError(const std::string& host)
{
	std::cout << "[LOG] Ошибка соединения с " << host << std::endl;
}

void HandleClient(const FileDescriptor& clientDescriptor, ProxyStatistics& statistics)
{
	SetReceiveTimeout(clientDescriptor, CLIENT_TIMEOUT_SECONDS);

	const std::string header = ReadHttpRequestHeader(clientDescriptor);
	if (header.empty())
	{
		return;
	}

	const std::optional<ParsedRequest> request = ParseRequest(header);
	if (!request)
	{
		SendAll(clientDescriptor, BuildErrorResponse(400, "Bad Request", "Не удалось разобрать запрос\n"));
		++statistics.errors;
		return;
	}

	++statistics.requests;
	const std::string url = BuildRequestUrl(*request);
	LogRequest(request->method, url);

	if (request->method != "GET")
	{
		SendAll(clientDescriptor, BuildErrorResponse(501, "Not Implemented", "Поддерживается только GET\n"));
		++statistics.errors;
		return;
	}

	const std::string cachePath = CachePathFor(*request);
	if (const std::optional<std::string> cached = ReadCacheFile(cachePath))
	{
		LogCacheHit(url);
		SendAll(clientDescriptor, *cached);
		++statistics.hits;
		return;
	}

	LogCacheMiss(*request);
	const std::optional<std::string> response = FetchFromOrigin(*request);
	if (!response)
	{
		LogOriginError(request->host);
		SendAll(clientDescriptor, BuildErrorResponse(502, "Bad Gateway", "Не удалось получить ответ от сервера\n"));
		++statistics.errors;
		return;
	}

	SendAll(clientDescriptor, *response);
	if (IsCacheableResponse(*response))
	{
		WriteCacheFile(cachePath, *response);
	}
	++statistics.misses;
}

void PrintListeningMessage(const std::uint16_t port)
{
	std::cout << "Прокси-сервер слушает порт " << port << ", кэш в ./" << CACHE_DIR << std::endl;
}

void PrintStoppedMessage()
{
	std::cout << "Сервер остановлен" << std::endl;
}

void PrintStatistics(const ProxyStatistics& statistics)
{
	std::cout << "--- Статистика прокси ---" << std::endl
	          << "Запросов: " << statistics.requests
	          << ", кэш-попаданий: " << statistics.hits
	          << ", кэш-промахов: " << statistics.misses
	          << ", ошибок: " << statistics.errors << std::endl;
}

void RunAcceptLoop(const FileDescriptor& listener, ProxyStatistics& statistics)
{
	while (!g_stopRequested)
	{
		sockaddr_in clientAddress{};
		socklen_t clientAddressLength = sizeof(clientAddress);

		FileDescriptor clientSocket(
			accept(listener.Get(), reinterpret_cast<sockaddr*>(&clientAddress), &clientAddressLength));
		if (clientSocket.Get() < 0)
		{
			continue;
		}

		try
		{
			HandleClient(clientSocket, statistics);
		}
		catch (const std::exception& error)
		{
			std::cerr << error.what() << std::endl;
			++statistics.errors;
		}
	}
}
}

int main(const int argc, char* argv[])
{
	try
	{
		RegisterSigintHandler();
		CreateCacheDirectory();

		const std::uint16_t port = ParsePort(argc, argv);
		const FileDescriptor listener = CreateListeningSocket();
		EnableAddressReuse(listener);
		BindListeningSocket(listener, BuildListenAddress(port));
		StartListening(listener);

		PrintListeningMessage(port);

		ProxyStatistics statistics;
		RunAcceptLoop(listener, statistics);

		PrintStoppedMessage();
		PrintStatistics(statistics);
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
