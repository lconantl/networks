#include "MarkdownFormatter.hpp"
#include <stdexcept>

namespace
{
void AssertHeaderLevelValid(int level)
{
	if (level < 1 || level > 6)
	{
		throw std::invalid_argument("Уровень заголовка Markdown должен быть от 1 до 6");
	}
}
} // namespace

void MarkdownFormatter::AddHeader(int level, const std::string& text)
{
	AssertHeaderLevelValid(level);

	m_buffer += std::string(static_cast<size_t>(level), '#');
	m_buffer += " ";
	m_buffer += text;
	m_buffer += "\n\n";
}

void MarkdownFormatter::AddCodeBlock(const std::string& language, const std::string& code)
{
	m_buffer += "```" + language + "\n";
	m_buffer += code;

	if (!code.empty() && code.back() != '\n')
	{
		m_buffer += "\n";
	}

	m_buffer += "```\n\n";
}

void MarkdownFormatter::AddText(const std::string& text)
{
	m_buffer += text;
	m_buffer += "\n\n";
}

std::string MarkdownFormatter::Build() const
{
	return m_buffer;
}