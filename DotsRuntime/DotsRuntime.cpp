#include "DotsRuntime.h"

using namespace core::database;

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

context& context::get()
{
	static context instance;
	return instance;
}

void context::free(alloc_type type, void* data)
{
	switch (type)
	{
	case alloc_type::fastbin:
		if (!fastbin.try_enqueue(data))
			::free(data);
		break;
	case alloc_type::smallbin:
		if (!smallbin.try_enqueue(data))
			::free(data);
		break;
	case alloc_type::largebin:
		if (!largebin.try_enqueue(data))
			::free(data);
		break;
	}
}

void* context::malloc(alloc_type type)
{
	void* result = nullptr;
	switch (type)
	{
	case alloc_type::fastbin:
		if (!fastbin.try_dequeue(result))
			result = ::malloc(kFastBinSize);
		break;
	case alloc_type::smallbin:
		if (!smallbin.try_dequeue(result))
			result = ::malloc(kSmallBinSize);
		break;
	case alloc_type::largebin:
		if (!largebin.try_dequeue(result))
			result = ::malloc(kLargeBinSize);
		break;
	}
	return result;
}

type_index context::register_type(component_desc desc)
{
	{
		auto i = hash2type.find(desc.GUID);
		if (i != hash2type.end())
			return static_cast<index_t>(i->second);
	}
	uint32_t rid = 0;
	if (desc.entityRefs != nullptr)
	{
		rid = (uint32_t)entityRefs.size();
		for (int i = 0; i < desc.entityRefCount; ++i)
			entityRefs.push_back(desc.entityRefs[i]);
	}
	component_type type;
	if (desc.size == 0)
		type = ct_tag;
	else if (desc.isElement)
		type = ct_buffer;
	else if (desc.isManaged)
		type = ct_managed;
	else
		type = ct_pod;
	if (type == ct_managed)
	{
		desc.manualClean = false;
		desc.manualCopy = false;
	}
	index_t id = (index_t)infos.size();
	id = type_index{ id, type };
	type_registry i{ desc.GUID, desc.size, desc.elementSize, desc.alignment, rid, desc.entityRefCount, desc.name, desc.vtable };
	infos.push_back(i);
	uint8_t s = 0;
	if (desc.manualClean)
		s = s | ManualCleaning;
	if (desc.manualCopy)
		s = s | ManualCopying;
	tracks.push_back((track_state)s);
	hash2type.insert({ desc.GUID, id });

	if (desc.manualCopy)
	{
		index_t id2 = (index_t)infos.size();
		id2 = type_index{ id2, type };
		type_registry i2{ desc.GUID, desc.size, desc.elementSize, desc.alignment, rid, desc.entityRefCount, desc.name, desc.vtable };
		tracks.push_back(Copying);
		infos.push_back(i2);
	}

	return id;
}
