#include <survey/Survey.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

namespace
{
constexpr std::uint16_t DEFAULT_PORT = 8080;
constexpr unsigned int MIN_USER_PORT = 1024;
constexpr unsigned int MAX_PORT = 65535;
constexpr auto DEFAULT_ROOT = "www";
constexpr auto SERVER_NAME = "lab5-webserver";
constexpr auto INDEX_FILE = "index.html";
constexpr std::size_t MAX_HEADER_BYTES = 16 * 1024;
constexpr std::size_t READ_CHUNK_BYTES = 16 * 1024;
constexpr std::size_t FILE_CHUNK_BYTES = 64 * 1024;
constexpr std::size_t MAX_LOGGED_LINE = 120;
constexpr std::size_t MAX_WORKER_THREADS = 512;
constexpr std::size_t MAX_ASYNC_CONNECTIONS = 10000;
constexpr int EPOLL_BATCH = 256;
constexpr int EPOLL_TICK_MS = 500;
constexpr auto RECEIVE_TIMEOUT = std::chrono::seconds(5);
constexpr auto SEND_TIMEOUT = std::chrono::seconds(10);
constexpr auto IDLE_TIMEOUT = std::chrono::seconds(10);
constexpr auto SHUTDOWN_GRACE = std::chrono::seconds(5);

constexpr auto STYLE_RESET = "\x1b[0m";
constexpr auto STYLE_BOLD = "\x1b[1m";
constexpr auto STYLE_GRAY = "\x1b[90m";
constexpr auto STYLE_GREEN = "\x1b[32m";
constexpr auto STYLE_CYAN = "\x1b[36m";
constexpr auto STYLE_YELLOW = "\x1b[33m";
constexpr auto STYLE_RED = "\x1b[31m";

using Clock = std::chrono::steady_clock;

class SystemError : public std::runtime_error
{
public:
	SystemError(std::string_view action, int code);
};

SystemError::SystemError(const std::string_view action, const int code)
	: std::runtime_error(std::format("{}: {}", action, std::strerror(code)))
{
}

void AssertIsSucceeded(const int result, const std::string_view action)
{
	if (result < 0)
	{
		throw SystemError(action, errno);
	}
}

void AssertIsZero(const int code, const std::string_view action)
{
	if (code != 0)
	{
		throw SystemError(action, code);
	}
}

class FileDescriptor final
{
public:
	FileDescriptor() noexcept;
	explicit FileDescriptor(int descriptor) noexcept;
	~FileDescriptor() noexcept;

	FileDescriptor(const FileDescriptor&) = delete;
	FileDescriptor& operator=(const FileDescriptor&) = delete;
	FileDescriptor(FileDescriptor&& other) noexcept;
	FileDescriptor& operator=(FileDescriptor&& other) noexcept;

	[[nodiscard]] int Get() const noexcept;
	[[nodiscard]] bool IsValid() const noexcept;
	void Reset() noexcept;

private:
	int m_descriptor;
};

FileDescriptor::FileDescriptor() noexcept
	: m_descriptor(-1)
{
}

FileDescriptor::FileDescriptor(const int descriptor) noexcept
	: m_descriptor(descriptor)
{
}

FileDescriptor::~FileDescriptor() noexcept
{
	Reset();
}

FileDescriptor::FileDescriptor(FileDescriptor&& other) noexcept
	: m_descriptor(std::exchange(other.m_descriptor, -1))
{
}

FileDescriptor& FileDescriptor::operator=(FileDescriptor&& other) noexcept
{
	if (this != &other)
	{
		Reset();
		m_descriptor = std::exchange(other.m_descriptor, -1);
	}

	return *this;
}

int FileDescriptor::Get() const noexcept
{
	return m_descriptor;
}

bool FileDescriptor::IsValid() const noexcept
{
	return m_descriptor >= 0;
}

void FileDescriptor::Reset() noexcept
{
	if (IsValid())
	{
		close(m_descriptor);
		m_descriptor = -1;
	}
}

sigset_t BuildShutdownSignalSet()
{
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);

	return set;
}

sigset_t BlockShutdownSignals()
{
	const sigset_t set = BuildShutdownSignalSet();
	sigset_t previous;
	AssertIsZero(pthread_sigmask(SIG_BLOCK, &set, &previous), "Не удалось заблокировать SIGINT");

	return previous;
}

FileDescriptor OpenSignalDescriptor()
{
	const sigset_t set = BuildShutdownSignalSet();
	FileDescriptor descriptor(signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK));
	AssertIsSucceeded(descriptor.Get(), "Не удалось создать signalfd");

	return descriptor;
}

std::string DescribeSignal(const std::uint32_t number)
{
	switch (number)
	{
	case SIGINT: return "SIGINT";
	case SIGTERM: return "SIGTERM";
	default: return std::format("сигнал {}", number);
	}
}

class ShutdownSignal final
{
public:
	ShutdownSignal();
	~ShutdownSignal() noexcept;

	ShutdownSignal(const ShutdownSignal&) = delete;
	ShutdownSignal& operator=(const ShutdownSignal&) = delete;
	ShutdownSignal(ShutdownSignal&&) = delete;
	ShutdownSignal& operator=(ShutdownSignal&&) = delete;

	[[nodiscard]] int Descriptor() const noexcept;
	[[nodiscard]] std::string Consume() const;

private:
	sigset_t m_previousMask;
	sighandler_t m_previousPipeHandler;
	FileDescriptor m_descriptor;
};

ShutdownSignal::ShutdownSignal()
	: m_previousMask(BlockShutdownSignals())
	, m_previousPipeHandler(std::signal(SIGPIPE, SIG_IGN))
	, m_descriptor(OpenSignalDescriptor())
{
}

ShutdownSignal::~ShutdownSignal() noexcept
{
	std::signal(SIGPIPE, m_previousPipeHandler);
	pthread_sigmask(SIG_SETMASK, &m_previousMask, nullptr);
}

int ShutdownSignal::Descriptor() const noexcept
{
	return m_descriptor.Get();
}

std::string ShutdownSignal::Consume() const
{
	signalfd_siginfo info{};
	const ssize_t result = read(m_descriptor.Get(), &info, sizeof(info));
	if (result != static_cast<ssize_t>(sizeof(info)))
	{
		return "сигнал остановки";
	}

	return DescribeSignal(info.ssi_signo);
}

struct ServerConfig
{
	std::uint16_t port = DEFAULT_PORT;
	std::filesystem::path root = DEFAULT_ROOT;
	bool isPublic = false;
	bool isVerbose = true;
};

struct AcceptedClient
{
	FileDescriptor socket = {};
	std::string peer = {};
};

enum class ClientBlocking
{
	Blocking,
	NonBlocking,
};

void EnableAddressReuse(const int descriptor)
{
	const int enabled = 1;
	AssertIsSucceeded(setsockopt(descriptor, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)),
		"Не удалось включить SO_REUSEADDR");
}

