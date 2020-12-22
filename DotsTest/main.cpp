#include "pch.h"

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	core::database::initialize();
	core::codebase::initialize();
	install_test_components();
	install_test2_components();
	return RUN_ALL_TESTS();
}