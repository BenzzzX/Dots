#include "Codebase.h"
#include <algorithm>
#include <set>
using namespace core::codebase;
#define forloop(i, z, n) for(auto i = std::decay_t<decltype(n)>(z); i<(n); ++i)


constexpr uint16_t InvalidIndex = (uint16_t)-1;

template<class T>
T* allocate_inplace(char*& buffer, size_t size)
{
	char* allocated = buffer;
	buffer += size * sizeof(T);
	return (T*)allocated;
}

void pipeline::setup_kernel_dependency(kernel& k)
{
	std::set<kernel*> dependencies;
	int ati = 0;
	int eti = 0;
	forloop(i, 0, k.archetypeCount)
	{
		auto at = k.archetypes[i];
		while (archetypes[ati] != at)
		{
			eti += at->firstTag;
			ati++;
		}
		auto entries = denpendencyEntries.get() + eti;
		forloop(j, 0, k.paramCount)
		{
			auto localType = k.localType[i*k.paramCount + j];
			if (localType == InvalidIndex || localType > at->firstTag)
				continue;
			auto& entry = entries[localType];
			if (check_bit(k.readonly, j))
			{
				if (entry.owned)
					dependencies.insert(entry.owned);
				entry.shared.push_back(&k);
			}
			else
			{
				for (auto dp : entry.shared)
					dependencies.insert(dp);
				entry.shared.clear();
				entry.owned = &k;
			}
		}
	}
	k.dependencies = (kernel**)kernelStack.alloc(dependencies.size() * sizeof(kernel*));
	int i = 0;
	for (auto dp : dependencies)
		k.dependencies[i++] = dp;
}

pipeline::pipeline(world& ctx)
	:ctx(ctx)
{
	kernelStack.init(10000);
	archetypes = ctx.get_archetypes();
	size_t entryCount = 0;
	for (auto at : archetypes)
		entryCount += at->firstTag;
	denpendencyEntries = std::unique_ptr<dependency_entry[]>{ new dependency_entry[entryCount] };
}

pipeline::~pipeline()
{
}

chunk_vector<task> pipeline::create_tasks(kernel& k, int maxSlice)
{
	uint32_t count = k.chunkCount;
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

const core::entity* operation_base::get_entities() { return ctx.ctx.get_entities(slice.c) + slice.start; }

mask operation_base::get_mask() { return ctx.matched[matched]; }

bool operation_base::is_owned(int paramId)
{
	return ctx.localType[paramId] != InvalidIndex;
}