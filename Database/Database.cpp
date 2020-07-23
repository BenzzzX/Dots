#include "Database.h"

#define forloop(i, z, n) for(auto i = decltype(n)(z); i<n; ++i)

#define stack_array(type, name, size) \
type* name = (type*)alloca((size) * sizeof(type))


using namespace core;
using namespace database;

core::entity core::entity::Invalid{ (uint32_t)-1 };
uint32_t database::metaTimestamp;
constexpr uint16_t InvalidIndex = (uint16_t)-1;

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
index_t core::database::group_id = 2;
index_t core::database::mask_id = 3;

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
		desc.size = sizeof(mask);
		desc.hash = 3;
		desc.isElement = false;
		desc.name = "mask";
		desc.entityRefCount = 0;
		mask_id = register_type(desc);
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
		rid = (uint32_t)gd.entityRefs.size();
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

	if (desc.need_copy)
	{
		uint16_t id = (uint16_t)gd.infos.size();
		id = tagged_index{ id, desc.isElement, desc.size == 0 };
		type_data i{ desc.hash, desc.size, desc.elementSize, rid, desc.entityRefCount, desc.name, desc.vtable };
		gd.tracks.push_back(Copying);
		gd.infos.push_back(i);
	}
	
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
	if (id == InvalidIndex) return 0;
	return type->timestamps(this)[id];
}

void chunk::move(chunk_slice dst, uint16_t srcIndex) noexcept
{
	chunk* src = dst.c;
	uint16_t* offsets = src->type->offsets(src->ct);
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
	archetype* type = s.c->type;
	uint16_t* offsets = type->offsets(s.c->ct);
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

	uint16_t maskId = type->index(mask_id);
	if (maskId != (uint16_t)-1)
	{
		auto i = maskId;
		memset(srcData, -1, sizes[i] * s.count);
	}
}

