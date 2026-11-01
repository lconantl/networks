#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <survey/CancelledError.hpp>
#include <survey/SecretString.hpp>
#include <survey/Survey.hpp>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <wincrypt.h>
#else
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#ifdef SMTP_CLIENT_HAS_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

class Application
{
public:
	Application();
	~Application();

	void Run() const;
};

struct Message
{
	std::string from = {};
	std::vector<std::string> to = {};
	std::vector<std::string> cc = {};
	std::string subject = {};
	std::string text = {};
	std::vector<std::filesystem::path> attachments = {};
};

struct Credentials
{
	std::string login = {};
	survey::SecretString password = {};
};

enum class Security
{
	None,
	StartTls,
	ImplicitTls,
};

enum class LogMode
{
	Full,
	Summary,
};

struct ServerSettings
{
	std::string host = {};
	std::uint16_t port = 25;
	Security security = Security::None;
	bool verifyCertificate = true;
};

struct SendPlan
{
	ServerSettings server = {};
	std::optional<Credentials> credentials = {};
	Message message = {};
	std::vector<std::string> bcc = {};
	std::optional<std::filesystem::path> emlPath = {};
	std::optional<std::filesystem::path> logPath = {};
	LogMode logMode = LogMode::Full;
	bool shouldSend = true;
};

enum class LogDirection
{
	Client,
	Server,
	Info,
};

class ISessionLog
{
public:
	virtual ~ISessionLog() = default;

	virtual void Record(LogDirection direction, std::string_view text) = 0;
};

struct MessageStamp
{
	std::string boundary = {};
	std::string messageId = {};
	std::string date = {};
};

MessageStamp CreateMessageStamp(std::string_view from);

struct SmtpReply
{
	int code = 0;
	std::vector<std::string> lines = {};

	[[nodiscard]] std::string Text() const;
};

struct ReplyLine
{
	int code = 0;
	bool isLast = true;
	std::string text = {};
};

ReplyLine ParseReplyLine(std::string_view line);

void SaveMessage(
	const SendPlan& plan, const MessageStamp& stamp, const std::filesystem::path& path);

SmtpReply DeliverMessage(const SendPlan& plan, const MessageStamp& stamp, ISessionLog& log);

bool IsNotBlank(std::string_view text);

bool IsPort(std::string_view text);

std::uint16_t ParsePort(std::string_view text);

bool IsReadableFile(std::string_view text);

bool IsEmptyOrReadableFile(std::string_view text);

std::string UnescapeNewlines(std::string_view text);

SendPlan RunSendWizard(const survey::Survey& prompt);

struct ServerPreset
{
	std::string label = {};
	ServerSettings settings = {};
	bool requiresAuth = false;
	bool isLocal = false;
};

std::vector<ServerPreset> BuildServerPresets(bool isTlsSupported);

std::uint16_t DefaultPort(Security security) noexcept;

class CompositeSessionLog final : public ISessionLog
{
public:
	void Add(std::unique_ptr<ISessionLog> log);

	void Record(LogDirection direction, std::string_view text) override;

private:
	std::vector<std::unique_ptr<ISessionLog>> m_logs;
};

class ConsoleSessionLog final : public ISessionLog
{
public:
	explicit ConsoleSessionLog(bool isVerbose) noexcept;

	void Record(LogDirection direction, std::string_view text) override;

private:
	bool m_isVerbose;
};

class FileSessionLog final : public ISessionLog
{
public:
	explicit FileSessionLog(const std::filesystem::path& path);

	void Record(LogDirection direction, std::string_view text) override;

private:
	std::ofstream m_file;
};

bool IsEmailAddress(std::string_view text) noexcept;

std::vector<std::string> SplitAddresses(std::string_view text);

bool IsAddressList(std::string_view text);

bool IsOptionalAddressList(std::string_view text);

inline constexpr std::size_t BASE64_LINE_INPUT_BYTES = 57;

std::string EncodeBase64(std::string_view data);

std::string EncodeBase64Lines(std::string_view data);

bool IsAscii(std::string_view text) noexcept;

std::string EncodeHeaderText(std::string_view text);

std::string BuildNameParameter(std::string_view fileName);

std::string BuildFilenameParameter(std::string_view fileName);

class IByteSink
{
public:
	virtual ~IByteSink() = default;

	virtual void Write(std::string_view data) = 0;
};

void WriteMessage(const Message& message, const MessageStamp& stamp, IByteSink& sink);

std::uint64_t EstimateMessageSize(const Message& message);

std::string DetectContentType(const std::filesystem::path& path);

struct addrinfo;

class AddressList final
{
public:
	AddressList(const std::string& host, std::uint16_t port);

	[[nodiscard]] const addrinfo* First() const noexcept;

private:
	struct Deleter
	{
		void operator()(addrinfo* list) const noexcept;
	};

	std::unique_ptr<addrinfo, Deleter> m_list;
};

class NetworkError : public std::runtime_error
{
public:
	explicit NetworkError(const std::string& message);
};

class NetworkSession final
{
public:
	NetworkSession();
	~NetworkSession() noexcept;

	NetworkSession(const NetworkSession&) = delete;
	NetworkSession& operator=(const NetworkSession&) = delete;
	NetworkSession(NetworkSession&&) = delete;
	NetworkSession& operator=(NetworkSession&&) = delete;
};

#ifdef _WIN32
using NativeSocket = std::uintptr_t;
#else
using NativeSocket = int;
#endif

class Socket final
{
public:
	explicit Socket(NativeSocket handle) noexcept;
	~Socket() noexcept;

	Socket(const Socket&) = delete;
	Socket& operator=(const Socket&) = delete;
	Socket(Socket&& other) noexcept;
	Socket& operator=(Socket&& other) noexcept;

	[[nodiscard]] std::size_t ReadSome(std::span<char> buffer);
	void WriteAll(std::string_view data);
	void SetTimeout(std::chrono::seconds timeout);

	[[nodiscard]] NativeSocket Native() const noexcept;
	[[nodiscard]] bool IsValid() const noexcept;

private:
	void Close() noexcept;

	NativeSocket m_handle;
};

NativeSocket InvalidSocket() noexcept;

Socket ConnectTcp(const std::string& host, std::uint16_t port, std::chrono::seconds timeout);

std::string ReadHostName();

class FileSink final : public IByteSink
{
public:
	explicit FileSink(const std::filesystem::path& path);

	void Write(std::string_view data) override;

private:
	std::filesystem::path m_path;
	std::ofstream m_file;
};

class StringSink final : public IByteSink
{
public:
	void Write(std::string_view data) override;

	[[nodiscard]] const std::string& Text() const noexcept;

private:
	std::string m_text;
};

struct Capabilities
{
	bool isExtended = false;
	bool hasStartTls = false;
	bool hasEightBitMime = false;
	std::set<std::string> authMechanisms = {};
	std::optional<std::uint64_t> maxMessageSize = {};
};

Capabilities ParseCapabilities(const SmtpReply& ehloReply);

enum class AuthMethod
{
	Plain,
	Login,
};

AuthMethod ChooseAuthMethod(const Capabilities& capabilities);

survey::SecretString BuildPlainToken(std::string_view login, std::string_view password);

survey::SecretString EncodeSecret(std::string_view secret);

struct DotStuffingState
{
	bool isLineStart = true;
	char previous = '\0';
};

std::string StuffDots(std::string_view chunk, DotStuffingState& state);

struct TlsSettings
{
	std::string host;
	bool verifyCertificate = true;
};

class ITransport
{
public:
	virtual ~ITransport() = default;

	virtual std::size_t Read(std::span<char> buffer) = 0;
	virtual void Write(std::string_view data) = 0;
	virtual std::unique_ptr<ITransport> StartTls(const TlsSettings& settings) = 0;
	[[nodiscard]] virtual std::string Describe() const = 0;
};

class SmtpChannel
{
public:
	SmtpChannel(std::unique_ptr<ITransport> transport, ISessionLog& log);

	void SendCommand(std::string_view command);
	void SendCommand(std::string_view command, std::string_view loggedAs);
	void WriteRaw(std::string_view data);
	void StartTls(const TlsSettings& settings);

	[[nodiscard]] SmtpReply ReadReply();
	[[nodiscard]] ISessionLog& Log() const noexcept;

private:
	[[nodiscard]] std::string ReadLine();
	void FillBuffer();

	std::unique_ptr<ITransport> m_transport;
	ISessionLog& m_log;
	std::string m_buffer;
};

class DataSink final : public IByteSink
{
public:
	explicit DataSink(SmtpChannel& channel);

	void Write(std::string_view data) override;
	void Finish();

private:
	void Flush();
	void CapturePreview(std::string_view data);
	void LogContent() const;

	SmtpChannel& m_channel;
	DotStuffingState m_state;
	std::string m_buffer;
	std::string m_preview;
	std::uint64_t m_totalBytes;
	bool m_isPreviewComplete;
};

struct Envelope
{
	std::string from = {};
	std::vector<std::string> recipients = {};
};

using MessageWriterFunction = std::function<void(IByteSink&)>;

class SmtpClient
{
public:
	SmtpClient(std::unique_ptr<ITransport> transport, ISessionLog& log);
	~SmtpClient() noexcept;

	SmtpClient(const SmtpClient&) = delete;
	SmtpClient& operator=(const SmtpClient&) = delete;
	SmtpClient(SmtpClient&&) = delete;
	SmtpClient& operator=(SmtpClient&&) = delete;

	void Greet();
	void Hello(const std::string& clientName);
	void StartTls(const TlsSettings& settings);
	void Authenticate(const Credentials& credentials);
	SmtpReply Send(const Envelope& envelope, const MessageWriterFunction& writeMessage);
	void Quit();

	[[nodiscard]] const Capabilities& GetCapabilities() const noexcept;

private:
	SmtpReply Exchange(
		std::string_view command, std::initializer_list<int> expected, std::string_view stage);
	void AuthenticatePlain(const Credentials& credentials);
	void AuthenticateLogin(const Credentials& credentials);
	void SendRecipients(const Envelope& envelope);

	SmtpChannel m_channel;
	std::string m_clientName;
	Capabilities m_capabilities;
	bool m_isQuitSent;
};

class SmtpError : public std::runtime_error
{
public:
	SmtpError(const std::string& message, int code);

	[[nodiscard]] int Code() const noexcept;

private:
	int m_code;
};

std::filesystem::path PathFromUtf8(std::string_view text);

std::string PathToUtf8(const std::filesystem::path& path);

std::string NormalizePathInput(std::string_view text);

#ifdef SMTP_CLIENT_HAS_TLS

std::string CollectOpenSslErrors();

#endif

#ifdef SMTP_CLIENT_HAS_TLS

struct ssl_ctx_st;

void LoadSystemCertificates(ssl_ctx_st* context);

#endif

#ifdef SMTP_CLIENT_HAS_TLS

struct ssl_ctx_st;

class TlsContext final
{
public:
	explicit TlsContext(bool verifyCertificate);

	[[nodiscard]] ssl_ctx_st* Get() const noexcept;

private:
	struct Deleter
	{
		void operator()(ssl_ctx_st* context) const noexcept;
	};

	std::unique_ptr<ssl_ctx_st, Deleter> m_context;
};

#endif

class TlsError : public std::runtime_error
{
public:
	explicit TlsError(const std::string& message);
};

