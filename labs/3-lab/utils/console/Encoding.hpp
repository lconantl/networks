#pragma once

#include <clocale>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

class Encoding final
{
public:
	Encoding()
	{
#ifdef _WIN32
		m_previousOutputCp = GetConsoleOutputCP();
		m_previousInputCp = GetConsoleCP();

		if (m_previousOutputCp == 0 || m_previousInputCp == 0)
		{
			return;
		}

		AssertIsEncodingSet(SetConsoleOutputCP(CP_UTF8) != 0);

		if (SetConsoleCP(CP_UTF8) == 0)
		{
			SetConsoleOutputCP(m_previousOutputCp);
			AssertIsEncodingSet(false);
		}

		m_isConfigured = true;
#else
		const char* previousLocale = std::setlocale(LC_CTYPE, nullptr);
		AssertIsEncodingSet(previousLocale != nullptr);

		m_previousLocale = previousLocale;

		AssertIsEncodingSet(std::setlocale(LC_CTYPE, "") != nullptr);
#endif
	}

	~Encoding() noexcept
	{
#ifdef _WIN32
		if (m_isConfigured)
		{
			SetConsoleCP(m_previousInputCp);
			SetConsoleOutputCP(m_previousOutputCp);
		}
#else
		if (!m_previousLocale.empty())
		{
			std::setlocale(LC_CTYPE, m_previousLocale.c_str());
		}
#endif
	}

	Encoding(const Encoding&) = delete;
	Encoding& operator=(const Encoding&) = delete;
	Encoding(Encoding&&) = delete;
	Encoding& operator=(Encoding&&) = delete;

private:
	static void AssertIsEncodingSet(const bool success)
	{
		if (!success)
		{
			throw std::runtime_error("Couldn't configure console encoding");
		}
	}

#ifdef _WIN32
	UINT m_previousOutputCp{};
	UINT m_previousInputCp{};
	bool m_isConfigured{};
#else
	std::string m_previousLocale;
#endif
};