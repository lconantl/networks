#include "utils/console/ConsoleEncoding.hpp"
#include <iostream>

int main()
{
	try
	{
		ConsoleEncoding encoding;
		std::cout << "SMTP-mailer" << std::endl;
	}
	catch (std::exception& exception)
	{
		std::cerr << "[error]\t" << exception.what() << std::endl;

		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}