std::unique_ptr<ITransport> CreateTlsTransport(Socket socket, const TlsSettings& settings);

bool IsTlsSupported() noexcept;

#ifdef SMTP_CLIENT_HAS_TLS

struct ssl_st;

class TlsTransport final : public ITransport
{
public:
	TlsTransport(Socket socket, const TlsSettings& settings);
	~TlsTransport() noexcept override;

	TlsTransport(const TlsTransport&) = delete;
	TlsTransport& operator=(const TlsTransport&) = delete;
	TlsTransport(TlsTransport&&) = delete;
	TlsTransport& operator=(TlsTransport&&) = delete;

	std::size_t Read(std::span<char> buffer) override;
	void Write(std::string_view data) override;
	std::unique_ptr<ITransport> StartTls(const TlsSettings& settings) override;
	[[nodiscard]] std::string Describe() const override;

private:
	struct SslDeleter
	{
		void operator()(ssl_st* ssl) const noexcept;
	};

	Socket m_socket;
	TlsContext m_context;
	std::unique_ptr<ssl_st, SslDeleter> m_ssl;
};

#endif

class PlainTransport final : public ITransport
{
public:
	explicit PlainTransport(Socket socket) noexcept;

	std::size_t Read(std::span<char> buffer) override;
	void Write(std::string_view data) override;
	std::unique_ptr<ITransport> StartTls(const TlsSettings& settings) override;
	[[nodiscard]] std::string Describe() const override;

private:
	Socket m_socket;
};

class ConsoleEncoding final
{
public:
	ConsoleEncoding() noexcept;
	~ConsoleEncoding() noexcept;

	ConsoleEncoding(const ConsoleEncoding&) = delete;
	ConsoleEncoding& operator=(const ConsoleEncoding&) = delete;
	ConsoleEncoding(ConsoleEncoding&&) = delete;
	ConsoleEncoding& operator=(ConsoleEncoding&&) = delete;

private:
#ifdef _WIN32
	unsigned int m_previousOutputCodePage;
	unsigned int m_previousInputCodePage;
#endif
};

namespace
{
constexpr auto TITLE_STYLE = "\x1b[1m";
constexpr auto SUCCESS_STYLE = "\x1b[32m";
constexpr auto APP_RESET_STYLE = "\x1b[0m";

void PrintBanner()
{
	std::cout << TITLE_STYLE << "SMTP-клиент — лабораторная работа №7" << APP_RESET_STYLE
			  << std::endl
			  << "Стрелки — выбор, Пробел — отметить, Enter — подтвердить, Esc — выход" << std::endl
			  << std::endl;
}

CompositeSessionLog BuildSessionLog(const SendPlan& plan)
{
	CompositeSessionLog log;
	log.Add(std::make_unique<ConsoleSessionLog>(plan.logMode == LogMode::Full));
	if (plan.logPath.has_value())
	{
		log.Add(std::make_unique<FileSessionLog>(*plan.logPath));
	}

	return log;
}

void SaveIfRequested(const SendPlan& plan, const MessageStamp& stamp)
{
	if (!plan.emlPath.has_value())
	{
		return;
	}

	SaveMessage(plan, stamp, *plan.emlPath);
	std::cout << "Письмо сохранено в " << PathToUtf8(*plan.emlPath) << std::endl;
}

void PrintDelivered(const SmtpReply& reply, const SendPlan& plan)
{
	std::cout << std::endl
			  << SUCCESS_STYLE << "Письмо принято сервером: " << reply.code << " " << reply.Text()
			  << APP_RESET_STYLE << std::endl;

	if (plan.logPath.has_value())
	{
		std::cout << "Лог сессии: " << PathToUtf8(*plan.logPath) << std::endl;
	}
}

void SendIfRequested(const SendPlan& plan, const MessageStamp& stamp)
{
	if (!plan.shouldSend)
	{
		std::cout << "Отправка пропущена" << std::endl;
		return;
	}

	std::cout << std::endl;
	CompositeSessionLog log = BuildSessionLog(plan);
	PrintDelivered(DeliverMessage(plan, stamp, log), plan);
}
} // namespace

Application::Application() = default;

Application::~Application() = default;

void Application::Run() const
{
	const survey::Survey prompt;
	PrintBanner();

	const SendPlan plan = RunSendWizard(prompt);
	const MessageStamp stamp = CreateMessageStamp(plan.message.from);

	SaveIfRequested(plan, stamp);
	SendIfRequested(plan, stamp);
}

namespace
{
constexpr auto NETWORK_TIMEOUT = std::chrono::seconds(30);
constexpr double BYTES_PER_MEGABYTE = 1024.0 * 1024.0;

TlsSettings BuildTlsSettings(const ServerSettings& server)
{
	return {server.host, server.verifyCertificate};
}

std::unique_ptr<ITransport> OpenTransport(const ServerSettings& server)
{
	auto transport
		= std::make_unique<PlainTransport>(ConnectTcp(server.host, server.port, NETWORK_TIMEOUT));
	if (server.security == Security::ImplicitTls)
	{
		return transport->StartTls(BuildTlsSettings(server));
	}

	return transport;
}

Envelope BuildEnvelope(const SendPlan& plan)
{
	Envelope envelope{plan.message.from, plan.message.to};
	envelope.recipients.insert(
		envelope.recipients.end(), plan.message.cc.begin(), plan.message.cc.end());
	envelope.recipients.insert(envelope.recipients.end(), plan.bcc.begin(), plan.bcc.end());

	return envelope;
}

void AssertIsSizeAccepted(const Capabilities& capabilities, const std::uint64_t estimatedSize)
{
	if (capabilities.maxMessageSize.has_value() && estimatedSize > *capabilities.maxMessageSize)
	{
		throw SmtpError(std::format("Письмо (~{:.1f} МБ) больше лимита сервера ({:.1f} МБ)",
							static_cast<double>(estimatedSize) / BYTES_PER_MEGABYTE,
							static_cast<double>(*capabilities.maxMessageSize) / BYTES_PER_MEGABYTE),
			0);
	}
}

void Introduce(SmtpClient& client, const SendPlan& plan)
{
	client.Greet();
	client.Hello(ReadHostName());

	if (plan.server.security == Security::StartTls)
	{
		client.StartTls(BuildTlsSettings(plan.server));
	}

	if (plan.credentials.has_value())
	{
		client.Authenticate(*plan.credentials);
	}
}
} // namespace

void SaveMessage(const SendPlan& plan, const MessageStamp& stamp, const std::filesystem::path& path)
{
	FileSink sink(path);
	WriteMessage(plan.message, stamp, sink);
}

SmtpReply DeliverMessage(const SendPlan& plan, const MessageStamp& stamp, ISessionLog& log)
{
	const NetworkSession network;
	log.Record(
		LogDirection::Info, std::format("Подключение к {}:{}", plan.server.host, plan.server.port));

	std::unique_ptr<ITransport> transport = OpenTransport(plan.server);
	log.Record(LogDirection::Info, "Соединение установлено: " + transport->Describe());

	SmtpClient client(std::move(transport), log);
	Introduce(client, plan);
	AssertIsSizeAccepted(client.GetCapabilities(), EstimateMessageSize(plan.message));

	SmtpReply reply = client.Send(BuildEnvelope(plan),
		[&plan, &stamp](IByteSink& sink) { WriteMessage(plan.message, stamp, sink); });
	client.Quit();

	return reply;
}

namespace
{
constexpr auto ESCAPED_NEWLINE = "\\n";

std::optional<unsigned int> TryParseNumber(const std::string_view text) noexcept
{
	unsigned int value = 0;
	const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
	if (error != std::errc{} || end != text.data() + text.size())
	{
		return std::nullopt;
	}

	return value;
}

void AssertIsPort(const bool isPort)
{
	if (!isPort)
	{
		throw std::invalid_argument("Порт должен быть числом от 1 до 65535");
	}
}
} // namespace

bool IsNotBlank(const std::string_view text)
{
	return text.find_first_not_of(" \t") != std::string_view::npos;
}

bool IsPort(const std::string_view text)
{
	const std::optional<unsigned int> value = TryParseNumber(text);
	return value.has_value() && *value >= 1 && *value <= std::numeric_limits<std::uint16_t>::max();
}

std::uint16_t ParsePort(const std::string_view text)
{
	AssertIsPort(IsPort(text));
	return static_cast<std::uint16_t>(*TryParseNumber(text));
}

bool IsReadableFile(const std::string_view text)
{
	std::error_code error;
	return std::filesystem::is_regular_file(PathFromUtf8(NormalizePathInput(text)), error);
}

bool IsEmptyOrReadableFile(const std::string_view text)
{
	return NormalizePathInput(text).empty() || IsReadableFile(text);
}

std::string UnescapeNewlines(const std::string_view text)
{
	std::string result;
	std::size_t start = 0;

	for (std::size_t found = text.find(ESCAPED_NEWLINE); found != std::string_view::npos;
		 found = text.find(ESCAPED_NEWLINE, start))
	{
		result.append(text.substr(start, found - start));
		result += '\n';
		start = found + 2;
	}

	result.append(text.substr(start));
	return result;
}

