#pragma once

/**
 * cli::FlagDispatcher - header-only разбор флагов командной строки без аргументов.
 * Требуется C++20. Реализует паттерн Команда (GoF).
 *
 * БЫСТРЫЙ СТАРТ
 *
 *   struct Options
 *   {
 *       bool isHelpRequested = false;
 *       bool useClipboard = false;
 *   };
 *
 *   cli::FlagDispatcher<Options> dispatcher;
 *   dispatcher.BindSwitch({ 'h', "help", "Показать справку" }, &Options::isHelpRequested);
 *   dispatcher.BindSwitch({ 'c', "clipboard", "Скопировать в буфер" }, &Options::useClipboard);
 *   dispatcher.AddConflict('c', 'h');
 *
 *   Options options;
 *   std::vector<std::string> paths = dispatcher.Dispatch(cli::ReadArguments(argc, argv), options);
 *
 * ПОДДЕРЖИВАЕМЫЙ СИНТАКСИС
 *
 *   -c              короткий флаг
 *   -ch             склейка коротких флагов, эквивалентна -c -h
 *   --clipboard     длинный флаг
 *   --              конец флагов, всё дальше считается позиционным аргументом
 *   -               позиционный аргумент (по соглашению stdin/stdout)
 *
 * НЕ ПОДДЕРЖИВАЕТСЯ
 *
 *   -o file, --output=file   флаги со значением, --long=value отклоняется с ошибкой
 *   --clip                   сокращения длинных имён в стиле GNU
 *   -vvv                     счётчики, повтор флага срабатывает ровно один раз
 *
 * РОЛИ ПАТТЕРНА КОМАНДА
 *
 *   Command          ICommand<Receiver>
 *   ConcreteCommand  SetSwitchCommand (выставляет поле), ActionCommand (вызывает лямбду)
 *   Receiver         пользовательская структура опций
 *   Invoker          FlagDispatcher
 *   Client           код, вызывающий BindSwitch и BindAction
 *
 *   Получатель передаётся в Execute(Receiver&), а не хранится в команде. Это исключает
 *   висячие ссылки и делает диспетчер безсостояточным: его можно создавать и
 *   переиспользовать в любой момент.
 *
 * ПРАВИЛА ПОВЕДЕНИЯ
 *
 *   1. Команды выполняются в порядке регистрации, а не в порядке argv, поэтому -ch и -hc
 *      эквивалентны. Правило "последний победил" (как -l и -1 у ls) не поддерживается:
 *      для него нужен порядок из argv.
 *   2. Сначала разбирается вся строка, затем проверяются конфликты и только потом
 *      выполняются команды. При любой ошибке получатель остаётся нетронутым.
 *   3. ReadArguments пропускает argv[0], обрезает пробелы по краям аргументов и
 *      отбрасывает пустые строки. Путь с пробелами по краям будет подрезан.
 *
 * ИСКЛЮЧЕНИЯ
 *
 *   cli::UsageError       ошибка пользователя: неизвестный флаг, значение у флага,
 *                         конфликт флагов. Принято печатать справку и возвращать EXIT_CODE.
 *   std::logic_error      ошибка программиста: дубликат флага, конфликт для
 *   std::invalid_argument незарегистрированного флага, некорректное имя, пустая команда.
 */