sockaddr_in BuildListenAddress(const ServerConfig& config)
{
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(config.port);
	address.sin_addr.s_addr = htonl(config.isPublic ? INADDR_ANY : INADDR_LOOPBACK);

	return address;
}

void AssertIsBound(const int result, const std::uint16_t port)
{
	if (result == 0)
	{
		return;
	}

	if (errno == EADDRINUSE)
	{
		throw std::runtime_error(std::format("Порт {} уже занят другой программой", port));
	}

	throw SystemError(std::format("Не удалось привязать сокет к порту {}", port), errno);
}

FileDescriptor OpenListener(const ServerConfig& config)
{
	FileDescriptor listener(socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
	AssertIsSucceeded(listener.Get(), "Не удалось создать TCP-сокет");
	EnableAddressReuse(listener.Get());

	const sockaddr_in address = BuildListenAddress(config);
	AssertIsBound(
		bind(listener.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)),
		config.port);
	AssertIsSucceeded(listen(listener.Get(), SOMAXCONN), "Не удалось начать прослушивание");

	return listener;
}

std::string DescribePeer(const sockaddr_in& address)
{
	std::array<char, INET_ADDRSTRLEN> text{};
	inet_ntop(AF_INET, &address.sin_addr, text.data(), text.size());

	return std::format("{}:{}", text.data(), ntohs(address.sin_port));
}

bool IsTransientAcceptError(const int code) noexcept
{
	return code == EAGAIN || code == EWOULDBLOCK || code == EINTR || code == ECONNABORTED
		|| code == EPROTO || code == EMFILE || code == ENFILE || code == ENOBUFS || code == ENOMEM;
}

std::optional<AcceptedClient> AcceptClient(const int listener, const ClientBlocking blocking)
{
	sockaddr_in address{};
	socklen_t length = sizeof(address);
	const int flags = SOCK_CLOEXEC | (blocking == ClientBlocking::NonBlocking ? SOCK_NONBLOCK : 0);

	const int descriptor = accept4(listener, reinterpret_cast<sockaddr*>(&address), &length, flags);
	if (descriptor >= 0)
	{
		return AcceptedClient{FileDescriptor(descriptor), DescribePeer(address)};
	}

	if (IsTransientAcceptError(errno))
	{
		return std::nullopt;
	}

	throw SystemError("Ошибка accept", errno);
}

timeval ToTimeval(const std::chrono::seconds duration) noexcept
{
	timeval value{};
	value.tv_sec = static_cast<decltype(value.tv_sec)>(duration.count());

	return value;
}

void ApplyBlockingTimeouts(const int descriptor)
{
	const timeval receive = ToTimeval(RECEIVE_TIMEOUT);
	const timeval send = ToTimeval(SEND_TIMEOUT);

	AssertIsSucceeded(setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &receive, sizeof(receive)),
		"Не удалось установить таймаут чтения");
	AssertIsSucceeded(setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &send, sizeof(send)),
		"Не удалось установить таймаут записи");
}

enum class HttpStatus
{
	Ok = 200,
	MovedPermanently = 301,
	BadRequest = 400,
	Forbidden = 403,
	NotFound = 404,
	MethodNotAllowed = 405,
	RequestTimeout = 408,
	HeaderFieldsTooLarge = 431,
	InternalServerError = 500,
	ServiceUnavailable = 503,
	VersionNotSupported = 505,
};

int ToCode(const HttpStatus status) noexcept
{
	return static_cast<int>(status);
}

std::string_view ReasonPhrase(const HttpStatus status) noexcept
{
	switch (status)
	{
	case HttpStatus::Ok: return "OK";
	case HttpStatus::MovedPermanently: return "Moved Permanently";
	case HttpStatus::BadRequest: return "Bad Request";
	case HttpStatus::Forbidden: return "Forbidden";
	case HttpStatus::NotFound: return "Not Found";
	case HttpStatus::MethodNotAllowed: return "Method Not Allowed";
	case HttpStatus::RequestTimeout: return "Request Timeout";
	case HttpStatus::HeaderFieldsTooLarge: return "Request Header Fields Too Large";
	case HttpStatus::InternalServerError: return "Internal Server Error";
	case HttpStatus::ServiceUnavailable: return "Service Unavailable";
	case HttpStatus::VersionNotSupported: return "HTTP Version Not Supported";
	}

	return "Unknown";
}

struct HttpRequest
{
	std::string method = {};
	std::string target = {};
	std::string version = {};
};

struct RequestParseResult
{
	std::optional<HttpRequest> request = {};
	HttpStatus error = HttpStatus::BadRequest;
};

std::optional<std::size_t> FindHeaderEnd(const std::string_view buffer) noexcept
{
	const std::size_t crlf = buffer.find("\r\n\r\n");
	if (crlf != std::string_view::npos)
	{
		return crlf + 4;
	}

	const std::size_t lf = buffer.find("\n\n");
	if (lf != std::string_view::npos)
	{
		return lf + 2;
	}

	return std::nullopt;
}

std::string_view ExtractRequestLine(const std::string_view head) noexcept
{
	std::string_view line = head.substr(0, head.find('\n'));
	if (!line.empty() && line.back() == '\r')
	{
		line.remove_suffix(1);
	}

	return line;
}

std::vector<std::string_view> SplitBySpace(const std::string_view line)
{
	std::vector<std::string_view> parts;
	std::size_t start = 0;

	while (start <= line.size())
	{
		const std::size_t end = std::min(line.find(' ', start), line.size());
		parts.push_back(line.substr(start, end - start));
		start = end + 1;
	}

	return parts;
}

bool IsMethodToken(const std::string_view method) noexcept
{
	return !method.empty() && std::all_of(method.begin(), method.end(), [](const char symbol) {
		return symbol >= 'A' && symbol <= 'Z';
	});
}

bool IsSupportedVersion(const std::string_view version) noexcept
{
	return version == "HTTP/1.1" || version == "HTTP/1.0";
}

std::optional<std::string> NormalizeTarget(const std::string_view target)
{
	constexpr std::string_view ABSOLUTE_PREFIX = "http://";
	if (target.starts_with('/'))
	{
		return std::string(target);
	}

	if (!target.starts_with(ABSOLUTE_PREFIX))
	{
		return std::nullopt;
	}

	const std::size_t pathStart = target.find('/', ABSOLUTE_PREFIX.size());
	return pathStart == std::string_view::npos ? std::string("/")
											   : std::string(target.substr(pathStart));
}