void chunk::destruct(chunk_slice s) noexcept
{
	archetype* type = s.c->type;
	uint16_t* offsets = type->offsets(s.c->ct);
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
	archetype* type = src->type;
	archetype *dstType = dst.c->type;
	uint16_t dstI = 0;
	uint16_t* offsets = type->offsets(src->ct);
	uint16_t* dstOffsets = dstType->offsets(dst.c->ct);
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
	archetype* g = s.c->type;
	uint16_t* offsets = g->offsets(s.c->ct);
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
	archetype* type = s.c->type;
	uint16_t* offsets = type->offsets(s.c->ct);
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

size_t chunk::get_size()
{
	switch (ct)
	{
	case chunk_type::fast:
		return kFastBinSize;
	case chunk_type::small:
		return kSmallBinSize;
	case chunk_type::large:
		return kLargeBinSize;
	}
}

void chunk::cast(chunk_slice dst, chunk* src, uint16_t srcIndex) noexcept
{
	archetype* srcType = src->type;
	archetype* dstType = dst.c->type;
	uint16_t dstI = 0;
	uint16_t srcI = 0;
	uint16_t count = dst.count;
	uint16_t* srcOffsets = srcType->offsets(src->ct);
	uint16_t* dstOffsets = dstType->offsets(dst.c->ct);
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

	uint16_t srcMaskId = srcType->index(mask_id);
	uint16_t dstMaskId = dstType->index(mask_id);
	if (srcMaskId != InvalidIndex && dstMaskId != InvalidIndex)
	{
		mask* s = (mask*)(src->data() + srcOffsets[srcMaskId] + srcSizes[srcMaskId] * srcIndex);
		mask* d = (mask*)(dst.c->data() + dstOffsets[dstMaskId] + dstSizes[dstMaskId] * dst.start);
		forloop(i, 0, count)
		{
			srcI = dstI = 0;
			d[i].reset();
			while (srcI < srcType->componentCount && dstI < dstType->componentCount)
			{
				auto st = to_valid_type(srcTypes[srcI]);
				auto dt = to_valid_type(dstTypes[dstI]);
				if (st < dt) //destruct 
					;
				else if (st > dt) //construct
					d[i].set(dstI);
				else //move
					if(s[i].test(srcI))
						d[i].set(dstI);
			}
			while (dstI < dstType->componentCount)
				d[i].set(dstI);
		}
	}
	else if (dstMaskId != InvalidIndex)
	{
		mask* d = (mask*)(dst.c->data() + dstOffsets[dstMaskId] + dstSizes[dstMaskId] * dst.start);
		memset(d, -1, dstSizes[dstMaskId] * count);
	}
}

inline uint32_t* archetype::timestamps(chunk* c) noexcept { return (uint32_t*)(c->data() + c->get_size()) - firstTag; }

uint16_t archetype::index(index_t type) noexcept
{
	index_t* ts = types();
	index_t* result = std::lower_bound(ts, ts + componentCount, type);
	if (result != ts + componentCount && *result == type)
		return uint16_t(result - ts);
	else
		return uint16_t(-1);
}

mask archetype::get_mask(const typeset& subtype) noexcept
{
	mask ret;
	auto ta = get_type().types;
	auto tb = subtype;
	uint16_t i = 0, j = 0;
	while (i < ta.length && j < tb.length)
	{
		if (ta[i] > tb[j])
		{
			j++;
			//ret.v.reset();
			//break;
		}
		else if (ta[i] < tb[j])
		{
			i++;
		}
		else
		{
			ret.set(i);
			(j++, i++);
		}
	}
	return ret;
}

entity_type archetype::get_type()
{
	index_t* ts = types();
	entity* ms = metatypes();
	return entity_type
	{
		typeset { ts, componentCount },
		metaset { ms, metaCount }
	};

}

size_t archetype::calculate_size(uint16_t componentCount, uint16_t firstTag, uint16_t metaCount)
{
	return sizeof(index_t)* componentCount +
		sizeof(uint16_t) * firstTag +
		sizeof(uint16_t) * firstTag * 3 +
		sizeof(entity) * metaCount +
		sizeof(archetype);// +40;
}

uint16_t get_filter_size(const archetype_filter& f)
{
	auto totalSize = f.all.types.length * sizeof(index_t) + f.all.metatypes.length * sizeof(entity) +
		f.any.types.length * sizeof(index_t) + f.any.metatypes.length * sizeof(entity) +
		f.none.types.length * sizeof(index_t) + f.none.metatypes.length * sizeof(entity);
	return (uint16_t)totalSize;
}

archetype_filter clone_filter(const archetype_filter& f, char* data)
{
	archetype_filter f2;
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
	auto writetype = [&](const entity_type& t, entity_type& r)
	{
		write(t.types, r.types);
		writemeta(t.metatypes, r.metatypes);
	};
	writetype(f.all, f2.all);
	writetype(f.any, f2.any);
	writetype(f.none, f2.none);
	return f2;
}

context::query_cache& context::get_query_cache(const archetype_filter& f)
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
				if ((gd.tracks[type.index()] & Copying) != 0)
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


			uint16_t size;
			estimate_shared_size(size, g);
			stack_array(index_t, _type, size);
			stack_array(index_t, _buffer, size);
			typeset type{ _type, 0 };
			typeset buffer{ _buffer ,0 };
			get_shared_type(type, g, buffer);

			return f.match(g->get_type(), type);
		};
		for (auto i : archetypes)
		{
			auto g = i.second;
			if (valid_group(g))
			{
				auto type = g->get_type();
				auto cc = f.all.types.length + f.any.types.length;
				stack_array(index_t, cd, cc);
				auto checking = typeset::merge(f.all.types, f.any.types, cd);
				mask m = g->get_mask( checking );
				cache.archetypes.push_back({ .type = g, .matched = m });
			}
		}
		cache.includeClean = includeClean;
		cache.includeDisabled = includeDisabled;
		auto totalSize = get_filter_size(f);
		cache.data.reset(new char[totalSize]);
		char* data = cache.data.get();
		cache.filter = clone_filter(f, data);
		auto p = queries.insert({ cache.filter, std::move(cache) });
		return p.first->second;
	}
}

