#include "MemoryModel.h"

#define forloop(i, z, n) for(auto i = decltype(n)(z); i<n; ++i)

#define stack_array(type, name, size) \
type* name = (type*)alloca((size) * sizeof(type))

entity entity::Invalid{ -1 };
uint32_t database::metaTimestamp;

using namespace core;
using namespace database;

struct type_data
{
	size_t hash;
	uint16_t size;
	uint16_t elementSize;
	uint32_t entityRefs;
	uint16_t entityRefCount;
	const char* name;
	component_vtable vtable;
};

index_t core::database::disable_id = 0;
index_t core::database::cleanup_id = 1;
index_t core::database::group_id = 1;

struct metainfo
{
	uint32_t refCount = 0;
	uint32_t timestamp = 0;
};

struct global_data
{
	std::vector<type_data> infos;
	std::vector<track_state> tracks;
	std::vector<intptr_t> entityRefs;
	std::unordered_map<size_t, uint16_t> hash2type;
	
	global_data()
	{
		component_desc desc{};
		desc.size = 0;
		desc.hash = 0;
		desc.name = "cleaning";
		cleanup_id = register_type(desc);
		desc.size = 0;
		desc.hash = 1;
		desc.name = "disabled";
		disable_id = register_type(desc);
		desc.size = sizeof(entity) * 5;
		desc.hash = 2;
		desc.isElement = true;
		desc.elementSize = sizeof(entity);
		static intptr_t entityRefs[] = { (intptr_t)offsetof(group, e) };
		desc.entityRefs = entityRefs;
		desc.entityRefCount = 1;
		desc.name = "group";
		group_id = register_type(desc);
	}
};

static global_data gd;

component_vtable& set_vtable(index_t m)
{
	return gd.infos[m].vtable;
}

index_t database::register_type(component_desc desc)
{
	uint32_t rid = -1;
	if (desc.entityRefs != nullptr)
	{
		rid = gd.entityRefs.size();
		forloop(i, 0, desc.entityRefCount)
			gd.entityRefs.push_back(desc.entityRefs[i]);
	}
	
	uint16_t id = (uint16_t)gd.infos.size();
	id = tagged_index{ id, desc.isElement, desc.size == 0 };
	type_data i{ desc.hash, desc.size, desc.elementSize, rid, desc.entityRefCount, desc.name, desc.vtable };
	gd.infos.push_back(i);
	uint8_t s = 0;
	if (desc.need_clean)
		s = s | NeedCleaning;
	if (desc.need_copy)
		s = s | NeedCopying;
	gd.tracks.push_back((track_state)s);
	gd.hash2type.insert({ desc.hash, id });

	id = (uint16_t)gd.infos.size();
	id = tagged_index{ id, desc.isElement, desc.size == 0 };
	type_data i{ desc.hash, desc.size, desc.elementSize, rid, desc.entityRefCount, desc.name, desc.vtable };
	gd.tracks.push_back(Copying);
	gd.infos.push_back(i);
	return id;
}

inline bool chunk_slice::full() { return start == 0 && count == c->get_count(); }

inline chunk_slice::chunk_slice(chunk* c) : c(c), start(0), count(c->get_count()) {}

void chunk::link(chunk* c) noexcept
{
	if (c != nullptr)
	{
		c->next = next;
		c->prev = this;
	}
	if (next != nullptr)
		next->prev = c;
	next = c;
}

void chunk::unlink() noexcept
{
	if (prev != nullptr)
		prev->next = next;
	if (next != nullptr)
		next->prev = prev;
	prev = next = nullptr;
}

uint32_t chunk::get_timestamp(index_t t) noexcept
{
	uint16_t id = type->index(t);
	if (id == -1) return 0;
	return type->timestamps(this)[id];
}

void chunk::move(chunk_slice dst, uint16_t srcIndex) noexcept
{
	chunk* src = dst.c;
	uint16_t* offsets = src->type->offsets();
	uint16_t* sizes = src->type->sizes();
	forloop(i, 0, src->type->firstTag)
		memcpy(
			src->data() + offsets[i] + sizes[i] * dst.start,
			src->data() + offsets[i] + sizes[i] * srcIndex,
			dst.count * sizes[i]
		);
}

#define srcData (s.c->data() + offsets[i] + sizes[i] * s.start)
void chunk::construct(chunk_slice s) noexcept
{
	context::archetype* type = s.c->type;
	uint16_t* offsets = type->offsets();
	uint16_t* sizes = type->sizes();
#ifndef NOINITIALIZE
	forloop(i, 0, type->firstBuffer)
		memset(srcData, 0, sizes[i] * s.count);
#endif
	forloop(i, type->firstBuffer, type->firstTag)
	{
		char* src = srcData;
		forloop(j, 0, s.count)
			new(j * sizes[i] + src) buffer{ sizes[i] - sizeof(buffer) };
	}
}

void chunk::destruct(chunk_slice s) noexcept
{
	context::archetype* type = s.c->type;
	uint16_t* offsets = type->offsets();
	uint16_t* sizes = type->sizes();
	index_t* types = type->types();
	forloop(i, type->firstBuffer, type->firstTag)
	{
		char* src = srcData;
		forloop(j, 0, s.count)
			((buffer*)(j * sizes[i] + src))->~buffer();
	}
}

void memdup(void* dst, const void* src, size_t size, size_t count) noexcept
{
	size_t copied = 1;
	memcpy(dst, src, size);
	while (copied < count)
	{
		size_t toCopy = std::min(copied, count - copied);
		memcpy((char*)dst + copied * size, dst, toCopy * size);
		copied += toCopy;
	}
}

tagged_index to_valid_type(tagged_index t)
{
	if (gd.tracks[t.index()] == Copying)
		return t - 1;
	else
		return t;
}