RequestParseResult ParseRequestHead(const std::string_view head)
{
	const std::vector<std::string_view> parts = SplitBySpace(ExtractRequestLine(head));
	if (parts.size() != 3 || !IsMethodToken(parts[0]) || parts[1].empty())
	{
		return {std::nullopt, HttpStatus::BadRequest};
	}

	if (!IsSupportedVersion(parts[2]))
	{
		const bool isHttp = parts[2].starts_with("HTTP/");
		return {std::nullopt, isHttp ? HttpStatus::VersionNotSupported : HttpStatus::BadRequest};
	}

	const std::optional<std::string> target = NormalizeTarget(parts[1]);
	if (!target.has_value())
	{
		return {std::nullopt, HttpStatus::BadRequest};
	}

	return {HttpRequest{std::string(parts[0]), *target, std::string(parts[2])}, HttpStatus::Ok};
}

std::optional<int> DecodeHexDigit(const char symbol) noexcept
{
	if (symbol >= '0' && symbol <= '9')
	{
		return symbol - '0';
	}
	if (symbol >= 'a' && symbol <= 'f')
	{
		return symbol - 'a' + 10;
	}
	if (symbol >= 'A' && symbol <= 'F')
	{
		return symbol - 'A' + 10;
	}

	return std::nullopt;
}

std::optional<std::string> PercentDecode(const std::string_view text)
{
	std::string decoded;
	decoded.reserve(text.size());

	for (std::size_t index = 0; index < text.size(); ++index)
	{
		if (text[index] != '%')
		{
			decoded += text[index];
			continue;
		}

		if (index + 2 >= text.size())
		{
			return std::nullopt;
		}

		const std::optional<int> high = DecodeHexDigit(text[index + 1]);
		const std::optional<int> low = DecodeHexDigit(text[index + 2]);
		if (!high.has_value() || !low.has_value())
		{
			return std::nullopt;
		}

		decoded += static_cast<char>(*high * 16 + *low);
		index += 2;
	}

	return decoded;
}

std::string_view StripQuery(const std::string_view target) noexcept
{
	return target.substr(0, target.find_first_of("?#"));
}

bool IsInsideRoot(const std::filesystem::path& root, const std::filesystem::path& path)
{
	const auto [rootEnd, pathEnd]
		= std::mismatch(root.begin(), root.end(), path.begin(), path.end());
	return rootEnd == root.end();
}

struct Resolution
{
	HttpStatus status = HttpStatus::NotFound;
	std::filesystem::path file = {};
	std::string location = {};
};

void AssertIsDirectory(const std::filesystem::path& root)
{
	std::error_code error;
	if (!std::filesystem::is_directory(root, error))
	{
		throw std::runtime_error("Папка сайта не найдена: " + root.string());
	}
}

std::filesystem::path CanonicalRoot(const std::filesystem::path& root)
{
	AssertIsDirectory(root);
	return std::filesystem::canonical(root);
}

class StaticSite
{
public:
	explicit StaticSite(const std::filesystem::path& root);

	[[nodiscard]] Resolution Resolve(std::string_view target) const;
	[[nodiscard]] const std::filesystem::path& Root() const noexcept;

private:
	[[nodiscard]] Resolution ResolveDirectory(
		const std::filesystem::path& directory, std::string_view path) const;

	std::filesystem::path m_root;
};

StaticSite::StaticSite(const std::filesystem::path& root)
	: m_root(CanonicalRoot(root))
{
}

Resolution StaticSite::Resolve(const std::string_view target) const
{
	const std::string_view path = StripQuery(target);
	const std::optional<std::string> decoded = PercentDecode(path);
	if (!decoded.has_value() || decoded->find('\0') != std::string::npos)
	{
		return {HttpStatus::BadRequest};
	}

	const std::size_t relativeStart = std::min(decoded->find_first_not_of('/'), decoded->size());
	std::error_code error;
	const std::filesystem::path candidate
		= std::filesystem::weakly_canonical(m_root / decoded->substr(relativeStart), error);

	if (error)
	{
		return {HttpStatus::NotFound};
	}
	if (!IsInsideRoot(m_root, candidate))
	{
		return {HttpStatus::Forbidden};
	}
	if (std::filesystem::is_directory(candidate, error))
	{
		return ResolveDirectory(candidate, path);
	}
	if (!std::filesystem::is_regular_file(candidate, error))
	{
		return {HttpStatus::NotFound};
	}

	return {HttpStatus::Ok, candidate};
}

const std::filesystem::path& StaticSite::Root() const noexcept
{
	return m_root;
}

Resolution StaticSite::ResolveDirectory(
	const std::filesystem::path& directory, const std::string_view path) const
{
	if (!path.ends_with('/'))
	{
		return {HttpStatus::MovedPermanently, {}, std::string(path) + "/"};
	}

	std::error_code error;
	const std::filesystem::path index = directory / INDEX_FILE;
	if (!std::filesystem::is_regular_file(index, error))
	{
		return {HttpStatus::NotFound};
	}

	return {HttpStatus::Ok, index};
}

constexpr std::array<std::pair<std::string_view, std::string_view>, 20> MIME_TYPES = {{
	{".html", "text/html; charset=utf-8"},
	{".htm", "text/html; charset=utf-8"},
	{".css", "text/css; charset=utf-8"},
	{".js", "text/javascript; charset=utf-8"},
	{".json", "application/json"},
	{".txt", "text/plain; charset=utf-8"},
	{".md", "text/markdown; charset=utf-8"},
	{".xml", "application/xml"},
	{".png", "image/png"},
	{".jpg", "image/jpeg"},
	{".jpeg", "image/jpeg"},
	{".gif", "image/gif"},
	{".svg", "image/svg+xml"},
	{".ico", "image/x-icon"},
	{".webp", "image/webp"},
	{".pdf", "application/pdf"},
	{".mp4", "video/mp4"},
	{".mp3", "audio/mpeg"},
	{".wasm", "application/wasm"},
	{".zip", "application/zip"},
}};

constexpr auto FALLBACK_MIME_TYPE = "application/octet-stream";

std::string ToLowerAscii(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](const char symbol) {
		return symbol >= 'A' && symbol <= 'Z' ? static_cast<char>(symbol - 'A' + 'a') : symbol;
	});

	return text;
}

std::string_view DetectMimeType(const std::filesystem::path& path)
{
	const std::string extension = ToLowerAscii(path.extension().string());
	for (const auto& [knownExtension, type] : MIME_TYPES)
	{
		if (knownExtension == extension)
		{
			return type;
		}
	}

	return FALLBACK_MIME_TYPE;
}

std::string FormatHttpDate()
{
	const std::time_t now = std::time(nullptr);
	std::tm utc{};
	gmtime_r(&now, &utc);

	std::array<char, 64> buffer{};
	const std::size_t length
		= std::strftime(buffer.data(), buffer.size(), "%a, %d %b %Y %H:%M:%S GMT", &utc);

	return std::string(buffer.data(), length);
}

struct ResponsePlan
{
	HttpStatus status = HttpStatus::Ok;
	std::string head = {};
	std::string body = {};
	FileDescriptor file = {};
	std::uint64_t fileSize = 0;
};

