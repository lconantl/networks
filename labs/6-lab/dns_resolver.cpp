#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace
{
using Bytes = std::vector<std::uint8_t>;

constexpr std::uint16_t DNS_PORT = 53;
constexpr std::size_t DNS_HEADER_BYTES = 12;
constexpr std::size_t MAX_DNS_MESSAGE_BYTES = 65535;
constexpr std::size_t MAX_DNS_RECORDS = 1024;
constexpr std::size_t MAX_NAME_STEPS = 128;
constexpr int NETWORK_TIMEOUT_SECONDS = 3;
constexpr int MAX_UDP_ATTEMPTS = 2;
constexpr int MAX_DELEGATIONS = 32;
constexpr int MAX_LOOKUP_DEPTH = 16;
constexpr int MAX_CNAME_LINKS = 16;
constexpr std::size_t MAX_NS_CANDIDATES = 4;
constexpr auto CACHE_FILE = ".dns_resolver.cache";

constexpr std::array<std::string_view, 13> ROOT_SERVERS = {
	"198.41.0.4", "170.247.170.2", "192.33.4.12", "199.7.91.13",
	"192.203.230.10", "192.5.5.241", "192.112.36.4", "198.97.190.53",
	"192.36.148.17", "192.58.128.30", "193.0.14.129", "199.7.83.42",
	"202.12.27.33",
};

constexpr std::uint16_t TYPE_A = 1;
constexpr std::uint16_t TYPE_NS = 2;
constexpr std::uint16_t TYPE_CNAME = 5;
constexpr std::uint16_t TYPE_SOA = 6;
constexpr std::uint16_t TYPE_PTR = 12;
constexpr std::uint16_t TYPE_MX = 15;
constexpr std::uint16_t TYPE_TXT = 16;
constexpr std::uint16_t TYPE_AAAA = 28;
constexpr std::uint16_t TYPE_SRV = 33;
constexpr std::uint16_t TYPE_DNAME = 39;
constexpr std::uint16_t TYPE_OPT = 41;
constexpr std::uint16_t TYPE_DS = 43;
constexpr std::uint16_t TYPE_RRSIG = 46;
constexpr std::uint16_t TYPE_DNSKEY = 48;
constexpr std::uint16_t TYPE_CAA = 257;

class NetworkError : public std::runtime_error
{
public:
	using std::runtime_error::runtime_error;
};

class ProtocolError : public std::runtime_error
{
public:
	using std::runtime_error::runtime_error;
};

class RecordNotFound : public std::runtime_error
{
public:
	using std::runtime_error::runtime_error;
};

class FileDescriptor final
{
public:
	explicit FileDescriptor(const int descriptor = -1) noexcept
		: m_descriptor(descriptor)
	{
	}

	~FileDescriptor() noexcept
	{
		if (m_descriptor >= 0)
		{
			close(m_descriptor);
		}
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
			if (m_descriptor >= 0)
			{
				close(m_descriptor);
			}
			m_descriptor = std::exchange(other.m_descriptor, -1);
		}
		return *this;
	}

	[[nodiscard]] int Get() const noexcept
	{
		return m_descriptor;
	}

private:
	int m_descriptor;
};

struct SocketAddress
{
	sockaddr_storage storage{};
	socklen_t length = 0;
	int family = AF_UNSPEC;
};

struct DnsRecord
{
	std::string name;
	std::uint16_t type = 0;
	std::uint16_t recordClass = 0;
	std::uint32_t ttl = 0;
	std::string value;
	std::string target;
	std::string address;
};

struct DnsMessage
{
	std::uint8_t responseCode = 0;
	bool isAuthoritative = false;
	std::vector<DnsRecord> answers;
	std::vector<DnsRecord> authorities;
	std::vector<DnsRecord> additionals;
};

struct LookupResult
{
	std::vector<DnsRecord> records;
	std::uint32_t ttl = 0;
};

struct Referral
{
	std::string zone;
	std::vector<std::string> nameservers;
};

enum class Command
{
	Resolve,
	ShowCache,
	ClearCache,
};

struct Options
{
	Command command = Command::Resolve;
	std::string domain;
	std::uint16_t type = TYPE_A;
	bool isDebug = false;
	bool useCache = true;
	bool requestDnssec = false;
};

std::string TypeName(const std::uint16_t type)
{
	switch (type)
	{
	case TYPE_A: return "A";
	case TYPE_NS: return "NS";
	case TYPE_CNAME: return "CNAME";
	case TYPE_SOA: return "SOA";
	case TYPE_PTR: return "PTR";
	case TYPE_MX: return "MX";
	case TYPE_TXT: return "TXT";
	case TYPE_AAAA: return "AAAA";
	case TYPE_SRV: return "SRV";
	case TYPE_DNAME: return "DNAME";
	case TYPE_OPT: return "OPT";
	case TYPE_DS: return "DS";
	case TYPE_RRSIG: return "RRSIG";
	case TYPE_DNSKEY: return "DNSKEY";
	case TYPE_CAA: return "CAA";
	default: return std::format("TYPE{}", type);
	}
}

std::string ToUpper(std::string text)
{
	for (char& character : text)
	{
		character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
	}
	return text;
}

std::string NormalizeDomain(std::string domain)
{
	if (domain == ".")
	{
		return domain;
	}
	if (!domain.empty() && domain.back() == '.')
	{
		domain.pop_back();
	}
	if (domain.empty() || domain.size() > 253)
	{
		throw std::invalid_argument("Некорректная длина доменного имени");
	}

	std::size_t labelLength = 0;
	for (char& character : domain)
	{
		const auto value = static_cast<unsigned char>(character);
		if (character == '.')
		{
			if (labelLength == 0 || labelLength > 63)
			{
				throw std::invalid_argument("Некорректная длина DNS-метки");
			}
			labelLength = 0;
			continue;
		}
		if (value > 127 || (!std::isalnum(value) && character != '-' && character != '_'))
		{
			throw std::invalid_argument("Домен должен быть ASCII (для IDN используйте Punycode)");
		}
		character = static_cast<char>(std::tolower(value));
		++labelLength;
	}
	if (labelLength == 0 || labelLength > 63)
	{
		throw std::invalid_argument("Некорректная последняя DNS-метка");
	}
	return domain;
}

