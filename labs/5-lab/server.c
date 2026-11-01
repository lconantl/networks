#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef STUDENT_NAME
#define STUDENT_NAME "Ivan Petrov"
#endif

#define SERVER_NAME "Server of " STUDENT_NAME

enum
{
	DEFAULT_PORT = 5555,
	MIN_PORT = 1024,
	MAX_PORT = 65535,
	SERVER_NUMBER = 50,
	MIN_NUMBER = 1,
	MAX_NUMBER = 100,
	MAX_MESSAGE_BYTES = 512,
	MAX_NAME_BYTES = 128,
	NUMBER_TEXT_BYTES = 32,
	PEER_TEXT_BYTES = 64,
	INPUT_BYTES = 64,
	LISTEN_BACKLOG = 1024,
	CLIENT_TIMEOUT_SECONDS = 10,
	MAX_CONNECTIONS = 4096,
	EPOLL_BATCH = 256,
	MIN_WORKERS = 2,
	MAX_WORKERS = 16,
	SHUTDOWN_EXIT_CODE = 3,
	FAILURE_EXIT_CODE = 1,
};

static const uint64_t NANOSECONDS_PER_SECOND = 1000000000ULL;

typedef enum ServerMode
{
	ServerModeSequential = 1,
	ServerModeForking = 2,
	ServerModePooled = 3,
} ServerMode;

typedef struct ServerOptions
{
	ServerMode mode;
	uint16_t port;
	bool isVerbose;
} ServerOptions;

typedef struct ClientMessage
{
	char name[MAX_NAME_BYTES];
	int number;
} ClientMessage;

typedef enum MessageParse
{
	MessageParseIncomplete,
	MessageParseComplete,
	MessageParseInvalid,
} MessageParse;

typedef enum ServeOutcome
{
	ServeOutcomeAnswered,
	ServeOutcomeShutdownRequested,
	ServeOutcomeFailed,
} ServeOutcome;

static void PrintTimestamp(void)
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
}

static void LogEvent(const char* format, ...) __attribute__((format(printf, 1, 2)));

static void LogEvent(const char* format, ...)
{
	flockfile(stdout);
	PrintTimestamp();

	va_list arguments;
	va_start(arguments, format);
	vprintf(format, arguments);
	va_end(arguments);

	putchar('\n');
	fflush(stdout);
	funlockfile(stdout);
}

static void LogSystemError(const char* action)
{
	LogEvent("[Ошибка] %s: %s", action, strerror(errno));
}

static uint64_t NowNanoseconds(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	return (uint64_t)now.tv_sec * NANOSECONDS_PER_SECOND + (uint64_t)now.tv_nsec;
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

static bool IsPrintableName(const char* name, const size_t length)
{
	for (size_t index = 0; index < length; ++index)
	{
		if ((unsigned char)name[index] < 0x20)
		{
			return false;
		}
	}

	return length > 0;
}

static MessageParse ParseClientMessage(
	const char* buffer, const size_t length, ClientMessage* message)
{
	const MessageParse incomplete
		= length >= MAX_MESSAGE_BYTES - 1 ? MessageParseInvalid : MessageParseIncomplete;

	const char* nameEnd = memchr(buffer, '\n', length);
	if (nameEnd == NULL)
	{
		return incomplete;
	}

	const char* numberStart = nameEnd + 1;
	const char* numberEnd = memchr(numberStart, '\n', length - (size_t)(numberStart - buffer));
	if (numberEnd == NULL)
	{
		return incomplete;
	}

	size_t nameLength = (size_t)(nameEnd - buffer);
	if (nameLength > 0 && buffer[nameLength - 1] == '\r')
	{
		--nameLength;
	}

	char numberText[NUMBER_TEXT_BYTES];
	const size_t numberLength = (size_t)(numberEnd - numberStart);
	if (!IsPrintableName(buffer, nameLength) || nameLength >= MAX_NAME_BYTES || numberLength == 0
		|| numberLength >= sizeof(numberText))
	{
		return MessageParseInvalid;
	}

	memcpy(message->name, buffer, nameLength);
	message->name[nameLength] = '\0';
	memcpy(numberText, numberStart, numberLength);
	numberText[numberLength] = '\0';

	return ParseInteger(numberText, &message->number) ? MessageParseComplete : MessageParseInvalid;
}

static MessageParse ParseAtEndOfStream(char* buffer, const size_t length, ClientMessage* message)
{
	if (length == 0 || length >= MAX_MESSAGE_BYTES - 1 || buffer[length - 1] == '\n')
	{
		return MessageParseInvalid;
	}

	buffer[length] = '\n';
	const MessageParse result = ParseClientMessage(buffer, length + 1, message);

	return result == MessageParseComplete ? result : MessageParseInvalid;
}

static void PrintExchange(const char* peer, const ClientMessage* message)
{
	const long long sum = (long long)message->number + SERVER_NUMBER;

	flockfile(stdout);
	PrintTimestamp();
	printf("Сообщение от %s\n", peer);
	printf("    Имя клиента:    %s\n", message->name);
	printf("    Имя сервера:    %s\n", SERVER_NAME);
	printf("    Число клиента:  %d\n", message->number);
	printf("    Число сервера:  %d\n", SERVER_NUMBER);
	printf("    Сумма:          %lld\n", sum);
	fflush(stdout);
	funlockfile(stdout);
}

static int FormatReply(char* buffer, const size_t capacity)
{
	return snprintf(buffer, capacity, "%s\n%d\n", SERVER_NAME, SERVER_NUMBER);
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
			return false;
		}

		data += sent;
		length -= (size_t)sent;
	}

	return true;
}

