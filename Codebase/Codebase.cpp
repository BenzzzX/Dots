#include "Codebase.h"
#include <algorithm>
using namespace core::codebase;
#define forloop(i, z, n) for(auto i = std::decay_t<decltype(n)>(z); i<(n); ++i)

template<class T>
T* allocate_inplace(char*& buffer, size_t size)
{
	char* allocated = buffer;
	buffer += size * sizeof(T);
	return (T*)allocated;
}

void pipeline::setup_kernel_dependency(kernel& k)
{
}

pipeline::pipeline(world& ctx)
	:ctx(ctx)
{
	kernelStack.init(10000);
	archetypes = ctx.get_archetypes();
}

pipeline::~pipeline()
{
	for (auto k : kernels)
	{
		k->chunks.reset();
		k->dependencies.reset();
	}
}

chunk_vector<task> pipeline::create_tasks(kernel& k, int maxSlice)
{
	uint32_t count = k.chunks.size;
	int archIndex = 0;
	int indexInKernel = 0;
	chunk_vector<task> result;
	forloop(i, 0, count)
	{
		chunk* c = k.chunks[i];
		for (; k.archetypes[archIndex] != c->get_type(); ++archIndex);
		uint32_t allocated = 0;
		while (allocated != c->get_count())
		{
			uint32_t sliceCount;
			if (maxSlice == -1)
				sliceCount = c->get_count();
			else
				sliceCount = std::min(c->get_count() - allocated, (uint32_t)maxSlice);
			task newTask{ };
			newTask.matched = archIndex;
			newTask.slice = chunk_slice{ c, allocated, sliceCount };
			newTask.indexInKernel = indexInKernel;
			allocated += sliceCount;
			indexInKernel += sliceCount;
			result.push(newTask);
		}
	}
	return result;
}

constexpr uint16_t InvalidIndex = (uint16_t)-1;

const core::entity* operation_base::get_entities() { return ctx.ctx.get_entities(slice.c) + slice.start; }

mask operation_base::get_mask() { return ctx.matched[matched]; }

void* operation_base::get(int paramId)
{

	auto localType = ctx.localType[matched * ctx.paramCount + paramId];
	if (localType >= ctx.archetypes[matched]->firstTag)
		return nullptr;
	if (check_bit(ctx.readonly, paramId))
	{
		if (localType == InvalidIndex)
			return const_cast<void*>(ctx.ctx.get_shared_ro(ctx.archetypes[matched], ctx.types[paramId]));
		else
			return const_cast<void*>(ctx.ctx.get_owned_ro_local(slice.c, localType));
	}
	else
	{
		if (localType == InvalidIndex)
			return nullptr; // 不允许非 owner 修改 share 的值
		else
			return const_cast<void*>(ctx.ctx.get_owned_rw_local(slice.c, localType));
	}
}

void* operation_base::get(int paramId, entity e, bool forceOwned)
{
	if (!check_bit(ctx.randomAccess, paramId))
		return nullptr;
	if (check_bit(ctx.readonly, paramId))
	{
		if(forceOwned)
			return const_cast<void*>(ctx.ctx.get_owned_ro(e, ctx.types[paramId]));
		else
			return const_cast<void*>(ctx.ctx.get_component_ro(e, ctx.types[paramId]));
	}
	else
		return const_cast<void*>(ctx.ctx.get_owned_rw(e, ctx.types[paramId]));
}

bool operation_base::is_owned(int paramId)
{
	return ctx.localType[paramId] != InvalidIndex;
}