#include "DotsRuntime.h"

using namespace core::database;
namespace core::database
{
	context* DotsContext;
}
void context::initialize()
{
	static context instance;
	DotsContext = &instance;
}

context::context()
{
	using namespace guid_parse::literals;
	{
		component_desc desc{};
		desc.size = 0;
		desc.GUID = "00000000-0000-0000-0000-000000000001"_guid;
		desc.name = "cleanup";
		cleanup_id = register_type(desc);
	}
	{
		component_desc desc{}; 
		desc.size = 0;
		desc.GUID = "00000000-0000-0000-0000-000000000002"_guid;
		desc.name = "disabled";
		disable_id = register_type(desc);
	}
	{
		component_desc desc{};
		desc.size = sizeof(entity) * 5;
		desc.GUID = "00000000-0000-0000-0000-000000000003"_guid;
		desc.isElement = true;
		desc.elementSize = sizeof(entity);
		static intptr_t ers[] = { (intptr_t)offsetof(group, e) };
		desc.entityRefs = ers;
		desc.entityRefCount = 1;
		desc.name = "group";
		group_id = register_type(desc);
	}
	{
		component_desc desc{};
		desc.size = sizeof(mask);
		desc.GUID = "00000000-0000-0000-0000-000000000004"_guid;
		desc.isElement = false;
		desc.name = "mask";
		desc.entityRefCount = 0;
		mask_id = register_type(desc);
	}
#ifdef ENABLE_GUID_COMPONENT
	{
		component_desc desc{};
		desc.size = sizeof(GUID);
		desc.GUID = "00000000-0000-0000-0000-000000000005"_guid;
		desc.name = "guid";
#ifndef GUID_COMPONENT_NO_CLEAN
		desc.manualClean = true;
#endif
		guid_id = register_type(desc);
	}
#endif

	stack.init(10000);
}