static bool SendReply(const int socket, const char* peer, const bool isVerbose)
{
	char reply[MAX_MESSAGE_BYTES];
	const int length = FormatReply(reply, sizeof(reply));

	if (!SendAll(socket, reply, (size_t)length))
	{
		LogEvent("[Ошибка] Не удалось отправить ответ %s: %s", peer, strerror(errno));
		return false;
	}

	if (isVerbose)
	{
		LogEvent("Отправлено %d байт клиенту %s: имя «%s», число %d",
			length,
			peer,
			SERVER_NAME,
			SERVER_NUMBER);
	}
	return true;
}

static void CloseClient(const int socket, const char* peer, const bool isVerbose)
{
	close(socket);
	if (isVerbose)
	{
		LogEvent("Соединение с %s закрыто", peer);
	}
}

static void DescribePeer(const struct sockaddr_in* address, char* text, const size_t capacity)
{
	char host[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &address->sin_addr, host, sizeof(host));
	snprintf(text, capacity, "%s:%u", host, ntohs(address->sin_port));
}

static int AcceptClient(const int listener, const int flags, char* peer, const size_t capacity)
{
	struct sockaddr_in address;
	socklen_t length = sizeof(address);

	const int client = accept4(listener, (struct sockaddr*)&address, &length, SOCK_CLOEXEC | flags);
	if (client >= 0)
	{
		DescribePeer(&address, peer, capacity);
	}

	return client;
}

static bool IsTransientAcceptError(const int code)
{
	return code == EAGAIN || code == EWOULDBLOCK || code == EINTR || code == ECONNABORTED
		|| code == EPROTO || code == EMFILE || code == ENFILE || code == ENOBUFS || code == ENOMEM;
}

static bool SetSocketTimeouts(const int socket, const int seconds)
{
	const struct timeval timeout = {.tv_sec = seconds, .tv_usec = 0};

	return setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0
		&& setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0;
}

static int CreateListener(const uint16_t port)
{
	const int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (listener < 0)
	{
		LogSystemError("Не удалось создать TCP-сокет");
		return -1;
	}
	LogEvent("Создан TCP-сокет (дескриптор %d)", listener);

	const int enabled = 1;
	setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));

	struct sockaddr_in address;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_ANY);

	if (bind(listener, (const struct sockaddr*)&address, sizeof(address)) != 0)
	{
		LogEvent("[Ошибка] Не удалось привязать сокет к порту %u: %s", port, strerror(errno));
		close(listener);
		return -1;
	}
	LogEvent("Сокет привязан к 0.0.0.0:%u", port);

	if (listen(listener, LISTEN_BACKLOG) != 0)
	{
		LogSystemError("Не удалось начать прослушивание");
		close(listener);
		return -1;
	}
	LogEvent("Сервер «%s» слушает порт %u (очередь %d)", SERVER_NAME, port, LISTEN_BACKLOG);

	return listener;
}

static void CloseListener(const int listener)
{
	close(listener);
	LogEvent("Слушающий сокет %d закрыт, новые подключения не принимаются", listener);
}

typedef struct SignalGuard
{
	sigset_t previousMask;
	int descriptor;
} SignalGuard;

static bool OpenSignalGuard(SignalGuard* guard, const bool isChildSignalWatched)
{
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	if (isChildSignalWatched)
	{
		sigaddset(&set, SIGCHLD);
	}

	if (pthread_sigmask(SIG_BLOCK, &set, &guard->previousMask) != 0)
	{
		LogEvent("[Ошибка] Не удалось заблокировать сигналы");
		return false;
	}

	guard->descriptor = signalfd(-1, &set, SFD_CLOEXEC | SFD_NONBLOCK);
	if (guard->descriptor < 0)
	{
		LogSystemError("Не удалось создать signalfd");
		pthread_sigmask(SIG_SETMASK, &guard->previousMask, NULL);
		return false;
	}

	return true;
}

static void CloseSignalGuard(SignalGuard* guard)
{
	close(guard->descriptor);
	pthread_sigmask(SIG_SETMASK, &guard->previousMask, NULL);
}

static int ReadSignal(const SignalGuard* guard)
{
	struct signalfd_siginfo info;
	const ssize_t result = read(guard->descriptor, &info, sizeof(info));

	return result == (ssize_t)sizeof(info) ? (int)info.ssi_signo : 0;
}

static const char* DescribeSignal(const int number)
{
	switch (number)
	{
	case SIGINT: return "SIGINT (Ctrl+C)";
	case SIGTERM: return "SIGTERM";
	default: return "сигнал остановки";
	}
}

typedef enum ReceiveStatus
{
	ReceiveStatusComplete,
	ReceiveStatusTimeout,
	ReceiveStatusClosed,
	ReceiveStatusInvalid,
	ReceiveStatusError,
} ReceiveStatus;

static ReceiveStatus ReceiveMessageBlocking(
	const int socket, const char* peer, ClientMessage* message, const bool isVerbose)
{
	char buffer[MAX_MESSAGE_BYTES];
	size_t length = 0;

	while (true)
	{
		const ssize_t received = recv(socket, buffer + length, sizeof(buffer) - 1 - length, 0);
		if (received < 0 && errno == EINTR)
		{
			continue;
		}
		if (received < 0)
		{
			return errno == EAGAIN || errno == EWOULDBLOCK ? ReceiveStatusTimeout
														   : ReceiveStatusError;
		}
		if (received == 0)
		{
			return ParseAtEndOfStream(buffer, length, message) == MessageParseComplete
				? ReceiveStatusComplete
				: ReceiveStatusClosed;
		}

		length += (size_t)received;
		if (isVerbose)
		{
			LogEvent("Получено %zd байт от %s", received, peer);
		}

		const MessageParse result = ParseClientMessage(buffer, length, message);
		if (result == MessageParseComplete)
		{
			return ReceiveStatusComplete;
		}
		if (result == MessageParseInvalid)
		{
			return ReceiveStatusInvalid;
		}
	}
}

