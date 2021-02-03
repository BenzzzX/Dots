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

pipeline::pipeline(world&& ctx)
	:world(std::move(ctx)), passIndex(0)
{
	on_archetype_update = [this](archetype* at, bool add)
	{
		update_archetype(at, add);
	};
	for (auto at : get_archetypes())
	{
		std::unique_ptr<dependency_entry[]> entries{ new dependency_entry[at->firstTag + 1] };
		dependencyEntries.try_emplace(at, std::move(entries));
	}
}

void pipeline::update_archetype(archetype* at, bool add)
{
	if (add)
	{
		std::unique_ptr<dependency_entry[]> entries{ new dependency_entry[at->firstTag + 1] };
		dependencyEntries.try_emplace(at, std::move(entries));
	}
	else
		dependencyEntries.erase(at);
}

pipeline::~pipeline()
{
	on_archetype_update = std::function<void(archetype*, bool)>();
}

world pipeline::release()
{
	on_archetype_update = std::function<void(archetype*, bool)>();
	return std::move(*this);
}

void pipeline::sync_archetype(archetype* at) const
{
	auto pair = dependencyEntries.find(at);
	auto entries = pair->second.get();
	std::vector<std::weak_ptr<custom_pass>> deps;
	auto count = at->firstTag + 1;
	forloop(i, 0, count)
	{
		if (entries[i].shared.empty())
		{
			if (!entries[i].owned.expired())
				deps.push_back(entries[i].owned);
		}
		else
		{
			for (auto p : entries[i].shared)
				deps.push_back(p);
		}
		entries[i].shared.clear();
		entries[i].owned.reset();
	}
	sync_dependencies(deps);
}

void pipeline::sync_entry(archetype* at, index_t type) const
{
	auto pair = dependencyEntries.find(at);
	auto entries = pair->second.get();
	std::vector<std::weak_ptr<custom_pass>> deps;
	auto i = at->index(type);
	//assert(i <= at->firstTag);
	if (entries[i].shared.empty())
	{
		if (!entries[i].owned.expired())
			deps.push_back(entries[i].owned);
	}
	else
	{
		for (auto p : entries[i].shared)
			deps.push_back(p);
	}
	entries[i].shared.clear();
	entries[i].owned.reset();
	sync_dependencies(deps);
}

void pipeline::sync_all_ro() const
{
	std::vector<std::weak_ptr<custom_pass>> deps;
	for (auto& pair : dependencyEntries)
	{
		auto entries = pair.second.get();
		forloop(i, 0, pair.first->firstTag)
		{
			if (!entries[i].owned.expired())
				deps.push_back(entries[i].owned);
			entries[i].owned.reset();
		}
	}
	sync_dependencies(deps);
}

void core::codebase::initialize(core::GUID(*new_guid_func)())
{
	core::database::initialize(new_guid_func);
	auto bi = get_builtin();
	cid<group> = bi.group_id;
	cid<disable> = bi.disable_id;
	cid<cleanup> = bi.cleanup_id;
	cid<mask> = bi.mask_id;
}

void custom_pass::release_dependencies()
{
	if (!dependencies)
		return;
	delete[] dependencies;
	dependencies = nullptr;
}

custom_pass::~custom_pass()
{
	release_dependencies();
}

chunk_vector<chunk_slice> pipeline::allocate(const entity_type& type, uint32_t count)
{
	archetype* g = get_archetype(type);
	return allocate(g, count);
}

chunk_vector<chunk_slice> pipeline::instantiate(entity src, uint32_t count)
{
	auto group_data = (buffer*)get_component_ro(src, get_builtin().group_id);
	if (group_data == nullptr)
	{
		sync_archetype(get_archetype(src));
		sync_archetype(get_casted(get_archetype(src), {}, true));
		return world::instantiate_single(src, count);
	}
	else
	{
		auto group = buffer_t<entity>(group_data);
		for (auto& e : group)
		{
			sync_archetype(get_archetype(e));
			sync_archetype(get_casted(get_archetype(e), {}, true));
		}
		return world::instantiate_group(group_data, count);
	}
}