#undef srcData
#define dstData (dst.c->data() + offsets[i] + sizes[i] * dst.start)
#define srcData (src->data() + offsets[i] + sizes[i] * srcIndex)
void chunk::duplicate(chunk_slice dst, const chunk* src, uint16_t srcIndex) noexcept
{
	context::archetype* type = src->type;
	context::archetype *dstType = dst.c->type;
	uint16_t dstI = 0;
	uint16_t* offsets = type->offsets();
	uint16_t* dstOffsets = dstType->offsets();
	uint16_t* sizes = type->sizes();
	index_t* types = type->types();
	forloop(i, 0, type->firstBuffer)
	{
		tagged_index st = to_valid_type(type->types()[i]);
		tagged_index dt = to_valid_type(dstType->types()[dstI]);
		
		if (st != dt)
			continue;
		memdup(dstData, srcData, sizes[i], dst.count);
		dstI++;
	}
	forloop(i, type->firstBuffer, type->firstTag)
	{
		tagged_index st = to_valid_type(type->types()[i]);
		tagged_index dt = to_valid_type(dstType->types()[dstI]);

		if (st != dt)
			continue;
		const char* s = srcData;
		char* d = dstData;
		forloop(j, 0, dst.count)
			new(d + sizes[i]*j) buffer{ *(buffer*)s };
		dstI++;
	}
}

void patch_entity(entity& e, uint32_t start, const entity* target, uint32_t count) noexcept
{
	if (e.id < start || e.id > start + count) return;
	e = target[e.id - start];
}

void depatch_entity(entity& e, const entity* src, const entity* target, uint32_t count) noexcept
{
	forloop(i, 0, count)
		if (e == src[i])
		{
			e = target[i];
			return;
		}
}

void chunk::patch(chunk_slice s, patcher_i* patcher) noexcept
{
	context::archetype* g = s.c->type;
	uint16_t* offsets = g->offsets();
	uint16_t* sizes = g->sizes();
	tagged_index* types = (tagged_index*)g->types();
	forloop(i, 0, g->firstBuffer)
	{
		const auto& t = gd.infos[types[i].index()];
		char* arr = s.c->data() + offsets[i] + sizes[i]*s.start;
		auto f = t.vtable.patch;
		if (f != nullptr)
		{
			forloop(j, 0, s.count)
			{
				char* data = arr + sizes[i] * j;
				forloop(k, 0, t.entityRefCount)
				{
					entity& e = *(entity*)(data + gd.entityRefs[t.entityRefs + k]);
					e = patcher->patch(e);
				}
				f(data, patcher);
				patcher->move();
			}
		}
		else
		{
			forloop(j, 0, s.count)
			{
				char* data = arr + sizes[i] * j;
				forloop(k, 0, t.entityRefCount)
				{
					entity& e = *(entity*)(data + gd.entityRefs[t.entityRefs + k]);
					e = patcher->patch(e);
				}
				patcher->move();
			}
		}
		patcher->reset();
	}

	forloop(i, g->firstBuffer, g->firstTag)
	{
		const auto& t = gd.infos[types[i].index()];
		char* arr = s.c->data() + offsets[i] + sizes[i] * s.start;
		forloop(j, 0, s.count)
		{
			buffer *b =(buffer*)(arr + sizes[i] * j);
			uint16_t n = b->size / t.elementSize;
			auto f = t.vtable.patch;
			if (f != nullptr)
			{
				forloop(l, 0, n)
				{
					char* data = b->data() + t.elementSize * l;
					forloop(k, 0, t.entityRefCount)
					{
						entity& e = *(entity*)(data + gd.entityRefs[t.entityRefs + k]);
						e = patcher->patch(e);
					}
					f(data, patcher);
					patcher->move();
				}
			}
			else
			{
				forloop(l, 0, n)
				{
					char* data = b->data() + t.elementSize * l;
					forloop(k, 0, t.entityRefCount)
					{
						entity& e = *(entity*)(data + gd.entityRefs[t.entityRefs + k]);
						e = patcher->patch(e);
					}
					patcher->move();
				}
			}
		}
		patcher->reset();
	}
}

//TODO: handle transient data?
void chunk::serialize(chunk_slice s, serializer_i* stream)
{
	context::archetype* type = s.c->type;
	uint16_t* offsets = type->offsets();
	uint16_t* sizes = type->sizes();
	tagged_index* types = (tagged_index*)type->types();
	stream->stream(&s.count, sizeof(uint16_t));

	forloop(i, 0, type->firstTag)
	{
		char* arr = s.c->data() + offsets[i] + sizes[i] * s.start;
		stream->stream(arr, sizes[i] * s.count);
	}

	forloop(i, type->firstBuffer, type->firstTag)
	{
		char* arr = s.c->data() + offsets[i] + sizes[i] * s.start;
		forloop(j, 0, s.count)
		{
			buffer* b = (buffer*)(arr + j * sizes[i]);
			stream->stream(b->data(), b->size);
		}
	}
}

void chunk::cast(chunk_slice dst, chunk* src, uint16_t srcIndex) noexcept
{
	context::archetype* srcType = src->type;
	context::archetype* dstType = dst.c->type;
	uint16_t dstI = 0;
	uint16_t srcI = 0;
	uint16_t count = dst.count;
	uint16_t* srcOffsets = srcType->offsets();
	uint16_t* dstOffsets = dstType->offsets();
	uint16_t* srcSizes = srcType->sizes();
	uint16_t* dstSizes = dstType->sizes();
	index_t* srcTypes = srcType->types(); index_t* dstTypes = dstType->types();

	while (srcI < srcType->firstBuffer && dstI < dstType->firstBuffer)
	{
		auto st = to_valid_type(srcTypes[srcI]);
		auto dt = to_valid_type(dstTypes[dstI]);
		char* s = src->data() + srcOffsets[srcI] + srcSizes[srcI] * srcIndex;
		char* d = dst.c->data() + dstOffsets[dstI] + dstSizes[dstI] * dst.start;
		if (st < dt) //destruct 
			srcI++;
		else if (st > dt) //construct
#ifndef NOINITIALIZE
			memset(d, 0, dstSizes[dstI++] * count);
#else
			dstI++;
#endif
		else //move
			memcpy(d, s, dstSizes[(srcI++, dstI++)] * count);
	}

	srcI = srcType->firstBuffer; // destruct
#ifndef NOINITIALIZE
	while (dstI < dstType->firstBuffer) //construct
	{
		char* d = dst.c->data() + dstOffsets[dstI] + dstSizes[dstI] * dst.start;
		memset(d, 0, dstSizes[dstI] * count);
		dstI++;
	}
#else
	dstI = dstType->firstBuffer;
#endif
	while (srcI < srcType->firstTag && dstI < dstType->firstTag)
	{
		auto st = to_valid_type(srcTypes[srcI]);
		auto dt = to_valid_type(dstTypes[dstI]);
		char* s = src->data() + srcOffsets[srcI] + srcSizes[srcI] * srcIndex;
		char* d = dst.c->data() + dstOffsets[dstI] + dstSizes[dstI] * dst.start;
		if (st < dt) //destruct 
		{
			forloop(j, 0, count)
				((buffer*)(j * srcSizes[srcI] + s))->~buffer();
			srcI++;
		}
		else if (st > dt) //construct
		{
			forloop(j, 0, count)
				new(j * dstSizes[dstI] + d) buffer{ dstSizes[dstI] - sizeof(buffer) };
			dstI++;
		}
		else //move
		{
			memcpy(d, s, dstSizes[dstI] * count);
#ifndef NOINITIALIZE
			forloop(j, 0, count)
				new(j * srcSizes[srcI] + s) buffer{ srcSizes[dstI] - sizeof(buffer) };
#endif
			dstI++; srcI++;
		}
	}
	while (srcI < srcType->firstTag) //destruct 
	{
		char* s = src->data() + srcOffsets[srcI] + srcSizes[srcI] * srcIndex;
		forloop(j, 0, count)
			((buffer*)(j * srcSizes[srcI] + s))->~buffer();
		srcI++;
	}
	while (dstI < dstType->firstTag) //construct
	{
		char* d = dst.c->data() + dstOffsets[dstI] + dstSizes[dstI] * dst.start;
		forloop(j, 0, count)
			new(j * dstSizes[dstI] + d) buffer{ dstSizes[dstI] - sizeof(buffer) };
		dstI++;
	}
}