namespace
{
constexpr auto LOCAL_SENDER = "student@test.local";
constexpr auto LOCAL_RECIPIENT = "teacher@test.local";
constexpr auto DEFAULT_SUBJECT = "Лабораторная работа №7";
constexpr auto DEFAULT_BODY = "Hello! This is test message!";
constexpr auto DEFAULT_EML = "message.eml";
constexpr auto DEFAULT_LOG = "smtp-session.log";
constexpr auto ADDRESS_ERROR = "Нужны адреса вида name@domain через запятую";

enum class Extra
{
	Cc,
	Bcc,
	Attachments,
	SaveEml,
	SaveLog,
};

enum class BodySource
{
	Console,
	File,
};

bool HasExtra(const std::vector<Extra>& extras, const Extra extra)
{
	return std::find(extras.begin(), extras.end(), extra) != extras.end();
}

Security AskSecurity(const survey::Survey& prompt)
{
	auto builder = prompt.Select<Security>("Шифрование");
	builder.Option("Без шифрования (порт 25)", Security::None);

	if (IsTlsSupported())
	{
		builder.Option("STARTTLS (порт 587)", Security::StartTls);
		builder.Option("TLS сразу (порт 465)", Security::ImplicitTls);
	}

	return builder.Ask();
}

ServerPreset AskCustomServer(const survey::Survey& prompt)
{
	ServerPreset preset;
	preset.settings.host = prompt.Input("Адрес SMTP-сервера")
							   .Validate(IsNotBlank, "Адрес не может быть пустым")
							   .Ask();
	preset.settings.security = AskSecurity(prompt);
	preset.settings.port
		= ParsePort(prompt.Input("Порт")
						.Default(std::to_string(DefaultPort(preset.settings.security)))
						.Validate(IsPort, "Порт — число от 1 до 65535")
						.Ask());

	const bool isEncrypted = preset.settings.security != Security::None;
	if (isEncrypted)
	{
		preset.settings.verifyCertificate = prompt.Confirm("Проверять сертификат сервера?", true);
	}

	preset.requiresAuth = prompt.Confirm("Нужна авторизация (AUTH)?", isEncrypted);
	return preset;
}

ServerPreset AskServer(const survey::Survey& prompt)
{
	const std::vector<ServerPreset> presets = BuildServerPresets(IsTlsSupported());

	auto builder = prompt.Select<std::size_t>("Куда отправляем?");
	for (std::size_t index = 0; index < presets.size(); ++index)
	{
		builder.Option(presets[index].label, index);
	}
	builder.Option("Другой сервер", presets.size());

	const std::size_t choice = builder.Ask();
	return choice < presets.size() ? presets[choice] : AskCustomServer(prompt);
}

Credentials AskCredentials(const survey::Survey& prompt)
{
	Credentials credentials;
	credentials.login = prompt.Input("Логин (обычно полный адрес почты)")
							.Validate(IsNotBlank, "Логин не может быть пустым")
							.Ask();
	credentials.password
		= prompt.Password("Пароль (для Яндекса, Mail.ru и Gmail — пароль приложения)")
			  .Validate(IsNotBlank, "Пароль не может быть пустым")
			  .Ask();

	return credentials;
}

std::string SuggestSender(const ServerPreset& preset, const std::optional<Credentials>& credentials)
{
	if (credentials.has_value() && IsEmailAddress(credentials->login))
	{
		return credentials->login;
	}

	return preset.isLocal ? LOCAL_SENDER : "";
}

std::string AskSender(const survey::Survey& prompt, const std::string& suggestion)
{
	auto builder = prompt.Input("От кого (MAIL FROM)");
	if (!suggestion.empty())
	{
		builder.Default(suggestion);
	}

	return builder.Validate(IsEmailAddress, "Нужен адрес вида name@domain").Ask();
}

std::vector<std::string> AskRecipients(const survey::Survey& prompt, const bool isLocal)
{
	auto builder = prompt.Input("Кому (несколько адресов — через запятую)");
	if (isLocal)
	{
		builder.Default(LOCAL_RECIPIENT);
	}

	return SplitAddresses(builder.Validate(IsAddressList, ADDRESS_ERROR).Ask());
}

std::vector<std::string> AskOptionalAddresses(
	const survey::Survey& prompt, const std::string& title)
{
	return SplitAddresses(prompt.Input(title).Validate(IsOptionalAddressList, ADDRESS_ERROR).Ask());
}

std::vector<Extra> AskExtras(const survey::Survey& prompt)
{
	return prompt.MultiSelect<Extra>("Что ещё добавить? (Enter — ничего)")
		.Option("Копия (Cc)", Extra::Cc)
		.Option("Скрытая копия (Bcc)", Extra::Bcc)
		.Option("Вложения", Extra::Attachments)
		.Option("Сохранить письмо в .eml", Extra::SaveEml)
		.Option("Записать SMTP-диалог в файл", Extra::SaveLog)
		.Ask();
}

void AssertIsTextFileRead(const std::ifstream& file, const std::filesystem::path& path)
{
	if (!file.is_open())
	{
		throw std::runtime_error("Не удалось открыть файл с текстом " + PathToUtf8(path));
	}
}

std::string ReadTextFile(const std::filesystem::path& path)
{
	std::ifstream file(path, std::ios::binary);
	AssertIsTextFileRead(file, path);

	return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::string AskBody(const survey::Survey& prompt)
{
	const BodySource source = prompt.Select<BodySource>("Текст письма")
								  .Option("Ввести в консоли", BodySource::Console)
								  .Option("Загрузить из файла", BodySource::File)
								  .Ask();

	if (source == BodySource::Console)
	{
		return UnescapeNewlines(
			prompt.Input("Текст (\\n — новая строка)").Default(DEFAULT_BODY).Ask());
	}

	const std::string path = prompt.Input("Путь к файлу с текстом (UTF-8)")
								 .Validate(IsReadableFile, "Файл не найден")
								 .Ask();
	return ReadTextFile(PathFromUtf8(NormalizePathInput(path)));
}

std::vector<std::filesystem::path> AskAttachments(const survey::Survey& prompt)
{
	std::vector<std::filesystem::path> attachments;

	while (true)
	{
		const std::string title
			= std::format("Вложение {} — путь к файлу (Enter — готово)", attachments.size() + 1);
		const std::string input = NormalizePathInput(
			prompt.Input(title).Validate(IsEmptyOrReadableFile, "Файл не найден").Ask());

		if (input.empty())
		{
			return attachments;
		}
		attachments.push_back(PathFromUtf8(input));
	}
}

std::filesystem::path AskOutputPath(
	const survey::Survey& prompt, const std::string& title, const std::string& suggestion)
{
	return PathFromUtf8(NormalizePathInput(prompt.Input(title)
											   .Default(suggestion)
											   .Validate(IsNotBlank, "Путь не может быть пустым")
											   .Ask()));
}

LogMode AskLogMode(const survey::Survey& prompt)
{
	return prompt.Select<LogMode>("Как показывать SMTP-диалог?")
		.Option("Полностью — строки S: и C:", LogMode::Full)
		.Option("Только итог", LogMode::Summary)
		.Ask();
}

void AskMessage(const survey::Survey& prompt, const ServerPreset& preset, SendPlan& plan)
{
	plan.message.from = AskSender(prompt, SuggestSender(preset, plan.credentials));
	plan.message.to = AskRecipients(prompt, preset.isLocal);
	plan.message.subject = prompt.Input("Тема").Default(DEFAULT_SUBJECT).Ask();
	plan.message.text = AskBody(prompt);
}

void AskExtrasDetails(
	const survey::Survey& prompt, const std::vector<Extra>& extras, SendPlan& plan)
{
	if (HasExtra(extras, Extra::Cc))
	{
		plan.message.cc = AskOptionalAddresses(prompt, "Копия (Cc), через запятую");
	}
	if (HasExtra(extras, Extra::Bcc))
	{
		plan.bcc = AskOptionalAddresses(prompt, "Скрытая копия (Bcc), через запятую");
	}
	if (HasExtra(extras, Extra::Attachments))
	{
		plan.message.attachments = AskAttachments(prompt);
	}
	if (HasExtra(extras, Extra::SaveEml))
	{
		plan.emlPath = AskOutputPath(prompt, "Файл для сохранения письма", DEFAULT_EML);
	}
	if (HasExtra(extras, Extra::SaveLog))
	{
		plan.logPath = AskOutputPath(prompt, "Файл для лога сессии", DEFAULT_LOG);
	}
}
} // namespace

SendPlan RunSendWizard(const survey::Survey& prompt)
{
	SendPlan plan;

	const ServerPreset preset = AskServer(prompt);
	plan.server = preset.settings;
	if (preset.requiresAuth)
	{
		plan.credentials = AskCredentials(prompt);
	}

	AskMessage(prompt, preset, plan);
	AskExtrasDetails(prompt, AskExtras(prompt), plan);

	plan.logMode = AskLogMode(prompt);
	plan.shouldSend = prompt.Confirm("Отправить письмо?", true);

	return plan;
}

namespace
{
constexpr std::uint16_t PLAIN_PORT = 25;
constexpr std::uint16_t SUBMISSION_PORT = 587;
constexpr std::uint16_t SMTPS_PORT = 465;
constexpr std::uint16_t LOCAL_TEST_PORT = 2525;

ServerPreset BuildLocalPreset()
{
	return {"Локальный тестовый сервер — 127.0.0.1:2525, без шифрования",
		{"127.0.0.1", LOCAL_TEST_PORT, Security::None},
		false,
		true};
}

std::vector<ServerPreset> BuildPublicPresets()
{
	return {
		{"Яндекс — smtp.yandex.ru:587, STARTTLS",
			{"smtp.yandex.ru", SUBMISSION_PORT, Security::StartTls},
			true,
			false},
		{"Mail.ru — smtp.mail.ru:465, TLS",
			{"smtp.mail.ru", SMTPS_PORT, Security::ImplicitTls},
			true,
			false},
		{"Gmail — smtp.gmail.com:587, STARTTLS",
			{"smtp.gmail.com", SUBMISSION_PORT, Security::StartTls},
			true,
			false},
	};
}
} // namespace

std::vector<ServerPreset> BuildServerPresets(const bool isTlsSupported)
{
	std::vector<ServerPreset> presets = {BuildLocalPreset()};
	if (isTlsSupported)
	{
		const std::vector<ServerPreset> publicPresets = BuildPublicPresets();
		presets.insert(presets.end(), publicPresets.begin(), publicPresets.end());
	}

	return presets;
}

std::uint16_t DefaultPort(const Security security) noexcept
{
	switch (security)
	{
	case Security::None: return PLAIN_PORT;
	case Security::StartTls: return SUBMISSION_PORT;
	case Security::ImplicitTls: return SMTPS_PORT;
	}

	return PLAIN_PORT;
}

namespace
{
void AssertIsLogSet(const std::unique_ptr<ISessionLog>& log)
{
	if (log == nullptr)
	{
		throw std::invalid_argument("Нельзя добавить пустой лог");
	}
}
} // namespace

void CompositeSessionLog::Add(std::unique_ptr<ISessionLog> log)
{
	AssertIsLogSet(log);
	m_logs.push_back(std::move(log));
}

void CompositeSessionLog::Record(const LogDirection direction, const std::string_view text)
{
	for (const auto& log : m_logs)
	{
		log->Record(direction, text);
	}
}

namespace
{
constexpr auto CLIENT_STYLE = "\x1b[36m";
constexpr auto SERVER_STYLE = "\x1b[32m";
constexpr auto INFO_STYLE = "\x1b[90m";
constexpr auto LOG_RESET_STYLE = "\x1b[0m";

std::string FormatLine(const LogDirection direction, const std::string_view text)
{
	switch (direction)
	{
	case LogDirection::Client:
		return std::string(CLIENT_STYLE) + "C: " + std::string(text) + LOG_RESET_STYLE;
	case LogDirection::Server:
		return std::string(SERVER_STYLE) + "S: " + std::string(text) + LOG_RESET_STYLE;
	case LogDirection::Info:
		return std::string(INFO_STYLE) + "* " + std::string(text) + LOG_RESET_STYLE;
	}

	return std::string(text);
}
} // namespace

ConsoleSessionLog::ConsoleSessionLog(const bool isVerbose) noexcept
	: m_isVerbose(isVerbose)
{
}

void ConsoleSessionLog::Record(const LogDirection direction, const std::string_view text)
{
	if (m_isVerbose || direction == LogDirection::Info)
	{
		std::cout << FormatLine(direction, text) << std::endl;
	}
}

namespace
{
std::string_view DescribePrefix(const LogDirection direction) noexcept
{
	switch (direction)
	{
	case LogDirection::Client: return "C: ";
	case LogDirection::Server: return "S: ";
	case LogDirection::Info: return "* ";
	}

	return "";
}

void AssertIsLogOpen(const std::ofstream& file, const std::filesystem::path& path)
{
	if (!file.is_open())
	{
		throw std::runtime_error("Не удалось открыть файл лога " + PathToUtf8(path));
	}
}
} // namespace

FileSessionLog::FileSessionLog(const std::filesystem::path& path)
	: m_file(path, std::ios::binary | std::ios::trunc)
{
	AssertIsLogOpen(m_file, path);
}

void FileSessionLog::Record(const LogDirection direction, const std::string_view text)
{
	m_file << DescribePrefix(direction) << text << std::endl;
}

namespace
{
constexpr auto ADDRESS_WHITESPACE = " \t";
constexpr std::string_view FORBIDDEN_SYMBOLS = " <>(),;:\"[]\\";

std::string_view TrimAddressView(const std::string_view text) noexcept
{
	const std::size_t first = text.find_first_not_of(ADDRESS_WHITESPACE);
	if (first == std::string_view::npos)
	{
		return {};
	}

	return text.substr(first, text.find_last_not_of(ADDRESS_WHITESPACE) - first + 1);
}

bool HasForbiddenSymbols(const std::string_view text) noexcept
{
	return text.find_first_of(FORBIDDEN_SYMBOLS) != std::string_view::npos;
}

bool IsDomain(const std::string_view domain) noexcept
{
	const std::size_t dot = domain.find('.');
	return dot != std::string_view::npos && dot > 0 && domain.back() != '.'
		&& domain.find("..") == std::string_view::npos;
}

bool IsSeparator(const char symbol) noexcept
{
	return symbol == ',' || symbol == ';';
}
} // namespace

bool IsEmailAddress(const std::string_view text) noexcept
{
	const std::size_t at = text.find('@');
	if (at == std::string_view::npos || at == 0 || text.rfind('@') != at
		|| HasForbiddenSymbols(text))
	{
		return false;
	}

	return IsDomain(text.substr(at + 1));
}

std::vector<std::string> SplitAddresses(const std::string_view text)
{
	std::vector<std::string> addresses;
	std::size_t start = 0;

	while (start <= text.size())
	{
		const auto separator = std::find_if(
			text.begin() + static_cast<std::ptrdiff_t>(start), text.end(), IsSeparator);
		const std::size_t end = static_cast<std::size_t>(separator - text.begin());

		const std::string_view address = TrimAddressView(text.substr(start, end - start));
		if (!address.empty())
		{
			addresses.emplace_back(address);
		}
		start = end + 1;
	}

	return addresses;
}

bool IsAddressList(const std::string_view text)
{
	const std::vector<std::string> addresses = SplitAddresses(text);
	return !addresses.empty()
		&& std::all_of(addresses.begin(), addresses.end(), [](const auto& address) {
			   return IsEmailAddress(address);
		   });
}

bool IsOptionalAddressList(const std::string_view text)
{
	return TrimAddressView(text).empty() || IsAddressList(text);
}

namespace
{
constexpr std::string_view ALPHABET
	= "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
constexpr char PADDING = '=';
constexpr auto BASE64_LINE_END = "\r\n";

unsigned int ByteAt(const std::string_view data, const std::size_t index) noexcept
{
	return index < data.size() ? static_cast<unsigned char>(data[index]) : 0U;
}

void AppendGroup(std::string& output, const std::string_view data, const std::size_t offset)
{
	const std::size_t available = data.size() - offset;
	const unsigned int group
		= (ByteAt(data, offset) << 16) | (ByteAt(data, offset + 1) << 8) | ByteAt(data, offset + 2);

	output += ALPHABET[(group >> 18) & 0x3F];
	output += ALPHABET[(group >> 12) & 0x3F];
	output += available > 1 ? ALPHABET[(group >> 6) & 0x3F] : PADDING;
	output += available > 2 ? ALPHABET[group & 0x3F] : PADDING;
}
} // namespace

std::string EncodeBase64(const std::string_view data)
{
	std::string output;
	output.reserve((data.size() + 2) / 3 * 4);

	for (std::size_t offset = 0; offset < data.size(); offset += 3)
	{
		AppendGroup(output, data, offset);
	}

	return output;
}

std::string EncodeBase64Lines(const std::string_view data)
{
	std::string output;
	for (std::size_t offset = 0; offset < data.size(); offset += BASE64_LINE_INPUT_BYTES)
	{
		const std::size_t length = std::min(BASE64_LINE_INPUT_BYTES, data.size() - offset);
		output += EncodeBase64(data.substr(offset, length));
		output += BASE64_LINE_END;
	}

	return output;
}

namespace
{
constexpr std::size_t MAX_WORD_SOURCE_BYTES = 45;
constexpr auto WORD_PREFIX = "=?UTF-8?B?";
constexpr auto WORD_SUFFIX = "?=";
constexpr auto FOLD = "\r\n ";
constexpr std::string_view HEX_DIGITS = "0123456789ABCDEF";

bool IsContinuationByte(const char symbol) noexcept
{
	return (static_cast<unsigned char>(symbol) & 0xC0) == 0x80;
}

std::size_t FindChunkEnd(const std::string_view text, const std::size_t start) noexcept
{
	std::size_t end = std::min(start + MAX_WORD_SOURCE_BYTES, text.size());
	while (end < text.size() && end > start && IsContinuationByte(text[end]))
	{
		--end;
	}

	return end;
}

std::string EncodeWord(const std::string_view chunk)
{
	return WORD_PREFIX + EncodeBase64(chunk) + WORD_SUFFIX;
}

bool IsAttributeChar(const unsigned char symbol) noexcept
{
	return (symbol >= 'a' && symbol <= 'z') || (symbol >= 'A' && symbol <= 'Z')
		|| (symbol >= '0' && symbol <= '9') || symbol == '.' || symbol == '-' || symbol == '_';
}

std::string PercentEncode(const std::string_view text)
{
	std::string encoded;
	for (const char raw : text)
	{
		const auto symbol = static_cast<unsigned char>(raw);
		if (IsAttributeChar(symbol))
		{
			encoded += raw;
			continue;
		}

		encoded += '%';
		encoded += HEX_DIGITS[symbol >> 4];
		encoded += HEX_DIGITS[symbol & 0x0F];
	}

	return encoded;
}

std::string QuoteAscii(const std::string_view text)
{
	std::string quoted = "\"";
	for (const char symbol : text)
	{
		if (symbol == '"' || symbol == '\\')
		{
			quoted += '\\';
		}
		quoted += symbol;
	}

	return quoted + "\"";
}
} // namespace

bool IsAscii(const std::string_view text) noexcept
{
	for (const char symbol : text)
	{
		if (static_cast<unsigned char>(symbol) >= 0x80)
		{
			return false;
		}
	}

	return true;
}

std::string EncodeHeaderText(const std::string_view text)
{
	if (IsAscii(text))
	{
		return std::string(text);
	}

	std::string encoded;
	for (std::size_t start = 0; start < text.size();)
	{
		const std::size_t end = FindChunkEnd(text, start);
		encoded += encoded.empty() ? "" : FOLD;
		encoded += EncodeWord(text.substr(start, end - start));
		start = end;
	}

	return encoded;
}

std::string BuildNameParameter(const std::string_view fileName)
{
	if (IsAscii(fileName))
	{
		return "name=" + QuoteAscii(fileName);
	}

	return "name=\"" + EncodeWord(fileName) + "\"";
}

std::string BuildFilenameParameter(const std::string_view fileName)
{
	if (IsAscii(fileName))
	{
		return "filename=" + QuoteAscii(fileName);
	}

	return std::format("filename*=UTF-8''{}", PercentEncode(fileName));
}

namespace
{
constexpr auto BOUNDARY_PREFIX = "=_smtp_client_";
constexpr auto FALLBACK_DOMAIN = "localhost";
constexpr std::array<const char*, 7> WEEKDAYS = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
constexpr std::array<const char*, 12> MONTHS
	= {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::string GenerateToken()
{
	std::random_device device;
	std::mt19937_64 generator(device());

	return std::format("{:016x}{:016x}", generator(), generator());
}

std::string_view ExtractDomain(const std::string_view address)
{
	const std::size_t at = address.rfind('@');
	if (at == std::string_view::npos || at + 1 >= address.size())
	{
		return FALLBACK_DOMAIN;
	}

	return address.substr(at + 1);
}

std::tm ToUtc(const std::time_t moment)
{
	std::tm utc{};
#ifdef _WIN32
	gmtime_s(&utc, &moment);
#else
	gmtime_r(&moment, &utc);
#endif
	return utc;
}

std::string FormatRfc5322Date(const std::tm& utc)
{
	return std::format("{}, {:02} {} {} {:02}:{:02}:{:02} +0000",
		WEEKDAYS[static_cast<std::size_t>(utc.tm_wday)],
		utc.tm_mday,
		MONTHS[static_cast<std::size_t>(utc.tm_mon)],
		utc.tm_year + 1900,
		utc.tm_hour,
		utc.tm_min,
		utc.tm_sec);
}
} // namespace

MessageStamp CreateMessageStamp(const std::string_view from)
{
	const std::string token = GenerateToken();

	return {
		BOUNDARY_PREFIX + token,
		std::format("<{}@{}>", token, ExtractDomain(from)),
		FormatRfc5322Date(ToUtc(std::time(nullptr))),
	};
}

namespace
{
constexpr auto CRLF = "\r\n";
constexpr auto MAILER = "smtp-client-wsl (lab 7)";
constexpr std::size_t MAX_SEVEN_BIT_LINE = 998;
constexpr std::size_t ATTACHMENT_CHUNK_BYTES = BASE64_LINE_INPUT_BYTES * 1152;
constexpr std::uint64_t HEADERS_RESERVE_BYTES = 2048;
constexpr auto PREAMBLE = "This is a multi-part message in MIME format.";

std::string NormalizeLineEndings(const std::string_view text)
{
	std::string normalized;
	normalized.reserve(text.size() + text.size() / 32);

	for (std::size_t index = 0; index < text.size(); ++index)
	{
		const char symbol = text[index];
		if (symbol == '\r')
		{
			continue;
		}
		if (symbol == '\n')
		{
			normalized += CRLF;
			continue;
		}
		normalized += symbol;
	}

	return normalized;
}

std::size_t FindLongestLine(const std::string_view text) noexcept
{
	std::size_t longest = 0;
	std::size_t current = 0;
	for (const char symbol : text)
	{
		current = symbol == '\n' ? 0 : current + 1;
		longest = std::max(longest, current);
	}

	return longest;
}

bool IsSevenBitSafe(const std::string_view text) noexcept
{
	return IsAscii(text) && FindLongestLine(text) <= MAX_SEVEN_BIT_LINE;
}

std::string JoinAddresses(const std::vector<std::string>& addresses)
{
	std::string joined;
	for (const auto& address : addresses)
	{
		joined += joined.empty() ? address : "," + std::string(CRLF) + " " + address;
	}

	return joined;
}

void WriteHeader(IByteSink& sink, const std::string_view name, const std::string_view value)
{
	sink.Write(std::format("{}: {}{}", name, value, CRLF));
}

void WriteCommonHeaders(const Message& message, const MessageStamp& stamp, IByteSink& sink)
{
	WriteHeader(sink, "From", message.from);
	WriteHeader(sink, "To", JoinAddresses(message.to));
	if (!message.cc.empty())
	{
		WriteHeader(sink, "Cc", JoinAddresses(message.cc));
	}
	WriteHeader(sink, "Subject", EncodeHeaderText(message.subject));
	WriteHeader(sink, "Date", stamp.date);
	WriteHeader(sink, "Message-ID", stamp.messageId);
	WriteHeader(sink, "MIME-Version", "1.0");
	WriteHeader(sink, "X-Mailer", MAILER);
}

void WriteTextPart(const std::string& text, IByteSink& sink)
{
	const std::string normalized = NormalizeLineEndings(text);
	const bool isSevenBit = IsSevenBitSafe(normalized);

	WriteHeader(sink, "Content-Type", "text/plain; charset=utf-8");
	WriteHeader(sink, "Content-Transfer-Encoding", isSevenBit ? "7bit" : "base64");
	sink.Write(CRLF);

	sink.Write(isSevenBit ? normalized : EncodeBase64Lines(normalized));
	if (isSevenBit && !normalized.ends_with(CRLF))
	{
		sink.Write(CRLF);
	}
}

void AssertIsAttachmentOpen(const std::ifstream& file, const std::filesystem::path& path)
{
	if (!file.is_open())
	{
		throw std::runtime_error("Не удалось открыть вложение " + PathToUtf8(path));
	}
}

void AssertIsAttachmentRead(const std::ifstream& file, const std::filesystem::path& path)
{
	if (file.bad())
	{
		throw std::runtime_error("Ошибка чтения вложения " + PathToUtf8(path));
	}
}

void WriteAttachmentBody(const std::filesystem::path& path, IByteSink& sink)
{
	std::ifstream file(path, std::ios::binary);
	AssertIsAttachmentOpen(file, path);

	std::vector<char> chunk(ATTACHMENT_CHUNK_BYTES);
	while (file)
	{
		file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
		AssertIsAttachmentRead(file, path);

		const auto count = static_cast<std::size_t>(file.gcount());
		sink.Write(EncodeBase64Lines(std::string_view(chunk.data(), count)));
	}
}

void WriteAttachmentPart(const std::filesystem::path& path, IByteSink& sink)
{
	const std::string fileName = PathToUtf8(path.filename());

	WriteHeader(
		sink, "Content-Type", DetectContentType(path) + "; " + BuildNameParameter(fileName));
	WriteHeader(sink, "Content-Transfer-Encoding", "base64");
	WriteHeader(sink, "Content-Disposition", "attachment; " + BuildFilenameParameter(fileName));
	sink.Write(CRLF);

	WriteAttachmentBody(path, sink);
}

void WriteBoundary(const MessageStamp& stamp, IByteSink& sink)
{
	sink.Write(std::format("{}--{}{}", CRLF, stamp.boundary, CRLF));
}

void WriteMultipartBody(const Message& message, const MessageStamp& stamp, IByteSink& sink)
{
	WriteHeader(
		sink, "Content-Type", std::format("multipart/mixed; boundary=\"{}\"", stamp.boundary));
	sink.Write(CRLF);
	sink.Write(PREAMBLE);

	WriteBoundary(stamp, sink);
	WriteTextPart(message.text, sink);

	for (const auto& attachment : message.attachments)
	{
		WriteBoundary(stamp, sink);
		WriteAttachmentPart(attachment, sink);
	}

	sink.Write(std::format("{}--{}--{}", CRLF, stamp.boundary, CRLF));
}

std::uint64_t EstimateEncodedSize(const std::uint64_t bytes) noexcept
{
	const std::uint64_t lines = (bytes + BASE64_LINE_INPUT_BYTES - 1) / BASE64_LINE_INPUT_BYTES;
	return lines * (76 + 2);
}

std::uint64_t ReadFileSize(const std::filesystem::path& path)
{
	std::error_code error;
	const std::uintmax_t size = std::filesystem::file_size(path, error);

	return error ? 0 : static_cast<std::uint64_t>(size);
}
} // namespace

void WriteMessage(const Message& message, const MessageStamp& stamp, IByteSink& sink)
{
	WriteCommonHeaders(message, stamp, sink);

	if (message.attachments.empty())
	{
		WriteTextPart(message.text, sink);
		return;
	}

	WriteMultipartBody(message, stamp, sink);
}

std::uint64_t EstimateMessageSize(const Message& message)
{
	std::uint64_t total = HEADERS_RESERVE_BYTES + EstimateEncodedSize(message.text.size());
	for (const auto& attachment : message.attachments)
	{
		total += HEADERS_RESERVE_BYTES + EstimateEncodedSize(ReadFileSize(attachment));
	}

	return total;
}

namespace
{
constexpr auto FALLBACK_TYPE = "application/octet-stream";

constexpr std::array<std::pair<std::string_view, std::string_view>, 32> CONTENT_TYPES = {{
	{".txt", "text/plain; charset=utf-8"},
	{".md", "text/markdown; charset=utf-8"},
	{".csv", "text/csv; charset=utf-8"},
	{".log", "text/plain; charset=utf-8"},
	{".cpp", "text/plain; charset=utf-8"},
	{".hpp", "text/plain; charset=utf-8"},
	{".h", "text/plain; charset=utf-8"},
	{".py", "text/plain; charset=utf-8"},
	{".html", "text/html; charset=utf-8"},
	{".htm", "text/html; charset=utf-8"},
	{".json", "application/json"},
	{".xml", "application/xml"},
	{".pdf", "application/pdf"},
	{".zip", "application/zip"},
	{".7z", "application/x-7z-compressed"},
	{".rar", "application/vnd.rar"},
	{".doc", "application/msword"},
	{".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
	{".xls", "application/vnd.ms-excel"},
	{".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
	{".ppt", "application/vnd.ms-powerpoint"},
	{".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
	{".png", "image/png"},
	{".jpg", "image/jpeg"},
	{".jpeg", "image/jpeg"},
	{".gif", "image/gif"},
	{".webp", "image/webp"},
	{".svg", "image/svg+xml"},
	{".bmp", "image/bmp"},
	{".mp3", "audio/mpeg"},
	{".mp4", "video/mp4"},
	{".wav", "audio/wav"},
}};

std::string ToLowerAscii(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char symbol) {
		return static_cast<char>(std::tolower(symbol));
	});

	return text;
}
} // namespace

std::string DetectContentType(const std::filesystem::path& path)
{
	const std::string extension = ToLowerAscii(path.extension().string());
	for (const auto& [knownExtension, type] : CONTENT_TYPES)
	{
		if (knownExtension == extension)
		{
			return std::string(type);
		}
	}

	return FALLBACK_TYPE;
}

namespace
{
addrinfo BuildHints() noexcept
{
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;

	return hints;
}

std::string DescribeResolveError(const int code)
{
#ifdef _WIN32
	return "код " + std::to_string(code);
#else
	return gai_strerror(code);
#endif
}

addrinfo* AssertIsResolved(const int code, addrinfo* list, const std::string& host)
{
	if (code != 0 || list == nullptr)
	{
		throw NetworkError(
			"Не удалось найти адрес сервера " + host + ": " + DescribeResolveError(code));
	}

	return list;
}

addrinfo* Resolve(const std::string& host, const std::uint16_t port)
{
	const addrinfo hints = BuildHints();
	addrinfo* list = nullptr;
	const int code = getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &list);