std::uint16_t ParseType(const std::string_view input, bool& requestDnssec)
{
	const std::string text = ToUpper(std::string(input));
	if (text == "DNSSEC")
	{
		requestDnssec = true;
		return TYPE_DNSKEY;
	}

	constexpr std::array<std::pair<std::string_view, std::uint16_t>, 13> known = {{
		{"A", TYPE_A}, {"AAAA", TYPE_AAAA}, {"NS", TYPE_NS},
		{"CNAME", TYPE_CNAME}, {"SOA", TYPE_SOA}, {"PTR", TYPE_PTR},
		{"MX", TYPE_MX}, {"TXT", TYPE_TXT}, {"SRV", TYPE_SRV},
		{"CAA", TYPE_CAA}, {"DS", TYPE_DS}, {"DNSKEY", TYPE_DNSKEY},
		{"RRSIG", TYPE_RRSIG},
	}};
	for (const auto& [name, type] : known)
	{
		if (text == name)
		{
			requestDnssec = type == TYPE_DS || type == TYPE_DNSKEY || type == TYPE_RRSIG;
			return type;
		}
	}

	if (text.starts_with("TYPE"))
	{
		unsigned int number = 0;
		const std::string_view digits(text.data() + 4, text.size() - 4);
		const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), number);
		if (error == std::errc{} && end == digits.data() + digits.size()
			&& number > 0 && number <= std::numeric_limits<std::uint16_t>::max())
		{
			return static_cast<std::uint16_t>(number);
		}
	}
	throw std::invalid_argument("Неизвестный тип записи: " + text);
}

Options ParseOptions(const int argc, char* argv[])
{
	Options options;
	if (argc == 2 && std::string_view(argv[1]) == "--cache-show")
	{
		options.command = Command::ShowCache;
		return options;
	}
	if (argc == 2 && std::string_view(argv[1]) == "--cache-clear")
	{
		options.command = Command::ClearCache;
		return options;
	}

	std::vector<std::string_view> positional;
	for (int index = 1; index < argc; ++index)
	{
		const std::string_view argument = argv[index];
		if (argument == "-d" || argument == "--debug")
		{
			options.isDebug = true;
		}
		else if (argument == "--no-cache")
		{
			options.useCache = false;
		}
		else if (argument.starts_with('-'))
		{
			throw std::invalid_argument("Неизвестный аргумент: " + std::string(argument));
		}
		else
		{
			positional.push_back(argument);
		}
	}
	if (positional.size() != 2)
	{
		throw std::invalid_argument(
			"Использование: dns_resolver <домен> <A|AAAA|NS|MX|TXT|...> [-d] [--no-cache]\n"
			"             dns_resolver --cache-show | --cache-clear");
	}
	options.domain = NormalizeDomain(std::string(positional[0]));
	options.type = ParseType(positional[1], options.requestDnssec);
	return options;
}

