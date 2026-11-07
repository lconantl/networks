#include "Options.hpp"
#include "Sender.hpp"

#include <exception>
#include <iostream>

int main(int argc, const char* const* argv)
{
	try
	{
		Rdtp::Sender sender(Rdtp::ParseSenderOptions(argc, argv));
		sender.Run();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Ошибка: " << error.what() << std::endl;
		return 1;
	}
}