static void LogReceiveFailure(const ReceiveStatus status, const char* peer)
{
	switch (status)
	{
	case ReceiveStatusTimeout:
		LogEvent("Клиент %s молчит %d с — соединение разорвано по таймауту",
			peer,
			CLIENT_TIMEOUT_SECONDS);
		break;
	case ReceiveStatusClosed: LogEvent("Клиент %s отключился, не прислав сообщение", peer); break;
	case ReceiveStatusInvalid:
		LogEvent("Клиент %s прислал сообщение неверного формата", peer);
		break;
	case ReceiveStatusError: LogEvent("[Ошибка] Приём от %s: %s", peer, strerror(errno)); break;
	case ReceiveStatusComplete: break;
	}
}

static ServeOutcome AnswerClient(
	const int socket, const char* peer, const ClientMessage* message, const bool isVerbose)
{
	if (isVerbose)
	{
		PrintExchange(peer, message);
	}

	if (!SendReply(socket, peer, isVerbose))
	{
		return ServeOutcomeFailed;
	}

	if (!IsNumberInRange(message->number))
	{
		LogEvent("Число %d вне диапазона %d–%d: получена команда остановки сервера",
			message->number,
			MIN_NUMBER,
			MAX_NUMBER);
		return ServeOutcomeShutdownRequested;
	}

	return ServeOutcomeAnswered;
}

static ServeOutcome ServeClientBlocking(const int socket, const char* peer, const bool isVerbose)
{
	if (isVerbose)
	{
		LogEvent("Подключился клиент %s (дескриптор %d)", peer, socket);
	}

	if (!SetSocketTimeouts(socket, CLIENT_TIMEOUT_SECONDS))
	{
		LogSystemError("Не удалось установить таймауты");
		CloseClient(socket, peer, isVerbose);
		return ServeOutcomeFailed;
	}

	ClientMessage message;
	const ReceiveStatus status = ReceiveMessageBlocking(socket, peer, &message, isVerbose);

	ServeOutcome outcome = ServeOutcomeFailed;
	if (status == ReceiveStatusComplete)
	{
		outcome = AnswerClient(socket, peer, &message, isVerbose);
	}
	else
	{
		LogReceiveFailure(status, peer);
	}

	CloseClient(socket, peer, isVerbose);
	return outcome;
}

typedef enum WaitResult
{
	WaitResultClient,
	WaitResultSignal,
	WaitResultError,
} WaitResult;

static WaitResult WaitForClientOrSignal(const int listener, const SignalGuard* guard)
{
	struct pollfd watched[2] = {
		{.fd = listener, .events = POLLIN, .revents = 0},
		{.fd = guard->descriptor, .events = POLLIN, .revents = 0},
	};

	while (true)
	{
		const int ready = poll(watched, 2, -1);
		if (ready < 0 && errno == EINTR)
		{
			continue;
		}
		if (ready < 0)
		{
			LogSystemError("Ошибка poll");
			return WaitResultError;
		}

		if ((watched[1].revents & POLLIN) != 0)
		{
			return WaitResultSignal;
		}
		if ((watched[0].revents & POLLIN) != 0)
		{
			return WaitResultClient;
		}
	}
}

static int RunSequential(const int listener, const SignalGuard* guard, const bool isVerbose)
{
	unsigned long long served = 0;

	while (true)
	{
		const WaitResult wait = WaitForClientOrSignal(listener, guard);
		if (wait == WaitResultSignal)
		{
			LogEvent("Получен %s — сервер останавливается", DescribeSignal(ReadSignal(guard)));
			break;
		}
		if (wait == WaitResultError)
		{
			return FAILURE_EXIT_CODE;
		}

		char peer[PEER_TEXT_BYTES];
		const int client = AcceptClient(listener, 0, peer, sizeof(peer));
		if (client < 0)
		{
			if (!IsTransientAcceptError(errno))
			{
				LogSystemError("Ошибка accept");
			}
			continue;
		}

		const ServeOutcome outcome = ServeClientBlocking(client, peer, isVerbose);
		served += outcome != ServeOutcomeFailed ? 1 : 0;

		if (outcome == ServeOutcomeShutdownRequested)
		{
			break;
		}
	}

	CloseListener(listener);
	LogEvent("Обслужено клиентов: %llu", served);
	return EXIT_SUCCESS;
}

typedef struct ChildTracker
{
	unsigned long long started;
	unsigned long long finished;
	unsigned long long active;
	bool isShutdownRequested;
} ChildTracker;

static void RecordChildExit(ChildTracker* tracker, const pid_t pid, const int status)
{
	--tracker->active;
	++tracker->finished;

	if (WIFEXITED(status) && WEXITSTATUS(status) == SHUTDOWN_EXIT_CODE)
	{
		tracker->isShutdownRequested = true;
		LogEvent("Процесс %d передал команду остановки сервера", (int)pid);
	}
}

static void ReapChildren(ChildTracker* tracker, const bool isBlocking)
{
	while (tracker->active > 0)
	{
		int status = 0;
		const pid_t pid = waitpid(-1, &status, isBlocking ? 0 : WNOHANG);
		if (pid < 0 && errno == EINTR)
		{
			continue;
		}
		if (pid <= 0)
		{
			return;
		}

		RecordChildExit(tracker, pid, status);
	}
}

