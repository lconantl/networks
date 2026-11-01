#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#ifndef STUDENT_NAME
#define STUDENT_NAME "Ivan Petrov"
#endif

#define CLIENT_NAME "Client of " STUDENT_NAME
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT "5555"

enum
{
	MIN_NUMBER = 1,
	MAX_NUMBER = 100,
	MAX_MESSAGE_BYTES = 512,
	MAX_NAME_BYTES = 128,
	INPUT_BYTES = 64,
	RECEIVE_TIMEOUT_SECONDS = 10,
};

typedef struct ServerReply
{
	char name[MAX_NAME_BYTES];
	int number;
} ServerReply;

typedef enum ReplyParse
{
	ReplyParseIncomplete,
	ReplyParseComplete,
	ReplyParseInvalid,
} ReplyParse;

static void LogStep(const char* format, ...) __attribute__((format(printf, 1, 2)));

static void LogStep(const char* format, ...)
{
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	struct tm local;
	localtime_r(&now.tv_sec, &local);

	printf(
		"[%02d:%02d:%02d.%03ld] ",
		local.tm_hour,
		local.tm_min,
		local.tm_sec,
		now.tv_nsec / 1000000);

	va_list arguments;
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);

	putchar('\n');
	fflush(stdout);
}

static void ReportError(const char* action)
{
	fprintf(stderr, "[Ошибка] %s: %s\n", action, strerror(errno));
}

static bool ParseInteger(const char* text, int* value)
{
	char* end = NULL;
	errno = 0;
	const long parsed = strtol(text, &end, 10);

	while (end != NULL && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
	{
		++end;
	}

	if (errno != 0 || end == text || *end != '\0' || parsed < INT_MIN || parsed > INT_MAX)
	{
		return false;
	}

	*value = (int)parsed;
	return true;
}

static bool IsNumberInRange(const int number)
{
	return number >= MIN_NUMBER && number <= MAX_NUMBER;
}

static bool AskNumber(int* number)
{
	char input[INPUT_BYTES];

	while (true)
	{
		printf(
			"Введите число от %d до %d (другое число остановит сервер): ", MIN_NUMBER, MAX_NUMBER);
		fflush(stdout);

		if (fgets(input, sizeof(input), stdin) == NULL)
		{
			fprintf(stderr, "[Ошибка] Ввод закончился, число не получено\n");
			return false;
		}

		if (ParseInteger(input, number))
		{
			return true;
		}

		printf("Это не целое число, попробуйте ещё раз\n");
	}
}

static int ConnectToServer(const char* host, const char* port)
{
	struct addrinfo hints;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	struct addrinfo* addresses = NULL;
	const int resolveResult = getaddrinfo(host, port, &hints, &addresses);
	if (resolveResult != 0)
	{
		fprintf(
			stderr, "[Ошибка] Не удалось найти адрес %s: %s\n", host, gai_strerror(resolveResult));
		return -1;
	}

	int connected = -1;
	for (const struct addrinfo* address = addresses; address != NULL && connected < 0;
		 address = address->ai_next)
	{
		const int candidate
			= socket(address->ai_family, address->ai_socktype | SOCK_CLOEXEC, address->ai_protocol);
		if (candidate < 0)
		{
			ReportError("Не удалось создать сокет");
			continue;
		}

		LogStep("Создан TCP-сокет (дескриптор %d)", candidate);
		LogStep("Подключение к %s:%s...", host, port);

		if (connect(candidate, address->ai_addr, address->ai_addrlen) == 0)
		{
			connected = candidate;
			break;
		}

		ReportError("Не удалось подключиться");
		close(candidate);
		LogStep("Сокет %d закрыт", candidate);
	}

	freeaddrinfo(addresses);
	return connected;
}

static bool ApplyReceiveTimeout(const int socket)
{
	struct timeval timeout = {.tv_sec = RECEIVE_TIMEOUT_SECONDS, .tv_usec = 0};
	if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0)
	{
		ReportError("Не удалось установить таймаут ожидания");
		return false;
	}

	return true;
}

static bool SendAll(const int socket, const char* data, size_t length)
{
	while (length > 0)
	{
		const ssize_t sent = send(socket, data, length, MSG_NOSIGNAL);
		if (sent < 0 && errno == EINTR)
		{
			continue;
		}
		if (sent <= 0)
		{
			ReportError("Ошибка отправки");
			return false;
		}

		data += sent;
		length -= (size_t)sent;
	}

	return true;
}

static bool SendRequest(const int socket, const int number)
{
	char message[MAX_MESSAGE_BYTES];
	const int length = snprintf(message, sizeof(message), "%s\n%d\n", CLIENT_NAME, number);

	if (!SendAll(socket, message, (size_t)length))
	{
		return false;
	}

	LogStep("Отправлено %d байт: имя «%s», число %d", length, CLIENT_NAME, number);
	return true;
}

