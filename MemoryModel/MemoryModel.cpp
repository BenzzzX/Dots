#include "MemoryModel.h"

#define forloop(i, z, n) for(auto i = decltype(n)(z); i<n; ++i)

#define stack_array(type, name, size) \
type* name = (type*)alloca((size) * sizeof(type))

#define adaptive_array(type, name, size) \
type* name; \
if ((size) * sizeof(type) <= 1024*4) name = (type*)alloca((size) * sizeof(type)); \
else name = (type*)malloc((size) * sizeof(type)); \
guard guard##__LINE__ {[&]{if((size) * sizeof(type) > 1024*4) ::free(name);}}

entity entity::Invalid{ -1, -1 };

template<typename F>
struct guard
{
	F f;
	~guard() { f(); }
};
template<typename F>
guard(F&&)->guard<std::remove_reference_t<F>>;

using namespace ecs;
using namespace memory_model;

struct type_data
{
	size_t hash;
	uint16_t size;
	uint16_t elementSize;
	uint32_t entityRefs;
	uint16_t entityRefCount;
	component_vtable vtable;
};

index_t ecs::memory_model::disable_id = 0;
index_t ecs::memory_model::cleanup_id = 1;
index_t ecs::memory_model::group_id = 1;

struct global_data
{
	std::vector<type_data> infos;
	std::vector<track_state> tracks;
	std::vector<intptr_t> entityRefs;
	std::unordered_map<size_t, uint16_t> hash2type;
	std::function<void(metakey)> release_metatype;
	std::unordered_map<metakey, metainfo, metakey::hash> metainfos;
	
	global_data()
	{
		component_desc desc{};
		desc.size = 0;
		desc.hash = 0;
		cleanup_id = register_type(desc);
		desc.size = 0;
		desc.hash = 1;
		disable_id = register_type(desc);
		desc.size = sizeof(entity) * 5;
		desc.hash = 2;
		desc.isElement = true;
		desc.elementSize = sizeof(entity);
		static intptr_t entityRefs[] = { (intptr_t)offsetof(group, e) };
		desc.entityRefs = entityRefs;
		desc.entityRefCount = 1;
		group_id = register_type(desc);
	}
};

static global_data gd;

component_vtable& set_vtable(index_t m)
{
	return gd.infos[m].vtable;
}

void memory_model::set_meta_release_function(std::function<void(metakey)> func)
{
	gd.release_metatype = std::move(func);
}