void AppendU16(Bytes& packet, const std::uint16_t value)
{
	packet.push_back(static_cast<std::uint8_t>(value >> 8));
	packet.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void AppendU32(Bytes& packet, const std::uint32_t value)
{
	AppendU16(packet, static_cast<std::uint16_t>(value >> 16));
	AppendU16(packet, static_cast<std::uint16_t>(value & 0xFFFF));
}

std::uint16_t ReadU16(const Bytes& packet, std::size_t& position)
{
	if (position > packet.size() || packet.size() - position < 2)
	{
		throw ProtocolError("Обрезанное 16-битное поле DNS");
	}
	const auto value = static_cast<std::uint16_t>(
		(static_cast<std::uint16_t>(packet[position]) << 8) | packet[position + 1]);
	position += 2;
	return value;
}

std::uint32_t ReadU32(const Bytes& packet, std::size_t& position)
{
	const std::uint32_t high = ReadU16(packet, position);
	const std::uint32_t low = ReadU16(packet, position);
	return (high << 16) | low;
}

void AppendName(Bytes& packet, const std::string_view domain)
{
	if (domain == ".")
	{
		packet.push_back(0);
		return;
	}
	std::size_t begin = 0;
	while (begin < domain.size())
	{
		const std::size_t end = domain.find('.', begin);
		const std::size_t length = (end == std::string_view::npos ? domain.size() : end) - begin;
		packet.push_back(static_cast<std::uint8_t>(length));
		for (std::size_t index = 0; index < length; ++index)
		{
			packet.push_back(static_cast<std::uint8_t>(domain[begin + index]));
		}
		if (end == std::string_view::npos)
		{
			break;
		}
		begin = end + 1;
	}
	packet.push_back(0);
}

std::string ReadName(const Bytes& packet, std::size_t& position)
{
	std::size_t cursor = position;
	bool isCompressed = false;
	std::string domain;
	for (std::size_t step = 0; step < MAX_NAME_STEPS; ++step)
	{
		if (cursor >= packet.size())
		{
			throw ProtocolError("DNS-имя выходит за границы пакета");
		}
		const auto length = packet[cursor++];
		if ((length & 0xC0) == 0xC0)
		{
			if (cursor >= packet.size())
			{
				throw ProtocolError("Обрезанный указатель сжатого DNS-имени");
			}
			const std::size_t pointer = (static_cast<std::size_t>(length & 0x3F) << 8)
				| packet[cursor++];
			if (pointer >= packet.size())
			{
				throw ProtocolError("DNS-указатель выходит за границы пакета");
			}
			if (!isCompressed)
			{
				position = cursor;
				isCompressed = true;
			}
			cursor = pointer;
			continue;
		}
		if ((length & 0xC0) != 0)
		{
			throw ProtocolError("Недопустимый формат DNS-метки");
		}
		if (length == 0)
		{
			if (!isCompressed)
			{
				position = cursor;
			}
			return domain.empty() ? "." : domain;
		}
		if (cursor > packet.size() || packet.size() - cursor < length)
		{
			throw ProtocolError("Обрезанная DNS-метка");
		}
		if (!domain.empty())
		{
			domain.push_back('.');
		}
		for (std::size_t index = 0; index < length; ++index)
		{
			const auto value = packet[cursor + index];
			if (value < 33 || value > 126 || value == '.' || value == '\\')
			{
				throw ProtocolError("Неподдерживаемые символы в DNS-имени");
			}
			domain.push_back(static_cast<char>(std::tolower(value)));
		}
		if (domain.size() > 253)
		{
			throw ProtocolError("Слишком длинное DNS-имя в ответе");
		}
		cursor += length;
	}
	throw ProtocolError("Циклическое DNS-сжатие имени");
}

std::string EncodeBase64(const std::uint8_t* data, const std::size_t length)
{
	constexpr std::string_view alphabet =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	std::string result;
	result.reserve(((length + 2) / 3) * 4);
	for (std::size_t index = 0; index < length; index += 3)
	{
		const std::uint32_t chunk = (static_cast<std::uint32_t>(data[index]) << 16)
			| (index + 1 < length ? static_cast<std::uint32_t>(data[index + 1]) << 8 : 0)
			| (index + 2 < length ? data[index + 2] : 0);
		result.push_back(alphabet[(chunk >> 18) & 63]);
		result.push_back(alphabet[(chunk >> 12) & 63]);
		result.push_back(index + 1 < length ? alphabet[(chunk >> 6) & 63] : '=');
		result.push_back(index + 2 < length ? alphabet[chunk & 63] : '=');
	}
	return result;
}

std::string EncodeHex(const std::uint8_t* data, const std::size_t length)
{
	constexpr std::string_view alphabet = "0123456789ABCDEF";
	std::string result;
	result.reserve(length * 2);
	for (std::size_t index = 0; index < length; ++index)
	{
		result.push_back(alphabet[data[index] >> 4]);
		result.push_back(alphabet[data[index] & 15]);
	}
	return result;
}

std::string EscapeText(const std::uint8_t* data, const std::size_t length)
{
	std::string result;
	for (std::size_t index = 0; index < length; ++index)
	{
		const auto value = data[index];
		if (value == '"' || value == '\\')
		{
			result.push_back('\\');
			result.push_back(static_cast<char>(value));
		}
		else if (value >= 32 && value <= 126)
		{
			result.push_back(static_cast<char>(value));
		}
		else
		{
			result += std::format("\\x{:02X}", value);
		}
	}
	return result;
}

DnsRecord ReadRecord(const Bytes& packet, std::size_t& position)
{
	DnsRecord record;
	record.name = ReadName(packet, position);
	record.type = ReadU16(packet, position);
	record.recordClass = ReadU16(packet, position);
	record.ttl = ReadU32(packet, position);
	const std::size_t length = ReadU16(packet, position);
	if (position > packet.size() || packet.size() - position < length)
	{
		throw ProtocolError("RDATA выходит за границы DNS-пакета");
	}

	std::size_t cursor = position;
	const std::size_t end = position + length;
	const auto* data = packet.data() + position;
	if ((record.type == TYPE_A && length != 4)
		|| (record.type == TYPE_AAAA && length != 16))
	{
		throw ProtocolError("Некорректная длина A/AAAA-записи");
	}
	if (record.type == TYPE_A && length == 4)
	{
		std::array<char, INET_ADDRSTRLEN> address{};
		if (inet_ntop(AF_INET, data, address.data(), address.size()) == nullptr)
		{
			throw ProtocolError("Не удалось прочитать IPv4-адрес");
		}
		record.address = address.data();
		record.value = record.address;
	}
	else if (record.type == TYPE_AAAA && length == 16)
	{
		std::array<char, INET6_ADDRSTRLEN> address{};
		if (inet_ntop(AF_INET6, data, address.data(), address.size()) == nullptr)
		{
			throw ProtocolError("Не удалось прочитать IPv6-адрес");
		}
		record.address = address.data();
		record.value = record.address;
	}
	else if (record.type == TYPE_NS || record.type == TYPE_CNAME
		|| record.type == TYPE_PTR || record.type == TYPE_DNAME)
	{
		record.target = ReadName(packet, cursor);
		if (cursor != end)
		{
			throw ProtocolError("Лишние байты в DNS-записи с именем");
		}
		record.value = record.target;
	}
	else if (record.type == TYPE_MX && length >= 3)
	{
		const auto preference = ReadU16(packet, cursor);
		record.target = ReadName(packet, cursor);
		if (cursor != end)
		{
			throw ProtocolError("Неверная длина MX-записи");
		}
		record.value = std::format("{} {}", preference, record.target);
	}
	else if (record.type == TYPE_TXT)
	{
		while (cursor < end)
		{
			const std::size_t textLength = packet[cursor++];
			if (end - cursor < textLength)
			{
				throw ProtocolError("Обрезанная TXT-строка");
			}
			if (!record.value.empty())
			{
				record.value.push_back(' ');
			}
			record.value += '"' + EscapeText(packet.data() + cursor, textLength) + '"';
			cursor += textLength;
		}
	}
	else if (record.type == TYPE_SOA)
	{
		const std::string primary = ReadName(packet, cursor);
		const std::string mailbox = ReadName(packet, cursor);
		if (cursor > end || end - cursor != 20)
		{
			throw ProtocolError("Неверная длина SOA-записи");
		}
		const auto serial = ReadU32(packet, cursor);
		const auto refresh = ReadU32(packet, cursor);
		const auto retry = ReadU32(packet, cursor);
		const auto expire = ReadU32(packet, cursor);
		const auto minimum = ReadU32(packet, cursor);
		record.value = std::format("{} {} {} {} {} {} {}",
			primary, mailbox, serial, refresh, retry, expire, minimum);
	}
	else if (record.type == TYPE_SRV && length >= 7)
	{
		const auto priority = ReadU16(packet, cursor);
		const auto weight = ReadU16(packet, cursor);
		const auto port = ReadU16(packet, cursor);
		record.target = ReadName(packet, cursor);
		if (cursor != end)
		{
			throw ProtocolError("Неверная длина SRV-записи");
		}
		record.value = std::format("{} {} {} {}", priority, weight, port, record.target);
	}
	else if (record.type == TYPE_DS && length >= 4)
	{
		const auto keyTag = ReadU16(packet, cursor);
		const auto algorithm = packet[cursor++];
		const auto digestType = packet[cursor++];
		record.value = std::format("{} {} {} {}", keyTag,
			static_cast<unsigned int>(algorithm), static_cast<unsigned int>(digestType),
			EncodeHex(packet.data() + cursor, end - cursor));
	}
	else if (record.type == TYPE_DNSKEY && length >= 4)
	{
		const auto flags = ReadU16(packet, cursor);
		const auto protocol = packet[cursor++];
		const auto algorithm = packet[cursor++];
		record.value = std::format("{} {} {} {}", flags,
			static_cast<unsigned int>(protocol), static_cast<unsigned int>(algorithm),
			EncodeBase64(packet.data() + cursor, end - cursor));
	}
	else if (record.type == TYPE_RRSIG && length >= 19)
	{
		const auto covered = ReadU16(packet, cursor);
		const auto algorithm = packet[cursor++];
		const auto labels = packet[cursor++];
		const auto originalTtl = ReadU32(packet, cursor);
		const auto expiration = ReadU32(packet, cursor);
		const auto inception = ReadU32(packet, cursor);
		const auto keyTag = ReadU16(packet, cursor);
		const std::string signer = ReadName(packet, cursor);
		if (cursor > end)
		{
			throw ProtocolError("Неверная длина RRSIG-записи");
		}
		record.value = std::format("{} {} {} {} {} {} {} {} {}",
			TypeName(covered), static_cast<unsigned int>(algorithm),
			static_cast<unsigned int>(labels), originalTtl, expiration, inception,
			keyTag, signer, EncodeBase64(packet.data() + cursor, end - cursor));
	}
	else if (record.type == TYPE_CAA && length >= 2)
	{
		const auto flags = packet[cursor++];
		const std::size_t tagLength = packet[cursor++];
		if (end - cursor < tagLength)
		{
			throw ProtocolError("Неверная длина CAA-записи");
		}
		const std::string tag(reinterpret_cast<const char*>(packet.data() + cursor), tagLength);
		cursor += tagLength;
		record.value = std::format("{} {} \"{}\"", static_cast<unsigned int>(flags), tag,
			EscapeText(packet.data() + cursor, end - cursor));
	}
	else
	{
		record.value = EncodeHex(data, length);
	}
	position = end;
	return record;
}

Bytes BuildQuery(const std::string_view domain, const std::uint16_t type,
	const std::uint16_t id, const bool requestDnssec)
{
	Bytes packet;
	packet.reserve(512);
	AppendU16(packet, id);
	AppendU16(packet, 0); // RD=0: запросы только итеративные.
	AppendU16(packet, 1); // Одна DNS-вопросная запись.
	AppendU16(packet, 0);
	AppendU16(packet, 0);
	AppendU16(packet, requestDnssec ? 1 : 0); // EDNS0 OPT при DNSSEC.
	AppendName(packet, domain);
	AppendU16(packet, type);
	AppendU16(packet, 1); // IN.
	if (requestDnssec)
	{
		packet.push_back(0); // Имя корневой зоны для OPT.
		AppendU16(packet, TYPE_OPT);
		AppendU16(packet, 1232); // Размер UDP-ответа с EDNS0.
		AppendU32(packet, 0x00008000); // DNSSEC OK (DO).
		AppendU16(packet, 0);
	}
	return packet;
}

bool IsTruncated(const Bytes& packet, const std::uint16_t expectedId)
{
	if (packet.size() < DNS_HEADER_BYTES)
	{
		throw ProtocolError("Слишком короткий DNS-ответ");
	}
	std::size_t cursor = 0;
	if (ReadU16(packet, cursor) != expectedId)
	{
		throw ProtocolError("DNS transaction ID не совпадает");
	}
	const auto flags = ReadU16(packet, cursor);
	if ((flags & 0x8000) == 0)
	{
		throw ProtocolError("Получен DNS-запрос вместо ответа");
	}
	return (flags & 0x0200) != 0;
}

DnsMessage ParseResponse(const Bytes& packet, const std::uint16_t id,
	const std::string_view domain, const std::uint16_t type)
{
	if (packet.size() < DNS_HEADER_BYTES)
	{
		throw ProtocolError("Слишком короткий DNS-ответ");
	}
	std::size_t cursor = 0;
	if (ReadU16(packet, cursor) != id)
	{
		throw ProtocolError("DNS transaction ID не совпадает");
	}
	const auto flags = ReadU16(packet, cursor);
	if ((flags & 0x8000) == 0 || (flags & 0x7800) != 0)
	{
		throw ProtocolError("Некорректные флаги DNS-ответа");
	}
	if ((flags & 0x0200) != 0)
	{
		throw ProtocolError("DNS-ответ по TCP также обрезан (TC=1)");
	}

	const auto questions = ReadU16(packet, cursor);
	const auto answerCount = ReadU16(packet, cursor);
	const auto authorityCount = ReadU16(packet, cursor);
	const auto additionalCount = ReadU16(packet, cursor);
	if (questions != 1 || static_cast<std::size_t>(answerCount) + authorityCount
		+ additionalCount > MAX_DNS_RECORDS)
	{
		throw ProtocolError("Некорректное количество записей в DNS-ответе");
	}
	const std::string question = ReadName(packet, cursor);
	const auto questionType = ReadU16(packet, cursor);
	const auto questionClass = ReadU16(packet, cursor);
	if (question != domain || questionType != type || questionClass != 1)
	{
		throw ProtocolError("DNS-ответ содержит другой вопрос");
	}

	DnsMessage message;
	message.responseCode = static_cast<std::uint8_t>(flags & 0x000F);
	message.isAuthoritative = (flags & 0x0400) != 0;
	for (std::size_t index = 0; index < answerCount; ++index)
	{
		message.answers.push_back(ReadRecord(packet, cursor));
	}
	for (std::size_t index = 0; index < authorityCount; ++index)
	{
		message.authorities.push_back(ReadRecord(packet, cursor));
	}
	for (std::size_t index = 0; index < additionalCount; ++index)
	{
		message.additionals.push_back(ReadRecord(packet, cursor));
	}
	return message;
}

SocketAddress ParseAddress(const std::string& ip)
{
	SocketAddress result;
	result.length = sizeof(sockaddr_in);
	result.family = AF_INET;
	auto* ipv4 = reinterpret_cast<sockaddr_in*>(&result.storage);
	ipv4->sin_family = AF_INET;
	ipv4->sin_port = htons(DNS_PORT);
	if (inet_pton(AF_INET, ip.c_str(), &ipv4->sin_addr) == 1)
	{
		return result;
	}

	result = SocketAddress{};
	result.length = sizeof(sockaddr_in6);
	result.family = AF_INET6;
	auto* ipv6 = reinterpret_cast<sockaddr_in6*>(&result.storage);
	ipv6->sin6_family = AF_INET6;
	ipv6->sin6_port = htons(DNS_PORT);
	if (inet_pton(AF_INET6, ip.c_str(), &ipv6->sin6_addr) == 1)
	{
		return result;
	}
	throw NetworkError("Некорректный IP-адрес DNS-сервера: " + ip);
}

[[noreturn]] void ThrowNetworkError(const std::string_view action)
{
	throw NetworkError(std::string(action) + ": " + std::strerror(errno));
}

void SetSocketTimeouts(const int descriptor)
{
	timeval timeout{};
	timeout.tv_sec = NETWORK_TIMEOUT_SECONDS;
	if (setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0
		|| setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)
	{
		ThrowNetworkError("Не удалось установить таймаут сокета");
	}
}

Bytes QueryUdp(const std::string& server, const Bytes& query)
{
	const SocketAddress address = ParseAddress(server);
	FileDescriptor socketDescriptor(socket(address.family, SOCK_DGRAM, IPPROTO_UDP));
	if (socketDescriptor.Get() < 0)
	{
		ThrowNetworkError("Не удалось создать UDP-сокет");
	}
	SetSocketTimeouts(socketDescriptor.Get());
	if (connect(socketDescriptor.Get(), reinterpret_cast<const sockaddr*>(&address.storage),
		address.length) < 0)
	{
		ThrowNetworkError("UDP connect");
	}

	const auto written = send(socketDescriptor.Get(), query.data(), query.size(), 0);
	if (written < 0 || static_cast<std::size_t>(written) != query.size())
	{
		ThrowNetworkError("Не удалось отправить UDP-запрос");
	}

	Bytes answer(MAX_DNS_MESSAGE_BYTES);
	const auto received = recv(socketDescriptor.Get(), answer.data(), answer.size(), 0);
	if (received < 0)
	{
		ThrowNetworkError("Не удалось получить UDP-ответ");
	}
	answer.resize(static_cast<std::size_t>(received));
	return answer;
}

FileDescriptor ConnectTcp(const SocketAddress& address)
{
	FileDescriptor descriptor(socket(address.family, SOCK_STREAM, IPPROTO_TCP));
	if (descriptor.Get() < 0)
	{
		ThrowNetworkError("Не удалось создать TCP-сокет");
	}
	const int originalFlags = fcntl(descriptor.Get(), F_GETFL, 0);
	if (originalFlags < 0 || fcntl(descriptor.Get(), F_SETFL, originalFlags | O_NONBLOCK) < 0)
	{
		ThrowNetworkError("Не удалось настроить TCP-сокет");
	}

	const int result = connect(descriptor.Get(), reinterpret_cast<const sockaddr*>(&address.storage),
		address.length);
	if (result < 0)
	{
		if (errno != EINPROGRESS)
		{
			ThrowNetworkError("TCP connect");
		}
		pollfd event{};
		event.fd = descriptor.Get();
		event.events = POLLOUT;
		int polled = -1;
		do
		{
			polled = poll(&event, 1, NETWORK_TIMEOUT_SECONDS * 1000);
		} while (polled < 0 && errno == EINTR);
		if (polled == 0)
		{
			throw NetworkError("Таймаут TCP connect");
		}
		if (polled < 0)
		{
			ThrowNetworkError("Ошибка ожидания TCP connect");
		}
		int socketError = 0;
		socklen_t errorLength = sizeof(socketError);
		if (getsockopt(descriptor.Get(), SOL_SOCKET, SO_ERROR, &socketError, &errorLength) < 0)
		{
			ThrowNetworkError("Ошибка проверки TCP connect");
		}
		if (socketError != 0)
		{
			throw NetworkError(std::string("TCP connect: ") + std::strerror(socketError));
		}
	}
	if (fcntl(descriptor.Get(), F_SETFL, originalFlags) < 0)
	{
		ThrowNetworkError("Не удалось вернуть блокирующий режим TCP");
	}
	SetSocketTimeouts(descriptor.Get());
	return descriptor;
}

void WriteAll(const int descriptor, const std::uint8_t* data, const std::size_t length)
{
	std::size_t written = 0;
	while (written < length)
	{
		const auto count = send(descriptor, data + written, length - written, MSG_NOSIGNAL);
		if (count < 0 && errno == EINTR)
		{
			continue;
		}
		if (count <= 0)
		{
			ThrowNetworkError("Не удалось отправить TCP-пакет");
		}
		written += static_cast<std::size_t>(count);
	}
}

void ReadExact(const int descriptor, std::uint8_t* data, const std::size_t length)
{
	std::size_t received = 0;
	while (received < length)
	{
		const auto count = recv(descriptor, data + received, length - received, 0);
		if (count < 0 && errno == EINTR)
		{
			continue;
		}
		if (count == 0)
		{
			throw NetworkError("DNS-сервер закрыл TCP-соединение до конца ответа");
		}
		if (count < 0)
		{
			ThrowNetworkError("Не удалось прочитать TCP-ответ");
		}
		received += static_cast<std::size_t>(count);
	}
}

Bytes QueryTcp(const std::string& server, const Bytes& query)
{
	FileDescriptor descriptor = ConnectTcp(ParseAddress(server));
	Bytes request;
	request.reserve(query.size() + 2);
	AppendU16(request, static_cast<std::uint16_t>(query.size()));
	request.insert(request.end(), query.begin(), query.end());
	WriteAll(descriptor.Get(), request.data(), request.size());

	std::array<std::uint8_t, 2> prefix{};
	ReadExact(descriptor.Get(), prefix.data(), prefix.size());
	const std::size_t length = (static_cast<std::size_t>(prefix[0]) << 8) | prefix[1];
	if (length < DNS_HEADER_BYTES)
	{
		throw ProtocolError("Некорректная длина DNS-ответа по TCP");
	}
	Bytes answer(length);
	ReadExact(descriptor.Get(), answer.data(), answer.size());
	return answer;
}

bool IsSubdomainOf(const std::string_view name, const std::string_view zone)
{
	if (zone == ".")
	{
		return true;
	}
	if (name == zone)
	{
		return true;
	}
	return name.size() > zone.size()
		&& name.compare(name.size() - zone.size(), zone.size(), zone) == 0
		&& name[name.size() - zone.size() - 1] == '.';
}

void AppendUnique(std::vector<std::string>& destination, const std::string& value)
{
	if (!value.empty() && std::find(destination.begin(), destination.end(), value) == destination.end())
	{
		destination.push_back(value);
	}
}

std::optional<Referral> FindReferral(const DnsMessage& message, const std::string& name)
{
	Referral referral;
	for (const DnsRecord& record : message.authorities)
	{
		if (record.type != TYPE_NS || record.recordClass != 1
			|| !IsSubdomainOf(name, record.name))
		{
			continue;
		}
		if (record.name.size() > referral.zone.size())
		{
			referral.zone = record.name;
			referral.nameservers.clear();
		}
		if (record.name == referral.zone)
		{
			AppendUnique(referral.nameservers, record.target);
		}
	}
	if (referral.nameservers.empty())
	{
		return std::nullopt;
	}
	return referral;
}

class PendingLookup final
{
public:
	PendingLookup(std::set<std::string>& active, std::string key)
		: m_active(active)
		, m_key(std::move(key))
	{
		if (!m_active.insert(m_key).second)
		{
			throw NetworkError("Обнаружен цикл разрешения DNS: " + m_key);
		}
	}

	~PendingLookup()
	{
		m_active.erase(m_key);
	}

	PendingLookup(const PendingLookup&) = delete;
	PendingLookup& operator=(const PendingLookup&) = delete;

private:
	std::set<std::string>& m_active;
	std::string m_key;
};

class DnsResolver final
{
public:
	explicit DnsResolver(const bool isDebug)
		: m_isDebug(isDebug)
		, m_generator(std::random_device{}())
	{
	}

	LookupResult Resolve(const std::string& name, const std::uint16_t type, const bool dnssec)
	{
		std::set<std::string> active;
		return ResolveName(name, type, dnssec, 0, active);
	}

private:
	void Debug(const std::string_view message) const
	{
		if (m_isDebug)
		{
			std::cerr << "[dns] " << message << '\n';
		}
	}

	DnsMessage Exchange(const std::string& server, const std::string& name,
		const std::uint16_t type, const bool dnssec)
	{
		const auto id = static_cast<std::uint16_t>(
			std::uniform_int_distribution<unsigned int>(0, 65535)(m_generator));
		const Bytes query = BuildQuery(name, type, id, dnssec);
		for (int attempt = 1; attempt <= MAX_UDP_ATTEMPTS; ++attempt)
		{
			try
			{
				Debug(std::format("UDP {}: {} {} (попытка {})", server, name, TypeName(type), attempt));
				Bytes answer = QueryUdp(server, query);
				if (IsTruncated(answer, id))
				{
					Debug(std::format("TC=1 от {}: повторяем тот же запрос по TCP", server));
					answer = QueryTcp(server, query);
					Debug(std::format("Получен полный TCP-ответ от {}", server));
				}
				return ParseResponse(answer, id, name, type);
			}
			catch (const NetworkError& error)
			{
				Debug(std::format("Ошибка сети {}: {}", server, error.what()));
				if (attempt == MAX_UDP_ATTEMPTS)
				{
					throw;
				}
			}
			catch (const ProtocolError& error)
			{
				Debug(std::format("Ошибка DNS-пакета {}: {}", server, error.what()));
				if (attempt == MAX_UDP_ATTEMPTS)
				{
					throw;
				}
			}
		}
		throw NetworkError("Нет ответа от DNS-сервера " + server);
	}

	std::vector<std::string> FindServerAddresses(const Referral& referral,
		const DnsMessage& message, const int depth, std::set<std::string>& active)
	{
		std::vector<std::string> addresses;
		for (const std::string& server : referral.nameservers)
		{
			for (const DnsRecord& record : message.additionals)
			{
				if (record.name == server && record.recordClass == 1
					&& (record.type == TYPE_A || record.type == TYPE_AAAA))
				{
					AppendUnique(addresses, record.address);
				}
			}
		}
		if (!addresses.empty())
		{
			Debug(std::format("Glue: получено {} адресов NS для {}", addresses.size(), referral.zone));
			return addresses;
		}

		Debug(std::format("Для {} нет glue: разрешаем имена NS отдельно", referral.zone));
		std::size_t candidates = 0;
		for (const std::string& server : referral.nameservers)
		{
			if (++candidates > MAX_NS_CANDIDATES)
			{
				break;
			}
			for (const auto type : {TYPE_A, TYPE_AAAA})
			{
				try
				{
					const LookupResult result = ResolveName(server, type, false, depth + 1, active);
					for (const DnsRecord& record : result.records)
					{
						AppendUnique(addresses, record.address);
					}
				}
				catch (const std::exception& error)
				{
					Debug(std::format("Не удалось найти {} для NS {}: {}",
						TypeName(type), server, error.what()));
				}
			}
			if (!addresses.empty())
			{
					break;
			}
		}
		return addresses;
	}

	std::optional<LookupResult> FindAnswers(const DnsMessage& message,
		const std::string& name, const std::uint16_t type,
		const bool dnssec, const int depth, std::set<std::string>& active)
	{
		std::string current = name;
		std::uint32_t minimumTtl = std::numeric_limits<std::uint32_t>::max();
		std::set<std::string> aliases;
		for (int step = 0; step < MAX_CNAME_LINKS; ++step)
		{
			if (!aliases.insert(current).second)
			{
				throw ProtocolError("Циклическая цепочка CNAME");
			}
			LookupResult result;
			for (const DnsRecord& record : message.answers)
			{
				if (record.name == current && record.type == type && record.recordClass == 1)
				{
					result.records.push_back(record);
					minimumTtl = std::min(minimumTtl, record.ttl);
				}
			}
			if (!result.records.empty())
			{
				result.ttl = minimumTtl;
				return result;
			}

			const auto alias = std::find_if(message.answers.begin(), message.answers.end(),
				[&](const DnsRecord& record)
				{
					return record.name == current && record.type == TYPE_CNAME
						&& record.recordClass == 1;
				});
			if (alias == message.answers.end() || alias->target.empty())
			{
				if (current != name)
				{
					LookupResult resolved = ResolveName(current, type, dnssec, depth + 1, active);
					resolved.ttl = std::min(resolved.ttl, minimumTtl);
					return resolved;
				}
				return std::nullopt;
			}
			minimumTtl = std::min(minimumTtl, alias->ttl);
			Debug(std::format("CNAME: {} -> {}", current, alias->target));
			current = alias->target;
		}
		throw ProtocolError("Слишком длинная цепочка CNAME");
	}

	LookupResult ResolveName(const std::string& name, const std::uint16_t type,
		const bool dnssec, const int depth, std::set<std::string>& active)
	{
		if (depth > MAX_LOOKUP_DEPTH)
		{
			throw NetworkError("Превышена глубина итеративного разрешения DNS");
		}
		PendingLookup pending(active, std::format("{}:{}", name, type));
		std::vector<std::string> servers(ROOT_SERVERS.begin(), ROOT_SERVERS.end());
		std::set<std::string> visitedDelegations;
		for (int hop = 0; hop < MAX_DELEGATIONS; ++hop)
		{
			std::vector<std::string> nextServers;
			std::string error = "DNS-серверы не ответили";
			for (const std::string& server : servers)
			{
				try
				{
					const DnsMessage response = Exchange(server, name, type, dnssec);
					if (response.responseCode == 3)
					{
						throw RecordNotFound("NXDOMAIN: домен " + name + " не существует");
					}
					if (response.responseCode != 0)
					{
						error = std::format("{} вернул DNS RCODE={}", server, response.responseCode);
						Debug(error);
						continue;
					}

					if (auto answer = FindAnswers(response, name, type, dnssec, depth, active))
					{
						Debug(std::format("Готово: {} записей типа {}", answer->records.size(),
							TypeName(type)));
						return *answer;
					}
					if (response.isAuthoritative)
					{
						throw RecordNotFound(
							std::format("Нет записи {} для {}", TypeName(type), name));
					}

					const auto referral = FindReferral(response, name);
					if (!referral)
					{
						error = "Ответ не содержит ни результата, ни делегирования NS";
						continue;
					}
					Debug(std::format("Делегирование: {} -> {} серверов NS", referral->zone,
						referral->nameservers.size()));
					nextServers = FindServerAddresses(*referral, response, depth, active);
					if (nextServers.empty())
					{
						error = "Не удалось определить IP-адреса делегированных NS";
						continue;
					}
					const std::string fingerprint = std::format("{}:{}", referral->zone, server);
					if (!visitedDelegations.insert(fingerprint).second)
					{
						throw NetworkError("Обнаружен повтор одного и того же делегирования");
					}
					break;
				}
				catch (const RecordNotFound&)
				{
					throw;
				}
				catch (const std::exception& exception)
				{
					error = exception.what();
					Debug(std::format("Сервер {}: {}", server, error));
					nextServers.clear();
				}
			}
			if (nextServers.empty())
			{
				throw NetworkError(std::format("Не удалось разрешить {} {}: {}",
					name, TypeName(type), error));
			}
			servers = std::move(nextServers);
		}
		throw NetworkError("Превышено количество переходов между DNS-серверами");
	}

	bool m_isDebug;
	std::mt19937 m_generator;
};

std::string FormatRecord(const DnsRecord& record)
{
	if (record.type == TYPE_A || record.type == TYPE_AAAA)
	{
		return record.value;
	}
	return std::format("{} {} {}", record.name, TypeName(record.type), record.value);
}

class DnsCache final
{
public:
	explicit DnsCache(std::filesystem::path path)
		: m_path(std::move(path))
	{
		Load();
	}

	[[nodiscard]] std::optional<std::vector<std::string>> Find(
		const std::string& domain, const std::uint16_t type) const
	{
		const auto entry = m_entries.find({domain, type});
		if (entry == m_entries.end() || entry->second.expiresAt <= std::time(nullptr))
		{
			return std::nullopt;
		}
		return entry->second.lines;
	}

	void Store(const std::string& domain, const std::uint16_t type,
		const std::uint32_t ttl, const std::vector<std::string>& lines)
	{
		if (ttl == 0 || lines.empty())
		{
			return;
		}
		m_entries[{domain, type}] = {
			static_cast<std::int64_t>(std::time(nullptr)) + ttl, lines};
		Save();
	}

	void Show() const
	{
		const auto now = static_cast<std::int64_t>(std::time(nullptr));
		bool hasEntries = false;
		for (const auto& [key, entry] : m_entries)
		{
			if (entry.expiresAt <= now)
			{
				continue;
			}
			hasEntries = true;
			for (const std::string& line : entry.lines)
			{
				std::cout << key.first << ' ' << TypeName(key.second) << " TTL="
					<< entry.expiresAt - now << "s " << line << '\n';
			}
		}
		if (!hasEntries)
		{
			std::cout << "Кэш пуст\n";
		}
	}

	void Clear()
	{
		m_entries.clear();
		std::error_code error;
		std::filesystem::remove(m_path, error);
		if (error)
		{
			throw std::runtime_error("Не удалось очистить кэш: " + error.message());
		}
	}

private:
	struct Entry
	{
		std::int64_t expiresAt = 0;
		std::vector<std::string> lines;
	};

	void Load()
	{
		std::ifstream file(m_path);
		std::string line;
		const auto now = static_cast<std::int64_t>(std::time(nullptr));
		while (std::getline(file, line))
		{
			std::istringstream input(line);
			std::string domain;
			std::string value;
			unsigned int type = 0;
			std::int64_t expiresAt = 0;
			if (!(input >> std::quoted(domain) >> type >> expiresAt >> std::quoted(value))
				|| type == 0 || type > std::numeric_limits<std::uint16_t>::max()
				|| expiresAt <= now)
			{
				continue;
			}
			auto& entry = m_entries[{domain, static_cast<std::uint16_t>(type)}];
			if (entry.expiresAt != expiresAt)
			{
				entry.expiresAt = expiresAt;
				entry.lines.clear();
			}
			entry.lines.push_back(value);
		}
	}

	void Save() const
	{
		const auto temporary = m_path.string() + ".tmp";
		std::ofstream file(temporary, std::ios::trunc);
		if (!file)
		{
			throw std::runtime_error("Не удалось создать файл кэша");
		}
		const auto now = static_cast<std::int64_t>(std::time(nullptr));
		for (const auto& [key, entry] : m_entries)
		{
			if (entry.expiresAt <= now)
			{
				continue;
			}
			for (const std::string& line : entry.lines)
			{
				file << std::quoted(key.first) << ' ' << key.second << ' '
					<< entry.expiresAt << ' ' << std::quoted(line) << '\n';
			}
		}
		file.close();
		if (!file)
		{
			throw std::runtime_error("Не удалось сохранить файл кэша");
		}
		std::filesystem::rename(temporary, m_path);
	}

	std::filesystem::path m_path;
	std::map<std::pair<std::string, std::uint16_t>, Entry> m_entries;
};

} // namespace