#include <algorithm>
#include <cctype>
#include <functional>
#include <iomanip>
#include <memory>
#include <ostream>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cli
{
class UsageError : public std::runtime_error
{
public:
	static constexpr int EXIT_CODE = 2;

	explicit UsageError(const std::string& message)
		: std::runtime_error(message)
	{
	}
};

struct FlagSpec
{
	char shortName;
	std::string longName;
	std::string description;
};

template <typename Receiver>
class ICommand
{
public:
	virtual ~ICommand() = default;

	virtual void Execute(Receiver& receiver) const = 0;
};

namespace detail
{
inline constexpr std::size_t NotFound = static_cast<std::size_t>(-1);

template <typename Receiver>
struct Binding
{
	FlagSpec spec;
	std::unique_ptr<ICommand<Receiver>> command;
};

template <typename Receiver>
using Bindings = std::vector<Binding<Receiver>>;

struct Conflict
{
	std::size_t first;
	std::size_t second;
};

using Conflicts = std::vector<Conflict>;

struct ParseResult
{
	std::vector<bool> triggered;
	std::vector<std::string> positionals;
};

inline bool HasValueSeparator(const std::string_view text) noexcept
{
	return text.find('=') != std::string_view::npos;
}

inline std::string_view StripValue(const std::string_view text) noexcept
{
	return text.substr(0, text.find('='));
}

inline std::string FormatFlag(const FlagSpec& spec)
{
	return std::string("-") + spec.shortName + ", --" + spec.longName;
}

inline bool SharesName(const FlagSpec& left, const FlagSpec& right) noexcept
{
	return left.shortName == right.shortName || left.longName == right.longName;
}

inline void AssertIsArgumentCountPositive(const int argc)
{
	if (argc <= 0)
	{
		throw std::invalid_argument("Недопустимое количество аргументов командной строки");
	}
}

inline void AssertIsArgumentArrayPresent(const char* const* argv)
{
	if (argv == nullptr)
	{
		throw std::invalid_argument("Массив аргументов командной строки отсутствует");
	}
}

template <typename Pointer>
void AssertIsNotNull(const Pointer& pointer)
{
	if (pointer == nullptr)
	{
		throw std::invalid_argument("Обработчик флага не задан");
	}
}

inline void AssertIsValidShortName(const char name)
{
	if (std::isalnum(static_cast<unsigned char>(name)) == 0)
	{
		throw std::invalid_argument("Короткое имя флага должно быть латинской буквой или цифрой");
	}
}

inline void AssertIsValidLongName(const std::string_view name)
{
	if (name.empty() || name.starts_with('-') || HasValueSeparator(name))
	{
		throw std::invalid_argument("Некорректное длинное имя флага: " + std::string(name));
	}
}

template <typename Receiver>
void AssertIsUnique(const Bindings<Receiver>& bindings, const FlagSpec& spec)
{
	const bool isTaken = std::ranges::any_of(bindings, [&spec](const auto& binding) {
		return SharesName(binding.spec, spec);
	});

	if (isTaken)
	{
		throw std::logic_error("Флаг уже зарегистрирован: " + FormatFlag(spec));
	}
}

inline void AssertIsRegistered(const std::size_t index, const char name)
{
	if (index == NotFound)
	{
		throw std::logic_error("Флаг не зарегистрирован: -" + std::string(1, name));
	}
}

inline void AssertAreDifferent(const std::size_t first, const std::size_t second)
{
	if (first == second)
	{
		throw std::logic_error("Флаг не может конфликтовать сам с собой");
	}
}

inline void AssertIsKnownFlag(const std::size_t index, const std::string& flag)
{
	if (index == NotFound)
	{
		throw UsageError("Неизвестный флаг: " + flag);
	}
}

inline void AssertHasNoValue(const std::string_view name)
{
	if (HasValueSeparator(name))
	{
		throw UsageError("Флаг --" + std::string(StripValue(name)) + " не принимает значение");
	}
}

inline bool IsWhitespace(const char symbol) noexcept
{
	return std::isspace(static_cast<unsigned char>(symbol)) != 0;
}

inline std::string Trim(const std::string_view text)
{
	const auto begin = std::ranges::find_if_not(text, IsWhitespace);
	const auto end = std::find_if_not(text.rbegin(), text.rend(), IsWhitespace).base();

	return begin < end ? std::string(begin, end) : std::string();
}

inline bool IsEndOfFlags(const std::string_view argument) noexcept
{
	return argument == "--";
}

inline bool IsLongFlag(const std::string_view argument) noexcept
{
	return argument.size() > 2 && argument.starts_with("--");
}

inline bool IsShortFlagGroup(const std::string_view argument) noexcept
{
	return argument.size() > 1 && argument.starts_with('-') && !argument.starts_with("--");
}

template <typename Receiver>
std::size_t FindIndexByLongName(const Bindings<Receiver>& bindings, const std::string_view name)
{
	const auto it = std::ranges::find_if(bindings, [name](const auto& binding) {
		return binding.spec.longName == name;
	});

	return it == bindings.end() ? NotFound : static_cast<std::size_t>(it - bindings.begin());
}

template <typename Receiver>
std::size_t FindIndexByShortName(const Bindings<Receiver>& bindings, const char name)
{
	const auto it = std::ranges::find_if(bindings, [name](const auto& binding) {
		return binding.spec.shortName == name;
	});

	return it == bindings.end() ? NotFound : static_cast<std::size_t>(it - bindings.begin());
}

template <typename Receiver>
std::size_t ResolveRegisteredFlag(const Bindings<Receiver>& bindings, const char name)
{
	const std::size_t index = FindIndexByShortName(bindings, name);
	AssertIsRegistered(index, name);

	return index;
}

template <typename Receiver>
void MarkLongFlag(
	const Bindings<Receiver>& bindings, const std::string_view name, std::vector<bool>& triggered)
{
	AssertHasNoValue(name);

	const std::size_t index = FindIndexByLongName(bindings, name);
	AssertIsKnownFlag(index, "--" + std::string(name));

	triggered[index] = true;
}

template <typename Receiver>
void MarkShortFlagGroup(
	const Bindings<Receiver>& bindings, const std::string_view group, std::vector<bool>& triggered)
{
	for (const char name : group)
	{
		const std::size_t index = FindIndexByShortName(bindings, name);
		AssertIsKnownFlag(index, "-" + std::string(1, name));

		triggered[index] = true;
	}
}

template <typename Receiver>
void ParseArgument(
	const Bindings<Receiver>& bindings, const std::string& argument, ParseResult& result)
{
	const std::string_view view = argument;

	if (IsLongFlag(view))
	{
		MarkLongFlag(bindings, view.substr(2), result.triggered);
	}
	else if (IsShortFlagGroup(view))
	{
		MarkShortFlagGroup(bindings, view.substr(1), result.triggered);
	}
	else
	{
		result.positionals.push_back(argument);
	}
}

template <typename Receiver>
ParseResult Parse(const Bindings<Receiver>& bindings, const std::vector<std::string>& arguments)
{
	ParseResult result{std::vector<bool>(bindings.size(), false), {}};

	for (auto it = arguments.begin(); it != arguments.end(); ++it)
	{
		if (IsEndOfFlags(*it))
		{
			result.positionals.insert(result.positionals.end(), std::next(it), arguments.end());
			break;
		}
		ParseArgument(bindings, *it, result);
	}

	return result;
}

template <typename Receiver>
void AssertHasNoConflict(
	const Bindings<Receiver>& bindings, const Conflict& conflict, const std::vector<bool>& triggered)
{
	if (triggered[conflict.first] && triggered[conflict.second])
	{
		throw UsageError("Флаги --" + bindings[conflict.first].spec.longName + " и --"
			+ bindings[conflict.second].spec.longName + " нельзя использовать одновременно");
	}
}

template <typename Receiver>
void AssertHasNoConflicts(
	const Bindings<Receiver>& bindings, const Conflicts& conflicts, const std::vector<bool>& triggered)
{
	for (const Conflict& conflict : conflicts)
	{
		AssertHasNoConflict(bindings, conflict, triggered);
	}
}

template <typename Receiver>
void ExecuteTriggered(
	const Bindings<Receiver>& bindings, const std::vector<bool>& triggered, Receiver& receiver)
{
	for (std::size_t index = 0; index < bindings.size(); ++index)
	{
		if (triggered[index])
		{
			bindings[index].command->Execute(receiver);
		}
	}
}

template <typename Receiver>
std::size_t CalculateNameWidth(const Bindings<Receiver>& bindings)
{
	std::size_t width = 0;
	for (const auto& binding : bindings)
	{
		width = std::max(width, binding.spec.longName.size());
	}

	return width;
}

inline void WriteHelpLine(std::ostream& stream, const FlagSpec& spec, const std::size_t nameWidth)
{
	stream << "  -" << spec.shortName << ", --" << std::left
		   << std::setw(static_cast<int>(nameWidth)) << spec.longName << "  " << spec.description
		   << std::endl;
}

template <typename Receiver>
std::string BuildHelp(const Bindings<Receiver>& bindings)
{
	const std::size_t nameWidth = CalculateNameWidth(bindings);
	std::ostringstream stream;

	for (const auto& binding : bindings)
	{
		WriteHelpLine(stream, binding.spec, nameWidth);
	}

	return stream.str();
}
} // namespace detail

template <typename Receiver>
class SetSwitchCommand final : public ICommand<Receiver>
{
public:
	explicit SetSwitchCommand(bool Receiver::* field)
		: m_field(field)
	{
		detail::AssertIsNotNull(m_field);
	}

	void Execute(Receiver& receiver) const override
	{
		receiver.*m_field = true;
	}

private:
	bool Receiver::* m_field;
};

template <typename Receiver>
class ActionCommand final : public ICommand<Receiver>
{
public:
	using Action = std::function<void(Receiver&)>;

	explicit ActionCommand(Action action)
		: m_action(std::move(action))
	{
		detail::AssertIsNotNull(m_action);
	}

	void Execute(Receiver& receiver) const override
	{
		m_action(receiver);
	}

private:
	Action m_action;
};

template <typename Receiver>
class FlagDispatcher
{
public:
	using Action = ActionCommand<Receiver>::Action;

	void Bind(FlagSpec spec, std::unique_ptr<ICommand<Receiver>> command)
	{
		detail::AssertIsNotNull(command);
		detail::AssertIsValidShortName(spec.shortName);
		detail::AssertIsValidLongName(spec.longName);
		detail::AssertIsUnique(m_bindings, spec);

		m_bindings.push_back({std::move(spec), std::move(command)});
	}

	void BindSwitch(FlagSpec spec, bool Receiver::* field)
	{
		Bind(std::move(spec), std::make_unique<SetSwitchCommand<Receiver>>(field));
	}

	void BindAction(FlagSpec spec, Action action)
	{
		Bind(std::move(spec), std::make_unique<ActionCommand<Receiver>>(std::move(action)));
	}

	void AddConflict(const char first, const char second)
	{
		const std::size_t firstIndex = detail::ResolveRegisteredFlag(m_bindings, first);
		const std::size_t secondIndex = detail::ResolveRegisteredFlag(m_bindings, second);
		detail::AssertAreDifferent(firstIndex, secondIndex);

		m_conflicts.push_back({firstIndex, secondIndex});
	}

	std::vector<std::string> Dispatch(
		const std::vector<std::string>& arguments, Receiver& receiver) const
	{
		detail::ParseResult result = detail::Parse(m_bindings, arguments);
		detail::AssertHasNoConflicts(m_bindings, m_conflicts, result.triggered);
		detail::ExecuteTriggered(m_bindings, result.triggered, receiver);

		return std::move(result.positionals);
	}

	[[nodiscard]] std::string BuildHelp() const
	{
		return detail::BuildHelp(m_bindings);
	}

private:
	detail::Bindings<Receiver> m_bindings;
	detail::Conflicts m_conflicts;
};

inline std::vector<std::string> ReadArguments(const int argc, const char* const* argv)
{
	detail::AssertIsArgumentCountPositive(argc);
	detail::AssertIsArgumentArrayPresent(argv);

	std::vector<std::string> arguments;
	arguments.reserve(static_cast<std::size_t>(argc));

	for (const char* rawArgument : std::span(argv, static_cast<std::size_t>(argc)).subspan(1))
	{
		std::string argument = detail::Trim(rawArgument);
		if (!argument.empty())
		{
			arguments.push_back(std::move(argument));
		}
	}

	return arguments;
}
} // namespace cli