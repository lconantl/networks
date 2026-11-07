#include "Options.hpp"
#include "Receiver.hpp"

#include <exception>
#include <iostream>

int main(int argc, const char* const* argv)
{
	try
	{
		Rdtp::Receiver receiver(Rdtp::ParseReceiverOptions(argc, argv));
		receiver.Run();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "Ошибка: " << error.what() << std::endl;
		return 1;
	}
}
