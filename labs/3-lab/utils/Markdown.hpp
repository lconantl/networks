#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <unordered_map>

class Markdown final
{
public:
	Markdown() = default;
	~Markdown() = default;

	void AddHeader(int level, const std::string& text)
	{
		if (level < 1 || level > 6)
		{
			throw std::invalid_argument("Уровень заголовка Markdown должен быть от 1 до 6");
		}

		m_buffer += std::string(static_cast<std::size_t>(level), '#');
		m_buffer += " ";
		m_buffer += text;
		m_buffer += "\n\n";
	}

	void AddCodeBlock(const std::string& language, const std::string& code)
	{
		m_buffer += "```" + language + "\n";
		m_buffer += code;

		if (!code.empty() && code.back() != '\n')
		{
			m_buffer += "\n";
		}

		m_buffer += "```\n\n";
	}

	void AddFileCodeBlock(const std::filesystem::path& path,
		const std::string& code)
	{
		AddCodeBlock(DetectLanguage(path), code);
	}

	void AddText(const std::string& text)
	{
		m_buffer += text;
		m_buffer += "\n\n";
	}

	[[nodiscard]] std::string Build() const
	{
		return m_buffer;
	}

	[[nodiscard]] static std::string DetectLanguage(const std::filesystem::path& path)
	{
		const std::string byFileName = FindLanguage(
			GetFileNameLanguages(),
			ToLower(path.filename().string()));

		if (!byFileName.empty())
		{
			return byFileName;
		}

		return FindLanguage(
			GetExtensionLanguages(),
			ToLower(path.extension().string()));
	}

private:
	using LanguageTable = std::unordered_map<std::string, std::string>;

	static const LanguageTable& GetFileNameLanguages()
	{
		static const LanguageTable table = {
			{"cmakelists.txt", "cmake"},
			{"makefile", "makefile"},
			{"dockerfile", "dockerfile"},
			{".gitignore", "gitignore"},
			{".gitattributes", "gitattributes"},
			{".clang-format", "yaml"},
			{".editorconfig", "ini"},
		};

		return table;
	}

	static const LanguageTable& GetExtensionLanguages()
	{
		static const LanguageTable table = {
			{".c", "c"},
			{".h", "c"},
			{".cpp", "cpp"},
			{".cc", "cpp"},
			{".cxx", "cpp"},
			{".hpp", "cpp"},
			{".hxx", "cpp"},
			{".ipp", "cpp"},
			{".cs", "csharp"},
			{".java", "java"},
			{".kt", "kotlin"},
			{".swift", "swift"},
			{".go", "go"},
			{".rs", "rust"},
			{".py", "python"},
			{".rb", "ruby"},
			{".php", "php"},
			{".lua", "lua"},
			{".js", "javascript"},
			{".mjs", "javascript"},
			{".jsx", "jsx"},
			{".ts", "typescript"},
			{".tsx", "tsx"},
			{".html", "html"},
			{".css", "css"},
			{".scss", "scss"},
			{".json", "json"},
			{".xml", "xml"},
			{".yml", "yaml"},
			{".yaml", "yaml"},
			{".toml", "toml"},
			{".ini", "ini"},
			{".cfg", "ini"},
			{".sql", "sql"},
			{".sh", "bash"},
			{".bash", "bash"},
			{".ps1", "powershell"},
			{".bat", "batch"},
			{".cmd", "batch"},
			{".cmake", "cmake"},
			{".md", "markdown"},
			{".txt", "text"},
			{".log", "text"},
		};

		return table;
	}

	static std::string ToLower(std::string text)
	{
		std::ranges::transform(
			text,
			text.begin(),
			[](unsigned char symbol) {
				return static_cast<char>(std::tolower(symbol));
			});

		return text;
	}

	static std::string FindLanguage(const LanguageTable& table, const std::string& key)
	{
		const auto it = table.find(key);

		return it == table.end() ? std::string() : it->second;
	}

	std::string m_buffer;
};
