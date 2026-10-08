#include "CodeLanguage.hpp"
#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace
{
using LanguageTable = std::unordered_map<std::string, std::string>;

const LanguageTable& GetFileNameLanguages()
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

const LanguageTable& GetExtensionLanguages()
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

std::string ToLower(std::string text)
{
	std::ranges::transform(text, text.begin(), [](const unsigned char symbol) {
		return static_cast<char>(std::tolower(symbol));
	});

	return text;
}

std::string FindLanguage(const LanguageTable& table, const std::string& key)
{
	const auto it = table.find(key);

	return it == table.end() ? std::string() : it->second;
}
} // namespace

std::string DetectCodeLanguage(const std::filesystem::path& path)
{
	const std::string byFileName
		= FindLanguage(GetFileNameLanguages(), ToLower(path.filename().string()));

	if (!byFileName.empty())
	{
		return byFileName;
	}

	return FindLanguage(GetExtensionLanguages(), ToLower(path.extension().string()));
}