static void RunChild(const int client,
	const char* peer,
	const int listener,
	const SignalGuard* guard,
	const bool isVerbose)
{
	close(listener);
	close(guard->descriptor);

	const ServeOutcome outcome = ServeClientBlocking(client, peer, isVerbose);
	fflush(stdout);

	_exit(outcome == ServeOutcomeShutdownRequested ? SHUTDOWN_EXIT_CODE : EXIT_SUCCESS);
}

static void SpawnChild(
	const int listener, const SignalGuard* guard, ChildTracker* tracker, const bool isVerbose)
{
	char peer[PEER_TEXT_BYTES];
	const int client = AcceptClient(listener, 0, peer, sizeof(peer));
	if (client < 0)
	{
		if (!IsTransientAcceptError(errno))
		{
			LogSystemError("Ошибка accept");
		}
		return;
	}

	fflush(NULL);
	const pid_t pid = fork();
	if (pid == 0)
	{
		RunChild(client, peer, listener, guard, isVerbose);
	}

	if (pid < 0)
	{
		LogSystemError("Не удалось создать процесс");
		close(client);
		return;
	}

	close(client);
	++tracker->started;
	++tracker->active;
	if (isVerbose)
	{
		LogEvent("Клиент %s передан процессу %d (активных процессов: %llu)",
			peer,
			(int)pid,
			tracker->active);
	}
}

static bool HandleForkingSignal(const SignalGuard* guard, ChildTracker* tracker)
{
	const int number = ReadSignal(guard);
	if (number == SIGCHLD)
	{
		ReapChildren(tracker, false);
		return tracker->isShutdownRequested;
	}

	LogEvent("Получен %s — сервер останавливается", DescribeSignal(number));
	return true;
}

static int RunForking(int listener, const SignalGuard* guard, const bool isVerbose)
{
	ChildTracker tracker = {0, 0, 0, false};

	while (true)
	{
		const WaitResult wait = WaitForClientOrSignal(listener, guard);
		if (wait == WaitResultError)
		{
			break;
		}
		if (wait == WaitResultSignal && HandleForkingSignal(guard, &tracker))
		{
			break;
		}
		if (wait == WaitResultClient)
		{
			SpawnChild(listener, guard, &tracker, isVerbose);
		}
	}

	CloseListener(listener);
	LogEvent("Ожидание завершения дочерних процессов: %llu", tracker.active);
	ReapChildren(&tracker, true);
	LogEvent("Процессов запущено: %llu, завершено: %llu", tracker.started, tracker.finished);

	return EXIT_SUCCESS;
}

typedef struct Statistics
{
	atomic_int active;
	atomic_ullong accepted;
	atomic_ullong processed;
	atomic_ullong timedOut;
	atomic_ullong rejected;
	atomic_ullong totalNanoseconds;
} Statistics;

typedef struct StatisticsSnapshot
{
	int active;
	unsigned long long accepted;
	unsigned long long processed;
	unsigned long long timedOut;
	unsigned long long rejected;
	unsigned long long totalNanoseconds;
} StatisticsSnapshot;

static StatisticsSnapshot TakeSnapshot(Statistics* statistics)
{
	StatisticsSnapshot snapshot = {
		atomic_load(&statistics->active),
		atomic_load(&statistics->accepted),
		atomic_load(&statistics->processed),
		atomic_load(&statistics->timedOut),
		atomic_load(&statistics->rejected),
		atomic_load(&statistics->totalNanoseconds),
	};

	return snapshot;
}

static bool IsSameSnapshot(const StatisticsSnapshot* left, const StatisticsSnapshot* right)
{
	return left->active == right->active && left->accepted == right->accepted
		&& left->processed == right->processed && left->timedOut == right->timedOut
		&& left->rejected == right->rejected;
}

static double AverageMilliseconds(const StatisticsSnapshot* snapshot)
{
	if (snapshot->processed == 0)
	{
		return 0.0;
	}

	return (double)snapshot->totalNanoseconds / (double)snapshot->processed / 1e6;
}

static void PrintStatistics(const StatisticsSnapshot* snapshot)
{
	LogEvent("[статистика] активных: %d · принято: %llu · обработано: %llu · среднее время "
			 "обработки: %.3f мс"
			 " · таймаутов: %llu · отказов: %llu",
		snapshot->active,
		snapshot->accepted,
		snapshot->processed,
		AverageMilliseconds(snapshot),
		snapshot->timedOut,
		snapshot->rejected);
}

typedef struct Job
{
	int socket;
	char peer[PEER_TEXT_BYTES];
	ClientMessage message;
	uint64_t receivedAt;
} Job;

typedef struct JobQueue
{
	Job* jobs;
	size_t capacity;
	size_t head;
	size_t count;
	bool isClosed;
	pthread_mutex_t mutex;
	pthread_cond_t hasJobs;
} JobQueue;

static bool InitJobQueue(JobQueue* queue, const size_t capacity)
{
	queue->jobs = calloc(capacity, sizeof(Job));
	if (queue->jobs == NULL)
	{
		LogEvent("[Ошибка] Не хватает памяти для очереди задач");
		return false;
	}

	queue->capacity = capacity;
	queue->head = 0;
	queue->count = 0;
	queue->isClosed = false;
	pthread_mutex_init(&queue->mutex, NULL);
	pthread_cond_init(&queue->hasJobs, NULL);

	return true;
}

static void DestroyJobQueue(JobQueue* queue)
{
	pthread_cond_destroy(&queue->hasJobs);
	pthread_mutex_destroy(&queue->mutex);
	free(queue->jobs);
	queue->jobs = NULL;
}

