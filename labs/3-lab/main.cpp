#include "utils/Encoding.hpp"
#include <cstdlib>
#include <iostream>

int main()
{
	Encoding encoding;

	try
	{
	}
	catch (const std::exception& exception)
	{
		std::cout << "[error]\t" << exception.what() << std::endl;
		return EXIT_FAILURE;
	}

	return EXIT_SUCCESS;
}