chunk_vector<chunk_slice> pipeline::allocate(archetype* g, uint32_t count)
{
	sync_archetype(g);
	return world::allocate(g, count);
}

void pipeline::destroy(chunk_slice s)
{
	sync_archetype(world::get_archetype(s));
	world::destroy(s);
}

chunk_vector<chunk_slice> pipeline::cast(chunk_slice s, type_diff diff)
{
	archetype* g = get_casted(world::get_archetype(s), diff);
	return cast(s, g);
}

chunk_vector<chunk_slice> pipeline::cast(chunk_slice s, const entity_type& type)
{
	archetype* g = get_archetype(type);
	return cast(s, g);
}

chunk_vector<chunk_slice> pipeline::cast(chunk_slice s, archetype* g)
{
	sync_archetype(world::get_archetype(s));
	sync_archetype(g);
	return world::cast(s, g);
}


chunk_vector<chunk*> pipeline::query(archetype* g, const chunk_filter& filter)
{
	if(filter.changed.length > 0)
		sync_archetype(g); //同步 Timestamp
	return world::query(g, filter);
}


const void* pipeline::get_component_ro(entity e, index_t type) const noexcept
{
	if (!exist(e))
		return nullptr;
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	tsize_t id = g->index(type);
	if (id >= g->firstTag)
		return nullptr;
	if (id == InvalidIndex)
		return get_shared_ro(g, type);
	sync_entry(g, type);
	return c->data() + (size_t)g->offsets[(int)c->ct][id] + (size_t)data.i * g->sizes[id];
}

const void* pipeline::get_owned_ro(entity e, index_t type) const noexcept
{
	if (!exist(e))
		return nullptr;
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	tsize_t id = g->index(type);
	if (id == InvalidIndex || id >= g->firstTag)
		return nullptr;
	sync_entry(g, type);
	return c->data() + g->offsets[(int)c->ct][id] + (size_t)data.i * g->sizes[id];
}

const void* pipeline::get_shared_ro(entity e, index_t type) const noexcept
{
	return get_shared_ro(get_archetype(e), type);
}

bool pipeline::is_component_enabled(entity e, const typeset& type) const noexcept
{
	if (!exist(e))
		return false;
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	if (!g->withMask)
		return true;
	mask mm = g->get_mask(type);
	auto id = g->index(get_builtin().mask_id);
	sync_entry(g, get_builtin().mask_id);
	auto& m = *(mask*)(c->data() + g->offsets[(int)c->ct][id] + (size_t)data.i * g->sizes[id]);
	return (m & mm) == mm;
}

void* pipeline::get_owned_rw(entity e, index_t type) const noexcept
{
	if (!exist(e))
		return nullptr;
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	tsize_t id = g->index(type);
	if (id == InvalidIndex || id >= g->firstTag)
		return nullptr;
	sync_entry(g, type);
	g->timestamps(c)[id] = timestamp;
	return c->data() + g->offsets[(int)c->ct][id] + (size_t)data.i * g->sizes[id];
}
void pipeline::enable_component(entity e, const typeset& type) const noexcept
{
	if (!exist(e))
		return;
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	if (!g->withMask)
		return;
	mask mm = g->get_mask(type);
	auto id = g->index(get_builtin().mask_id);
	sync_entry(g, get_builtin().mask_id);
	g->timestamps(c)[id] = timestamp;
	auto& m = *(mask*)(c->data() + g->offsets[(int)c->ct][id] + (size_t)data.i * g->sizes[id]);
	m |= mm;
}
void pipeline::disable_component(entity e, const typeset& type) const noexcept
{
	if (!exist(e))
		return;
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	if (!g->withMask)
		return;
	mask mm = g->get_mask(type);
	auto id = g->index(get_builtin().mask_id);
	sync_entry(g, get_builtin().mask_id);
	g->timestamps(c)[id] = timestamp;
	auto& m = *(mask*)(c->data() + g->offsets[(int)c->ct][id] + (size_t)data.i * g->sizes[id]);
	m &= ~mm;
}