inline uint32_t* context::archetype::timestamps(chunk* c) noexcept { return (uint32_t*)(c->data() + kChunkBufferSize) - firstTag; }

uint16_t context::archetype::index(index_t type) noexcept
{
	index_t* ts = types();
	index_t* result = std::lower_bound(ts, ts + componentCount, type);
	if (result != ts + componentCount && *result == type)
		return uint16_t(result - ts);
	else
		return uint16_t(-1);
}

bool context::archetype::match_filter(const entity_filter& filter)
{
	//TODO
	return false;
}

entity_type context::archetype::get_type()
{
	index_t* ts = types();
	entity* ms = metatypes();
	return entity_type
	{
		typeset { ts, componentCount },
		metaset { ms, metaCount }
	};

}

size_t context::archetype::calculate_size(uint16_t componentCount, uint16_t firstTag, uint16_t metaCount)
{
	return sizeof(index_t)* componentCount +
		sizeof(uint16_t) * firstTag +
		sizeof(uint16_t) * firstTag +
		sizeof(entity) * metaCount +
		sizeof(context::archetype);// +40;
}

uint16_t get_filter_size(const entity_filter& f)
{
	auto totalSize = f.all.types.length * sizeof(index_t) + f.all.metatypes.length * sizeof(entity) +
		f.any.types.length * sizeof(index_t) + f.any.metatypes.length * sizeof(entity) +
		f.none.types.length * sizeof(index_t) + f.none.metatypes.length * sizeof(entity) +
		f.changed.length * sizeof(index_t);
	return totalSize;
}

entity_filter clone_filter(const entity_filter& f, char* data)
{
	entity_filter f2;
	auto write = [&](const typeset& t, typeset& r)
	{
		r.data = (index_t*)data; r.length = t.length;
		memcpy(data, t.data, t.length * sizeof(index_t));
		data += t.length * sizeof(index_t);
	};
	auto writemeta = [&](const metaset& t, metaset& r)
	{
		r.data = (entity*)data; r.length = t.length;
		memcpy(data, t.data, t.length * sizeof(entity));
		data += t.length * sizeof(entity);
	};
#define writeType(name) \
		write(f.name.types, f2.name.types); writemeta(f.name.metatypes, f2.name.metatypes);
	writeType(all);
	writeType(any);
	writeType(none);
	write(f.changed, f2.changed);
#undef writeType
	f2.prevTimestamp = f.prevTimestamp;
	return f2;
}

context::query_cache& context::get_query_cache(const entity_filter& f)
{
	auto iter = queries.find(f);
	if (iter != queries.end())
	{
		return iter->second;
	}
	else
	{
		query_cache cache;
		bool includeClean = false;
		bool includeDisabled = false;
		bool includeCopy = [&]
		{
			auto at = f.all.types;
			forloop(i, 0, at.length)
			{
				tagged_index type = at[i];
				if (type == cleanup_id)
					includeClean = true;
				if (type == disable_id)
					includeDisabled = true;
				if (gd.tracks[type.index()] & Copying != 0)
					return true;
			}
			return false;
		}();
		includeCopy |= includeClean;
		includeDisabled |= includeCopy;
		auto valid_group = [&](archetype* g)
		{
			if (includeClean < g->cleaning)
				return false;
			if (includeDisabled < g->disabled)
				return false;
			return f.match(g->get_type());
		};
		for (auto i : archetypes)
		{
			if (valid_group(i.second))
				cache.archetypes.push_back(i.second);
		}
		cache.includeClean = includeClean;
		cache.includeDisabled = includeDisabled;
		auto totalSize = get_filter_size(f);
		cache.data.resize(totalSize);
		char* data = cache.data.data();
		cache.filter = clone_filter(f, data);
		auto p = queries.insert({ cache.filter, std::move(cache) });
		return p.first->second;
	}
}

void context::update_queries(archetype* g, bool add)
{
	auto match_cache = [&](query_cache& cache)
	{
		if (cache.includeClean < g->cleaning)
			return false;
		if (cache.includeDisabled < g->disabled)
			return false;
		return cache.filter.match(g->get_type());
	};
	for (auto i : queries)
	{
		auto& cache = i.second;
		if (match_cache(cache))
		{
			auto& gs = cache.archetypes;
			if (add)
				gs.erase(std::remove(gs.begin(), gs.end(), g), gs.end());
			else
				gs.push_back(g);
		}
	}
}

void context::remove(chunk*& h, chunk*& t, chunk* c)
{
	if (c == t)
		t = t->prev;
	if (h == c)
		h = h->next;
	c->unlink();
}