static bool PushJob(JobQueue* queue, const Job* job)
{
	pthread_mutex_lock(&queue->mutex);

	const bool isAccepted = !queue->isClosed && queue->count < queue->capacity;
	if (isAccepted)
	{
		queue->jobs[(queue->head + queue->count) % queue->capacity] = *job;
		++queue->count;
		pthread_cond_signal(&queue->hasJobs);
	}

	pthread_mutex_unlock(&queue->mutex);
	return isAccepted;
}

static bool PopJob(JobQueue* queue, Job* job)
{
	pthread_mutex_lock(&queue->mutex);

	while (queue->count == 0 && !queue->isClosed)
	{
		pthread_cond_wait(&queue->hasJobs, &queue->mutex);
	}

	const bool hasJob = queue->count > 0;
	if (hasJob)
	{
		*job = queue->jobs[queue->head];
		queue->head = (queue->head + 1) % queue->capacity;
		--queue->count;
	}

	pthread_mutex_unlock(&queue->mutex);
	return hasJob;
}

static void CloseJobQueue(JobQueue* queue)
{
	pthread_mutex_lock(&queue->mutex);
	queue->isClosed = true;
	pthread_cond_broadcast(&queue->hasJobs);
	pthread_mutex_unlock(&queue->mutex);
}

typedef struct PoolShared
{
	JobQueue queue;
	Statistics statistics;
	int wakeDescriptor;
	atomic_bool isShutdownRequested;
	atomic_bool isStopping;
	bool isVerbose;
} PoolShared;

static void WakeMainThread(const PoolShared* shared)
{
	const uint64_t one = 1;
	const ssize_t written = write(shared->wakeDescriptor, &one, sizeof(one));
	(void)written;
}

static void FinishJob(PoolShared* shared, const Job* job, const ServeOutcome outcome)
{
	CloseClient(job->socket, job->peer, shared->isVerbose);

	if (outcome != ServeOutcomeFailed)
	{
		atomic_fetch_add(&shared->statistics.processed, 1);
		atomic_fetch_add(&shared->statistics.totalNanoseconds, NowNanoseconds() - job->receivedAt);
	}
	atomic_fetch_sub(&shared->statistics.active, 1);

	if (outcome == ServeOutcomeShutdownRequested)
	{
		atomic_store(&shared->isShutdownRequested, true);
	}
	if (outcome == ServeOutcomeShutdownRequested || atomic_load(&shared->isStopping))
	{
		WakeMainThread(shared);
	}
}

static void* RunWorker(void* argument)
{
	PoolShared* shared = argument;
	Job job;

	while (PopJob(&shared->queue, &job))
	{
		const ServeOutcome outcome
			= AnswerClient(job.socket, job.peer, &job.message, shared->isVerbose);
		FinishJob(shared, &job, outcome);
	}

	return NULL;
}

typedef enum EventSourceKind
{
	EventSourceListener,
	EventSourceSignal,
	EventSourceWake,
	EventSourceTimer,
	EventSourceConnection,
} EventSourceKind;

typedef struct EventSource
{
	EventSourceKind kind;
} EventSource;

typedef struct Connection
{
	EventSource source;
	int socket;
	char peer[PEER_TEXT_BYTES];
	char buffer[MAX_MESSAGE_BYTES];
	size_t length;
	uint64_t lastActivity;
	struct Connection* previous;
	struct Connection* next;
} Connection;

typedef struct ConnectionList
{
	Connection* head;
	Connection* tail;
	size_t count;
} ConnectionList;

static void AppendConnection(ConnectionList* list, Connection* connection)
{
	connection->previous = list->tail;
	connection->next = NULL;

	if (list->tail != NULL)
	{
		list->tail->next = connection;
	}
	else
	{
		list->head = connection;
	}

	list->tail = connection;
	++list->count;
}

static void RemoveConnection(ConnectionList* list, Connection* connection)
{
	if (connection->previous != NULL)
	{
		connection->previous->next = connection->next;
	}
	else
	{
		list->head = connection->next;
	}

	if (connection->next != NULL)
	{
		connection->next->previous = connection->previous;
	}
	else
	{
		list->tail = connection->previous;
	}

	--list->count;
}

typedef struct PooledServer
{
	PoolShared shared;
	int epoll;
	int listener;
	int timer;
	const SignalGuard* signals;
	EventSource listenerSource;
	EventSource signalSource;
	EventSource wakeSource;
	EventSource timerSource;
	ConnectionList connections;
	pthread_t* workers;
	size_t workerCount;
	bool isStopping;
	StatisticsSnapshot lastPrinted;
} PooledServer;

static bool WatchDescriptor(
	const PooledServer* server, const int descriptor, const uint32_t events, void* source)
{
	struct epoll_event event;
	memset(&event, 0, sizeof(event));
	event.events = events;
	event.data.ptr = source;

	if (epoll_ctl(server->epoll, EPOLL_CTL_ADD, descriptor, &event) != 0)
	{
		LogSystemError("Ошибка epoll_ctl");
		return false;
	}

	return true;
}

static void UnwatchDescriptor(const PooledServer* server, const int descriptor)
{
	epoll_ctl(server->epoll, EPOLL_CTL_DEL, descriptor, NULL);
}

static int CreateStatisticsTimer(void)
{
	const int timer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
	if (timer < 0)
	{
		LogSystemError("Не удалось создать таймер статистики");
		return -1;
	}

	const struct itimerspec period = {.it_interval = {1, 0}, .it_value = {1, 0}};
	timerfd_settime(timer, 0, &period, NULL);

	return timer;
}

static size_t ChooseWorkerCount(void)
{
	const long processors = sysconf(_SC_NPROCESSORS_ONLN);
	const long count = processors < MIN_WORKERS
		? MIN_WORKERS
		: (processors > MAX_WORKERS ? MAX_WORKERS : processors);

	return (size_t)count;
}

