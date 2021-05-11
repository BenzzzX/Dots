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

std::shared_ptr<custom_pass> pipeline::create_custom_pass(gsl::span<shared_entry> sharedEntries)
{
	char* buffer = (char*)::malloc(sizeof(custom_pass));
	custom_pass* k = new(buffer) custom_pass{ *this };
	std::shared_ptr<custom_pass> ret{ k };
	k->passIndex = passIndex++;
	setup_custom_pass_dependency(ret, sharedEntries);
	setup_custom_pass(ret);
	return ret;
}


std::pair<chunk_vector<task>, chunk_vector<task_group>> pipeline::create_tasks(pass& k, int batchCount)
{
	int indexInKernel = 0;
	chunk_vector<task> result;
	chunk_vector<task_group> groups;
	task_group group;
	group.begin = group.end = 0;
	int batch = batchCount;
	forloop(i, 0, k.archetypeCount)
		for (auto c : k.ctx.query(k.archetypes[i], k.filter.chunkFilter))
		{
			uint32_t allocated = 0;
			while (allocated != c->get_count())
			{
				uint32_t sliceCount;
				sliceCount = std::min(c->get_count() - allocated, (uint32_t)batch);
				task newTask{ };
				newTask.gid = i;
				newTask.slice = chunk_slice{ c, allocated, sliceCount };
				newTask.indexInKernel = indexInKernel;
				allocated += sliceCount;
				indexInKernel += sliceCount;
				batch -= sliceCount;
				result.push(newTask);

				if (batch == 0)
				{
					group.end = result.size;
					groups.push(group);
					group.begin = group.end;
					batch = batchCount;
				}
			}
		}
	if (group.end != result.size)
	{
		group.end = result.size;
		groups.push(group);
	}
	return { std::move(result), std::move(groups) };
}

uint32_t pass::calc_size() const
{
	uint32_t entityCount = 0;
	if (filter.chunkFilter.changed.length > 0)
		forloop(i, 0, archetypeCount)
			ctx.sync_archetype(archetypes[i]);
	auto& wrd = (world&)ctx;
	forloop(i, 0, archetypeCount)
		for (auto j : wrd.query(archetypes[i], filter.chunkFilter))
			entityCount += j->get_count();
	return entityCount;
}


void setup_shared_dependency(const std::shared_ptr<custom_pass>& k, gsl::span<shared_entry> sharedEntries, detail::weak_ptr_set& dependencies)
{
	for (auto& i : sharedEntries)
	{
		auto& entry = i.entry;
		if (i.readonly)
		{
			if (!entry.owned.expired())
				dependencies.insert(entry.owned);
			entry.shared.erase(remove_if(entry.shared.begin(), entry.shared.end(), [](auto& n) {return n.expired(); }), entry.shared.end());
			entry.shared.push_back(k);
		}
		else
		{
			for (auto dp : entry.shared)
				dependencies.insert(dp);
			if (entry.shared.empty() && !entry.owned.expired())
				dependencies.insert(entry.owned);
			entry.shared.clear();
			entry.owned = k;
		}
	}
}

void pipeline::setup_custom_pass_dependency(std::shared_ptr<custom_pass>& k, gsl::span<shared_entry> sharedEntries)
{
	detail::weak_ptr_set dependencies;
	setup_shared_dependency(k, sharedEntries, dependencies);
	k->dependencies = new std::weak_ptr<custom_pass>[dependencies.size()];
	k->dependencyCount = static_cast<int>(dependencies.size());
	int i = 0;
	for (auto dp : dependencies)
		k->dependencies[i++] = dp;
}

