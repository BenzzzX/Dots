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

pipeline::pipeline()
{
	kernelStack.init(10000);
}

kernel* pipeline::create_kernel(world& w, const view& v)
{
	auto archs = w.query(v.archetypeFilter);
	size_t bufferSize = 
		sizeof(kernel) //自己
		+ archs.size * (sizeof(void*) + sizeof(mask)) // mask + archetype
		+ archs.size * sizeof(index_t)
		+ v.paramCount * sizeof(index_t) * (archs.size + 1) //type + local type list
		+ (v.paramCount / 4 + 1) * sizeof(index_t) * 2; //readonly + random access
	char* buffer = (char*)kernelStack.alloc(bufferSize, alignof(kernel));
	kernel* k = new(buffer) kernel{w};
	buffer += sizeof(kernel);
	k->archetypeCount = archs.size;
	k->paramCount = v.paramCount;
	k->archetypes = allocate_inplace<archetype*>(buffer, archs.size);
	k->matched = allocate_inplace<mask>(buffer, archs.size);
	k->chunkCount = allocate_inplace<index_t>(buffer, archs.size);
	k->types = allocate_inplace<index_t>(buffer, v.paramCount);
	k->readonly = allocate_inplace<index_t>(buffer, v.paramCount / 4 + 1);
	k->randomAccess = allocate_inplace<index_t>(buffer, v.paramCount / 4 + 1);
	k->localType = allocate_inplace<index_t>(buffer, v.paramCount * archs.size);
	forloop(t, 0, v.paramCount)
	{
		k->types[t] = v.params[t].typeIndex;
		set_bit(k->readonly, v.params[t].readonly);
		set_bit(k->randomAccess, v.params[t].randomAccess);
	}
	int counter = 0;
	for (auto i : archs)
	{
		k->chunkCount[counter] = 0;
		for (auto j : w.query(i.type, v.chunkFilter))
		{
			k->chunkCount[counter]++;
			k->chunks.push(j);
		}
		k->archetypes[counter] = i.type;
		v.entityFilter.apply(i);
		k->matched[counter] = i.matched;
		forloop(t, 0, v.paramCount)
			k->localType[t + counter*archs.size] = i.type->index(k->types[t]);
		counter++;
	}
	return k;
}

chunk_vector<task> pipeline::create_tasks(kernel& k, int maxSlice)
{
	int count = k.chunks.size;
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

const core::entity* operation::get_entities() { return ctx.ctx.get_entities(slice.c) + slice.start; }

mask operation::get_mask() { return ctx.matched[matched]; }

void* operation::get(int paramId)
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

void* operation::get(int paramId, entity e)
{
	if (!check_bit(ctx.randomAccess, paramId))
		return nullptr;
	if (check_bit(ctx.readonly, paramId))
		return const_cast<void*>(ctx.ctx.get_component_ro(e, ctx.types[paramId]));
	else
		return const_cast<void*>(ctx.ctx.get_owned_rw(e, ctx.types[paramId]));
}