chunk_vector<entity> pipeline::gather_reference(entity e)
{
	sync_archetype(world::get_archetype(e));
	return world::gather_reference(e);
}
void pipeline::serialize(serializer_i* s, entity e)
{
	auto group_data = (buffer*)get_component_ro(e, get_builtin().group_id);
	if (group_data == nullptr)
	{
		sync_archetype(world::get_archetype(e));
		serialize_single(s, e);
	}
	else
	{
		auto group = buffer_t<entity>(group_data);
		for (auto& e : group)
			sync_archetype(get_archetype(e));
		serialize_group(s, group_data);
	}
}
chunk_slice pipeline::deserialize_single(serializer_i* s, patcher_i* patcher)
{
	auto* g = deserialize_archetype(s, patcher, false);
	sync_archetype(g);
	chunk_slice slice;
	deserialize_slice(g, s, slice);
	ents.new_entities(slice);
	if (patcher)
		chunk::patch(slice, patcher);
	return slice;
}
entity pipeline::deserialize(serializer_i* s, patcher_i* patcher)
{
	auto first_entity = [](chunk_slice s)
	{
		return s.c->get_entities()[s.start];
	};
	auto slice = deserialize_single(s, patcher);
	entity src = first_entity(slice);
	auto group_data = (buffer*)get_component_ro(src, get_builtin().group_id);
	if (group_data != nullptr)
	{
		uint32_t size = group_data->size / sizeof(entity);
		auto members = new entity[size];
		//stack_array(entity, members, size);
		members[0] = src;
		forloop(i, 1, size)
			members[i] = first_entity(deserialize_single(s, patcher));
		prefab_to_group(members, size);
		delete[] members;
	}
	return src;
}

const void* pipeline::get_component_ro(chunk* c, index_t t) const noexcept
{
	archetype* g = c->type;
	tsize_t id = g->index(t);
	if (id >= g->firstTag)
		return nullptr;
	if (id == InvalidIndex)
		return get_shared_ro(g, t);
	sync_entry(g, t);
	return c->data() + c->type->offsets[(int)c->ct][id];
}
const void* pipeline::get_owned_ro(chunk* c, index_t t) const noexcept
{
	tsize_t id = c->type->index(t);
	if (id == InvalidIndex || id >= c->type->firstTag)
		return nullptr;
	sync_entry(c->type, t);
	return c->data() + c->type->offsets[(int)c->ct][id];
}
const void* pipeline::get_shared_ro(chunk* c, index_t t) const noexcept
{
	archetype* g = c->type;
	return get_shared_ro(g, t);
}
void* pipeline::get_owned_rw(chunk* c, index_t t) noexcept
{
	tsize_t id = c->type->index(t);
	if (id == InvalidIndex || id >= c->type->firstTag)
		return nullptr;
	sync_entry(c->type, t);
	c->type->timestamps(c)[id] = timestamp;
	return c->data() + c->type->offsets[(int)c->ct][id];
}

const void* pipeline::get_shared_ro(archetype* g, index_t type) const
{
	entity* metas = g->metatypes;
	forloop(i, 0, g->metaCount)
		if (const void* shared = get_component_ro(metas[i], type))
			return shared;
	return nullptr;
}

/*** per world ***/
void pipeline::move_context(world& src)
{
	sync_all();
	world::move_context(src);
}
void pipeline::patch_chunk(chunk* c, patcher_i* patcher)
{
	sync_archetype(world::get_archetype(c));
	world::patch_chunk(c, patcher);
}
//serialize
void pipeline::serialize(serializer_i* s)
{
	sync_all();
	world::serialize(s);
}
void pipeline::deserialize(serializer_i* s)
{
	sync_all();
	world::deserialize(s);
}
void pipeline::merge_chunks()
{
	sync_all();
	world::merge_chunks();
}

int filters::get_size() const
{
	return archetypeFilter.get_size() +
		chunkFilter.get_size() +
		entityFilter.get_size();
}

filters filters::clone(char*& buffer) const
{
	return {
		archetypeFilter.clone(buffer),
		chunkFilter.clone(buffer),
		entityFilter.clone(buffer)
	};
}