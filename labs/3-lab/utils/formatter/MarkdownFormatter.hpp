#pragma once

#include <string>

class MarkdownFormatter
{
public:
	MarkdownFormatter() = default;
	~MarkdownFormatter() = default;

	void AddHeader(int level, const std::string& text);
	void AddCodeBlock(const std::string& language, const std::string& code);
	void AddText(const std::string& text);

	[[nodiscard]] std::string Build() const;

private:
	std::string m_buffer;
};