	return AssertIsResolved(code, list, host);
}
} // namespace

void AddressList::Deleter::operator()(addrinfo* list) const noexcept
{
	freeaddrinfo(list);
}

AddressList::AddressList(const std::string& host, const std::uint16_t port)
	: m_list(Resolve(host, port))
{
}

const addrinfo* AddressList::First() const noexcept
{
	return m_list.get();
}

NetworkError::NetworkError(const std::string& message)
	: std::runtime_error(message)
{
}

namespace
{
#ifdef _WIN32
constexpr WORD WINSOCK_VERSION_2_2 = MAKEWORD(2, 2);

void AssertIsWinsockStarted(const int result)
{
	if (result != 0)
	{
		throw std::runtime_error(
			"Не удалось инициализировать Winsock, код " + std::to_string(result));
	}
}
#endif
} // namespace

#ifdef _WIN32

NetworkSession::NetworkSession()
{
	WSADATA data{};
	AssertIsWinsockStarted(WSAStartup(WINSOCK_VERSION_2_2, &data));
}

NetworkSession::~NetworkSession() noexcept
{
	WSACleanup();
}

#else

NetworkSession::NetworkSession()
{
	std::signal(SIGPIPE, SIG_IGN);
}

NetworkSession::~NetworkSession() noexcept = default;

#endif

namespace
{
#ifdef _WIN32
int LastSocketError() noexcept
{
	return WSAGetLastError();
}

bool IsTimeoutError(const int error) noexcept
{
	return error == WSAETIMEDOUT || error == WSAEWOULDBLOCK;
}

bool IsInterrupted(const int) noexcept
{
	return false;
}

long long ReadNative(const NativeSocket handle, std::span<char> buffer) noexcept
{
	return recv(static_cast<SOCKET>(handle), buffer.data(), static_cast<int>(buffer.size()), 0);
}

long long WriteNative(const NativeSocket handle, const std::string_view data) noexcept
{
	return send(static_cast<SOCKET>(handle), data.data(), static_cast<int>(data.size()), 0);
}

void CloseNative(const NativeSocket handle) noexcept
{
	closesocket(static_cast<SOCKET>(handle));
}

bool ApplyTimeout(const NativeSocket handle, const int option, const std::chrono::seconds timeout)
{
	const DWORD milliseconds = static_cast<DWORD>(timeout.count() * 1000);
	return setsockopt(static_cast<SOCKET>(handle),
			   SOL_SOCKET,
			   option,
			   reinterpret_cast<const char*>(&milliseconds),
			   sizeof(milliseconds))
		== 0;
}
#else
int LastSocketError() noexcept
{
	return errno;
}

bool IsTimeoutError(const int error) noexcept
{
	return error == EAGAIN || error == EWOULDBLOCK;
}

bool IsInterrupted(const int error) noexcept
{
	return error == EINTR;
}

long long ReadNative(const NativeSocket handle, std::span<char> buffer) noexcept
{
	return read(handle, buffer.data(), buffer.size());
}

long long WriteNative(const NativeSocket handle, const std::string_view data) noexcept
{
	return write(handle, data.data(), data.size());
}

void CloseNative(const NativeSocket handle) noexcept
{
	close(handle);
}

bool ApplyTimeout(const NativeSocket handle, const int option, const std::chrono::seconds timeout)
{
	timeval value{};
	value.tv_sec = static_cast<decltype(value.tv_sec)>(timeout.count());

	return setsockopt(handle, SOL_SOCKET, option, &value, sizeof(value)) == 0;
}
#endif

void AssertIsOpen(const bool isValid)
{
	if (!isValid)
	{
		throw NetworkError("Операция с закрытым сокетом");
	}
}

void AssertIsTransferred(const long long result, const int error, const char* operation)
{
	if (result >= 0)
	{
		return;
	}

	if (IsTimeoutError(error))
	{
		throw NetworkError("Сервер не ответил вовремя (таймаут)");
	}

	throw NetworkError(
		std::string("Ошибка сокета при ") + operation + ", код " + std::to_string(error));
}

void AssertIsTimeoutSet(const bool success)
{
	if (!success)
	{
		throw NetworkError("Не удалось установить таймаут сокета");
	}
}
} // namespace

NativeSocket InvalidSocket() noexcept
{
#ifdef _WIN32
	return static_cast<NativeSocket>(INVALID_SOCKET);
#else
	return -1;
#endif
}

Socket::Socket(const NativeSocket handle) noexcept
	: m_handle(handle)
{
}

Socket::~Socket() noexcept
{
	Close();
}

Socket::Socket(Socket&& other) noexcept
	: m_handle(std::exchange(other.m_handle, InvalidSocket()))
{
}

Socket& Socket::operator=(Socket&& other) noexcept
{
	if (this != &other)
	{
		Close();
		m_handle = std::exchange(other.m_handle, InvalidSocket());
	}

	return *this;
}

std::size_t Socket::ReadSome(const std::span<char> buffer)
{
	AssertIsOpen(IsValid());

	long long result = 0;
	do
	{
		result = ReadNative(m_handle, buffer);
	} while (result < 0 && IsInterrupted(LastSocketError()));

	AssertIsTransferred(result, LastSocketError(), "чтении");
	return static_cast<std::size_t>(result);
}

void Socket::WriteAll(std::string_view data)
{
	AssertIsOpen(IsValid());

	while (!data.empty())
	{
		const long long result = WriteNative(m_handle, data);
		if (result < 0 && IsInterrupted(LastSocketError()))
		{
			continue;
		}

		AssertIsTransferred(result, LastSocketError(), "записи");
		data.remove_prefix(static_cast<std::size_t>(result));
	}
}

void Socket::SetTimeout(const std::chrono::seconds timeout)
{
	AssertIsOpen(IsValid());
	AssertIsTimeoutSet(ApplyTimeout(m_handle, SO_RCVTIMEO, timeout));
	AssertIsTimeoutSet(ApplyTimeout(m_handle, SO_SNDTIMEO, timeout));
}

NativeSocket Socket::Native() const noexcept
{
	return m_handle;
}

bool Socket::IsValid() const noexcept
{
	return m_handle != InvalidSocket();
}

void Socket::Close() noexcept
{
	if (IsValid())
	{
		CloseNative(m_handle);
		m_handle = InvalidSocket();
	}
}

namespace
{
constexpr std::size_t HOST_NAME_CAPACITY = 256;
constexpr auto FALLBACK_HOST_NAME = "localhost";

Socket OpenSocket(const addrinfo& address) noexcept
{
	return Socket(static_cast<NativeSocket>(
		socket(address.ai_family, address.ai_socktype, address.ai_protocol)));
}

bool TryConnect(const Socket& socket, const addrinfo& address) noexcept
{
#ifdef _WIN32
	return connect(static_cast<SOCKET>(socket.Native()),
			   address.ai_addr,
			   static_cast<int>(address.ai_addrlen))
		== 0;
#else
	return connect(socket.Native(), address.ai_addr, address.ai_addrlen) == 0;
#endif
}

std::optional<Socket> ConnectToAddress(const addrinfo& address)
{
	Socket socket = OpenSocket(address);
	if (!socket.IsValid() || !TryConnect(socket, address))
	{
		return std::nullopt;
	}

	return socket;
}

Socket AssertIsConnected(
	std::optional<Socket> socket, const std::string& host, const std::uint16_t port)
{
	if (!socket.has_value())
	{
		throw NetworkError("Не удалось подключиться к " + host + ":" + std::to_string(port)
			+ ". Проверьте адрес, порт и что провайдер не блокирует порт");
	}

	return std::move(*socket);
}
} // namespace

Socket ConnectTcp(
	const std::string& host, const std::uint16_t port, const std::chrono::seconds timeout)
{
	const AddressList addresses(host, port);

	std::optional<Socket> connected;
	for (const addrinfo* address = addresses.First(); address != nullptr && !connected;
		 address = address->ai_next)
	{
		connected = ConnectToAddress(*address);
	}

	Socket socket = AssertIsConnected(std::move(connected), host, port);
	socket.SetTimeout(timeout);

	return socket;
}

std::string ReadHostName()
{
	std::array<char, HOST_NAME_CAPACITY> buffer{};
	if (gethostname(buffer.data(), static_cast<int>(buffer.size() - 1)) != 0
		|| buffer.front() == '\0')
	{
		return FALLBACK_HOST_NAME;
	}

	return buffer.data();
}

namespace
{
void AssertIsFileOpen(const std::ofstream& file, const std::filesystem::path& path)
{
	if (!file.is_open())
	{
		throw std::runtime_error("Не удалось открыть для записи файл " + PathToUtf8(path));
	}
}

void AssertIsWritten(const std::ofstream& file, const std::filesystem::path& path)
{
	if (!file.good())
	{
		throw std::runtime_error("Не удалось записать файл " + PathToUtf8(path));
	}
}
} // namespace

FileSink::FileSink(const std::filesystem::path& path)
	: m_path(path)
	, m_file(path, std::ios::binary | std::ios::trunc)
{
	AssertIsFileOpen(m_file, m_path);
}

void FileSink::Write(const std::string_view data)
{
	m_file.write(data.data(), static_cast<std::streamsize>(data.size()));
	AssertIsWritten(m_file, m_path);
}

void StringSink::Write(const std::string_view data)
{
	m_text += data;
}

const std::string& StringSink::Text() const noexcept
{
	return m_text;
}

namespace
{
void AssertIsAuthOffered(const Capabilities& capabilities)
{
	if (capabilities.authMechanisms.empty())
	{
		throw SmtpError("Сервер не предлагает AUTH. Возможно, сначала нужен STARTTLS", 0);
	}
}

void WipeTemporary(std::string& text) noexcept
{
	survey::SecretString wiped(std::move(text));
}
} // namespace

AuthMethod ChooseAuthMethod(const Capabilities& capabilities)
{
	AssertIsAuthOffered(capabilities);

	if (capabilities.authMechanisms.contains("PLAIN"))
	{
		return AuthMethod::Plain;
	}

	if (capabilities.authMechanisms.contains("LOGIN"))
	{
		return AuthMethod::Login;
	}

	throw SmtpError("Сервер не поддерживает AUTH PLAIN и AUTH LOGIN", 0);
}

survey::SecretString BuildPlainToken(const std::string_view login, const std::string_view password)
{
	std::string raw;
	raw.reserve(login.size() + password.size() + 2);
	raw += '\0';
	raw += login;
	raw += '\0';
	raw += password;

	survey::SecretString token = EncodeSecret(raw);
	WipeTemporary(raw);

	return token;
}

survey::SecretString EncodeSecret(const std::string_view secret)
{
	return survey::SecretString(EncodeBase64(secret));
}

namespace
{
std::string ToUpperAscii(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char symbol) {
		return static_cast<char>(std::toupper(symbol));
	});