int main(const int argc, char* argv[])
{
	try
	{
		const Options options = ParseOptions(argc, argv);
		DnsCache cache(CACHE_FILE);
		if (options.command == Command::ShowCache)
		{
			cache.Show();
			return 0;
		}
		if (options.command == Command::ClearCache)
		{
			cache.Clear();
			std::cout << "Кэш очищен\n";
			return 0;
		}

		if (options.useCache)
		{
			if (const auto cached = cache.Find(options.domain, options.type))
			{
				if (options.isDebug)
				{
					std::cerr << "[dns] Результат взят из локального кэша\n";
				}
				for (const std::string& line : *cached)
				{
					std::cout << line << '\n';
				}
				return 0;
			}
		}

		DnsResolver resolver(options.isDebug);
		const LookupResult result = resolver.Resolve(options.domain, options.type,
			options.requestDnssec);
		std::vector<std::string> output;
		for (const DnsRecord& record : result.records)
		{
			output.push_back(FormatRecord(record));
		}
		for (const std::string& line : output)
		{
			std::cout << line << '\n';
		}
		if (options.useCache)
		{
			try
			{
				cache.Store(options.domain, options.type, result.ttl, output);
			}
			catch (const std::exception& error)
			{
				if (options.isDebug)
				{
					std::cerr << "[dns] Не удалось сохранить кэш: " << error.what() << '\n';
				}
			}
		}
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Ошибка: " << error.what() << '\n';
		return 1;
	}
}