static bool StartWorkers(PooledServer* server)
{
	server->workerCount = ChooseWorkerCount();
	server->workers = calloc(server->workerCount, sizeof(pthread_t));
	if (server->workers == NULL)
	{
		LogEvent("[Ошибка] Не хватает памяти для пула потоков");
		return false;
	}

	for (size_t index = 0; index < server->workerCount; ++index)
	{
		const int result
			= pthread_create(&server->workers[index], NULL, RunWorker, &server->shared);
		if (result != 0)
		{
			LogEvent("[Ошибка] Не удалось создать рабочий поток: %s", strerror(result));
			server->workerCount = index;
			return false;
		}
	}

	LogEvent("Пул потоков запущен, рабочих потоков: %zu", server->workerCount);
	return true;
}

static void StopWorkers(PooledServer* server)
{
	CloseJobQueue(&server->shared.queue);

	for (size_t index = 0; index < server->workerCount; ++index)
	{
		pthread_join(server->workers[index], NULL);
	}

	free(server->workers);
	server->workers = NULL;

	LogEvent("Рабочие потоки остановлены: %zu", server->workerCount);
}

static void DropConnection(PooledServer* server, Connection* connection)
{
	UnwatchDescriptor(server, connection->socket);
	RemoveConnection(&server->connections, connection);
	CloseClient(connection->socket, connection->peer, server->shared.isVerbose);
	atomic_fetch_sub(&server->shared.statistics.active, 1);
	free(connection);
}

static bool MakeBlocking(const int socket)
{
	const int flags = fcntl(socket, F_GETFL);
	return flags >= 0 && fcntl(socket, F_SETFL, flags & ~O_NONBLOCK) == 0
		&& SetSocketTimeouts(socket, CLIENT_TIMEOUT_SECONDS);
}

static void HandOffConnection(
	PooledServer* server, Connection* connection, const ClientMessage* message)
{
	UnwatchDescriptor(server, connection->socket);
	RemoveConnection(&server->connections, connection);

	Job job;
	job.socket = connection->socket;
	memcpy(job.peer, connection->peer, sizeof(job.peer));
	job.message = *message;
	job.receivedAt = NowNanoseconds();

	if (!MakeBlocking(job.socket) || !PushJob(&server->shared.queue, &job))
	{
		LogEvent("Очередь задач переполнена — клиент %s отклонён", job.peer);
		atomic_fetch_add(&server->shared.statistics.rejected, 1);
		CloseClient(job.socket, job.peer, server->shared.isVerbose);
		atomic_fetch_sub(&server->shared.statistics.active, 1);
	}

	free(connection);
}

static Connection* CreateConnection(const int socket, const char* peer)
{
	Connection* connection = calloc(1, sizeof(Connection));
	if (connection == NULL)
	{
		return NULL;
	}

	connection->source.kind = EventSourceConnection;
	connection->socket = socket;
	memcpy(connection->peer, peer, sizeof(connection->peer));
	connection->lastActivity = NowNanoseconds();

	return connection;
}

static void RejectClient(
	PooledServer* server, const int socket, const char* peer, const char* reason)
{
	LogEvent("Клиент %s отклонён: %s", peer, reason);
	atomic_fetch_add(&server->shared.statistics.rejected, 1);
	close(socket);
}

static void RegisterClient(PooledServer* server, const int socket, const char* peer)
{
	if (atomic_load(&server->shared.statistics.active) >= MAX_CONNECTIONS)
	{
		RejectClient(server, socket, peer, "достигнут лимит соединений");
		return;
	}

	Connection* connection = CreateConnection(socket, peer);
	if (connection == NULL)
	{
		RejectClient(server, socket, peer, "не хватает памяти");
		return;
	}

	if (!WatchDescriptor(server, socket, EPOLLIN | EPOLLRDHUP, connection))
	{
		free(connection);
		RejectClient(server, socket, peer, "ошибка epoll");
		return;
	}

	AppendConnection(&server->connections, connection);
	atomic_fetch_add(&server->shared.statistics.active, 1);
	atomic_fetch_add(&server->shared.statistics.accepted, 1);

	if (server->shared.isVerbose)
	{
		LogEvent("Подключился клиент %s (дескриптор %d)", peer, socket);
	}
}

static void AcceptPending(PooledServer* server)
{
	while (!server->isStopping)
	{
		char peer[PEER_TEXT_BYTES];
		const int client = AcceptClient(server->listener, SOCK_NONBLOCK, peer, sizeof(peer));
		if (client < 0)
		{
			if (!IsTransientAcceptError(errno))
			{
				LogSystemError("Ошибка accept");
			}
			return;
		}

		RegisterClient(server, client, peer);
	}
}

static void TouchConnection(PooledServer* server, Connection* connection)
{
	connection->lastActivity = NowNanoseconds();
	RemoveConnection(&server->connections, connection);
	AppendConnection(&server->connections, connection);
}

static void FinishReading(PooledServer* server, Connection* connection, const bool isClosed)
{
	ClientMessage message;
	MessageParse result = ParseClientMessage(connection->buffer, connection->length, &message);
	if (result == MessageParseIncomplete && isClosed)
	{
		result = ParseAtEndOfStream(connection->buffer, connection->length, &message);
	}

	if (result == MessageParseComplete)
	{
		HandOffConnection(server, connection, &message);
		return;
	}

	if (result == MessageParseInvalid)
	{
		LogReceiveFailure(
			isClosed && connection->length == 0 ? ReceiveStatusClosed : ReceiveStatusInvalid,
			connection->peer);
		DropConnection(server, connection);
		return;
	}

	if (isClosed)
	{
		LogReceiveFailure(ReceiveStatusClosed, connection->peer);
		DropConnection(server, connection);
	}
}