context::archetype* context::get_archetype(const entity_type& key)
{
	auto iter = archetypes.find(key);
	if (iter != archetypes.end())
		return iter->second;

	const uint16_t count = key.types.length;
	uint16_t firstTag = 0;
	uint16_t firstBuffer = 0;
	uint16_t metaCount = key.metatypes.length;
	uint16_t c = 0;
	for (c = 0; c < count; c++)
	{
		auto type = (tagged_index)key.types[c];
		if (type.is_tag()) break;
	}
	firstTag = c;
	for (c = 0; c < firstTag; c++)
	{
		auto type = (tagged_index)key.types[c];
		if (type.is_buffer()) break;
	}
	firstBuffer = c;
	void* data = malloc(archetype::calculate_size(count, firstTag, metaCount));
	archetype* g = (archetype*)data;
	g->componentCount = count;
	g->metaCount = metaCount;
	g->firstBuffer = firstBuffer;
	g->firstTag = firstTag;
	g->cleaning = false;
	g->disabled = false;
	g->withTracked = false;
	g->zerosize = false;
	g->size = 0;
	g->timestamp = timestamp;
	g->lastChunk = g->firstChunk = g->firstFree = nullptr;
	index_t* types = g->types();
	entity* metatypes = g->metatypes();
	memcpy(types, key.types.data, count * sizeof(index_t));
	memcpy(metatypes, key.metatypes.data, key.metatypes.length * sizeof(entity));

	const uint16_t disableType = disable_id;
	const uint16_t cleanupType = cleanup_id;
	
	uint16_t* sizes = g->sizes();
	uint16_t* offsets = g->offsets();
	stack_array(size_t, hash, firstTag);
	stack_array(uint16_t, stableOrder, firstTag);
	uint16_t entitySize = sizeof(entity);
	forloop(i, 0, firstTag)
	{
		auto type = (tagged_index)key.types[i];
		if (type == disableType)
			g->disabled = true;
		else if (type == cleanupType)
			g->cleaning = true;
		auto info = gd.infos[type.index()];
		sizes[i] = info.size;
		hash[i] = info.hash;
		stableOrder[i] = i;
		entitySize += info.size;

		if (gd.tracks[type.index()] & NeedCC != 0)
			g->withTracked = true;
	}
	if (entitySize == sizeof(entity)) 
		g->zerosize = true;
	g->chunkCapacity = (kChunkBufferSize - sizeof(uint16_t) * firstTag) / entitySize;
	std::sort(stableOrder, stableOrder + firstTag, [&](uint16_t lhs, uint16_t rhs)
		{
			return hash[lhs] < hash[rhs];
		});
	uint16_t offset = sizeof(entity) * g->chunkCapacity;
	forloop(i, 0, firstTag)
	{
		uint16_t id = stableOrder[i];
		offsets[id] = offset;
		offset += sizes[id] * g->chunkCapacity;
	}

	archetypes.insert({ g->get_type(), g });
	update_queries(g, true);
	return g;
}

context::archetype* context::get_cleaning(archetype* g)
{
	if (g->cleaning) return g;
	else if (!g->withTracked) return nullptr;

	uint16_t k = 0, count = g->componentCount;
	const uint16_t cleanupType = cleanup_id;
	index_t* types = g->types();
	entity* metatypes = g->metatypes();

	stack_array(index_t, dstTypes, count + 1);
	dstTypes[k++] = cleanupType;
	forloop(i, 0, count)
	{
		auto type = (tagged_index)types[i];
		auto stage = gd.tracks[type.index()];
		if(stage & NeedCleaning != 0)
			dstTypes[k++] = type;
	}
	if (k == 1)
		return nullptr;

	auto dstKey = entity_type
	{
		typeset {dstTypes, k},
		metaset {metatypes, g->metaCount}
	};

	return get_archetype(dstKey);
}

bool context::is_cleaned(const entity_type& type)
{
	return type.types.length == 1;
}

context::archetype* context::get_instatiation(archetype* g)
{
	if (g->cleaning) return nullptr;
	else if (!g->withTracked) return g;

	uint16_t count = g->componentCount;
	index_t* types = g->types();
	entity* metatypes = g->metatypes();

	stack_array(index_t, dstTypes, count);
	forloop(i, 0, count)
	{
		auto type = (tagged_index)types[i];
		auto stage = gd.tracks[type.index()];
		if (stage & Copying != 0)
			dstTypes[i] = type + 1;
		else
			dstTypes[i] = type;
	}

	auto dstKey = entity_type
	{
		typeset {dstTypes, count},
		metaset {metatypes, g->metaCount}
	};

	return get_archetype(dstKey);
}

context::archetype* context::get_extending(archetype* g, const entity_type& ext)
{
	if (g->cleaning)
		return nullptr;

	entity_type srcType = g->get_type();
	stack_array(index_t, newTypes, srcType.types.length + ext.types.length);
	stack_array(entity, newMetaTypes, srcType.metatypes.length + ext.metatypes.length);
	entity_type key = entity_type::merge(srcType, ext, newTypes, newMetaTypes);
	if (!g->withTracked)
		return get_archetype(key);
	else
	{
		int k = 0, mk = 0;
		stack_array(index_t, newTypesx, key.types.length);
		auto can_zip = [&](int i)
		{
			auto type = (tagged_index)newTypes[i];
			auto stage = gd.tracks[type.index()];
			if ((stage & NeedCopying != 0) && (newTypes[i + 1] == type + 1))
				return true;
			return false;
		};
		forloop(i, 0, key.types.length)
		{
			auto type = (tagged_index)newTypes[i];
			newTypesx[k++] = type;
			if (i < key.types.length - 1 && can_zip(i))
				i++;
		}
		auto dstKey = entity_type
		{
			typeset {newTypesx, k},
			key.metatypes
		};
		return get_archetype(dstKey);
	}
}

context::archetype* context::get_shrinking(archetype* g, const entity_type& shr)
{
	if (!g->withTracked)
	{
		entity_type srcType = g->get_type();
		stack_array(index_t, newTypes, srcType.types.length);
		stack_array(entity, newMetaTypes, srcType.metatypes.length);
		auto key = entity_type::substract(srcType, shr, newTypes, newMetaTypes);
		return get_archetype(key);
	}
	else
	{
		entity_type srcType = g->get_type();
		stack_array(index_t, shrTypes, srcType.types.length * 2);
		int k = 0;
		forloop(i, 0, shr.types.length)
		{
			auto type = (tagged_index)shr.types[i];
			shrTypes[k++] = type;
			auto stage = gd.tracks[type.index()];
			if(stage & NeedCopying != 0)
				shrTypes[k++] = type + 1;
		}
		stack_array(index_t, newTypes, srcType.types.length);
		stack_array(entity, newMetaTypes, srcType.metatypes.length);
		entity_type shrx{ { shrTypes, k }, shr.metatypes };
		auto key = entity_type::substract(srcType, shrx, newTypes, newMetaTypes);
		if (g->cleaning && is_cleaned(key))
			return nullptr;
		else
			return get_archetype(key);
	}
}