static ReplyParse ParseServerReply(const char* buffer, const size_t length, ServerReply* reply)
{
	const char* nameEnd = memchr(buffer, '\n', length);
	if (nameEnd == NULL)
	{
		return length >= MAX_MESSAGE_BYTES - 1 ? ReplyParseInvalid : ReplyParseIncomplete;
	}

	const char* numberStart = nameEnd + 1;
	const char* numberEnd = memchr(numberStart, '\n', length - (size_t)(numberStart - buffer));
	if (numberEnd == NULL)
	{
		return length >= MAX_MESSAGE_BYTES - 1 ? ReplyParseInvalid : ReplyParseIncomplete;
	}

	size_t nameLength = (size_t)(nameEnd - buffer);
	if (nameLength > 0 && buffer[nameLength - 1] == '\r')
	{
		--nameLength;
	}
	if (nameLength == 0 || nameLength >= MAX_NAME_BYTES)
	{
		return ReplyParseInvalid;
	}

	char numberText[INPUT_BYTES];
	const size_t numberLength = (size_t)(numberEnd - numberStart);
	if (numberLength == 0 || numberLength >= sizeof(numberText))
	{
		return ReplyParseInvalid;
	}

	memcpy(numberText, numberStart, numberLength);
	numberText[numberLength] = '\0';
	memcpy(reply->name, buffer, nameLength);
	reply->name[nameLength] = '\0';

	return ParseInteger(numberText, &reply->number) ? ReplyParseComplete : ReplyParseInvalid;
}

static bool ReceiveReply(const int socket, ServerReply* reply)
{
	char buffer[MAX_MESSAGE_BYTES];
	size_t length = 0;

	while (length < sizeof(buffer) - 1)
	{
		const ssize_t received = recv(socket, buffer + length, sizeof(buffer) - 1 - length, 0);
		if (received < 0 && errno == EINTR)
		{
			continue;
		}
		if (received < 0)
		{
			ReportError(errno == EAGAIN ? "Сервер не ответил за 10 секунд" : "Ошибка приёма");
			return false;
		}
		if (received == 0)
		{
			fprintf(stderr, "[Ошибка] Сервер закрыл соединение, не прислав полный ответ\n");
			return false;
		}

		length += (size_t)received;
		LogStep("Получено %zd байт от сервера", received);

		const ReplyParse result = ParseServerReply(buffer, length, reply);
		if (result != ReplyParseIncomplete)
		{
			if (result == ReplyParseInvalid)
			{
				fprintf(stderr, "[Ошибка] Ответ сервера имеет неверный формат\n");
			}
			return result == ReplyParseComplete;
		}
	}

	fprintf(stderr, "[Ошибка] Ответ сервера слишком длинный\n");
	return false;
}

static void PrintResult(const int clientNumber, const ServerReply* reply)
{
	const long long sum = (long long)clientNumber + (long long)reply->number;

	printf("\n");
	printf("  Имя клиента:    %s\n", CLIENT_NAME);
	printf("  Имя сервера:    %s\n", reply->name);
	printf("  Число клиента:  %d\n", clientNumber);
	printf("  Число сервера:  %d\n", reply->number);
	printf("  Сумма:          %lld\n", sum);
	printf("\n");
	fflush(stdout);
}

static bool Exchange(const int socket, const int number)
{
	ServerReply reply;
	if (!ApplyReceiveTimeout(socket) || !SendRequest(socket, number)
		|| !ReceiveReply(socket, &reply))
	{
		return false;
	}

	PrintResult(number, &reply);
	return true;
}

static bool IsPortText(const char* text)
{
	int port = 0;
	return ParseInteger(text, &port) && port > 0 && port <= 65535;
}

int main(const int argc, char* argv[])
{
	const char* host = argc > 1 ? argv[1] : DEFAULT_HOST;
	const char* port = argc > 2 ? argv[2] : DEFAULT_PORT;

	if (!IsPortText(port))
	{
		fprintf(stderr, "[Ошибка] Порт должен быть числом от 1 до 65535: %s\n", port);
		return EXIT_FAILURE;
	}

	printf("%s → сервер %s:%s\n", CLIENT_NAME, host, port);

	int number = 0;
	if (!AskNumber(&number))
	{
		return EXIT_FAILURE;
	}
	if (!IsNumberInRange(number))
	{
		printf("Число %d вне диапазона %d–%d: сервер ответит и завершит работу\n",
			number,
			MIN_NUMBER,
			MAX_NUMBER);
	}

	const int socket = ConnectToServer(host, port);
	if (socket < 0)
	{
		return EXIT_FAILURE;
	}
	LogStep("Соединение установлено");

	const bool isSuccess = Exchange(socket, number);

	close(socket);
	LogStep("Сокет %d закрыт, клиент завершает работу", socket);

	return isSuccess ? EXIT_SUCCESS : EXIT_FAILURE;
}
