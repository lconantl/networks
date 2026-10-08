#pragma once

#include <string>

class ConsoleEncoding final
{
public:
	ConsoleEncoding();
	~ConsoleEncoding() noexcept;

	ConsoleEncoding(const ConsoleEncoding&) = delete;
	ConsoleEncoding& operator=(const ConsoleEncoding&) = delete;
	ConsoleEncoding(ConsoleEncoding&&) = delete;
	ConsoleEncoding& operator=(ConsoleEncoding&&) = delete;

private:
#ifdef _WIN32
	unsigned int m_previousOutputCp;
	unsigned int m_previousInputCp;
	int m_previousStdoutMode;
	int m_previousStderrMode;
#else
	std::string m_previousLocale;
#endif
};