	return text;
}

std::vector<std::string> SplitWords(const std::string& line)
{
	std::istringstream stream(ToUpperAscii(line));
	std::vector<std::string> words;

	for (std::string word; stream >> word;)
	{
		words.push_back(word);
	}

	return words;
}

void AddAuthMechanisms(Capabilities& capabilities, const std::vector<std::string>& words)
{
	capabilities.authMechanisms.insert(words.begin() + 1, words.end());
}

void AddLegacyAuth(Capabilities& capabilities, const std::string& keyword)
{
	std::istringstream stream(keyword.substr(keyword.find('=') + 1));
	for (std::string mechanism; std::getline(stream, mechanism, ',');)
	{
		capabilities.authMechanisms.insert(mechanism);
	}
}

void ApplySize(Capabilities& capabilities, const std::vector<std::string>& words)
{
	if (words.size() > 1 && words[1] != "0")
	{
		capabilities.maxMessageSize = std::stoull(words[1]);
	}
}

void ApplyLine(Capabilities& capabilities, const std::string& line)
{
	const std::vector<std::string> words = SplitWords(line);
	if (words.empty())
	{
		return;
	}

	const std::string& keyword = words.front();
	if (keyword == "STARTTLS")
	{
		capabilities.hasStartTls = true;
	}
	else if (keyword == "8BITMIME")
	{
		capabilities.hasEightBitMime = true;
	}
	else if (keyword == "AUTH")
	{
		AddAuthMechanisms(capabilities, words);
	}
	else if (keyword.starts_with("AUTH="))
	{
		AddLegacyAuth(capabilities, keyword);
	}
	else if (keyword == "SIZE")
	{
		ApplySize(capabilities, words);
	}
}
} // namespace