chunk* context::new_chunk(archetype* g)
{
	chunk* c = nullptr;
	if (poolSize == 0)
		c = (chunk*)malloc(kChunkSize);
	else
		c = chunkPool[--poolSize];

	c->count = 0;
	c->prev = c->next = nullptr;
	add_chunk(g, c);

	return c;
}

void context::add_chunk(archetype* g, chunk* c)
{
	structural_change(g, c, c->count);
	c->type = g;
	if (g->firstChunk == nullptr)
	{
		g->lastChunk = g->firstChunk = c;
		if (c->count < g->chunkCapacity)
			g->firstFree = c;
	}
	else if (c->count < g->chunkCapacity)
	{
		g->lastChunk->link(c);
		g->lastChunk = c;
		if (g->firstFree == nullptr)
			g->firstFree = c;
	}
	else
	{
		g->firstChunk->link(c);
	}
}

void context::remove_chunk(archetype* g, chunk* c)
{
	structural_change(g, c, -c->count);
	chunk* h = g->firstChunk;
	if (c == g->firstFree) 
		g->firstFree = c->next;
	remove(g->firstChunk, g->lastChunk, c);
	c->type = nullptr;
	if (g->firstChunk == nullptr)
	{
		release_reference(g);
		archetypes.erase(g->get_type());
		update_queries(g, false);
		::free(g);
	}
}

void context::mark_free(archetype* g, chunk* c)
{
	remove(g->firstChunk, g->lastChunk, c);
	g->lastChunk->link(c);
	g->lastChunk = c;
	if (g->firstFree == nullptr)
		g->firstFree = c;
}

void context::unmark_free(archetype* g, chunk* c)
{
	remove(g->firstFree, g->lastChunk, c);
	if (g->lastChunk == nullptr)
		g->lastChunk = c;
	if (c->next != g->firstFree)
		g->firstChunk->link(c);
}

void context::release_reference(archetype* g)
{
	//TODO
}

void context::serialize_archetype(archetype* g, serializer_i* s)
{
	entity_type type = g->get_type();
	uint16_t tlength = type.types.length, mlength = type.metatypes.length;
	s->stream(&tlength, sizeof(uint16_t));
	forloop(i, 0, tlength)
		s->stream(&gd.infos[tagged_index(type.types[i]).index()].hash, sizeof(size_t));
	s->stream(&mlength, sizeof(uint16_t));
	s->stream(type.metatypes.data, mlength * sizeof(entity));
}

context::archetype* context::deserialize_archetype(serializer_i* s)
{
	uint16_t tlength;
	stack_array(index_t, types, tlength);
	if (tlength == 0)
		return nullptr;
	forloop(i, 0, tlength)
	{
		size_t hash;
		s->stream(&hash, sizeof(size_t));
		//TODO: check validation
		types[i] = gd.hash2type[hash];
	}
	uint16_t mlength;
	s->stream(&mlength, sizeof(uint16_t));
	stack_array(entity, metatypes, mlength);
	s->stream(metatypes, mlength);
	//TODO patch
	std::sort(types, types + tlength);

	entity_type type = { {types, tlength}, {metatypes, mlength} };
	return get_archetype(type);
}

std::optional<chunk_slice> context::deserialize_slice(archetype* g, serializer_i* s)
{
	uint16_t count;
	s->stream(&count, sizeof(uint16_t));
	if (count == 0)
		return {};
	chunk* c;
	if (count == g->chunkCapacity)
		c = new_chunk(g);
	else
	{
		c = g->firstFree;
		while (c && c->count + count > g->chunkCapacity)
			c = c->next;
		if (c == nullptr)
			c = new_chunk(g);
	}
	uint16_t start = c->count;
	resize_chunk(c, start + count);
	chunk_slice slice = { c, start, count };
	chunk::deserialize(slice, s);
	return slice;
}

struct linear_patcher : patcher_i
{
	uint32_t start;
	entity* target;
	uint32_t count;
	entity patch(entity e) override;
};

void context::group_to_prefab(entity* src, uint32_t size, bool keepExternal)
{
	struct patcher : patcher_i
	{
		entity* source;
		uint32_t count;
		bool keepExternal;
		entity patch(entity e) override
		{
			forloop(i, 0, count)
				if (e == source[i])
					return entity{ i,-1 };
			return keepExternal?e : entity::Invalid;
		}
	} p;
	p.count = size;
	p.keepExternal = keepExternal;
	p.source = src;
	forloop(i, 0, size)
	{
		auto e = src[i];
		auto& data = ents.datas[e.id];
		chunk::patch({ data.c, data.i, 1 }, &p);
	}
}

void context::prefab_to_group(entity* members, uint32_t size)
{
	struct patcher : patcher_i
	{
		entity* source;
		int32_t count;
		entity patch(entity e) override
		{
			if (e.id > count || e.version != -1)
				return e;
			else
				return source[e.id];
		}
	} p;
	p.source = members;
	p.count = size;
	forloop(i, 0, size)
	{
		entity src = members[i];
		auto& data = ents.datas[src.id];
		chunk::patch({ data.c, data.i, 1 }, &p);
	}
}

void context::instantiate_prefab(entity* src, uint32_t size, entity* ret, uint32_t count)
{
	std::vector<entity> allEnts{ size * count };
	std::vector<chunk_slice> allSlices;
	allSlices.reserve(count);

	forloop(i, 0, size)
	{
		auto e = src[i];
		instantiate_single(e, &allEnts[i], count, &allSlices, size);
	}
	if (ret != nullptr)
		memcpy(ret, allEnts.data(), sizeof(entity) * count);

	struct patcher : patcher_i
	{
		entity* base;
		entity* curr;
		uint32_t count;
		void move() override { curr += 1; }
		void reset() override { base = curr; }
		entity patch(entity e) override
		{
			if (e.id > count || e.version != -1)
				return e;
			else
				return curr[e.id];
		}
	} p;
	p.count = size;
	uint32_t k = 0;
	//todo: multithread?
	for (auto& s : allSlices)
	{
		p.curr = p.base = &allEnts[(k % count) * size];
		chunk::patch(s, &p);
		k += s.count;
	}
}