void pipeline::setup_pass_dependency(std::shared_ptr<pass>& k, gsl::span<shared_entry> sharedEntries)
{
	constexpr uint16_t InvalidIndex = (uint16_t)-1;
	detail::weak_ptr_set dependencies;
	std::set<std::pair<archetype*, index_t>> syncedEntry;
	setup_shared_dependency(std::static_pointer_cast<custom_pass>(k), sharedEntries, dependencies);

	struct HELPER
	{
		static archetype* get_owning_archetype(world* ctx, archetype* sharing, index_t type)
		{
			if (sharing->index(type) != InvalidIndex)
				return sharing;
			entity* metas = sharing->metatypes;
			forloop(i, 0, sharing->metaCount)
				if (archetype* owning = get_owning_archetype(ctx, ctx->get_archetype(metas[i]), type))
					return owning;
			return nullptr;
		}
	};

	auto sync_entry = [&](archetype* at, index_t localType, bool readonly)
	{
		auto pair = std::make_pair(at, localType);
		if (syncedEntry.find(pair) != syncedEntry.end())
			return;
		syncedEntry.insert(pair);
		auto iter = dependencyEntries.find(at);
		if (iter == dependencyEntries.end())
			return;

		auto entries = (*iter).second.get();
		if (localType >= at->firstTag || localType == InvalidIndex)
			return;
		auto& entry = entries[localType];
		if (readonly)
		{
			if (!entry.owned.expired())
				dependencies.insert(entry.owned);
			entry.shared.erase(remove_if(entry.shared.begin(), entry.shared.end(), [](auto& n) {return n.expired(); }), entry.shared.end());
			entry.shared.push_back(k);
		}
		else
		{
			for (auto& dp : entry.shared)
				if (!dp.expired())
					dependencies.insert(dp);
			if (entry.shared.empty() && !entry.owned.expired())
				dependencies.insert(entry.owned);
			entry.shared.clear();
			entry.owned = k;
		}
	};

	auto sync_type = [&](index_t type, bool readonly)
	{
		for (auto& pair : dependencyEntries)
		{
			index_t localType = pair.first->index(type);
			auto entries = pair.second.get();
			if (localType >= pair.first->firstTag || localType == InvalidIndex)
				return;
			auto& entry = entries[localType];
			if (readonly)
			{
				if (!entry.owned.expired())
					dependencies.insert(entry.owned);
				entry.shared.erase(remove_if(entry.shared.begin(), entry.shared.end(), [](auto& n) {return n.expired(); }), entry.shared.end());
				entry.shared.push_back(k);
			}
			else
			{
				for (auto& dp : entry.shared)
					if (!dp.expired())
						dependencies.insert(dp);
				if (entry.shared.empty() && !entry.owned.expired())
					dependencies.insert(entry.owned);
				entry.shared.clear();
				entry.owned = k;
			}
		}
	};

	auto sync_entities = [&](archetype* at)
	{
		auto iter = dependencyEntries.find(at);
		if (iter == dependencyEntries.end())
			return;
		auto entries = (*iter).second.get();
		auto& entry = entries[at->firstTag];
		entry.shared.erase(remove_if(entry.shared.begin(), entry.shared.end(), [](auto& n) {return n.expired(); }), entry.shared.end());
		entry.shared.push_back(k);
	};

	forloop(i, 0, k->archetypeCount)
	{
		archetype* at = k->archetypes[i];
		forloop(j, 0, k->paramCount)
		{
			if (check_bit(k->randomAccess, j))
			{
				sync_type(k->types[j], check_bit(k->readonly, j));
			}
			else
			{
				auto localType = k->localType[i * k->paramCount + j];
				if (localType == InvalidIndex)
				{
					//assert(check_bit(k->readonly, j))
					auto type = k->types[j];
					auto oat = HELPER::get_owning_archetype((world*)this, at, type);
					if (!oat) // 存在 any 时可能出现
						continue;
					sync_entry(oat, oat->index(type), true);
				}
				else
					sync_entry(at, localType, check_bit(k->readonly, j));
			}
		}
		sync_entities(at);
		auto& changed = k->filter.chunkFilter.changed;
		forloop(j, 0, changed.length)
		{
			auto localType = at->index(changed[j]);
			sync_entry(at, localType, true);
		}
	}
	if (dependencies.size() > 0)
		k->dependencies = new std::weak_ptr<custom_pass>[dependencies.size()];
	else
		k->dependencies = nullptr;
	k->dependencyCount = static_cast<int>(dependencies.size());
	int i = 0;
	for (auto dp : dependencies)
		k->dependencies[i++] = dp;
}


#ifdef ENABLE_GUID_COMPONENT
void core::codebase::initialize(core::GUID(*guid_generator)())
#else
void core::codebase::initialize()
#endif 
{
#ifdef ENABLE_GUID_COMPONENT
	core::database::initialize(guid_generator);
#else
	core::database::initialize();
#endif
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