Capabilities ParseCapabilities(const SmtpReply& ehloReply)
{
	Capabilities capabilities;
	capabilities.isExtended = true;

	for (std::size_t index = 1; index < ehloReply.lines.size(); ++index)
	{
		ApplyLine(capabilities, ehloReply.lines[index]);
	}

	return capabilities;
}

namespace
{
constexpr std::size_t FLUSH_THRESHOLD = 64 * 1024;
constexpr std::size_t PREVIEW_LIMIT = 4 * 1024;
constexpr auto TERMINATOR = ".\r\n";
constexpr auto DATA_LINE_END = "\r\n";

void LogLines(ISessionLog& log, const std::string& text)
{
	std::size_t start = 0;
	while (start < text.size())
	{
		std::size_t end = text.find('\n', start);
		end = end == std::string::npos ? text.size() : end;

		std::string_view line(text.data() + start, end - start);
		if (!line.empty() && line.back() == '\r')
		{
			line.remove_suffix(1);
		}

		log.Record(LogDirection::Client, line);
		start = end + 1;
	}
}
} // namespace

DataSink::DataSink(SmtpChannel& channel)
	: m_channel(channel)
	, m_totalBytes(0)
	, m_isPreviewComplete(true)
{
}

void DataSink::Write(const std::string_view data)
{
	CapturePreview(data);
	m_totalBytes += data.size();

	m_buffer += StuffDots(data, m_state);
	if (m_buffer.size() >= FLUSH_THRESHOLD)
	{
		Flush();
	}
}

void DataSink::Finish()
{
	if (!m_state.isLineStart)
	{
		m_buffer += DATA_LINE_END;
	}

	m_buffer += TERMINATOR;
	Flush();
	LogContent();
}

void DataSink::Flush()
{
	m_channel.WriteRaw(m_buffer);
	m_buffer.clear();
}

void DataSink::CapturePreview(const std::string_view data)
{
	if (m_preview.size() + data.size() > PREVIEW_LIMIT)
	{
		m_isPreviewComplete = false;
		return;
	}

	m_preview += data;
}

void DataSink::LogContent() const
{
	ISessionLog& log = m_channel.Log();
	if (m_isPreviewComplete)
	{
		LogLines(log, m_preview);
	}
	else
	{
		log.Record(LogDirection::Client,
			std::format("[тело письма: {} байт, полностью не показывается]", m_totalBytes));
	}

	log.Record(LogDirection::Client, ".");
}

std::string StuffDots(const std::string_view chunk, DotStuffingState& state)
{
	std::string output;
	output.reserve(chunk.size() + chunk.size() / 64 + 2);

	for (const char symbol : chunk)
	{
		if (state.isLineStart && symbol == '.')
		{
			output += '.';
		}

		if (symbol == '\n' && state.previous != '\r')
		{
			output += '\r';
		}

		output += symbol;
		state.isLineStart = symbol == '\n';
		state.previous = symbol;
	}

	return output;
}

