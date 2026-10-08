#pragma once

#include <filesystem>
#include <string>

[[nodiscard]] std::string DetectCodeLanguage(const std::filesystem::path& path);