void context::update_queries(archetype* g, bool add)
{
	uint16_t size;
	estimate_shared_size(size, g);
	stack_array(index_t, _type, size);
	stack_array(index_t, _buffer, size);
	typeset type{ _type, 0 };
	typeset buffer{ _buffer ,0 };
	get_shared_type(type, g, buffer);
	auto match_cache = [&](query_cache& cache)
	{
		if (cache.includeClean < g->cleaning)
			return false;
		if (cache.includeDisabled < g->disabled)
			return false;
		return cache.filter.match(g->get_type(), type);
	};
	for (auto& i : queries)
	{
		auto& cache = i.second;
		if (match_cache(cache))
		{
			auto& gs = cache.archetypes;
			if (add)
			{
				int i = 0;
				for (; gs[i].type != g; ++i);
				if (i != (gs.size() - 1))
					std::swap(gs[i], gs[gs.size() - 1]);
				gs.pop_back();
			}
			else
				gs.push_back({ .type = g });
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


archetype* context::get_archetype(const entity_type& key)
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
	g->withMask = false;
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
	const uint16_t maskType = mask_id;

	uint16_t* sizes = g->sizes();
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
		else if (type == maskType)
			g->withMask = true;
		auto info = gd.infos[type.index()];
		sizes[i] = info.size;
		hash[i] = info.hash;
		stableOrder[i] = i;
		entitySize += info.size;

		if ((gd.tracks[type.index()] & NeedCC) != 0)
			g->withTracked = true;
	}
	if (entitySize == sizeof(entity))
		g->zerosize = true;
	size_t Caps[] = {kFastBinSize, kSmallBinSize, kLargeBinSize};
	std::sort(stableOrder, stableOrder + firstTag, [&](uint16_t lhs, uint16_t rhs)
		{
			return hash[lhs] < hash[rhs];
		});
	forloop(i, 0, 3)
	{
		uint16_t* offsets = g->offsets((chunk_type)i);
		g->chunkCapacity[i] = (Caps[i] - sizeof(uint16_t) * firstTag) / entitySize;
		if (g->chunkCapacity[i] == 0)
			continue;
		uint16_t offset = sizeof(entity) * g->chunkCapacity[i];
		forloop(i, 0, firstTag)
		{
			uint16_t id = stableOrder[i];
			offsets[id] = offset;
			offset += sizes[id] * g->chunkCapacity[i];
		}
	}
	

	archetypes.insert({ g->get_type(), g });
	update_queries(g, true);
	return g;
}

archetype* context::get_cleaning(archetype* g)
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
		if((stage & NeedCleaning) != 0)
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

archetype* context::get_instatiation(archetype* g)
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
		if ((stage & NeedCopying) != 0)
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

archetype* context::get_extending(archetype* g, const entity_type& ext)
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
		uint16_t k = 0, mk = 0;
		stack_array(index_t, newTypesx, key.types.length);
		auto can_zip = [&](int i)
		{
			auto type = (tagged_index)newTypes[i];
			auto stage = gd.tracks[type.index()];
			if (((stage & NeedCopying) != 0) && (newTypes[i + 1] == type + 1))
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
			{newTypesx, k},
			key.metatypes
		};
		return get_archetype(dstKey);
	}
}