index_t memory_model::register_type(component_desc desc)
{
	uint32_t rid = -1;
	if (desc.entityRefs != nullptr)
	{
		rid = gd.entityRefs.size();
		forloop(i, 0, desc.entityRefCount)
			gd.entityRefs.push_back(desc.entityRefs[i]);
	}
	
	uint16_t id = (uint16_t)gd.infos.size();
	id = tagged_index{ id, desc.isManaged, desc.isElement, desc.size == 0, desc.isMeta };
	type_data i{ desc.hash, desc.size, desc.elementSize, rid, desc.entityRefCount, desc.vtable };
	gd.infos.push_back(i);
	uint8_t s = 0;
	if (desc.need_clean)
		s = s | NeedCleaning;
	if (desc.need_copy)
		s = s | NeedCopying;
	gd.tracks.push_back((track_state)s);
	gd.hash2type.insert({ desc.hash, id });

	id = (uint16_t)gd.infos.size();
	id = tagged_index{ id, desc.isManaged, desc.isElement, desc.size == 0, desc.isMeta };
	type_data i{ desc.hash, desc.size, desc.elementSize, rid, desc.entityRefCount, desc.vtable };
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

uint32_t chunk::get_version(index_t t) noexcept
{
	uint16_t id = type->index(t);
	if (id == -1) return 0;
	return type->versions(this)[id];
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
void chunk::serialize(chunk_slice s, serializer_i *stream)
{
	context::archetype* type = s.c->type;
	uint16_t* offsets = type->offsets();
	uint16_t* sizes = type->sizes();
	tagged_index* types = (tagged_index*)type->types();
	stream->write(&s.count, sizeof(uint16_t));

	uint16_t maxSize = 0;
	forloop(i, 0, type->firstTag)
		maxSize = std::max(maxSize, sizes[i]);

	adaptive_array(char, temp, maxSize);

	forloop(i, 0, type->firstTag)
	{
		char* arr = s.c->data() + offsets[i] + sizes[i] * s.start;
		stream->write(arr, sizes[i] * s.count);
	}

	forloop(i, 0, type->firstBuffer)
	{
		tagged_index type = types[i];
		const auto& info = gd.infos[type.index()];
		auto f = info.vtable.serialize;
		if (f == nullptr)
			continue;
		char* arr = s.c->data() + offsets[i] + sizes[i] * s.start;
		forloop(j, 0, s.count)
			f(arr + j * sizes[i], stream);
	}

	forloop(i, type->firstBuffer, type->firstTag)
	{
		tagged_index type = types[i];
		const auto& info = gd.infos[type.index()];
		auto f = info.vtable.serialize;
		char* arr = s.c->data() + offsets[i] + sizes[i] * s.start;
		forloop(j, 0, s.count)
		{
			buffer* b = (buffer*)(arr + j * sizes[i]);
			stream->write(b->data(), b->size);
		}
		if (f == nullptr)
			continue;

		forloop(j, 0, s.count)
		{
			buffer* b = (buffer*)(arr + j * sizes[i]);
			uint16_t n = b->size / info.elementSize;
			forloop(l, 0, n)
			{
				char* data = b->data() + info.elementSize * l;
				f(data, stream);
			}
		}
		
	}
}

void chunk::deserialize(chunk_slice s, deserializer_i* stream)
{
	context::archetype* type = s.c->type;
	uint16_t* offsets = type->offsets();
	uint16_t* sizes = type->sizes();
	tagged_index* types = (tagged_index*)type->types();

	forloop(i, 0, type->firstTag)
	{
		char* arr = s.c->data() + offsets[i] + sizes[i] * s.start;
		stream->read(arr, sizes[i] * s.count);
	}

	forloop(i, 0, type->firstBuffer)
	{
		char* arr = s.c->data() + offsets[i] + sizes[i] * s.start;
		tagged_index type = types[i];
		const auto& info = gd.infos[type.index()];
		auto f = info.vtable.deserialize;
		if (f == nullptr)
			continue;
		char* arr = s.c->data() + offsets[i] + sizes[i] * s.start;
		forloop(j, 0, s.count)
			f(arr + j * sizes[i], stream);
	}

	forloop(i, type->firstBuffer, type->firstTag)
	{
		tagged_index type = types[i];
		const auto& info = gd.infos[type.index()];
		auto f = info.vtable.deserialize;
		char* arr = s.c->data() + offsets[i] + sizes[i] * s.start;
		forloop(j, 0, s.count)
		{
			buffer* b = (buffer*)(arr + j * sizes[i]);
			stream->read(b->data(), b->size);
		}

		if (f == nullptr)
			continue;

		forloop(j, 0, s.count)
		{
			buffer* b = (buffer*)(arr + j * sizes[i]);
			uint16_t n = b->size / info.elementSize;
			forloop(l, 0, n)
			{
				char* data = b->data() + info.elementSize * l;
				f(data, stream);
			}
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

inline uint32_t* context::archetype::versions(chunk* c) noexcept { return (uint32_t*)(c->data() + kChunkBufferSize) - firstTag; }

uint16_t context::archetype::index(index_t type) noexcept
{
	index_t* ts = types();
	index_t* result = std::lower_bound(ts, ts + componentCount, type);
	if (result != ts + componentCount && *result == type)
		return uint16_t(result - ts);
	else
		return uint16_t(-1);
}

entity_type context::archetype::get_type()
{
	index_t* ts = types();
	return entity_type
	{
		typeset { ts, componentCount },
		metaset { {metatypes(), uint16_t(componentCount - firstMeta)}, ts + firstMeta }
	};

}

size_t context::archetype::calculate_size(uint16_t componentCount, uint16_t firstTag, uint16_t firstMeta)
{
	return sizeof(index_t)* componentCount +
		sizeof(uint16_t) * firstTag +
		sizeof(uint16_t) * firstTag +
		sizeof(index_t) * (componentCount - firstMeta) +
		sizeof(context::archetype);// +40;
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
	uint16_t firstMeta = count - key.metatypes.length;
	uint16_t c = 0;
	for (c = 0; c < firstMeta; c++)
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
	void* data = malloc(archetype::calculate_size(count, firstTag, firstMeta));
	archetype* g = (archetype*)data;
	g->componentCount = count;
	g->firstMeta = firstMeta;
	g->firstBuffer = firstBuffer;
	g->firstTag = firstTag;
	g->cleaning = false;
	g->disabled = false;
	g->withTracked = false;
	g->zerosize = false;
	g->lastChunk = g->firstChunk = g->firstFree = nullptr;
	index_t* types = g->types();
	index_t* metatypes = g->metatypes();
	memcpy(types, key.types.data, count * sizeof(index_t));
	memcpy(metatypes, key.metatypes.data, key.metatypes.length * sizeof(index_t));
	forloop(i, g->firstMeta, g->componentCount)
	{
		auto& info = gd.metainfos[metakey{ types[i], metatypes[i - g->firstMeta] }];
		info.refCount++;
	}

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

	return g;
}

context::archetype* context::get_cleaning(archetype* g)
{
	if (g->cleaning) return g;
	else if (!g->withTracked) return nullptr;

	uint16_t k = 0, count = g->componentCount;
	uint16_t mk = 0, mcount = count - g->firstMeta;
	const uint16_t cleanupType = cleanup_id;
	index_t* types = g->types();
	index_t* metatypes = g->metatypes();

	stack_array(index_t, dstTypes, count + 1);
	stack_array(index_t, dstMetaTypes, mcount);
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

	forloop(i, 0, mcount)
	{
		auto type = (tagged_index)(types + g->firstMeta)[i];
		auto stage = gd.tracks[type.index()];
		if (stage & NeedCleaning != 0)
			dstMetaTypes[mk++] = metatypes[i];
	}

	auto dstKey = entity_type
	{
		typeset {dstTypes, k},
		metaset {{dstMetaTypes, mk}, dstTypes + k - mk}
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
	uint16_t mcount = count - g->firstMeta;
	index_t* types = g->types();
	index_t* metatypes = g->metatypes();

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
		metaset {{metatypes, mcount}, dstTypes + count - mcount}
	};

	return get_archetype(dstKey);
}

context::archetype* context::get_extending(archetype* g, const entity_type& ext)
{
	if (g->cleaning)
		return nullptr;

	entity_type srcType = g->get_type();
	stack_array(index_t, newTypes, srcType.types.length + ext.types.length);
	stack_array(index_t, newMetaTypes, srcType.metatypes.length + ext.metatypes.length);
	entity_type key = entity_type::merge(srcType, ext, newTypes, newMetaTypes);
	if (!g->withTracked)
		return get_archetype(key);
	else
	{
		int k = 0, mk = 0;
		stack_array(index_t, newTypesx, key.types.length);
		stack_array(index_t, newMetaTypesx, key.metatypes.length);
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
		int offset = key.types.length - key.metatypes.length;
		forloop(i, 0, key.metatypes.length)
		{
			auto type = (tagged_index)(newTypes + offset)[i];
			newMetaTypesx[mk++] = key.metatypes[i];
			if (i < key.metatypes.length - 1 && can_zip(i))
				i++;
		}
		auto dstKey = entity_type
		{
			typeset {newTypesx, k},
			metaset {{newMetaTypesx, mk}, newTypesx + k - mk}
		};
		return get_archetype(dstKey);
	}
}

context::archetype* context::get_shrinking(archetype* g, const typeset& shr)
{
	if (!g->withTracked)
	{
		entity_type srcType = g->get_type();
		stack_array(index_t, newTypes, srcType.types.length);
		stack_array(index_t, newMetaTypes, srcType.metatypes.length);
		auto key = entity_type::substract(srcType, shr, newTypes, newMetaTypes);
		return get_archetype(key);
	}
	else
	{
		entity_type srcType = g->get_type();
		stack_array(index_t, shrTypes, srcType.types.length * 2);
		int k = 0;
		forloop(i, 0, shr.length)
		{
			auto type = (tagged_index)shr[i];
			shrTypes[k++] = type;
			auto stage = gd.tracks[type.index()];
			if(stage & NeedCopying != 0)
				shrTypes[k++] = type + 1;
		}
		stack_array(index_t, newTypes, srcType.types.length);
		stack_array(index_t, newMetaTypes, srcType.metatypes.length);
		typeset shrx{ shrTypes, k };
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
	chunk* h = g->firstChunk;
	if (c == g->firstFree) 
		g->firstFree = c->next;
	remove(g->firstChunk, g->lastChunk, c);
	c->type = nullptr;
	if (g->firstChunk == nullptr)
	{
		release_reference(g);
		archetypes.erase(g->get_type());
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
	index_t* metatypes = g->metatypes();
	index_t* types = g->types();
	forloop(i, g->firstMeta, g->componentCount)
	{
		auto key = metakey{ types[i], metatypes[i - g->firstMeta] };
		auto iter = gd.metainfos.find(key);
		if (--iter->second.refCount == 0)
		{
			gd.release_metatype(key);
			gd.metainfos.erase(iter);
		}
	}
}

void context::serialize_archetype(archetype* g, serializer_i* s)
{
	entity_type type = g->get_type();
	uint16_t tlength = type.types.length, mlength = type.metatypes.length;
	s->write(&tlength, sizeof(uint16_t));
	forloop(i, 0, tlength)
		s->write(&gd.infos[tagged_index(type.types[i]).index()].hash, sizeof(size_t));
	s->write(&mlength, sizeof(uint16_t));
	forloop(i, 0, mlength)
		s->writemeta({ type.types[tlength - mlength + i], type.metatypes[i] });
}

context::archetype* context::deserialize_archetype(deserializer_i* s)
{
	uint16_t tlength;
	stack_array(index_t, types, tlength);
	if (tlength == 0)
		return nullptr;
	forloop(i, 0, tlength)
	{
		size_t hash;
		s->read(&hash, sizeof(size_t));
		//TODO: check validation
		types[i] = gd.hash2type[hash];
	}
	uint16_t mlength;
	s->read(&mlength, sizeof(uint16_t));
	stack_array(index_t, metatypes, mlength);
	s->read(metatypes, mlength);
	forloop(i, 0, mlength)
		metatypes[i] = s->readmeta({ types[tlength - mlength + i] });
	std::sort(types, types + tlength - mlength);

	index_t* arr = types + tlength - mlength;
	forloop(i, 0, mlength)
	{
		index_t* min = std::min_element(arr + i, arr + mlength);
		std::swap(arr[i], *min);
		std::swap(metatypes[i], metatypes[min - arr]);
	}

	entity_type type = { {types, tlength}, {{metatypes, mlength}, types + (tlength - mlength)} };
	return get_archetype(type);
}

std::optional<chunk_slice> context::deserialize_slice(archetype* g, deserializer_i* stream)
{
	uint16_t count;
	stream->read(&count, sizeof(uint16_t));
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
	chunk_slice s = { c, start, count };
	chunk::deserialize(s, stream);
	return s;
}

struct linear_patcher : patcher_i
{
	uint32_t start;
	entity* target;
	uint32_t count;
	entity patch(entity e) override;
};

void context::cast_to_prefab(entity* src, uint32_t size, bool keepExternal)
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

void context::uncast_from_prefab(entity* members, uint32_t size)
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

entity context::deserialize_single(deserializer_i* s)
{
	auto *g = deserialize_archetype(s);
	auto slice = deserialize_slice(g, s);
	ents.new_entities(*slice);
	return slice->c->get_entities()[slice->start];
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
	uint16_t start = c->count;
	uint16_t allocated = std::min(count, uint32_t(g->chunkCapacity - start));
	resize_chunk(c, start + allocated);
	return { c, start, allocated };
}

void context::free_slice(chunk_slice s)
{
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

context::~context()
{
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
	alloc_iterator iterator;
	iterator.ret = ret;
	iterator.count = count;
	iterator.cont = this;
	iterator.g = get_archetype(type);
	iterator.k = 0;
	return iterator;
}

void context::instantiate(entity src, entity* ret, uint32_t count)
{
	//todo: group
	auto group_data = (buffer*)get_component_ro(src, group_id);
	if (group_data == nullptr)
		instantiate_single(src, ret, count);
	else
	{
		uint32_t size = group_data->size / sizeof(entity);
		stack_array(entity, members, size);
		memcpy(members, group_data->data(), group_data->size);
		cast_to_prefab(members, size);
		instantiate_prefab(members, size, ret, count);
		uncast_from_prefab(members, size);
	}
}

context::batch_iterator context::batch(entity* ents, uint32_t count)
{
	return batch_iterator{ ents,count,this,0 };
}

context::chunk_iterator context::query(const entity_filter& type)
{
	auto iter = archetypes.begin();
	return { this,
		archetypes.empty() ? nullptr : iter->second->firstChunk,
		iter, type };
}

void context::destroy(chunk_slice s)
{
	//todo: group
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

void context::extend(chunk_slice s, const entity_type& type)
{
	archetype* g = get_extending(s.c->type, type);
	if (g == nullptr)
		return;
	else
		cast(s, g);
}

void context::shrink(chunk_slice s, const typeset& type)
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

const void* context::get_component_ro(entity e, index_t type)
{
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	uint16_t id = g->index(type);
	if (id == -1) return nullptr;
	return c->data() + g->offsets()[id] + data.i * g->sizes()[id];
}

void* context::get_component_rw(entity e, index_t type)
{
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	uint16_t id = g->index(type);
	if (id == -1) return nullptr;
	g->versions(c)[id] = version;
	return c->data() + g->offsets()[id] + data.i * g->sizes()[id];
}

const void* context::get_array_ro(chunk* c, index_t t) const noexcept
{
	uint16_t id = c->type->index(t);
	if (id == -1) return nullptr;
	return c->data() + c->type->offsets()[id];
}

void* context::get_array_rw(chunk* c, index_t t) noexcept
{
	uint16_t id = c->type->index(t);
	if (id == -1) return nullptr;
	c->type->versions(c)[id] = version;
	return c->data() + c->type->offsets()[id];
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
		cast_to_prefab(members, size, false);
		forloop(i, 0, size)
		{
			src = members[i];
			serialize_single(s, src);
		}
		uncast_from_prefab(members, size);
	}
}

void context::deserialize(deserializer_i* s, entity* ret, uint32_t times)
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
		uncast_from_prefab(members, size);
	}
}

void context::move_context(context& src, entity* patch, uint32_t count)
{
	for (auto& pair : src.archetypes)
	{
		archetype* g = pair.second;
		archetype* dstG = get_archetype(g->get_type());
		for (chunk* c = g->firstChunk; c;)
		{
			chunk* next = c->next;
			add_chunk(dstG, c);
			c = next;
		}
		release_reference(g);
		free(g);
	}
	src.archetypes.clear();
	auto& sents = src.ents;
	forloop(i, 0, count)
		if (sents.datas[i].c != nullptr)
			patch[i] = ents.new_entity();

	sents.size = sents.free = sents.capacity = 0;
	::free(sents.datas);
	sents.datas = nullptr;
}

void context::move_chunk(context& src, chunk* c, entity* patch, uint32_t count)
{
	archetype* dstG = get_archetype(c->type->get_type());

	src.ents.free_entities(c);
	src.remove_chunk(c->type, c);

	add_chunk(dstG, c);
	entity* es = (entity*)c->data();
	forloop(i, 0, c->count)
		if(es[i].id < count)
			patch[es[i].id] = ents.new_entity();
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
	s->write(&start, sizeof(uint32_t));
	s->write(&ents.size, sizeof(uint32_t));
	for (auto& pair : archetypes)
	{
		archetype* g = pair.second;
		serialize_archetype(g, s);

		for (chunk* c = g->firstChunk; c; c= c->next)
			chunk::serialize({ c, 0, c->count }, s);

		s->write(0, sizeof(uint16_t));
	}
	s->write(0, sizeof(uint16_t));
}

entity linear_patcher::patch(entity e)
{
	patch_entity(e, start, target, count);
}

void context::deserialize(deserializer_i* s)
{
	uint32_t start, count;
	s->read(&start, sizeof(uint32_t));
	s->read(&count, sizeof(uint32_t));

	adaptive_array(entity, patch, count);

	forloop(i, 0, count)
		patch[i] = ents.new_entity();
	linear_patcher patcher;
	patcher.start = start;
	patcher.target = patch;
	patcher.count = count;

	for(archetype* g = deserialize_archetype(s); g!=nullptr; g = deserialize_archetype(s))
		for(auto slice = deserialize_slice(g, s); slice; slice = deserialize_slice(g, s))
			chunk::patch(*slice, &patcher);
}

index_t context::get_metatype(entity e, index_t t)
{
	const auto& data = ents.datas[e.id];
	archetype* type = data.c->type;
	uint16_t id = type->index(t);
	if (id == -1) 
		return -1;
	return type->metatypes()[id - type->firstMeta];
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

index_t ecs::memory_model::context::get_metatype(chunk* c, index_t t)
{;
	archetype* type = c->type;
	uint16_t id = type->index(t);
	if (id == -1)
		return -1;
	return type->metatypes()[id - type->firstMeta];
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

std::optional<chunk*> context::chunk_iterator::next()
{
	auto valid_chunk = [&]
	{
		return filter.match_chunk(currg->second->get_type(), currg->second->versions(currc));
	};
	bool includeClean = false;
	bool includeDisabled = false;
	bool includeCopy = [&]
	{
		auto at = filter.all.types;
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
	auto valid_group = [&]
	{
		archetype* g = currg->second;
		if (includeClean < g->cleaning)
			return false;
		if (includeDisabled < g->disabled)
			return false;
		return  currg != cont->archetypes.end() &&
			filter.match(g->get_type());
	};
	if (currc != nullptr)
	{
		do
		{
			currc = currc->next;
		} while (!valid_chunk() && currc != nullptr);
	}
	if (currc == nullptr)
	{
		++currg;
		while (valid_group())
		{
			currc = currg->second->firstChunk;
			while (!valid_chunk() && currc != nullptr)
				currc = currc->next;
			if (currc != nullptr)
				return currc;
			++currg;
		}
		return {};
	}
	else
		return currc;
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