namespace
{
constexpr std::size_t READ_CHUNK_SIZE = 4096;
constexpr std::size_t MAX_LINE_LENGTH = 64 * 1024;
constexpr auto COMMAND_LINE_END = "\r\n";

void AssertIsTransportSet(const std::unique_ptr<ITransport>& transport)
{
	if (transport == nullptr)
	{
		throw std::invalid_argument("SMTP-канал создан без транспорта");
	}
}

void AssertIsNotClosed(const std::size_t received)
{
	if (received == 0)
	{
		throw NetworkError("Сервер закрыл соединение");
	}
}

void AssertIsLineShort(const std::string& buffer)
{
	if (buffer.size() > MAX_LINE_LENGTH)
	{
		throw SmtpError("Сервер прислал слишком длинную строку ответа", 0);
	}
}

void AssertIsBufferEmpty(const std::string& buffer)
{
	if (!buffer.empty())
	{
		throw SmtpError("Сервер прислал данные до начала TLS — соединение небезопасно", 0);
	}
}

void AssertIsSameCode(const SmtpReply& reply, const ReplyLine& line)
{
	if (!reply.lines.empty() && reply.code != line.code)
	{
		throw SmtpError("Коды в многострочном ответе сервера не совпадают", line.code);
	}
}

std::string TrimCarriageReturn(std::string line)
{
	if (!line.empty() && line.back() == '\r')
	{
		line.pop_back();
	}

	return line;
}
} // namespace

SmtpChannel::SmtpChannel(std::unique_ptr<ITransport> transport, ISessionLog& log)
	: m_transport(std::move(transport))
	, m_log(log)
{
	AssertIsTransportSet(m_transport);
}

void SmtpChannel::SendCommand(const std::string_view command)
{
	SendCommand(command, command);
}

void SmtpChannel::SendCommand(const std::string_view command, const std::string_view loggedAs)
{
	m_transport->Write(std::string(command) + COMMAND_LINE_END);
	m_log.Record(LogDirection::Client, loggedAs);
}

void SmtpChannel::WriteRaw(const std::string_view data)
{
	m_transport->Write(data);
}

void SmtpChannel::StartTls(const TlsSettings& settings)
{
	AssertIsBufferEmpty(m_buffer);
	m_transport = m_transport->StartTls(settings);
	m_log.Record(LogDirection::Info, "Шифрование включено: " + m_transport->Describe());
}

SmtpReply SmtpChannel::ReadReply()
{
	SmtpReply reply;

	while (true)
	{
		const std::string line = ReadLine();
		m_log.Record(LogDirection::Server, line);

		ReplyLine parsed = ParseReplyLine(line);
		AssertIsSameCode(reply, parsed);
		reply.code = parsed.code;
		reply.lines.push_back(std::move(parsed.text));

		if (parsed.isLast)
		{
			return reply;
		}
	}
}

ISessionLog& SmtpChannel::Log() const noexcept
{
	return m_log;
}

std::string SmtpChannel::ReadLine()
{
	std::size_t position = m_buffer.find('\n');
	while (position == std::string::npos)
	{
		AssertIsLineShort(m_buffer);
		FillBuffer();
		position = m_buffer.find('\n');
	}

	std::string line = m_buffer.substr(0, position);
	m_buffer.erase(0, position + 1);

	return TrimCarriageReturn(std::move(line));
}

void SmtpChannel::FillBuffer()
{
	std::array<char, READ_CHUNK_SIZE> chunk{};
	const std::size_t received = m_transport->Read(chunk);
	AssertIsNotClosed(received);

	m_buffer.append(chunk.data(), received);
}

namespace
{
constexpr int CODE_READY = 220;
constexpr int CODE_CLOSING = 221;
constexpr int CODE_AUTH_SUCCESS = 235;
constexpr int CODE_OK = 250;
constexpr int CODE_FORWARD = 251;
constexpr int CODE_AUTH_CONTINUE = 334;
constexpr int CODE_START_INPUT = 354;
constexpr int CODE_AUTH_FAILED = 535;
constexpr int FIRST_PERMANENT_ERROR = 500;
constexpr auto HIDDEN_SECRET = "********";

bool IsExpected(const SmtpReply& reply, const std::initializer_list<int> expected)
{
	return std::find(expected.begin(), expected.end(), reply.code) != expected.end();
}

void AssertIsAuthAccepted(const SmtpReply& reply)
{
	if (reply.code == CODE_AUTH_FAILED)
	{
		throw SmtpError("Неверный логин или пароль (535). Для Яндекса и Mail.ru нужен "
						"пароль приложения, а не пароль от почты",
			reply.code);
	}
}

void AssertIsExpected(
	const SmtpReply& reply, const std::initializer_list<int> expected, const std::string_view stage)
{
	if (!IsExpected(reply, expected))
	{
		throw SmtpError(
			std::format("Сервер отклонил {}: {} {}", stage, reply.code, reply.Text()), reply.code);
	}
}

void AssertIsStartTlsOffered(const Capabilities& capabilities)
{
	if (!capabilities.hasStartTls)
	{
		throw SmtpError("Сервер не предлагает STARTTLS", 0);
	}
}

void AssertHasRecipients(const Envelope& envelope)
{
	if (envelope.recipients.empty())
	{
		throw SmtpError("Не указан ни один получатель", 0);
	}
}

bool IsHeloFallbackNeeded(const SmtpReply& reply) noexcept
{
	return reply.code >= FIRST_PERMANENT_ERROR;
}
} // namespace

SmtpClient::SmtpClient(std::unique_ptr<ITransport> transport, ISessionLog& log)
	: m_channel(std::move(transport), log)
	, m_isQuitSent(false)
{
}

SmtpClient::~SmtpClient() noexcept
{
	if (m_isQuitSent)
	{
		return;
	}

	try
	{
		m_channel.SendCommand("QUIT");
	}
	catch (const std::exception&)
	{
	}
}

void SmtpClient::Greet()
{
	AssertIsExpected(m_channel.ReadReply(), {CODE_READY}, "подключение");
}

void SmtpClient::Hello(const std::string& clientName)
{
	m_clientName = clientName;
	m_channel.SendCommand("EHLO " + clientName);

	const SmtpReply reply = m_channel.ReadReply();
	if (reply.code == CODE_OK)
	{
		m_capabilities = ParseCapabilities(reply);
		return;
	}

	if (!IsHeloFallbackNeeded(reply))
	{
		AssertIsExpected(reply, {CODE_OK}, "EHLO");
	}

	m_capabilities = {};
	Exchange("HELO " + clientName, {CODE_OK}, "HELO");
}

void SmtpClient::StartTls(const TlsSettings& settings)
{
	AssertIsStartTlsOffered(m_capabilities);
	Exchange("STARTTLS", {CODE_READY}, "STARTTLS");

	m_channel.StartTls(settings);
	Hello(m_clientName);
}

void SmtpClient::Authenticate(const Credentials& credentials)
{
	if (ChooseAuthMethod(m_capabilities) == AuthMethod::Plain)
	{
		AuthenticatePlain(credentials);
		return;
	}

	AuthenticateLogin(credentials);
}

SmtpReply SmtpClient::Send(const Envelope& envelope, const MessageWriterFunction& writeMessage)
{
	AssertHasRecipients(envelope);

	Exchange("MAIL FROM:<" + envelope.from + ">", {CODE_OK}, "MAIL FROM");
	SendRecipients(envelope);
	Exchange("DATA", {CODE_START_INPUT}, "DATA");

	DataSink sink(m_channel);
	writeMessage(sink);
	sink.Finish();

	SmtpReply reply = m_channel.ReadReply();
	AssertIsExpected(reply, {CODE_OK}, "письмо");

	return reply;
}

void SmtpClient::Quit()
{
	m_isQuitSent = true;
	Exchange("QUIT", {CODE_CLOSING}, "QUIT");
}

const Capabilities& SmtpClient::GetCapabilities() const noexcept
{
	return m_capabilities;
}

SmtpReply SmtpClient::Exchange(const std::string_view command,
	const std::initializer_list<int> expected,
	const std::string_view stage)
{
	m_channel.SendCommand(command);

	SmtpReply reply = m_channel.ReadReply();
	AssertIsExpected(reply, expected, stage);

	return reply;
}

void SmtpClient::AuthenticatePlain(const Credentials& credentials)
{
	const survey::SecretString token
		= BuildPlainToken(credentials.login, credentials.password.View());
	m_channel.SendCommand(
		"AUTH PLAIN " + std::string(token.View()), std::string("AUTH PLAIN ") + HIDDEN_SECRET);

	const SmtpReply reply = m_channel.ReadReply();
	AssertIsAuthAccepted(reply);
	AssertIsExpected(reply, {CODE_AUTH_SUCCESS}, "AUTH PLAIN");
}

void SmtpClient::AuthenticateLogin(const Credentials& credentials)
{
	Exchange("AUTH LOGIN", {CODE_AUTH_CONTINUE}, "AUTH LOGIN");
	Exchange(EncodeSecret(credentials.login).View(), {CODE_AUTH_CONTINUE}, "логин");

	const survey::SecretString password = EncodeSecret(credentials.password.View());
	m_channel.SendCommand(password.View(), HIDDEN_SECRET);

	const SmtpReply reply = m_channel.ReadReply();
	AssertIsAuthAccepted(reply);
	AssertIsExpected(reply, {CODE_AUTH_SUCCESS}, "пароль");
}

void SmtpClient::SendRecipients(const Envelope& envelope)
{
	for (const auto& recipient : envelope.recipients)
	{
		Exchange("RCPT TO:<" + recipient + ">", {CODE_OK, CODE_FORWARD}, "RCPT TO " + recipient);
	}
}

SmtpError::SmtpError(const std::string& message, const int code)
	: std::runtime_error(message)
	, m_code(code)
{
}

int SmtpError::Code() const noexcept
{
	return m_code;
}

