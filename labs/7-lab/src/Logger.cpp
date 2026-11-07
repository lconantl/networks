#include "Logger.hpp"

#include <iostream>

namespace Rdtp
{
Logger::Logger(bool debug) noexcept
	: m_debug(debug)
{
}

void Logger::Info(const std::string& message) const
{
	std::cout << message << std::endl;
}

void Logger::Debug(const std::string& message) const
{
	if (m_debug)
	{
		std::cout << "[DEBUG] " << message << std::endl;
	}
}
}