static void ReadConnection(PooledServer* server, Connection* connection)
{
	bool isClosed = false;

	while (connection->length < MAX_MESSAGE_BYTES - 1)
	{
		const ssize_t received = recv(connection->socket,
			connection->buffer + connection->length,
			MAX_MESSAGE_BYTES - 1 - connection->length,
			0);

		if (received > 0)
		{
			connection->length += (size_t)received;
			if (server->shared.isVerbose)
			{
				LogEvent("Получено %zd байт от %s", received, connection->peer);
			}
			continue;
		}
		if (received < 0 && errno == EINTR)
		{
			continue;
		}
		if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
		{
			break;
		}

		isClosed = true;
		break;
	}

	TouchConnection(server, connection);
	FinishReading(server, connection, isClosed);
}

static void ExpireConnections(PooledServer* server)
{
	const uint64_t now = NowNanoseconds();
	const uint64_t timeout = (uint64_t)CLIENT_TIMEOUT_SECONDS * NANOSECONDS_PER_SECOND;

	while (
		server->connections.head != NULL && now - server->connections.head->lastActivity >= timeout)
	{
		Connection* connection = server->connections.head;
		LogReceiveFailure(ReceiveStatusTimeout, connection->peer);
		atomic_fetch_add(&server->shared.statistics.timedOut, 1);
		DropConnection(server, connection);
	}
}

static int ComputeWaitMilliseconds(const PooledServer* server)
{
	if (server->connections.head == NULL)
	{
		return -1;
	}

	const uint64_t deadline = server->connections.head->lastActivity
		+ (uint64_t)CLIENT_TIMEOUT_SECONDS * NANOSECONDS_PER_SECOND;
	const uint64_t now = NowNanoseconds();
	if (deadline <= now)
	{
		return 0;
	}

	return (int)((deadline - now + 999999) / 1000000);
}

static void BeginStopping(PooledServer* server, const char* reason)
{
	if (server->isStopping)
	{
		return;
	}

	server->isStopping = true;
	atomic_store(&server->shared.isStopping, true);

	UnwatchDescriptor(server, server->listener);
	LogEvent("%s — сервер останавливается", reason);
	CloseListener(server->listener);
	server->listener = -1;

	LogEvent("Ожидание активных соединений: %d (не дольше %d с)",
		atomic_load(&server->shared.statistics.active),
		CLIENT_TIMEOUT_SECONDS);
}

static void DropAllConnections(PooledServer* server)
{
	while (server->connections.head != NULL)
	{
		DropConnection(server, server->connections.head);
	}
}

static void HandleSignal(PooledServer* server)
{
	const int number = ReadSignal(server->signals);
	if (server->isStopping)
	{
		LogEvent("Повторный %s — незавершённые соединения закрываются немедленно",
			DescribeSignal(number));
		DropAllConnections(server);
		return;
	}

	char reason[INPUT_BYTES];
	snprintf(reason, sizeof(reason), "Получен %s", DescribeSignal(number));
	BeginStopping(server, reason);
}

static void HandleWake(PooledServer* server)
{
	uint64_t value = 0;
	const ssize_t received = read(server->shared.wakeDescriptor, &value, sizeof(value));
	(void)received;

	if (atomic_load(&server->shared.isShutdownRequested))
	{
		BeginStopping(server, "Клиент прислал число вне диапазона");
	}
}

static void HandleTimer(PooledServer* server)
{
	uint64_t expirations = 0;
	const ssize_t received = read(server->timer, &expirations, sizeof(expirations));
	(void)received;

	const StatisticsSnapshot snapshot = TakeSnapshot(&server->shared.statistics);
	if (!IsSameSnapshot(&snapshot, &server->lastPrinted))
	{
		PrintStatistics(&snapshot);
		server->lastPrinted = snapshot;
	}
}

static void DispatchEvent(PooledServer* server, const struct epoll_event* event)
{
	EventSource* source = event->data.ptr;

	switch (source->kind)
	{
	case EventSourceListener: AcceptPending(server); break;
	case EventSourceSignal: HandleSignal(server); break;
	case EventSourceWake: HandleWake(server); break;
	case EventSourceTimer: HandleTimer(server); break;
	case EventSourceConnection: ReadConnection(server, (Connection*)source); break;
	}
}

static bool IsFinished(PooledServer* server)
{
	return server->isStopping && atomic_load(&server->shared.statistics.active) == 0;
}

static void RunEventLoop(PooledServer* server)
{
	struct epoll_event events[EPOLL_BATCH];

	while (!IsFinished(server))
	{
		const int count
			= epoll_wait(server->epoll, events, EPOLL_BATCH, ComputeWaitMilliseconds(server));
		if (count < 0 && errno == EINTR)
		{
			continue;
		}
		if (count < 0)
		{
			LogSystemError("Ошибка epoll_wait");
			break;
		}

		for (int index = 0; index < count; ++index)
		{
			DispatchEvent(server, &events[index]);
		}

		ExpireConnections(server);
	}
}

