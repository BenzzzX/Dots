#include "Codebase.h"
#include <algorithm>
#include <set>
using namespace core::codebase;
#define forloop(i, z, n) for(auto i = std::decay_t<decltype(n)>(z); i<(n); ++i)


template<class T>
T* allocate_inplace(char*& buffer, size_t size)
{
	char* allocated = buffer;
	buffer += size * sizeof(T);
	return (T*)allocated;
}

pipeline::pipeline(world& ctx)
	:ctx(ctx), passIndex(0)
{
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
				deps.push_back(entries[i].owned.get());
		}
		else
		{
			for (auto p : entries[i].shared)
				deps.push_back(p.get());
		}
		entries[i].shared.clear();
		entries[i].owned = nullptr;
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
			deps.push_back(entries[i].owned.get());
	}
	else
	{
		for (auto p : entries[i].shared)
			deps.push_back(p.get());
	}
	entries[i].shared.clear();
	entries[i].owned = nullptr;
	on_sync(deps);
}

CODE_API void core::codebase::initialize()
{
	cid<group> = group_id;
	cid<disable> = disable_id;
	cid<cleanup> = cleanup_id;
	cid<mask> = mask_id;
}

void custom_pass::release_dependencies()
{
	delete[] dependencies;
}
