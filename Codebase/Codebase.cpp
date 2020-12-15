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
custom_pass* pipeline::create_custom_pass(gsl::span<shared_entry> sharedEntries)
{
	char* buffer = (char*)passStack.alloc(sizeof(custom_pass));
	custom_pass* k = new(buffer) custom_pass{ctx};
	k->passIndex = passIndex++;
	setup_pass_dependency(*k, sharedEntries);
	return k;
}

void setup_shared_dependency(custom_pass& k, gsl::span<shared_entry> sharedEntries, std::set<custom_pass*>& dependencies)
{
	for (auto& i : sharedEntries)
	{
		auto& entry = i.entry;
		if (i.readonly)
		{
			if (entry.owned)
				dependencies.insert(entry.owned);
			entry.shared.push_back(&k);
		}
		else
		{
			for (auto dp : entry.shared)
				dependencies.insert(dp);
			if (entry.shared.empty() && entry.owned)
				dependencies.insert(entry.owned);
			entry.shared.clear();
			entry.owned = &k;
		}
	}
}

void pipeline::setup_pass_dependency(custom_pass& k, gsl::span<shared_entry> sharedEntries)
{
	std::set<custom_pass*> dependencies;
	setup_shared_dependency(k, sharedEntries, dependencies);
	k.dependencies = (custom_pass**)passStack.alloc(dependencies.size() * sizeof(custom_pass*));
	k.dependencyCount = static_cast<int>(dependencies.size());
	int i = 0;
	for (auto dp : dependencies)
		k.dependencies[i++] = dp;
}

void pipeline::setup_pass_dependency(pass& k, gsl::span<shared_entry> sharedEntries)
{
	std::set<custom_pass*> dependencies;
	setup_shared_dependency(k, sharedEntries, dependencies);
	forloop(i, 0, k.archetypeCount)
	{
		auto at = k.archetypes[i];
		auto iter = dependencyEntries.find(at);
		if (iter == dependencyEntries.end())
			continue;

		auto entries = (*iter).second.get();
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
				if(entry.shared.empty() && entry.owned)
					dependencies.insert(entry.owned);
				entry.shared.clear();
				entry.owned = &k;
			}
		}
	}
	k.dependencies = (custom_pass**)passStack.alloc(dependencies.size() * sizeof(custom_pass*));
	k.dependencyCount = static_cast<int>(dependencies.size());
	int i = 0;
	for (auto dp : dependencies)
		k.dependencies[i++] = dp;
}

pipeline::pipeline(world& ctx)
	:ctx(ctx), passIndex(0)
{
	passStack.init(10000);
	ctx.on_archetype_update = [this](archetype* at, bool add)
	{
		update_archetype(at, add);
	};
	for (auto at : ctx.get_archetypes())
	{
		std::unique_ptr<dependency_entry[]> entries{ new dependency_entry[at->firstTag] };
		dependencyEntries.try_emplace(at, std::move(entries));
	}
}

void pipeline::update_archetype(archetype* at, bool add)
{
	if (add)
	{
		std::unique_ptr<dependency_entry[]> entries{ new dependency_entry[at->firstTag] };
		dependencyEntries.try_emplace(at, std::move(entries));
	}
	else
		dependencyEntries.erase(at);
}

pipeline::~pipeline()
{
	ctx.on_archetype_update = std::function<void(archetype*, bool)>();
}

chunk_vector<task> pipeline::create_tasks(pass& k, int maxSlice)
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
				sliceCount = std::min(c->get_count(), c->get_type()->chunkCapacity[(int)alloc_type::fastbin]);
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


void pipeline::sync_archetype(archetype* at)
{
	auto pair = dependencyEntries.find(at);
	auto entries = pair->second.get();
	std::vector<custom_pass*> deps;
	forloop(i, 0, at->firstTag)
	{
		if (entries[i].shared.empty())
		{
			if (entries[i].owned)
				deps.push_back(entries[i].owned);
		}
		else
		{
			for (auto p : entries[i].shared)
				deps.push_back(p);
		}
	}
	on_sync(deps);
}

void pipeline::sync_entry(archetype* at, index_t type)
{
	auto pair = dependencyEntries.find(at);
	auto entries = pair->second.get();
	std::vector<custom_pass*> deps;
	auto i = at->index(type);
	//assert(i <= at->firstTag);
	if (entries[i].shared.empty())
	{
		if (entries[i].owned)
			deps.push_back(entries[i].owned);
	}
	else
	{
		for (auto p : entries[i].shared)
			deps.push_back(p);
	}
	on_sync(deps);
}

const core::entity* operation_base::get_entities() { return ctx.ctx.get_entities(slice.c) + slice.start; }

mask operation_base::get_mask() { return ctx.matched[matched]; }

bool operation_base::is_owned(int paramId)
{
	return ctx.localType[paramId] != InvalidIndex;
}

CODE_API void core::codebase::initialize()
{
	cid<group> = group_id;
	cid<disable> = disable_id;
	cid<cleanup> = cleanup_id;
	cid<mask> = mask_id;
}
