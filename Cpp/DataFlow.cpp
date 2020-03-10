#include "DataFlow.hpp"
using namespace core::codebase;
data_handle::data_handle(const data_handle& other)
{
	owner->on_data_captured(*this);
}