void context::instantiate_single(entity src, entity* ret, uint32_t count, std::vector<chunk_slice>* slices, int32_t stride)
{
	const auto& data = ents.datas[src.id];
	archetype* g = get_instatiation(data.c->type);
	uint32_t k = 0;
	while (k < count)
	{
		chunk_slice s = allocate_slice(g, count - k);
		chunk::duplicate(s, data.c, data.i);
		ents.new_entities(s);
		if (ret != nullptr)
		{
			if (stride == 1)
				memcpy(ret + k, s.c->get_entities() + s.start, s.count * sizeof(entity));
			else
			{
				forloop(i, 0, s.count)
					ret[k * stride] = s.c->get_entities()[s.start + i];
			}
		}
		if(slices != nullptr)
			slices->push_back(s);
		k += s.count;
	}
}

void context::serialize_single(serializer_i* s, entity src)
{
	const auto& data = ents.datas[src.id];
	serialize_archetype(data.c->type, s);
	chunk::serialize({ data.c, data.i, 1 }, s);
}

void context::structural_change(archetype* g, chunk* c, int32_t count)
{
	entity_type t = g->get_type();
	
	g->size += count;
	if (g->timestamp != timestamp)
	{
		g->timestamp = timestamp;
		forloop(i, 0, t.types.length)
		{
			auto type = (tagged_index)t.types[i];
			typeTimestamps[type.index()] = timestamp;
		}
	}
	forloop(i, 0, t.types.length)
		g->timestamps(c)[i] = timestamp;
}

entity context::deserialize_single(serializer_i* s)
{
	auto *g = deserialize_archetype(s);
	auto slice = deserialize_slice(g, s);
	ents.new_entities(*slice);
	return slice->c->get_entities()[slice->start];
}

void context::destroy_single(chunk_slice s)
{
	archetype* g = s.c->type;
	if (g->cleaning)
		return;

	g = get_cleaning(g);
	if (g == nullptr)
	{
		ents.free_entities(s);
		free_slice(s);
	}
	else
	{
		cast_slice(s, g);
	}
}

void context::destroy_chunk(archetype* g, chunk* c)
{
	remove_chunk(g, c);
	if (poolSize < kChunkPoolCapacity)
		chunkPool[poolSize++] = c;
	else
		::free(c);
}

void context::resize_chunk(chunk* c, uint16_t count)
{
	archetype* g = c->type;
	if (count == 0)
		destroy_chunk(g, c);
	else
	{
		if (count == g->chunkCapacity)
			unmark_free(g, c);
		else if(c->count == g->chunkCapacity)
			mark_free(g, c);
		c->count = count;
	}
}

chunk_slice context::allocate_slice(archetype* g, uint32_t count)
{
	chunk* c = g->firstFree;
	if (c == nullptr)
		c = new_chunk(g);
	structural_change(g, c, count);
	uint16_t start = c->count;
	uint16_t allocated = std::min(count, uint32_t(g->chunkCapacity - start));
	resize_chunk(c, start + allocated);
	return { c, start, allocated };
}

void context::free_slice(chunk_slice s)
{
	archetype* g = s.c->type;
	structural_change(g, s.c, -s.count);
	uint16_t toMoveCount = std::min(s.count, uint16_t(s.c->count - s.start - s.count));
	if (toMoveCount > 0)
	{
		chunk_slice moveSlice{ s.c, s.start, toMoveCount };
		chunk::move(moveSlice, s.c->count - toMoveCount);
		ents.fill_entities(moveSlice, s.c->count - toMoveCount);
	}
	resize_chunk(s.c, s.c->count - s.count);
}

void context::cast_slice(chunk_slice src, archetype* g) 
{
	archetype* srcG = src.c->type;
	structural_change(srcG, src.c, -src.count);
	uint16_t k = 0;
	while (k < src.count)
	{
		chunk_slice s = allocate_slice(g, src.count - k);
		chunk::cast(s, src.c, src.start + k);
		ents.move_entities(s, src.c, src.start + k);
		k += s.count;
	}
	free_slice(src);
}

context::context(index_t typeCapacity)
{
	typeTimestamps = (uint32_t*)malloc(typeCapacity * sizeof(uint32_t));
	memset(typeTimestamps, 0, typeCapacity * sizeof(uint32_t));
}

context::~context()
{
	free(typeTimestamps);
	for (auto& g : archetypes)
	{
		chunk* c = g.second->firstChunk;
		while (c != nullptr)
		{
			chunk* next = c->next;
			chunk::destruct({ c,0,c->count });
			free(c);
			c = next;
		}
		release_reference(g.second);
		free(g.second);
	}
	forloop(i, 0, poolSize)
		free(chunkPool[i]);
}

context::alloc_iterator context::allocate(const entity_type& type, entity* ret, uint32_t count)
{
	alloc_iterator i;
	i.ret = ret;
	i.count = count;
	i.cont = this;
	i.g = get_archetype(type);
	i.k = 0;
	return i;
}

void context::instantiate(entity src, entity* ret, uint32_t count)
{
	auto group_data = (buffer*)get_component_ro(src, group_id);
	if (group_data == nullptr)
		instantiate_single(src, ret, count);
	else
	{
		uint32_t size = group_data->size / sizeof(entity);
		stack_array(entity, members, size);
		memcpy(members, group_data->data(), group_data->size);
		group_to_prefab(members, size);
		instantiate_prefab(members, size, ret, count);
		prefab_to_group(members, size);
	}
}

context::batch_iterator context::batch(entity* ents, uint32_t count)
{
	return batch_iterator{ ents,count,this,0 };
}

entity_filter context::cache_query(const entity_filter& type)
{
	auto& cache = get_query_cache(type);
	return cache.filter;
}

void context::destroy(chunk_slice s)
{
	archetype* g = s.c->type;
	uint16_t id = g->index(group_id);
	if (id != -1)
	{
		uint16_t* sizes = g->sizes();
		char* src = (s.c->data() + g->offsets()[id]);
		forloop(i, 0, s.count)
		{
			auto* group_data = (buffer*)(src + i * sizes[i]);
			uint16_t size = group_data->size / sizeof(entity);
			forloop(j, 1, size)
			{
				entity e = ((entity*)group_data->data())[i];
				auto& data = ents.datas[i];
				destroy_single({ data.c, data.i, 1 });
			}
		}
	}
	destroy_single(s);
}

void context::extend(chunk_slice s, const entity_type& type)
{
	archetype* g = get_extending(s.c->type, type);
	if (g == nullptr)
		return;
	else
		cast(s, g);
}

