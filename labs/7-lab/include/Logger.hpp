#pragma once

#include <string>

namespace Rdtp
{
class Logger final
{
public:
	explicit Logger(bool debug) noexcept;

	void Info(const std::string& message) const;
	void Debug(const std::string& message) const;

private:
	bool m_debug = false;
};
}