archetype* context::get_shrinking(archetype* g, const entity_type& shr)
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
		uint16_t k = 0;
		forloop(i, 0, shr.types.length)
		{
			auto type = (tagged_index)shr.types[i];
			shrTypes[k++] = type;
			auto stage = gd.tracks[type.index()];
			if((stage & NeedCopying) != 0)
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

chunk* context::malloc_chunk(chunk_type type)
{
	chunk* c = nullptr;
	switch (type)
	{
	case chunk_type::fast:
		if (fastbinSize == 0)
			c = (chunk*)malloc(kFastBinSize);
		else
			c = fastbin[--fastbinSize];
		break;
	case chunk_type::small:
		if (smallbinSize == 0)
			c = (chunk*)malloc(kSmallBinSize);
		else
			c = smallbin[--smallbinSize];
		break;
	case chunk_type::large:
		if (largebinSize == 0)
			c = (chunk*)malloc(kLargeBinSize);
		else
			c = largebin[--largebinSize];
		break;
	}
	c->ct = type;
	return c;
}

chunk* context::new_chunk(archetype* g, uint32_t hint)
{
	chunk* c = nullptr;
	if (g->chunkCount < kSmallBinThreshold && hint < g->chunkCapacity[1])
		c = malloc_chunk(chunk_type::small);
	else if (hint > g->chunkCapacity[0] * 8)
		c = malloc_chunk(chunk_type::large);
	else
		c = malloc_chunk(chunk_type::fast);
	c->count = 0;
	c->prev = c->next = nullptr;
	add_chunk(g, c);

	return c;
}

void context::add_chunk(archetype* g, chunk* c)
{
	structural_change(g, c, c->count);
	c->type = g;
	g->chunkCount++;
	if (g->firstChunk == nullptr)
	{
		g->lastChunk = g->firstChunk = c;
		if (c->count < g->chunkCapacity[(int)c->ct])
			g->firstFree = c;
	}
	else if (c->count < g->chunkCapacity[(int)c->ct])
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
	g->chunkCount--;
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
	stack_array(entity, metatypes, mlength);
	s->stream(metatypes, mlength * sizeof(entity));
}

archetype* context::deserialize_archetype(serializer_i* s, patcher_i* patcher)
{
	uint16_t tlength; 
	s->stream(&tlength, sizeof(uint16_t));
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
	forloop(i, 0, mlength)
		metatypes[i] = patcher->patch(metatypes[i]);
	std::sort(types, types + tlength);
	std::sort(metatypes, metatypes + mlength);
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
	c = g->firstFree;
	while (c && c->count + count > g->chunkCapacity[(int)c->ct])
		c = c->next;
	if (c == nullptr)
		c = new_chunk(g, count);
	uint16_t start = c->count;
	resize_chunk(c, start + count);
	chunk_slice slice = { c, start, count };
	chunk::serialize(slice, s);
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
	//TODO: should we patch meta?
	struct patcher : patcher_i
	{
		entity* source;
		uint32_t count;
		bool keepExternal;
		entity patch(entity e) override
		{
			forloop(i, 0, count)
				if (e == source[i])
					return entity{ i,(uint32_t)-1 };
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
		uint32_t count;
		entity patch(entity e) override
		{
			if (e.id > count || !e.is_transient())
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
			if (e.id > count || !e.is_transient())
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

entity context::deserialize_single(serializer_i* s, patcher_i* patcher)
{
	auto *g = deserialize_archetype(s, patcher);
	auto slice = deserialize_slice(g, s);
	ents.new_entities(*slice);
	chunk::patch(*slice, patcher);
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
	recycle_chunk(c);
}

void context::recycle_chunk(chunk* c)
{
	switch (c->ct)
	{
	case chunk_type::fast:
		if (fastbinSize < kLargeBinCapacity)
			fastbin[fastbinSize++] = c;
		else
			::free(c);
		break;
	case chunk_type::small:
		if (smallbinSize < kSmallBinCapacity)
			smallbin[smallbinSize++] = c;
		else
			::free(c);
		break;
	case chunk_type::large:
		if (largebinSize < kLargeBinCapacity)
			largebin[largebinSize++] = c;
		else
			::free(c);
		break;
	}
}

void context::resize_chunk(chunk* c, uint16_t count)
{
	archetype* g = c->type;
	if (count == 0)
		destroy_chunk(g, c);
	else
	{
		if (count == g->chunkCapacity[(int)c->ct])
			unmark_free(g, c);
		else if(c->count == g->chunkCapacity[(int)c->ct])
			mark_free(g, c);
		c->count = count;
	}
}

chunk_slice context::allocate_slice(archetype* g, uint32_t count)
{
	chunk* c = g->firstFree;
	if (c == nullptr)
		c = new_chunk(g, count);
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
	:typeCapacity(typeCapacity)
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
	forloop(i, 0, fastbinSize)
		free(fastbin[i]);
	forloop(i, 0, smallbinSize)
		free(smallbin[i]);
	forloop(i, 0, largebinSize)
		free(largebin[i]);
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

archetype_filter context::cache_query(const archetype_filter& type)
{
	auto& cache = get_query_cache(type);
	return cache.filter;
}

void context::estimate_shared_size(uint16_t& size, archetype* t) const
{
	entity* metas = t->metatypes();
	forloop(i, 0, t->metaCount)
	{
		auto g = get_archetype(metas[i]);
		size += g->componentCount;
		estimate_shared_size(size, g);
	}
}

void context::get_shared_type(typeset& type, archetype* t, typeset& buffer) const
{
	entity* metas = t->metatypes();
	forloop(i, 0, t->metaCount)
	{
		auto g = get_archetype(metas[i]);
		std::swap(type, buffer);
		type = typeset::merge(g->get_type().types, buffer, (index_t*)type.data);
		get_shared_type(type, g, buffer);
	}
}

void context::destroy(chunk_slice s)
{
	archetype* g = s.c->type;
	uint16_t id = g->index(group_id);
	if (id != InvalidIndex)
	{
		uint16_t* sizes = g->sizes();
		char* src = (s.c->data() + g->offsets(s.c->ct)[id]);
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
	if (g == nullptr)
	{
		ents.free_entities(s);
		free_slice(s);
	}
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

void context::extend(archetype* t, const entity_type& type)
{
	archetype* g = get_extending(t, type);
	if (g == nullptr)
		return;
	else
		for (auto c = t->firstChunk; c != nullptr; c = t->firstChunk)
			cast(c, g);
}

void context::shrink(archetype* t, const entity_type& type)
{
	archetype* g = get_shrinking(t, type);
	if (g == nullptr)
		for (auto c = t->firstChunk; c != nullptr; c = t->firstChunk)
		{
			ents.free_entities(c);
			destroy_chunk(t, c);
		}
	else
		for (auto c = t->firstChunk; c != nullptr; c = t->firstChunk)
			cast(c, g);
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

const void* context::get_component_ro(entity e, index_t type) const noexcept
{
	if (!exist(e))
		return nullptr;
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	uint16_t id = g->index(type);
	if (id == InvalidIndex)
		return get_shared_ro(g, type);
	return c->data() + g->offsets(c->ct)[id] + data.i * g->sizes()[id];
}

const void* context::get_owned_ro(entity e, index_t type) const noexcept
{
	if (!exist(e))
		return nullptr;
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	uint16_t id = g->index(type);
	if (id == InvalidIndex) return nullptr;
	return c->data() + g->offsets(c->ct)[id] + data.i * g->sizes()[id];
}

const void* context::get_shared_ro(entity e, index_t type) const noexcept
{
	if (!exist(e))
		return nullptr;
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	return get_shared_ro(g, type);
}

void* context::get_owned_rw(entity e, index_t type) const noexcept
{
	if (!exist(e))
		return nullptr;
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	uint16_t id = g->index(type);
	if (id == InvalidIndex) return nullptr;
	g->timestamps(c)[id] = timestamp;
	return c->data() + g->offsets(c->ct)[id] + data.i * g->sizes()[id];
}


const void* context::get_component_ro(chunk* c, index_t t) const noexcept
{
	archetype* g = c->type;
	uint16_t id = g->index(t);
	if (id == InvalidIndex) 
		return get_shared_ro(g, t);
	return c->data() + c->type->offsets(c->ct)[id];
}

const void* context::get_owned_ro(chunk* c, index_t t) const noexcept
{
	uint16_t id = c->type->index(t);
	if (id == InvalidIndex) return nullptr;
	return c->data() + c->type->offsets(c->ct)[id];
}

const void* context::get_shared_ro(chunk* c, index_t type) const noexcept
{
	archetype* g = c->type;
	return get_shared_ro(g, type);
}

void* context::get_owned_rw(chunk* c, index_t t) noexcept
{
	uint16_t id = c->type->index(t);
	if (id == InvalidIndex) return nullptr;
	c->type->timestamps(c)[id] = timestamp;
	return c->data() + c->type->offsets(c->ct)[id];
}

const void* context::get_shared_ro(archetype* g, index_t type) const
{
	entity* metas = g->metatypes();
	forloop(i, 0, g->metaCount)
		if (const void* shared = get_component_ro(metas[i], type))
			return shared;
	return nullptr;
}

bool context::share_component(archetype* g, const typeset& type) const
{
	entity* metas = g->metatypes();
	forloop(i, 0, g->metaCount)
		if (has_component(metas[i], type))
			return true;
	return false;
}

bool context::own_component(archetype* g, const typeset& type) const
{
	return g->get_type().types.all(type);
}

bool context::has_component(archetype* g, const typeset& type) const
{
	if (own_component(g, type))
		return true;
	else
		return share_component(g, type);
}

archetype* context::get_archetype(entity e) const noexcept
{
	if (!exist(e))
		return nullptr;
	const auto& data = ents.datas[e.id];
	return data.c->type;
}

const entity* context::get_entities(chunk* c) noexcept
{
	return c->get_entities();
}

uint16_t context::get_size(chunk* c, index_t t) const noexcept
{
	uint16_t id = c->type->index(t);
	if (id == InvalidIndex) return 0;
	return c->type->sizes()[id];
}

entity_type context::get_type(entity e) const noexcept
{
	const auto& data = ents.datas[e.id];
	return data.c->type->get_type();
}

void context::gather_reference(entity e, std::pmr::vector<entity>& entities)
{
	auto group_data = (buffer*)get_component_ro(e, group_id);
	if (group_data == nullptr)
	{
		struct gather : patcher_i
		{
			entity source;
			std::pmr::vector<entity>* ents;
			entity patch(entity e) override
			{
				if(e!=source)
					ents->push_back(e);
				return e;
			}
		} p;
		p.source = e;
		p.ents = &entities;
		auto& data = ents.datas[e.id];
		auto g = data.c->type;
		auto mt = g->metatypes();
		forloop(i, 0, g->metaCount)
			p.patch(mt[i]);
		chunk::patch({ data.c, data.i, 1 }, & p);
	}
	else
	{
		uint32_t size = group_data->size / sizeof(entity);
		stack_array(entity, members, size);
		memcpy(members, group_data->data(), group_data->size);
		struct gather : patcher_i
		{
			entity* source;
			uint32_t count;
			std::pmr::vector<entity>* ents;
			entity patch(entity e) override
			{
				forloop(i, 0, count)
					if (e == source[i])
						return e;
				ents->push_back(e);
				return e;
			}
		} p;
		p.source = members;
		p.count = size;
		p.ents = &entities;
		forloop(i, 0, size)
		{
			e = members[i];
			auto& data = ents.datas[e.id]; 
			auto g = data.c->type;
			auto mt = g->metatypes();
			forloop(i, 0, g->metaCount)
				p.patch(mt[i]);
			chunk::patch({ data.c, data.i, 1 }, &p);
		}
	}
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

void context::deserialize(serializer_i* s, patcher_i* patcher, entity* ret, uint32_t times)
{
	entity src = deserialize_single(s, patcher);
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
			members[i] = deserialize_single(s, patcher);
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


void context::create_snapshot(serializer_i* s)
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

	//todo: pack chunk into large chunk
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

void context::load_snapshot(serializer_i* s)
{
	clear();
	append_snapshot(s, nullptr);
}

entity linear_patcher::patch(entity e)
{
	patch_entity(e, start, target, count);
	return e;
}

void context::append_snapshot(serializer_i* s, entity* ret)
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
		if ((bitset[i / 4] & (1 << (i % 4))) != 0)
			patch[i] = ents.new_entity();
		else
			patch[i] = entity::Invalid;
	linear_patcher patcher;
	patcher.start = start;
	patcher.target = patch;
	patcher.count = count;

	//todo: multithread?
	for(archetype* g = deserialize_archetype(s, &patcher); g!=nullptr; g = deserialize_archetype(s, &patcher))
		for(auto slice = deserialize_slice(g, s); slice; slice = deserialize_slice(g, s))
			chunk::patch(*slice, &patcher);

	if (needFree)
		::free(patch);
	if (needFreeB)
		::free(bitset);
}

void context::clear()
{
	memset(typeTimestamps, 0, typeCapacity * sizeof(uint32_t));
	for (auto& g : archetypes)
	{
		chunk* c = g.second->firstChunk;
		while (c != nullptr)
		{
			chunk* next = c->next;
			chunk::destruct({ c,0,c->count });
			recycle_chunk(c);
			c = next;
		}
		release_reference(g.second);
		free(g.second);
	}
	queries.clear();
	archetypes.clear();
}

void context::gc_meta()
{
	//fix me: maintain archetype map
	for (auto& gi : archetypes)
	{
		auto g = gi.second;
		auto mt = g->metatypes();
		forloop(i, 0, g->metaCount)
		{
			if (!exist(mt[i]))
			{
				if (i + 1 != g->metaCount)
					std::swap(mt[i], mt[g->metaCount - 1]);
				g->metaCount--;
			}
		}
	}
}

bool context::is(entity e, const entity_type& type) const noexcept
{
	if (!exist(e))
		return false;
	const auto& data = ents.datas[e.id];
	auto et = data.c->type->get_type();
	return et.types.all(type.types) && et.metatypes.all(type.metatypes);
}

bool context::share_component(entity e, const typeset& type) const
{
	if (!exist(e))
		return false;
	const auto& data = ents.datas[e.id];
	return share_component(data.c->type, type);
}

bool context::has_component(entity e, const typeset& type) const noexcept
{
	if (!exist(e))
		return false;
	const auto& data = ents.datas[e.id];
	return has_component(data.c->type, type);
}

bool context::own_component(entity e, const typeset& type) const noexcept
{
	if (!exist(e))
		return false;
	const auto& data = ents.datas[e.id];
	return own_component(data.c->type, type);
}

void context::enable_component(entity e, const typeset& type) const noexcept
{
	if (!exist(e))
		return;
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	if (!g->withMask)
		return;
	mask mm = g->get_mask(type);
	auto id = g->index(mask_id);
	g->timestamps(c)[id] = timestamp;
	auto& m = *(mask*)(c->data() + g->offsets(c->ct)[id] + data.i * g->sizes()[id]);
	m |= mm;
}

void context::disable_component(entity e, const typeset& type) const noexcept
{
	if (!exist(e))
		return;
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	if (!g->withMask)
		return;
	mask mm = g->get_mask(type);
	auto id = g->index(mask_id);
	g->timestamps(c)[id] = timestamp;
	auto& m = *(mask*)(c->data() + g->offsets(c->ct)[id] + data.i * g->sizes()[id]);
	m &= ~mm;
}

bool context::is_component_enabled(entity e, const typeset& type) const noexcept
{
	if (!exist(e))
		return false;
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	if (!g->withMask)
		return true;
	mask mm = g->get_mask(type);
	auto id = g->index(mask_id);
	g->timestamps(c)[id] = timestamp;
	auto& m = *(mask*)(c->data() + g->offsets(c->ct)[id] + data.i * g->sizes()[id]);
	return (m & mm) == mm;
}

bool context::exist(entity e) const noexcept
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
		free = datas[free].nextFree;
		return { id, datas[id].v };
	}
}

void context::entities::free_entities(chunk_slice s)
{
	entity* toFree = (entity*)s.c->data() + s.start;
	forloop(i, 0, s.count - 1)
	{
		data& freeData = datas[toFree[i].id];
		freeData = { nullptr, 0, freeData.v + 1 };
		freeData.nextFree = toFree[i + 1].id;
	}
	data& freeData = datas[toFree[s.count - 1].id];
	freeData = { nullptr, 0, freeData.v + 1 };
	freeData.nextFree = free;
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

context::archetype_iterator context::query(const archetype_filter& filter)
{
	auto& cache = get_query_cache(filter);
	archetype_iterator iter;
	iter.iter = cache.archetypes.begin();
	iter.end = cache.archetypes.end();
	return iter;
}

context::chunk_iterator context::query(archetype* type , const chunk_filter& filter)
{
	chunk_iterator iter;
	iter.type = type;
	iter.iter = type->firstChunk;
	iter.filter = filter;
	return iter;
}

context::entity_iterator context::query(chunk* c, const entity_filter& filter)
{
	entity_iterator iter;
	iter.filter = filter;
	iter.index = 0;
	iter.size = c->get_count();
	iter.masks = (mask*)get_component_ro(c, mask_id);
	return iter;
}

std::optional<archetype*> context::archetype_iterator::next()
{
	if (iter == end)
		return {};
	curr = iter;
	iter++;
	return curr->type;
}

mask context::archetype_iterator::get_mask() const
{
	return curr->matched;
}

std::optional<chunk*> context::chunk_iterator::next()
{
	while (iter != nullptr)
	{
		if (filter.match(type->get_type(), type->timestamps(iter)))
		{
			chunk* c = iter;
			iter = c->next;
			return c;
		}
		iter = iter->next;
	}
	return {};
}

std::optional<uint16_t> context::entity_iterator::next()
{
	if (masks == nullptr)
	{
		if (index < size)
			return index++;
		else return {};
	}
	while (index < size)
	{
		if (filter.match(masks[index]))
		{
			uint16_t i = index;
			index++;
			return i;
		}
		index++;
	} 
	
	return {};
}