namespace
{
constexpr std::size_t CODE_LENGTH = 3;
constexpr char CONTINUATION_MARK = '-';
constexpr char LAST_LINE_MARK = ' ';

bool IsDigit(const char symbol) noexcept
{
	return symbol >= '0' && symbol <= '9';
}

bool HasCode(const std::string_view line) noexcept
{
	return line.size() >= CODE_LENGTH && IsDigit(line[0]) && IsDigit(line[1]) && IsDigit(line[2]);
}

bool HasValidSeparator(const std::string_view line) noexcept
{
	return line.size() == CODE_LENGTH || line[CODE_LENGTH] == CONTINUATION_MARK
		|| line[CODE_LENGTH] == LAST_LINE_MARK;
}

void AssertIsReplyLine(const std::string_view line)
{
	if (!HasCode(line) || !HasValidSeparator(line))
	{
		throw SmtpError("Сервер прислал некорректную строку ответа: " + std::string(line), 0);
	}
}

int ParseCode(const std::string_view line) noexcept
{
	return (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
}
} // namespace

std::string SmtpReply::Text() const
{
	std::string text;
	for (const auto& line : lines)
	{
		text += text.empty() ? line : " " + line;
	}

	return text;
}

ReplyLine ParseReplyLine(const std::string_view line)
{
	AssertIsReplyLine(line);

	const bool isLast = line.size() == CODE_LENGTH || line[CODE_LENGTH] == LAST_LINE_MARK;
	const std::string_view text = line.size() > CODE_LENGTH + 1 ? line.substr(CODE_LENGTH + 1) : "";

	return {ParseCode(line), isLast, std::string(text)};
}

namespace
{
constexpr auto PATH_WHITESPACE = " \t\r\n";

std::string_view TrimPathView(const std::string_view text) noexcept
{
	const std::size_t first = text.find_first_not_of(PATH_WHITESPACE);
	if (first == std::string_view::npos)
	{
		return {};
	}

	return text.substr(first, text.find_last_not_of(PATH_WHITESPACE) - first + 1);
}

bool IsQuoted(const std::string_view text) noexcept
{
	return text.size() >= 2 && text.front() == '"' && text.back() == '"';
}
} // namespace

std::filesystem::path PathFromUtf8(const std::string_view text)
{
	return {std::u8string(text.begin(), text.end())};
}

std::string PathToUtf8(const std::filesystem::path& path)
{
	const std::u8string text = path.u8string();
	return {text.begin(), text.end()};
}

std::string NormalizePathInput(const std::string_view text)
{
	std::string_view trimmed = TrimPathView(text);
	if (IsQuoted(trimmed))
	{
		trimmed = trimmed.substr(1, trimmed.size() - 2);
	}

	return std::string(trimmed);
}

#ifdef SMTP_CLIENT_HAS_TLS

namespace
{
constexpr std::size_t ERROR_TEXT_CAPACITY = 256;
constexpr auto NO_DETAILS = "подробностей нет";
} // namespace

std::string CollectOpenSslErrors()
{
	std::string text;
	std::array<char, ERROR_TEXT_CAPACITY> buffer{};

	for (unsigned long code = ERR_get_error(); code != 0; code = ERR_get_error())
	{
		ERR_error_string_n(code, buffer.data(), buffer.size());
		text += text.empty() ? "" : "; ";
		text += buffer.data();
	}

	return text.empty() ? NO_DETAILS : text;
}

#endif

#ifdef SMTP_CLIENT_HAS_TLS

namespace
{
#ifdef _WIN32
struct CertificateStoreDeleter
{
	void operator()(void* store) const noexcept
	{
		CertCloseStore(static_cast<HCERTSTORE>(store), 0);
	}
};

using CertificateStore = std::unique_ptr<void, CertificateStoreDeleter>;

struct X509Deleter
{
	void operator()(X509* certificate) const noexcept
	{
		X509_free(certificate);
	}
};

CertificateStore AssertIsStoreOpen(HCERTSTORE store)
{
	if (store == nullptr)
	{
		throw TlsError("Не удалось открыть хранилище корневых сертификатов Windows");
	}

	return CertificateStore(store);
}

std::unique_ptr<X509, X509Deleter> DecodeCertificate(const CERT_CONTEXT& certificate)
{
	const unsigned char* data = certificate.pbCertEncoded;
	return std::unique_ptr<X509, X509Deleter>(
		d2i_X509(nullptr, &data, static_cast<long>(certificate.cbCertEncoded)));
}

void AddCertificate(X509_STORE* store, const CERT_CONTEXT& certificate)
{
	const auto decoded = DecodeCertificate(certificate);
	if (decoded != nullptr)
	{
		X509_STORE_add_cert(store, decoded.get());
	}
}
#else
void AssertIsDefaultPathsLoaded(const int result)
{
	if (result != 1)
	{
		throw TlsError("Не удалось загрузить системные корневые сертификаты");
	}
}
#endif
} // namespace

#ifdef _WIN32

void LoadSystemCertificates(ssl_ctx_st* context)
{
	X509_STORE* target = SSL_CTX_get_cert_store(context);
	const CertificateStore store = AssertIsStoreOpen(CertOpenSystemStoreW(0, L"ROOT"));

	PCCERT_CONTEXT certificate = nullptr;
	while ((certificate = CertEnumCertificatesInStore(store.get(), certificate)) != nullptr)
	{
		AddCertificate(target, *certificate);
	}
}

#else

void LoadSystemCertificates(ssl_ctx_st* context)
{
	AssertIsDefaultPathsLoaded(SSL_CTX_set_default_verify_paths(context));
}

#endif

#endif

#ifdef SMTP_CLIENT_HAS_TLS

namespace
{
SSL_CTX* AssertIsContextCreated(SSL_CTX* context)
{
	if (context == nullptr)
	{
		throw TlsError("Не удалось создать TLS-контекст: " + CollectOpenSslErrors());
	}

	return context;
}

void AssertIsProtocolSet(const int result)
{
	if (result != 1)
	{
		throw TlsError("Не удалось ограничить TLS версией 1.2+: " + CollectOpenSslErrors());
	}
}

void ConfigureVerification(SSL_CTX* context, const bool verifyCertificate)
{
	if (!verifyCertificate)
	{
		SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
		return;
	}

	SSL_CTX_set_verify(context, SSL_VERIFY_PEER, nullptr);
	LoadSystemCertificates(context);
}

SSL_CTX* CreateContext(const bool verifyCertificate)
{
	SSL_CTX* context = AssertIsContextCreated(SSL_CTX_new(TLS_client_method()));
	AssertIsProtocolSet(SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION));
	ConfigureVerification(context, verifyCertificate);

	return context;
}
} // namespace

void TlsContext::Deleter::operator()(ssl_ctx_st* context) const noexcept
{
	SSL_CTX_free(context);
}

TlsContext::TlsContext(const bool verifyCertificate)
	: m_context(CreateContext(verifyCertificate))
{
}

ssl_ctx_st* TlsContext::Get() const noexcept
{
	return m_context.get();
}

#endif

TlsError::TlsError(const std::string& message)
	: std::runtime_error(message)
{
}

#ifdef SMTP_CLIENT_HAS_TLS

std::unique_ptr<ITransport> CreateTlsTransport(Socket socket, const TlsSettings& settings)
{
	return std::make_unique<TlsTransport>(std::move(socket), settings);
}

bool IsTlsSupported() noexcept
{
	return true;
}

#else

std::unique_ptr<ITransport> CreateTlsTransport(Socket, const TlsSettings&)
{
	throw TlsError("Клиент собран без OpenSSL: STARTTLS и TLS недоступны");
}

bool IsTlsSupported() noexcept
{
	return false;
}

#endif

#ifdef SMTP_CLIENT_HAS_TLS

namespace
{
SSL* AssertIsSessionCreated(SSL* ssl)
{
	if (ssl == nullptr)
	{
		throw TlsError("Не удалось создать TLS-сессию: " + CollectOpenSslErrors());
	}

	return ssl;
}

void AssertIsConfigured(const int result, const char* step)
{
	if (result != 1)
	{
		throw TlsError(
			std::string("Ошибка настройки TLS (") + step + "): " + CollectOpenSslErrors());
	}
}

std::string DescribeVerifyFailure(SSL* ssl)
{
	const long result = SSL_get_verify_result(ssl);
	if (result == X509_V_OK)
	{
		return CollectOpenSslErrors();
	}

	return std::string("сертификат сервера не прошёл проверку: ")
		+ X509_verify_cert_error_string(result);
}

void AssertIsHandshakeDone(const int result, SSL* ssl)
{
	if (result != 1)
	{
		throw TlsError("TLS-рукопожатие не удалось: " + DescribeVerifyFailure(ssl));
	}
}

void ConfigureSession(SSL* ssl, const Socket& socket, const TlsSettings& settings)
{
	AssertIsConfigured(SSL_set_fd(ssl, static_cast<int>(socket.Native())), "сокет");
	AssertIsConfigured(
		static_cast<int>(SSL_set_tlsext_host_name(ssl, settings.host.c_str())), "SNI");

	if (settings.verifyCertificate)
	{
		AssertIsConfigured(SSL_set1_host(ssl, settings.host.c_str()), "имя хоста");
	}
}

SSL* CreateSession(const TlsContext& context, const Socket& socket, const TlsSettings& settings)
{
	SSL* ssl = AssertIsSessionCreated(SSL_new(context.Get()));
	ConfigureSession(ssl, socket, settings);
	AssertIsHandshakeDone(SSL_connect(ssl), ssl);

	return ssl;
}

int ClampSize(const std::size_t size) noexcept
{
	return static_cast<int>(std::min<std::size_t>(size, INT_MAX));
}

void AssertIsTransferred(const int result, SSL* ssl, const char* operation)
{
	if (result <= 0)
	{
		throw TlsError(std::string("Ошибка TLS при ") + operation + ", код "
			+ std::to_string(SSL_get_error(ssl, result)) + ": " + CollectOpenSslErrors());
	}
}
} // namespace

void TlsTransport::SslDeleter::operator()(ssl_st* ssl) const noexcept
{
	SSL_free(ssl);
}

TlsTransport::TlsTransport(Socket socket, const TlsSettings& settings)
	: m_socket(std::move(socket))
	, m_context(settings.verifyCertificate)
	, m_ssl(CreateSession(m_context, m_socket, settings))
{
}

TlsTransport::~TlsTransport() noexcept
{
	SSL_shutdown(m_ssl.get());
}

std::size_t TlsTransport::Read(const std::span<char> buffer)
{
	const int result = SSL_read(m_ssl.get(), buffer.data(), ClampSize(buffer.size()));
	if (result <= 0 && SSL_get_error(m_ssl.get(), result) == SSL_ERROR_ZERO_RETURN)
	{
		return 0;
	}

	AssertIsTransferred(result, m_ssl.get(), "чтении");
	return static_cast<std::size_t>(result);
}

void TlsTransport::Write(std::string_view data)
{
	while (!data.empty())
	{
		const int result = SSL_write(m_ssl.get(), data.data(), ClampSize(data.size()));
		AssertIsTransferred(result, m_ssl.get(), "записи");
		data.remove_prefix(static_cast<std::size_t>(result));
	}
}

std::unique_ptr<ITransport> TlsTransport::StartTls(const TlsSettings&)
{
	throw TlsError("Соединение уже зашифровано");
}

std::string TlsTransport::Describe() const
{
	return std::string(SSL_get_version(m_ssl.get())) + ", " + SSL_get_cipher_name(m_ssl.get());
}

#endif

PlainTransport::PlainTransport(Socket socket) noexcept
	: m_socket(std::move(socket))
{
}

std::size_t PlainTransport::Read(const std::span<char> buffer)
{
	return m_socket.ReadSome(buffer);
}

void PlainTransport::Write(const std::string_view data)
{
	m_socket.WriteAll(data);
}

std::unique_ptr<ITransport> PlainTransport::StartTls(const TlsSettings& settings)
{
	return CreateTlsTransport(std::move(m_socket), settings);
}

std::string PlainTransport::Describe() const
{
	return "TCP без шифрования";
}

#ifdef _WIN32

ConsoleEncoding::ConsoleEncoding() noexcept
	: m_previousOutputCodePage(GetConsoleOutputCP())
	, m_previousInputCodePage(GetConsoleCP())
{
	SetConsoleOutputCP(CP_UTF8);
	SetConsoleCP(CP_UTF8);
}

ConsoleEncoding::~ConsoleEncoding() noexcept
{
	SetConsoleOutputCP(m_previousOutputCodePage);
	SetConsoleCP(m_previousInputCodePage);
}

#else

ConsoleEncoding::ConsoleEncoding() noexcept = default;

ConsoleEncoding::~ConsoleEncoding() noexcept = default;

#endif

namespace
{
constexpr auto ERROR_STYLE = "\x1b[31m";
constexpr auto MAIN_RESET_STYLE = "\x1b[0m";

void PrintError(const std::exception& error)
{
	std::cerr << std::endl
			  << ERROR_STYLE << "[Ошибка] " << error.what() << MAIN_RESET_STYLE << std::endl;
}
} // namespace

int main()
{
	const ConsoleEncoding console;

	try
	{
		const Application app;
		app.Run();
	}
	catch (const survey::CancelledError& e)
	{
		std::cout << std::endl << e.what() << std::endl;
		return EXIT_FAILURE;
	}
	catch (const std::exception& e)
	{
		PrintError(e);
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}