std::string BuildHead(const HttpStatus status,
	const std::string_view contentType,
	const std::uint64_t contentLength,
	const std::string_view extraHeaders)
{
	return std::format("HTTP/1.1 {} {}\r\n"
					   "Server: {}\r\n"
					   "Date: {}\r\n"
					   "Content-Type: {}\r\n"
					   "Content-Length: {}\r\n"
					   "Connection: close\r\n"
					   "{}\r\n",
		ToCode(status),
		ReasonPhrase(status),
		SERVER_NAME,
		FormatHttpDate(),
		contentType,
		contentLength,
		extraHeaders);
}

std::string BuildErrorBody(const HttpStatus status)
{
	if (status == HttpStatus::NotFound)
	{
		return "File Not Found";
	}

	return std::format("{} {}", ToCode(status), ReasonPhrase(status));
}

ResponsePlan BuildErrorPlan(
	const HttpStatus status, const bool isHeadRequest, const std::string_view extraHeaders = {})
{
	std::string body = BuildErrorBody(status);

	ResponsePlan plan;
	plan.status = status;
	plan.head = BuildHead(status, "text/plain; charset=utf-8", body.size(), extraHeaders);
	plan.body = isHeadRequest ? std::string() : std::move(body);

	return plan;
}

HttpStatus MapOpenError(const int code) noexcept
{
	switch (code)
	{
	case ENOENT:
	case ENOTDIR: return HttpStatus::NotFound;
	case EACCES:
	case EPERM: return HttpStatus::Forbidden;
	default: return HttpStatus::InternalServerError;
	}
}

std::optional<std::uint64_t> ReadFileSize(const FileDescriptor& file)
{
	struct stat info
	{
	};
	if (fstat(file.Get(), &info) != 0 || !S_ISREG(info.st_mode))
	{
		return std::nullopt;
	}

	return static_cast<std::uint64_t>(info.st_size);
}

ResponsePlan BuildFilePlan(const std::filesystem::path& path, const bool isHeadRequest)
{
	FileDescriptor file(open(path.c_str(), O_RDONLY | O_CLOEXEC));
	if (!file.IsValid())
	{
		return BuildErrorPlan(MapOpenError(errno), isHeadRequest);
	}

	const std::optional<std::uint64_t> size = ReadFileSize(file);
	if (!size.has_value())
	{
		return BuildErrorPlan(HttpStatus::InternalServerError, isHeadRequest);
	}

	ResponsePlan plan;
	plan.head = BuildHead(HttpStatus::Ok, DetectMimeType(path), *size, {});
	if (!isHeadRequest)
	{
		plan.file = std::move(file);
		plan.fileSize = *size;
	}

	return plan;
}

bool IsAllowedMethod(const std::string_view method) noexcept
{
	return method == "GET" || method == "HEAD";
}

ResponsePlan BuildResponse(const HttpRequest& request, const StaticSite& site)
{
	const bool isHeadRequest = request.method == "HEAD";
	if (!IsAllowedMethod(request.method))
	{
		return BuildErrorPlan(HttpStatus::MethodNotAllowed, false, "Allow: GET, HEAD\r\n");
	}

	const Resolution resolution = site.Resolve(request.target);
	if (resolution.status == HttpStatus::MovedPermanently)
	{
		return BuildErrorPlan(
			resolution.status, isHeadRequest, "Location: " + resolution.location + "\r\n");
	}
	if (resolution.status != HttpStatus::Ok)
	{
		return BuildErrorPlan(resolution.status, isHeadRequest);
	}

	return BuildFilePlan(resolution.file, isHeadRequest);
}

enum class WriteProgress
{
	Done,
	WouldBlock,
	Failed,
};

class ResponseWriter
{
public:
	explicit ResponseWriter(ResponsePlan plan);

	WriteProgress WriteSome(int socket);

	[[nodiscard]] HttpStatus Status() const noexcept;
	[[nodiscard]] std::uint64_t BytesSent() const noexcept;

private:
	bool RefillFromFile();

	HttpStatus m_status;
	std::string m_pending;
	std::size_t m_offset;
	FileDescriptor m_file;
	std::uint64_t m_fileRemaining;
	std::uint64_t m_bytesSent;
};

ResponseWriter::ResponseWriter(ResponsePlan plan)
	: m_status(plan.status)
	, m_pending(std::move(plan.head) + plan.body)
	, m_offset(0)
	, m_file(std::move(plan.file))
	, m_fileRemaining(plan.fileSize)
	, m_bytesSent(0)
{
}

WriteProgress ResponseWriter::WriteSome(const int socket)
{
	while (true)
	{
		if (m_offset == m_pending.size())
		{
			if (m_fileRemaining == 0)
			{
				return WriteProgress::Done;
			}
			if (!RefillFromFile())
			{
				return WriteProgress::Failed;
			}
		}

		const ssize_t written
			= write(socket, m_pending.data() + m_offset, m_pending.size() - m_offset);
		if (written < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			return errno == EAGAIN || errno == EWOULDBLOCK ? WriteProgress::WouldBlock
														   : WriteProgress::Failed;
		}

		m_offset += static_cast<std::size_t>(written);
		m_bytesSent += static_cast<std::uint64_t>(written);
	}
}

HttpStatus ResponseWriter::Status() const noexcept
{
	return m_status;
}

std::uint64_t ResponseWriter::BytesSent() const noexcept
{
	return m_bytesSent;
}

bool ResponseWriter::RefillFromFile()
{
	const std::size_t wanted
		= static_cast<std::size_t>(std::min<std::uint64_t>(FILE_CHUNK_BYTES, m_fileRemaining));
	m_pending.resize(wanted);

	ssize_t received = 0;
	do
	{
		received = read(m_file.Get(), m_pending.data(), wanted);
	} while (received < 0 && errno == EINTR);

	if (received <= 0)
	{
		return false;
	}

	m_pending.resize(static_cast<std::size_t>(received));
	m_offset = 0;
	m_fileRemaining -= static_cast<std::uint64_t>(received);

	return true;
}

struct RequestRecord
{
	std::string peer = {};
	std::string requestLine = {};
	std::optional<HttpStatus> status = {};
	std::uint64_t bytesSent = 0;
	Clock::duration duration = {};
	std::string worker = {};
	std::string note = {};
};

enum class ConnectionStep
{
	NeedRead,
	NeedWrite,
	Finished,
};

std::string Shorten(const std::string_view text)
{
	if (text.size() <= MAX_LOGGED_LINE)
	{
		return std::string(text);
	}

	return std::string(text.substr(0, MAX_LOGGED_LINE)) + "…";
}

class Connection
{
public:
	Connection(AcceptedClient client, const StaticSite& site, ClientBlocking blocking);