static bool InitPooledServer(
	PooledServer* server, const int listener, const SignalGuard* signals, const bool isVerbose)
{
	memset(server, 0, sizeof(*server));
	server->listener = listener;
	server->signals = signals;
	server->shared.isVerbose = isVerbose;
	server->listenerSource.kind = EventSourceListener;
	server->signalSource.kind = EventSourceSignal;
	server->wakeSource.kind = EventSourceWake;
	server->timerSource.kind = EventSourceTimer;
	server->epoll = -1;
	server->timer = -1;
	server->shared.wakeDescriptor = -1;

	if (!InitJobQueue(&server->shared.queue, MAX_CONNECTIONS))
	{
		return false;
	}

	server->epoll = epoll_create1(EPOLL_CLOEXEC);
	server->shared.wakeDescriptor = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	server->timer = CreateStatisticsTimer();
	if (server->epoll < 0 || server->shared.wakeDescriptor < 0 || server->timer < 0)
	{
		LogSystemError("Не удалось создать epoll/eventfd");
		return false;
	}

	return WatchDescriptor(server, listener, EPOLLIN, &server->listenerSource)
		&& WatchDescriptor(server, signals->descriptor, EPOLLIN, &server->signalSource)
		&& WatchDescriptor(server, server->shared.wakeDescriptor, EPOLLIN, &server->wakeSource)
		&& WatchDescriptor(server, server->timer, EPOLLIN, &server->timerSource);
}

static void CloseIfOpen(const int descriptor)
{
	if (descriptor >= 0)
	{
		close(descriptor);
	}
}

static void DestroyPooledServer(PooledServer* server)
{
	DropAllConnections(server);
	if (server->listener >= 0)
	{
		CloseListener(server->listener);
	}

	CloseIfOpen(server->timer);
	CloseIfOpen(server->shared.wakeDescriptor);
	CloseIfOpen(server->epoll);
	DestroyJobQueue(&server->shared.queue);
}

static int RunPooled(const int listener, const SignalGuard* signals, const bool isVerbose)
{
	PooledServer server;
	if (!InitPooledServer(&server, listener, signals, isVerbose) || !StartWorkers(&server))
	{
		if (server.workers != NULL)
		{
			StopWorkers(&server);
		}
		DestroyPooledServer(&server);
		return FAILURE_EXIT_CODE;
	}

	LogEvent("Основной поток ждёт события через epoll: подключения, сигналы, таймауты");
	RunEventLoop(&server);

	StopWorkers(&server);
	const StatisticsSnapshot final = TakeSnapshot(&server.shared.statistics);
	DestroyPooledServer(&server);

	LogEvent("Итоговая статистика:");
	PrintStatistics(&final);
	return EXIT_SUCCESS;
}

static bool ReadLine(char* buffer, const size_t capacity)
{
	if (fgets(buffer, (int)capacity, stdin) == NULL)
	{
		return false;
	}

	buffer[strcspn(buffer, "\r\n")] = '\0';
	return true;
}

static ServerMode AskMode(void)
{
	printf("Режим сервера:\n");
	printf("  1 — последовательный: клиенты по очереди (базовый уровень)\n");
	printf("  2 — конкурентный: процесс на клиента через fork() (+1)\n");
	printf("  3 — асинхронный: epoll + пул потоков, Ctrl+C, таймауты, статистика (+2)\n");

	char input[INPUT_BYTES];
	while (true)
	{
		printf("Выбор [3]: ");
		fflush(stdout);

		int choice = 0;
		if (!ReadLine(input, sizeof(input)) || input[0] == '\0')
		{
			return ServerModePooled;
		}
		if (ParseInteger(input, &choice) && choice >= ServerModeSequential
			&& choice <= ServerModePooled)
		{
			return (ServerMode)choice;
		}

		printf("Введите 1, 2 или 3\n");
	}
}

static uint16_t AskPort(void)
{
	char input[INPUT_BYTES];
	while (true)
	{
		printf("Порт [%d]: ", DEFAULT_PORT);
		fflush(stdout);

		int port = 0;
		if (!ReadLine(input, sizeof(input)) || input[0] == '\0')
		{
			return DEFAULT_PORT;
		}
		if (ParseInteger(input, &port) && port >= MIN_PORT && port <= MAX_PORT)
		{
			return (uint16_t)port;
		}

		printf("Нужен порт от %d до %d\n", MIN_PORT, MAX_PORT);
	}
}

static bool AskVerbose(void)
{
	char input[INPUT_BYTES];
	printf("Подробно печатать каждого клиента? [Д/н]: ");
	fflush(stdout);

	if (!ReadLine(input, sizeof(input)) || input[0] == '\0')
	{
		return true;
	}

	return input[0] != 'n' && input[0] != 'N' && strncmp(input, "н", strlen("н")) != 0
		&& strncmp(input, "Н", strlen("Н")) != 0;
}

static ServerOptions AskOptions(void)
{
	ServerOptions options;
	options.mode = AskMode();
	options.port = AskPort();
	options.isVerbose = AskVerbose();
	printf("\n");

	return options;
}

static int RunMode(const ServerOptions* options, const int listener, const SignalGuard* guard)
{
	switch (options->mode)
	{
	case ServerModeSequential: return RunSequential(listener, guard, options->isVerbose);
	case ServerModeForking: return RunForking(listener, guard, options->isVerbose);
	case ServerModePooled: return RunPooled(listener, guard, options->isVerbose);
	}

	return FAILURE_EXIT_CODE;
}

int main(void)
{
	printf("%s\n\n", SERVER_NAME);
	const ServerOptions options = AskOptions();

	SignalGuard guard;
	if (!OpenSignalGuard(&guard, options.mode == ServerModeForking))
	{
		return FAILURE_EXIT_CODE;
	}

	const int listener = CreateListener(options.port);
	if (listener < 0)
	{
		CloseSignalGuard(&guard);
		return FAILURE_EXIT_CODE;
	}

	LogEvent("Ожидание клиентов. Ctrl+C — корректная остановка");
	const int result = RunMode(&options, listener, &guard);

	CloseSignalGuard(&guard);
	LogEvent("Сервер завершил работу, все сокеты закрыты");

	return result;
}