void context::shrink(chunk_slice s, const entity_type& type)
{

	archetype* g = get_shrinking(s.c->type, type);
	if(g == nullptr)
		free_slice(s);
	else
		cast(s, g);
}

bool static_castable(const entity_type& typeA, const entity_type& typeB)
{
	uint16_t srcI = 0, dstI = 0;
	while (srcI < typeA.types.length && dstI < typeB.types.length)
	{
		tagged_index st = typeA.types[srcI];
		tagged_index dt = typeB.types[dstI];
		if (st.is_tag() || dt.is_tag())
			return true;
		if (to_valid_type(st) != to_valid_type(dt))
			return false;
		else if (st != dt)
			return false;
	}
	return true;
}

void context::cast(chunk_slice s, const entity_type& type)
{
	archetype* g = get_archetype(type);
	cast(s, g);
}

void context::cast(chunk_slice s, archetype* g)
{
	archetype* srcG = s.c->type;
	entity_type srcT = srcG->get_type();
	if (srcG == g)
		return;
	else if (s.full() && static_castable(srcT, g->get_type()))
	{
		remove_chunk(srcG, s.c);
		add_chunk(g, s.c);
	}
	else
		cast_slice(s, g);
}

#define foriter(c, iter) for (auto c = iter.next(); c.has_value(); c = iter.next())
void context::destroy(entity* es, int32_t count)
{
	auto iter = batch(es, count);
	foriter(s, iter)
		destroy(*s);
}

void context::extend(entity* es, int32_t count, const entity_type& type)
{
	auto iter = batch(es, count);
	foriter(s, iter)
		extend(*s, type);
}

void context::shrink(entity* es, int32_t count, const entity_type& type)
{
	auto iter = batch(es, count);
	foriter(s, iter)
		shrink(*s, type);
}

void context::cast(entity* es, int32_t count, const entity_type& type)
{
	auto iter = batch(es, count);
	foriter(s, iter)
		cast(*s, type);
}

const void* context::get_component_ro(entity e, index_t type) const
{
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	uint16_t id = g->index(type);
	if (id == -1)
		return get_shared_ro(g, type);
	return c->data() + g->offsets()[id] + data.i * g->sizes()[id];
}

const void* context::get_owned_ro(entity e, index_t type) const
{
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	uint16_t id = g->index(type);
	if (id == -1) return nullptr;
	return c->data() + g->offsets()[id] + data.i * g->sizes()[id];
}

const void* context::get_shared_ro(entity e, index_t type) const
{
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	return get_shared_ro(g, type);
}

void* context::get_owned_rw(entity e, index_t type)
{
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	uint16_t id = g->index(type);
	if (id == -1) return nullptr;
	g->timestamps(c)[id] = timestamp;
	return c->data() + g->offsets()[id] + data.i * g->sizes()[id];
}

const void* context::get_owned_ro(chunk* c, index_t t) const noexcept
{
	uint16_t id = c->type->index(t);
	if (id == -1) return nullptr;
	return c->data() + c->type->offsets()[id];
}

void* context::get_owned_rw(chunk* c, index_t t) noexcept
{
	uint16_t id = c->type->index(t);
	if (id == -1) return nullptr;
	c->type->timestamps(c)[id] = timestamp;
	return c->data() + c->type->offsets()[id];
}

const void* context::get_shared_ro(archetype* g, index_t type) const
{
	entity* metas = g->metatypes();
	forloop(i, 0, g->metaCount)
		if (const void* shared = get_component_ro(metas[i], type))
			return shared;
	return nullptr;
}

const entity* context::get_entities(chunk* c) noexcept
{
	return c->get_entities();
}

uint16_t context::get_size(chunk* c, index_t t) const noexcept
{
	uint16_t id = c->type->index(t);
	if (id == -1) return 0;
	return c->type->sizes()[id];
}

entity_type context::get_type(entity e) const noexcept
{
	const auto& data = ents.datas[e.id];
	return data.c->type->get_type();
}

void context::serialize(serializer_i* s, entity src)
{
	auto group_data = (buffer*)get_component_ro(src, group_id);
	if (group_data == nullptr)
		serialize_single(s, src);
	else
	{
		uint32_t size = group_data->size / sizeof(entity);
		stack_array(entity, members, size);
		memcpy(members, group_data->data(), group_data->size);
		group_to_prefab(members, size, false);
		forloop(i, 0, size)
		{
			src = members[i];
			serialize_single(s, src);
		}
		prefab_to_group(members, size);
	}
}

void context::deserialize(serializer_i* s, entity* ret, uint32_t times)
{
	entity src = deserialize_single(s);
	auto group_data = (buffer*)get_component_ro(src, group_id);
	if (ret != nullptr)
		ret[0] = src;
	if (group_data == nullptr)
	{
		if(times > 1)
			instantiate_single(src, ret + 1, times - 1);
	}
	else
	{
		uint32_t size = group_data->size / sizeof(entity);
		stack_array(entity, members, size);
		members[0] = src;
		forloop(i, 1, size)
		{
			members[i] = deserialize_single(s);
		}
		if (times > 1)
			instantiate_prefab(members, size, ret + 1, times - 1);
		prefab_to_group(members, size);
	}
}

void context::move_context(context& src)
{
	auto& sents = src.ents;
	uint32_t count = sents.size;
	entity* patch = new entity[count];
	forloop(i, 0, count)
		if (sents.datas[i].c != nullptr)
			patch[i] = ents.new_entity();

	linear_patcher p;
	p.start = 0;
	p.target = patch;
	p.count = count;
	//todo: multithread?
	for (auto& pair : src.archetypes)
	{
		archetype* g = pair.second;
		archetype* dstG = get_archetype(g->get_type());
		for (chunk* c = g->firstChunk; c;)
		{
			chunk* next = c->next;
			add_chunk(dstG, c);
			c = next;
			patch_chunk(c, &p);
		}
		release_reference(g);
		free(g);
	}
	src.archetypes.clear();

	sents.size = sents.free = sents.capacity = 0;
	::free(sents.datas);
	sents.datas = nullptr;
	::free(patch);
}

void context::patch_chunk(chunk* c, patcher_i* patcher)
{
	entity* es = (entity*)c->data();
	forloop(i, 0, c->count)
		es[i] = patcher->patch(es[i]);
	chunk::patch(c, patcher);
}