	ConnectionStep OnReadable();
	ConnectionStep OnWritable();
	ConnectionStep Respond(HttpStatus status);
	void Abort(std::string note);

	[[nodiscard]] bool IsWriting() const noexcept;
	[[nodiscard]] Clock::time_point LastActivity() const noexcept;
	[[nodiscard]] RequestRecord BuildRecord(std::string worker) const;

private:
	ConnectionStep HandleReceived();
	ConnectionStep HandleReadFailure(int code);
	ConnectionStep StartResponse(ResponsePlan plan);
	[[nodiscard]] bool IsBlocking() const noexcept;

	AcceptedClient m_client;
	const StaticSite& m_site;
	ClientBlocking m_blocking;
	std::string m_received;
	std::string m_requestLine;
	std::optional<ResponseWriter> m_writer;
	std::string m_note;
	Clock::time_point m_startedAt;
	Clock::time_point m_lastActivity;
};

Connection::Connection(AcceptedClient client, const StaticSite& site, const ClientBlocking blocking)
	: m_client(std::move(client))
	, m_site(site)
	, m_blocking(blocking)
	, m_startedAt(Clock::now())
	, m_lastActivity(m_startedAt)
{
}

ConnectionStep Connection::OnReadable()
{
	std::array<char, READ_CHUNK_BYTES> chunk{};
	const ssize_t received = read(m_client.socket.Get(), chunk.data(), chunk.size());

	if (received > 0)
	{
		m_lastActivity = Clock::now();
		m_received.append(chunk.data(), static_cast<std::size_t>(received));
		return HandleReceived();
	}

	if (received == 0)
	{
		m_note = "клиент закрыл соединение, не дождавшись ответа";
		return ConnectionStep::Finished;
	}

	return HandleReadFailure(errno);
}

ConnectionStep Connection::OnWritable()
{
	const std::uint64_t sentBefore = m_writer->BytesSent();
	const WriteProgress progress = m_writer->WriteSome(m_client.socket.Get());
	if (m_writer->BytesSent() != sentBefore)
	{
		m_lastActivity = Clock::now();
	}

	switch (progress)
	{
	case WriteProgress::Done: return ConnectionStep::Finished;
	case WriteProgress::Failed:
		m_note = "клиент оборвал соединение во время отправки";
		return ConnectionStep::Finished;
	case WriteProgress::WouldBlock: break;
	}

	if (IsBlocking())
	{
		m_note = "таймаут отправки";
		return ConnectionStep::Finished;
	}

	return ConnectionStep::NeedWrite;
}

ConnectionStep Connection::Respond(const HttpStatus status)
{
	return StartResponse(BuildErrorPlan(status, false));
}

void Connection::Abort(std::string note)
{
	m_note = std::move(note);
}

bool Connection::IsWriting() const noexcept
{
	return m_writer.has_value();
}

Clock::time_point Connection::LastActivity() const noexcept
{
	return m_lastActivity;
}

RequestRecord Connection::BuildRecord(std::string worker) const
{
	RequestRecord record;
	record.peer = m_client.peer;
	record.requestLine = m_requestLine.empty() ? "—" : m_requestLine;
	record.duration = Clock::now() - m_startedAt;
	record.worker = std::move(worker);
	record.note = m_note;

	if (m_writer.has_value())
	{
		record.status = m_writer->Status();
		record.bytesSent = m_writer->BytesSent();
	}

	return record;
}

ConnectionStep Connection::HandleReceived()
{
	const std::optional<std::size_t> headerEnd = FindHeaderEnd(m_received);
	if (!headerEnd.has_value())
	{
		const bool isTooLarge = m_received.size() > MAX_HEADER_BYTES;
		return isTooLarge ? Respond(HttpStatus::HeaderFieldsTooLarge) : ConnectionStep::NeedRead;
	}

	const std::string_view head(m_received.data(), *headerEnd);
	m_requestLine = Shorten(ExtractRequestLine(head));

	const RequestParseResult parsed = ParseRequestHead(head);
	if (!parsed.request.has_value())
	{
		return Respond(parsed.error);
	}

	return StartResponse(BuildResponse(*parsed.request, m_site));
}

ConnectionStep Connection::HandleReadFailure(const int code)
{
	if (code == EINTR)
	{
		return ConnectionStep::NeedRead;
	}

	if (code == EAGAIN || code == EWOULDBLOCK)
	{
		return IsBlocking() ? Respond(HttpStatus::RequestTimeout) : ConnectionStep::NeedRead;
	}

	m_note = std::format("ошибка чтения: {}", std::strerror(code));
	return ConnectionStep::Finished;
}

ConnectionStep Connection::StartResponse(ResponsePlan plan)
{
	m_writer.emplace(std::move(plan));
	return ConnectionStep::NeedWrite;
}

bool Connection::IsBlocking() const noexcept
{
	return m_blocking == ClientBlocking::Blocking;
}

void ServeBlocking(Connection& connection)
{
	ConnectionStep step = ConnectionStep::NeedRead;
	while (step == ConnectionStep::NeedRead)
	{
		step = connection.OnReadable();
	}

	while (step == ConnectionStep::NeedWrite)
	{
		step = connection.OnWritable();
	}
}

std::string FormatClock()
{
	const auto now = std::chrono::system_clock::now();
	const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
	const auto milliseconds
		= std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count()
		% 1000;

	std::tm local{};
	localtime_r(&seconds, &local);

	return std::format(
		"{:02}:{:02}:{:02}.{:03}", local.tm_hour, local.tm_min, local.tm_sec, milliseconds);
}

std::string FormatSize(const std::uint64_t bytes)
{
	if (bytes < 1024)
	{
		return std::format("{} Б", bytes);
	}
	if (bytes < 1024 * 1024)
	{
		return std::format("{:.1f} КБ", static_cast<double>(bytes) / 1024.0);
	}

	return std::format("{:.1f} МБ", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

std::string_view ChooseStatusStyle(const std::optional<HttpStatus> status) noexcept
{
	if (!status.has_value())
	{
		return STYLE_GRAY;
	}

	const int code = ToCode(*status);
	if (code < 300)
	{
		return STYLE_GREEN;
	}
	if (code < 400)
	{
		return STYLE_CYAN;
	}

	return code < 500 ? STYLE_YELLOW : STYLE_RED;
}

std::string DescribeStatus(const std::optional<HttpStatus> status)
{
	if (!status.has_value())
	{
		return "без ответа";
	}

	return std::format("{} {}", ToCode(*status), ReasonPhrase(*status));
}

std::string FormatRecord(const RequestRecord& record)
{
	const double milliseconds = std::chrono::duration<double, std::milli>(record.duration).count();
	std::string line = std::format("{}{}{}  {:<21}  {}  →  {}{}{}  {}  {:.1f} мс  {}[{}]{}",
		STYLE_GRAY,
		FormatClock(),
		STYLE_RESET,
		record.peer,
		record.requestLine,
		ChooseStatusStyle(record.status),
		DescribeStatus(record.status),
		STYLE_RESET,
		FormatSize(record.bytesSent),
		milliseconds,
		STYLE_GRAY,
		record.worker,
		STYLE_RESET);

	if (!record.note.empty())
	{
		line += std::format("  {}({}){}", STYLE_GRAY, record.note, STYLE_RESET);
	}

	return line;
}

class AccessLog
{
public:
	explicit AccessLog(bool isVerbose);

	void Record(const RequestRecord& record);
	void Info(std::string_view message);
	void PrintSummary() const;

private:
	bool m_isVerbose;
	mutable std::mutex m_mutex;
	std::map<int, std::uint64_t> m_statusCounts;
	std::uint64_t m_connections;
	std::uint64_t m_bytesSent;
	Clock::time_point m_startedAt;
};

AccessLog::AccessLog(const bool isVerbose)
	: m_isVerbose(isVerbose)
	, m_connections(0)
	, m_bytesSent(0)
	, m_startedAt(Clock::now())
{
}

void AccessLog::Record(const RequestRecord& record)
{
	const std::string line = m_isVerbose ? FormatRecord(record) : std::string();

	const std::lock_guard lock(m_mutex);
	++m_connections;
	m_bytesSent += record.bytesSent;
	++m_statusCounts[record.status.has_value() ? ToCode(*record.status) : 0];

	if (m_isVerbose)
	{
		std::cout << line << std::endl;
	}
}

void AccessLog::Info(const std::string_view message)
{
	const std::lock_guard lock(m_mutex);
	std::cout << STYLE_GRAY << "* " << message << STYLE_RESET << std::endl;
}

void AccessLog::PrintSummary() const
{
	const std::lock_guard lock(m_mutex);
	const double seconds = std::chrono::duration<double>(Clock::now() - m_startedAt).count();

	std::string statuses;
	for (const auto& [code, count] : m_statusCounts)
	{
		statuses += statuses.empty() ? "" : ", ";
		statuses += code == 0 ? std::format("без ответа: {}", count)
							  : std::format("{}: {}", code, count);
	}

	std::cout << std::endl
			  << STYLE_BOLD << "Итог работы" << STYLE_RESET << std::endl
			  << "  Время работы:  " << std::format("{:.1f} с", seconds) << std::endl
			  << "  Соединений:    " << m_connections
			  << (statuses.empty() ? "" : " (" + statuses + ")") << std::endl
			  << "  Отправлено:    " << FormatSize(m_bytesSent) << std::endl;
}

std::string CurrentThreadLabel()
{
	static std::atomic<int> nextId = 0;
	thread_local const int id = ++nextId;

	return std::format("поток {}", id);
}

struct ServerContext
{
	int listener;
	const StaticSite& site;
	AccessLog& log;
	const ShutdownSignal& signal;
};

class IServerMode
{
public:
	virtual ~IServerMode() = default;

	virtual void Run(const ServerContext& context) = 0;
	[[nodiscard]] virtual std::string_view Name() const noexcept = 0;
};

bool WaitForClientOrShutdown(const ServerContext& context)
{
	std::array<pollfd, 2> watched = {{
		{context.listener, POLLIN, 0},
		{context.signal.Descriptor(), POLLIN, 0},
	}};

	while (true)
	{
		const int ready = poll(watched.data(), watched.size(), -1);
		if (ready < 0 && errno == EINTR)
		{
			continue;
		}
		AssertIsSucceeded(ready, "Ошибка poll");

		if ((watched[1].revents & POLLIN) != 0)
		{
			context.log.Info(std::format(
				"Получен {}: новые подключения не принимаются", context.signal.Consume()));
			return false;
		}
		if ((watched[0].revents & POLLIN) != 0)
		{
			return true;
		}
	}
}

void ServeClient(AcceptedClient client, const ServerContext& context, std::string worker)
{
	ApplyBlockingTimeouts(client.socket.Get());
	Connection connection(std::move(client), context.site, ClientBlocking::Blocking);

	ServeBlocking(connection);
	context.log.Record(connection.BuildRecord(std::move(worker)));
}

void RejectBusy(AcceptedClient client, const ServerContext& context)
{
	ApplyBlockingTimeouts(client.socket.Get());
	Connection connection(std::move(client), context.site, ClientBlocking::Blocking);

	ConnectionStep step = connection.Respond(HttpStatus::ServiceUnavailable);
	while (step == ConnectionStep::NeedWrite)
	{
		step = connection.OnWritable();
	}

	connection.Abort("все рабочие потоки заняты");
	context.log.Record(connection.BuildRecord("главный поток"));
}

class SequentialServer final : public IServerMode
{
public:
	void Run(const ServerContext& context) override;
	[[nodiscard]] std::string_view Name() const noexcept override;
};

void SequentialServer::Run(const ServerContext& context)
{
	while (WaitForClientOrShutdown(context))
	{
		std::optional<AcceptedClient> client
			= AcceptClient(context.listener, ClientBlocking::Blocking);
		if (client.has_value())
		{
			ServeClient(std::move(*client), context, "главный поток");
		}
	}
}

std::string_view SequentialServer::Name() const noexcept
{
	return "последовательный";
}

class WorkerRegistry
{
public:
	WorkerRegistry() = default;
	~WorkerRegistry() noexcept;

	WorkerRegistry(const WorkerRegistry&) = delete;
	WorkerRegistry& operator=(const WorkerRegistry&) = delete;
	WorkerRegistry(WorkerRegistry&&) = delete;
	WorkerRegistry& operator=(WorkerRegistry&&) = delete;

	template <typename Task> void Start(Task task);

	void ReapFinished();
	void JoinAll();
	[[nodiscard]] std::size_t ActiveCount() const;

private:
	void MarkFinished(std::thread::id id);

	mutable std::mutex m_mutex;
	std::unordered_map<std::thread::id, std::thread> m_threads;
	std::vector<std::thread::id> m_finished;
};

WorkerRegistry::~WorkerRegistry() noexcept
{
	JoinAll();
}

template <typename Task> void WorkerRegistry::Start(Task task)
{
	const std::lock_guard lock(m_mutex);
	std::thread thread([this, task = std::move(task)]() mutable {
		task();
		MarkFinished(std::this_thread::get_id());
	});
	m_threads.emplace(thread.get_id(), std::move(thread));
}

void WorkerRegistry::ReapFinished()
{
	std::vector<std::thread> finished;
	{
		const std::lock_guard lock(m_mutex);
		for (const std::thread::id id : m_finished)
		{
			auto node = m_threads.extract(id);
			if (!node.empty())
			{
				finished.push_back(std::move(node.mapped()));
			}
		}
		m_finished.clear();
	}

	for (std::thread& thread : finished)
	{
		thread.join();
	}
}

void WorkerRegistry::JoinAll()
{
	std::vector<std::thread> threads;
	{
		const std::lock_guard lock(m_mutex);
		for (auto& [id, thread] : m_threads)
		{
			threads.push_back(std::move(thread));
		}
		m_threads.clear();
		m_finished.clear();
	}

	for (std::thread& thread : threads)
	{
		if (thread.joinable())
		{
			thread.join();
		}
	}
}

std::size_t WorkerRegistry::ActiveCount() const
{
	const std::lock_guard lock(m_mutex);
	return m_threads.size() - m_finished.size();
}

void WorkerRegistry::MarkFinished(const std::thread::id id)
{
	const std::lock_guard lock(m_mutex);
	m_finished.push_back(id);
}

void ServeInWorker(AcceptedClient client, const ServerContext& context)
{
	try
	{
		ServeClient(std::move(client), context, CurrentThreadLabel());
	}
	catch (const std::exception& error)
	{
		context.log.Info(std::format("Ошибка в рабочем потоке: {}", error.what()));
	}
}

class ThreadedServer final : public IServerMode
{
public:
	void Run(const ServerContext& context) override;
	[[nodiscard]] std::string_view Name() const noexcept override;

private:
	void Dispatch(AcceptedClient client, const ServerContext& context);

	WorkerRegistry m_workers;
};

void ThreadedServer::Run(const ServerContext& context)
{
	while (WaitForClientOrShutdown(context))
	{
		m_workers.ReapFinished();

		std::optional<AcceptedClient> client
			= AcceptClient(context.listener, ClientBlocking::Blocking);
		if (client.has_value())
		{
			Dispatch(std::move(*client), context);
		}
	}

	m_workers.ReapFinished();
	context.log.Info(
		std::format("Ожидаем завершения активных потоков: {}", m_workers.ActiveCount()));
	m_workers.JoinAll();
}

std::string_view ThreadedServer::Name() const noexcept
{
	return "многопоточный, поток на соединение";
}

void ThreadedServer::Dispatch(AcceptedClient client, const ServerContext& context)
{
	if (m_workers.ActiveCount() >= MAX_WORKER_THREADS)
	{
		RejectBusy(std::move(client), context);
		return;
	}

	m_workers.Start([client = std::move(client), &context]() mutable {
		ServeInWorker(std::move(client), context);
	});
}

struct AsyncSlot
{
	std::unique_ptr<Connection> connection = {};
	std::uint32_t events = 0;
};

class EpollServer final : public IServerMode
{
public:
	EpollServer();

	void Run(const ServerContext& context) override;
	[[nodiscard]] std::string_view Name() const noexcept override;

private:
	void Watch(int descriptor, std::uint32_t events);
	void Unwatch(int descriptor);
	void Dispatch(const epoll_event& event, const ServerContext& context);
	void AcceptAll(const ServerContext& context);
	void BeginShutdown(const ServerContext& context);
	void HandleClient(int descriptor, const ServerContext& context);
	void Apply(int descriptor, ConnectionStep step, const ServerContext& context);
	void Close(int descriptor, const ServerContext& context);
	void ExpireIdle(const ServerContext& context);
	void CloseRemaining(const ServerContext& context);
	[[nodiscard]] bool IsDone() const;

	FileDescriptor m_epoll;
	std::unordered_map<int, AsyncSlot> m_slots;
	bool m_isStopping;
	Clock::time_point m_deadline;
};

FileDescriptor CreateEpoll()
{
	FileDescriptor epoll(epoll_create1(EPOLL_CLOEXEC));
	AssertIsSucceeded(epoll.Get(), "Не удалось создать epoll");

	return epoll;
}

EpollServer::EpollServer()
	: m_epoll(CreateEpoll())
	, m_isStopping(false)
{
}

void EpollServer::Run(const ServerContext& context)
{
	Watch(context.listener, EPOLLIN);
	Watch(context.signal.Descriptor(), EPOLLIN);

	std::array<epoll_event, EPOLL_BATCH> events{};
	while (!IsDone())
	{
		const int count = epoll_wait(m_epoll.Get(), events.data(), EPOLL_BATCH, EPOLL_TICK_MS);
		if (count < 0 && errno == EINTR)
		{
			continue;
		}
		AssertIsSucceeded(count, "Ошибка epoll_wait");

		for (int index = 0; index < count; ++index)
		{
			Dispatch(events[static_cast<std::size_t>(index)], context);
		}
		ExpireIdle(context);
	}

	CloseRemaining(context);
}

std::string_view EpollServer::Name() const noexcept
{
	return "асинхронный, epoll в одном потоке";
}

void EpollServer::Watch(const int descriptor, const std::uint32_t events)
{
	epoll_event event{};
	event.events = events;
	event.data.fd = descriptor;
	AssertIsSucceeded(
		epoll_ctl(m_epoll.Get(), EPOLL_CTL_ADD, descriptor, &event), "Ошибка epoll_ctl ADD");
}

void EpollServer::Unwatch(const int descriptor)
{
	epoll_ctl(m_epoll.Get(), EPOLL_CTL_DEL, descriptor, nullptr);
}

void EpollServer::Dispatch(const epoll_event& event, const ServerContext& context)
{
	const int descriptor = event.data.fd;
	if (descriptor == context.signal.Descriptor())
	{
		BeginShutdown(context);
	}
	else if (descriptor == context.listener)
	{
		AcceptAll(context);
	}
	else
	{
		HandleClient(descriptor, context);
	}
}

void EpollServer::AcceptAll(const ServerContext& context)
{
	while (!m_isStopping && m_slots.size() < MAX_ASYNC_CONNECTIONS)
	{
		std::optional<AcceptedClient> client
			= AcceptClient(context.listener, ClientBlocking::NonBlocking);
		if (!client.has_value())
		{
			return;
		}

		const int descriptor = client->socket.Get();
		auto connection = std::make_unique<Connection>(
			std::move(*client), context.site, ClientBlocking::NonBlocking);
		m_slots.emplace(descriptor, AsyncSlot{std::move(connection), EPOLLIN});
		Watch(descriptor, EPOLLIN);
	}
}

void EpollServer::BeginShutdown(const ServerContext& context)
{
	const std::string signal = context.signal.Consume();
	if (m_isStopping)
	{
		return;
	}

	m_isStopping = true;
	m_deadline = Clock::now() + SHUTDOWN_GRACE;
	Unwatch(context.listener);

	context.log.Info(
		std::format("Получен {}: новые подключения не принимаются, активных соединений: {}",
			signal,
			m_slots.size()));
}

void EpollServer::HandleClient(const int descriptor, const ServerContext& context)
{
	const auto found = m_slots.find(descriptor);
	if (found == m_slots.end())
	{
		return;
	}

	Connection& connection = *found->second.connection;
	const bool wasWriting = connection.IsWriting();

	ConnectionStep step = wasWriting ? connection.OnWritable() : connection.OnReadable();
	if (step == ConnectionStep::NeedWrite && !wasWriting)
	{
		step = connection.OnWritable();
	}

	Apply(descriptor, step, context);
}

void EpollServer::Apply(
	const int descriptor, const ConnectionStep step, const ServerContext& context)
{
	if (step == ConnectionStep::Finished)
	{
		Close(descriptor, context);
		return;
	}

	AsyncSlot& slot = m_slots.at(descriptor);
	const std::uint32_t wanted = step == ConnectionStep::NeedWrite ? EPOLLOUT : EPOLLIN;
	if (slot.events == wanted)
	{
		return;
	}

	epoll_event event{};
	event.events = wanted;
	event.data.fd = descriptor;
	AssertIsSucceeded(
		epoll_ctl(m_epoll.Get(), EPOLL_CTL_MOD, descriptor, &event), "Ошибка epoll_ctl MOD");
	slot.events = wanted;
}

void EpollServer::Close(const int descriptor, const ServerContext& context)
{
	const auto found = m_slots.find(descriptor);
	if (found == m_slots.end())
	{
		return;
	}

	context.log.Record(found->second.connection->BuildRecord("epoll"));
	Unwatch(descriptor);
	m_slots.erase(found);
}

void EpollServer::ExpireIdle(const ServerContext& context)
{
	const Clock::time_point now = Clock::now();

	std::vector<int> expired;
	for (const auto& [descriptor, slot] : m_slots)
	{
		if (now - slot.connection->LastActivity() > IDLE_TIMEOUT)
		{
			expired.push_back(descriptor);
		}
	}

	for (const int descriptor : expired)
	{
		Connection& connection = *m_slots.at(descriptor).connection;
		if (connection.IsWriting())
		{
			connection.Abort("клиент не принимает данные, соединение закрыто по таймауту");
			Close(descriptor, context);
			continue;
		}

		ConnectionStep step = connection.Respond(HttpStatus::RequestTimeout);
		step = step == ConnectionStep::NeedWrite ? connection.OnWritable() : step;
		Apply(descriptor, step, context);
	}
}

void EpollServer::CloseRemaining(const ServerContext& context)
{
	std::vector<int> remaining;
	for (const auto& [descriptor, slot] : m_slots)
	{
		remaining.push_back(descriptor);
	}

	for (const int descriptor : remaining)
	{
		m_slots.at(descriptor).connection->Abort("прервано при остановке сервера");
		Close(descriptor, context);
	}
}

bool EpollServer::IsDone() const
{
	return m_isStopping && (m_slots.empty() || Clock::now() >= m_deadline);
}

enum class ServerModeKind
{
	Async,
	Threaded,
	Sequential,
};

std::unique_ptr<IServerMode> CreateServerMode(const ServerModeKind kind)
{
	switch (kind)
	{
	case ServerModeKind::Async: return std::make_unique<EpollServer>();
	case ServerModeKind::Threaded: return std::make_unique<ThreadedServer>();
	case ServerModeKind::Sequential: return std::make_unique<SequentialServer>();
	}

	return std::make_unique<SequentialServer>();
}

std::optional<unsigned int> ParseUnsigned(const std::string_view text) noexcept
{
	unsigned int value = 0;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	if (error != std::errc{} || end != text.data() + text.size())
	{
		return std::nullopt;
	}

	return value;
}

bool IsUserPort(const std::string_view text)
{
	const std::optional<unsigned int> port = ParseUnsigned(text);
	return port.has_value() && *port >= MIN_USER_PORT && *port <= MAX_PORT;
}

bool IsExistingDirectory(const std::string_view text)
{
	std::error_code error;
	return std::filesystem::is_directory(std::filesystem::path(std::string(text)), error);
}

struct LaunchPlan
{
	ServerModeKind mode = ServerModeKind::Async;
	ServerConfig config = {};
};

LaunchPlan AskLaunchPlan()
{
	const survey::Survey prompt;
	LaunchPlan plan;

	plan.mode
		= prompt.Select<ServerModeKind>("Режим работы сервера")
			  .Option("Асинхронный — epoll, один поток (+2)", ServerModeKind::Async)
			  .Option("Многопоточный — поток на каждое соединение (+1)", ServerModeKind::Threaded)
			  .Option("Последовательный — клиенты по очереди (базовый)", ServerModeKind::Sequential)
			  .Ask();

	plan.config.port = static_cast<std::uint16_t>(
		*ParseUnsigned(prompt.Input("Порт")
						   .Default(std::to_string(DEFAULT_PORT))
						   .Validate(IsUserPort, "Нужен порт от 1024 до 65535")
						   .Ask()));

	plan.config.root = prompt.Input("Папка с сайтом")
						   .Default(DEFAULT_ROOT)
						   .Validate(IsExistingDirectory, "Папка не найдена")
						   .Ask();

	plan.config.isPublic = prompt.Confirm("Открыть доступ из локальной сети (0.0.0.0)?", false);
	plan.config.isVerbose = prompt.Confirm("Печатать каждый запрос в консоль?", true);

	return plan;
}

void PrintBanner(const ServerConfig& config, const StaticSite& site, const IServerMode& mode)
{
	std::cout << std::endl
			  << STYLE_BOLD << "Сервер запущен: " << STYLE_GREEN
			  << "http://localhost:" << config.port << "/" << STYLE_RESET << std::endl
			  << "  Режим:  " << mode.Name() << std::endl
			  << "  Сайт:   " << site.Root().string() << std::endl
			  << "  Адрес:  "
			  << (config.isPublic ? "0.0.0.0 (вся локальная сеть)"
								  : "127.0.0.1 (только этот компьютер)")
			  << std::endl
			  << STYLE_GRAY << "Ctrl+C — корректная остановка" << STYLE_RESET << std::endl
			  << std::endl;
}

void RunServer(const LaunchPlan& plan)
{
	const StaticSite site(plan.config.root);
	AccessLog log(plan.config.isVerbose);
	const ShutdownSignal signal;
	const FileDescriptor listener = OpenListener(plan.config);

	const std::unique_ptr<IServerMode> mode = CreateServerMode(plan.mode);
	PrintBanner(plan.config, site, *mode);

	mode->Run(ServerContext{listener.Get(), site, log, signal});

	log.PrintSummary();
	std::cout << "Сервер остановлен, все сокеты закрыты" << std::endl;
}
} // namespace

int main()
{
	try
	{
		RunServer(AskLaunchPlan());
	}
	catch (const survey::CancelledError& error)
	{
		std::cout << std::endl << error.what() << std::endl;
		return EXIT_FAILURE;
	}
	catch (const std::exception& error)
	{
		std::cerr << std::endl
				  << STYLE_RED << "[Ошибка] " << error.what() << STYLE_RESET << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