/*
struct Data
{
	struct Section
	{
		struct entity_type type[1];
		struct chunk_slice datas[n];
	};
	Section sections[m];
};
*/


void context::serialize(serializer_i* s)
{
	const uint32_t start = 0;
	s->stream(&start, sizeof(uint32_t));
	s->stream(&ents.size, sizeof(uint32_t));
	
	//hack: save entity validation instead of patch all data
	//trade space for time
	auto size = ents.size / 4 + 1;
	bool needFree = false;
	char* bitset;
	if (size < 1024 * 4)
		bitset = (char*)alloca(size);
	else
	{
		needFree = true;
		bitset = (char*)malloc(size);
	}
	memset(bitset, 0, size);

	forloop(i, 0, ents.size)
		if (ents.datas[i].c != nullptr)
		{
			bitset[i/4] |= (1<<i%4);
		}
	s->stream(bitset, size);

	for (auto& pair : archetypes)
	{
		archetype* g = pair.second;
		serialize_archetype(g, s);

		for (chunk* c = g->firstChunk; c; c= c->next)
			chunk::serialize({ c, 0, c->count }, s);

		s->stream(0, sizeof(uint16_t));
	}
	s->stream(0, sizeof(uint16_t));

	if(needFree)
		::free(bitset);
}

entity linear_patcher::patch(entity e)
{
	patch_entity(e, start, target, count);
}

void context::deserialize(serializer_i* s, entity* ret)
{
	uint32_t start, count;
	s->stream(&start, sizeof(uint32_t));
	s->stream(&count, sizeof(uint32_t));
	auto size = count / 4 + 1;

	bool needFree = false, needFreeB = false;
	entity* patch;
	if (ret != nullptr)
		patch = ret;
	else if (count * sizeof(entity) <= 1024 * 4)
		patch = (entity*)alloca(count * sizeof(entity));
	else
	{
		needFree = true;
		patch = (entity*)malloc(count * sizeof(entity));
	}

	char* bitset;
	if (size < 1024 * 4)
		bitset = (char*)alloca(size);
	else
	{
		needFreeB = true;
		bitset = (char*)malloc(size);
	}
	s->stream(bitset, size);

	forloop(i, 0, count)
		if (bitset[i / 4] & (1 << (i % 4)) != 0)
			patch[i] = ents.new_entity();
		else
			patch[i] = entity::Invalid;
	linear_patcher patcher;
	patcher.start = start;
	patcher.target = patch;
	patcher.count = count;

	//todo: multithread?
	for(archetype* g = deserialize_archetype(s); g!=nullptr; g = deserialize_archetype(s))
		for(auto slice = deserialize_slice(g, s); slice; slice = deserialize_slice(g, s))
			chunk::patch(*slice, &patcher);

	if (needFree)
		::free(patch);
	if (needFreeB)
		::free(bitset);
}

bool context::has_component(entity e, index_t t) const
{
	const auto& data = ents.datas[e.id];
	return data.c->type->index(t) != -1;
}

bool context::exist(entity e) const
{
	return e.id < ents.size && e.version == ents.datas[e.id].v;
}

std::optional<chunk_slice> context::batch_iterator::next()
{
	if (i >= count) return {};
	const auto* datas = cont->ents.datas;
	const auto& start = datas[ents[i++].id];
	chunk_slice s{ start.c, (uint16_t)start.i, 1 };
	while (i < count)
	{
		const auto& curr = datas[ents[i].id];
		if (curr.i != start.i + s.count || curr.c != start.c)
			break;
		i++; s.count++;
	}
	return { s };
}

context::entities::~entities()
{
	::free(datas);
}

void context::entities::new_entities(chunk_slice s)
{
	entity* dst = (entity*)s.c->data() + s.start;
	forloop(i, 0, s.count)
	{
		entity newE = new_entity();
		dst[i] = newE;
		datas[newE.id].c = s.c;
		datas[newE.id].i = s.start + i;
	}
}

entity context::entities::new_prefab()
{
	uint32_t id;
	if (size == capacity)
	{
		uint32_t newCap = capacity == 0 ? 5 : capacity * 2;
		data* newDatas = (data*)malloc(sizeof(data) * newCap);
		if (datas != nullptr)
		{
			memcpy(newDatas, datas, sizeof(data) * capacity);
			::free(datas);
		}
		datas = newDatas;
		memset(datas + capacity, 0, (newCap - capacity) * sizeof(data));
		capacity = newCap;
	}
	id = size++;
	return { id, datas[id].v };
}

entity context::entities::new_entity()
{
	if (free == 0)
		return new_prefab();
	else
	{
		uint32_t id = free;
		free = datas[free].i;
		return { id, datas[id].v };
	}
}

void context::entities::free_entities(chunk_slice s)
{
	entity* toFree = (entity*)s.c->data() + s.start;
	forloop(i, 0, s.count - 1)
	{
		data& freeData = datas[toFree[i].id];
		freeData = { nullptr, toFree[i + 1].id, freeData.v + 1 };
	}
	data& freeData = datas[toFree[s.count - 1].id];
	freeData = { nullptr, free, freeData.v + 1 };
	free = toFree[0].id;
	//shrink
	while (size > 0 && datas[--size].c == nullptr);
	size += (datas[size].c != nullptr);
}

void context::entities::move_entities(chunk_slice dst, const chunk* src, uint16_t srcIndex)
{
	const entity* toMove = (entity*)src->data() + srcIndex;
	forloop(i, 0, dst.count)
	{
		data& d = datas[toMove[i].id];
		d.i = dst.start + i;
		d.c = dst.c;
	}
	memcpy((entity*)dst.c->data() + dst.start, toMove, dst.count * sizeof(entity));
}

void context::entities::fill_entities(chunk_slice dst, uint16_t srcIndex)
{
	const entity* toMove = (entity*)dst.c->data() + srcIndex;
	forloop(i, 0, dst.count)
		datas[toMove[i].id].i = dst.start + i;
	memcpy((entity*)dst.c->data() + dst.start, toMove, dst.count * sizeof(entity));
}

std::optional<chunk_slice> context::alloc_iterator::next()
{
	if (k < count)
	{
		chunk_slice s = cont->allocate_slice(g, count - k);
		chunk::construct(s);
		cont->ents.new_entities(s);
		memcpy(ret + k, s.c->get_entities() + s.start, s.count * sizeof(entity));
		k += s.count;
		return s;
	}
	else
		return {};
}