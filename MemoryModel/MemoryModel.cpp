#include "MemoryModel.h"

#define forloop(i, z, n) for(auto i = decltype(n)(z); i<n; ++i)

#define stack_array(type, name, size) \
type* name = (type*)alloca((size) * sizeof(type))

#define adaptive_array(type, name, size) \
type* name; \
if ((size) * sizeof(type) <= 1024*4) name = (type*)alloca((size) * sizeof(type)); \
else name = (type*)malloc((size) * sizeof(type)); \
guard guard##__LINE__ {[&]{if((size) * sizeof(type) > 1024*4) ::free(name);}}



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

struct info
{
	size_t hash;
	uint16_t size;
	uint16_t elementSize;
	uint32_t entityRefs;
	uint16_t entityRefCount;

};

static metatype_release_callback_t release_metatype;
static std::vector<info> infos;
static std::vector<intptr_t> entityRefs;
static std::unordered_map<size_t, uint16_t> hash2type;
static std::unordered_map<metakey, metainfo, metakey::hash> metainfos;
static uint16_t disable_id;
static uint16_t cleanup_id;


uint16_t ecs::memory_model::register_type(component_desc desc)
{
	uint32_t rid = -1;
	if (desc.entityRefs != nullptr)
	{
		rid = entityRefs.size();
		forloop(i, 0, desc.entityRefCount)
			entityRefs.push_back(desc.entityRefs[i]);
	}
	
	uint16_t id = (uint16_t)infos.size();
	info i{ desc.hash, desc.size, desc.elementSize, rid, desc.entityRefCount};
	infos.push_back(i);
	id = tagged_index{ id, desc.isInsternal, desc.isElement, i.size == 0, desc.isMeta };
	hash2type.insert({ desc.hash, id });
	return id;
}

uint16_t ecs::memory_model::register_disable()
{
	component_desc desc{};
	desc.size = 0;
	desc.hash = 0;
	return register_type(desc);
}

uint16_t ecs::memory_model::register_cleanup()
{
	component_desc desc{};
	desc.size = 0;
	desc.hash = 1;
	return register_type(desc);
}

void ecs::memory_model::register_metatype_release_callback(metatype_release_callback_t callback)
{
	release_metatype = callback;
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

uint32_t chunk::get_version(uint16_t t) noexcept
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
	context::group* type = s.c->type;
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
	context::group* type = s.c->type;
	uint16_t* offsets = type->offsets();
	uint16_t* sizes = type->sizes();
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

#undef srcData
#define dstData (dst.c->data() + offsets[i] + sizes[i] * dst.start)
#define srcData (src->data() + offsets[i] + sizes[i] * srcIndex)
void chunk::duplicate(chunk_slice dst, const chunk* src, uint16_t srcIndex) noexcept
{
	context::group* type = src->type;
	context::group *dstType = dst.c->type;
	uint16_t dstI = 0;
	uint16_t* offsets = type->offsets();
	uint16_t* dstOffsets = dstType->offsets();
	uint16_t* sizes = type->sizes();
	forloop(i, 0, type->firstBuffer)
	{
		if (type->types()[i] != dstType->types()[dstI])
			continue;
		memdup(dstData, srcData,sizes[i], dst.count);
	}
	forloop(i, type->firstBuffer, type->firstTag)
	{
		if (type->types()[i] != dstType->types()[dstI])
			continue;
		const char* s = srcData;
		char* d = dstData;
		forloop(j, 0, dst.count)
			new(d + sizes[i]*j) buffer{ *(buffer*)s };
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

void chunk::patch(chunk_slice s, uint32_t start, const entity* target, uint32_t count) noexcept
{
	context::group* g = s.c->type;
	uint16_t* offsets = g->offsets();
	uint16_t* sizes = g->sizes();
	tagged_index* types = (tagged_index*)g->types();
	forloop(i, 0, g->firstBuffer)
	{
		const auto& t = infos[types[i].index()];
		char* arr = s.c->data() + offsets[i] + sizes[i]*s.start;
		forloop(j, 0, s.count)
		{
			char* data = arr + sizes[i] * j;
			forloop(k, 0, t.entityRefCount)
			{
				entity& e = *(entity*)(data + entityRefs[t.entityRefs + k]);
				patch_entity(e, start, target, count);
			}
		}
	}
	forloop(i, g->firstBuffer, g->firstTag)
	{
		const auto& t = infos[types[i].index()];
		char* arr = s.c->data() + offsets[i] + sizes[i] * s.start;
		forloop(j, 0, s.count)
		{
			buffer *b =(buffer*)(arr + sizes[i] * j);
			uint16_t n = b->size / t.elementSize;
			forloop(l, 0, n)
			{
				char* data = b->data() + t.elementSize * l;
				forloop(k, 0, t.entityRefCount)
				{
					entity& e = *(entity*)(data + entityRefs[t.entityRefs + k]);
					patch_entity(e, start, target, count);
				}
			}
		}
	}
}

//TODO: Repeating my self?
void chunk::depatch(chunk_slice s, const entity* src, const entity* target, uint32_t count) noexcept
{
	context::group* g = s.c->type;
	uint16_t* offsets = g->offsets();
	uint16_t* sizes = g->sizes();
	tagged_index* types = (tagged_index*)g->types();
	forloop(i, 0, g->firstBuffer)
	{
		const auto& t = infos[types[i].index()];
		char* arr = s.c->data() + offsets[i] + sizes[i] * s.start;
		forloop(j, 0, s.count)
		{
			char* data = arr + sizes[i] * j;
			forloop(k, 0, t.entityRefCount)
			{
				entity& e = *(entity*)(data + entityRefs[t.entityRefs + k]);
				depatch_entity(e, src, target, count);
			}
		}
	}
	forloop(i, g->firstBuffer, g->firstTag)
	{
		const auto& t = infos[types[i].index()];
		char* arr = s.c->data() + offsets[i] + sizes[i] * s.start;
		forloop(j, 0, s.count)
		{
			buffer* b = (buffer*)(arr + sizes[i] * j);
			uint16_t n = b->size / t.elementSize;
			forloop(l, 0, n)
			{
				char* data = b->data() + t.elementSize * l;
				forloop(k, 0, t.entityRefCount)
				{
					entity& e = *(entity*)(data + entityRefs[t.entityRefs + k]);
					depatch_entity(e, src, target, count);
				}
			}
		}
	}
}

//TODO: handle transient data?
void chunk::serialize(chunk_slice s, serializer_i *stream)
{
	context::group* type = s.c->type;
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
		const auto& t = infos[types[i].index()];
		stream->write(arr, sizes[i] * s.count);
	}

	forloop(i, type->firstBuffer, type->firstTag)
	{
		forloop(j, 0, s.count)
		{
			uint16_t offset = offsets[i] + sizes[i] * s.start + j * sizes[i];
			stream->write(&offset, sizeof(uint16_t));
			buffer* b = (buffer*)(s.c->data() + offset);
			stream->write(b->data(), b->size);
		}
	}
	stream->write(0, sizeof(uint16_t));
	
}

void chunk::deserialize(chunk_slice s, deserializer_i* stream)
{
	context::group* type = s.c->type;
	uint16_t* offsets = type->offsets();
	uint16_t* sizes = type->sizes();
	tagged_index* types = (tagged_index*)type->types();

	forloop(i, 0, type->firstTag)
	{
		char* arr = s.c->data() + offsets[i] + sizes[i] * s.start;
		const auto& t = infos[types[i].index()];
		stream->read(arr, sizes[i] * s.count);
	}

	uint16_t offset;
	stream->read(&offset, sizeof(uint16_t));
	while (offset != 0)
	{
		buffer* b = (buffer*)(s.c->data() + offset);
		//b->d = (char*)malloc(b->size);
		//b->capacity = b->size;
		b->d = (char*)malloc(b->capacity);
		stream->read(b->d, b->size);
		stream->read(&offset, sizeof(uint16_t));
	}
}

void chunk::cast(chunk_slice dst, chunk* src, uint16_t srcIndex) noexcept
{
	context::group* srcType = src->type;
	context::group* dstType = dst.c->type;
	uint16_t dstI = 0;
	uint16_t srcI = 0;
	uint16_t count = dst.count;
	uint16_t* srcOffsets = srcType->offsets();
	uint16_t* dstOffsets = dstType->offsets();
	uint16_t* srcSizes = srcType->sizes();
	uint16_t* dstSizes = dstType->sizes();
	uint16_t* srcTypes = srcType->types(); uint16_t* dstTypes = dstType->types();
	while (srcI < srcType->firstBuffer && dstI < dstType->firstBuffer)
	{
		auto st = srcTypes[srcI];
		auto dt = dstTypes[dstI];
		char* s = src->data() + srcOffsets[srcI] + srcSizes[srcI] * srcIndex;
		char* d = dst.c->data() + dstOffsets[dstI] + dstSizes[dstI] * dst.start;
		if (st < dt) srcI++; //destruct 
		else if (st > dt) //construct
#ifndef NOINITIALIZE
			memset(d, 0, dstSizes[dstI++] * count);
#else
			dstI++;
#endif
		else //move
			memcpy(d, s, dstSizes[(srcI++, dstI++)] * count);
	}
	srcI = srcType->firstBuffer;
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
		auto st = srcTypes[srcI];
		auto dt = dstTypes[dstI];
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

inline uint32_t* context::group::versions(chunk* c) noexcept { return (uint32_t*)(c->data() + kChunkBufferSize) - firstTag; }

uint16_t context::group::index(uint16_t type) noexcept
{
	uint16_t* ts = types();
	uint16_t* result = std::lower_bound(ts, ts + componentCount, type);
	if (result != ts + componentCount && *result == type)
		return uint16_t(result - ts);
	else
		return uint16_t(-1);
}

entity_type context::group::get_type()
{
	uint16_t* ts = types();
	return entity_type
	{
		typeset { ts, componentCount },
		metaset { {metatypes(), uint16_t(componentCount - firstMeta)}, ts + firstMeta }
	};

}

size_t context::group::calculate_size(uint16_t componentCount, uint16_t firstTag, uint16_t firstMeta)
{
	return sizeof(uint16_t)* componentCount +
		sizeof(uint16_t) * firstTag +
		sizeof(uint16_t) * firstTag +
		sizeof(uint16_t) * (componentCount - firstMeta) +
		sizeof(context::group);// +40;
}

void context::remove(chunk*& h, chunk*& t, chunk* c)
{
	if (c == t)
		t = t->prev;
	if (h == c)
		h = h->next;
	c->unlink();
}


context::group* context::get_group(const entity_type& key)
{
	auto iter = groups.find(key);
	if (iter != groups.end())
		return iter->second;
	
	uint16_t firstTag = 0;
	uint16_t firstBuffer = 0;
	uint16_t c = 0;
	const uint16_t count = key.types.length;
	for (; c < count; c++)
	{
		auto type = (tagged_index)key.types[c];
		if (type.is_buffer()) break;
	}
	firstBuffer = c;
	for (c = 0; c < count; c++)
	{
		auto type = (tagged_index)key.types[c];
		if (type.is_tag()) break;
	}
	firstTag = c;
	if(firstBuffer == count)
		firstBuffer = firstTag;
	uint16_t firstMeta = count - key.metatypes.length;
	void* data = malloc(group::calculate_size(count, firstTag, firstMeta));
	group* g = (group*)data;
	g->componentCount = count;
	g->firstMeta = firstMeta;
	g->firstBuffer = firstBuffer;
	g->firstTag = firstTag;
	g->cleaning = false;
	g->disabled = false;
	g->needsClean = false;
	g->zerosize = false;
	g->lastChunk = g->firstChunk = g->firstFree = nullptr;
	uint16_t* types = g->types();
	uint16_t* metatypes = g->metatypes();
	memcpy(types, key.types.data, count * sizeof(uint16_t));
	memcpy(metatypes, key.metatypes.data, key.metatypes.length * sizeof(uint16_t));
	forloop(i, g->firstMeta, g->componentCount)
	{
		auto& info = metainfos[metakey{ types[i], metatypes[i - g->firstMeta] }];
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
		else if (type.is_internal())
			g->needsClean = true;
		auto info = infos[type.index()];
		sizes[i] = info.size;
		hash[i] = info.hash;
		stableOrder[i] = i;
		entitySize += info.size;
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

	groups.insert({ g->get_type(), g });

	return g;
}

context::group* context::get_cleaning(group* g)
{
	if (g->cleaning) return g;
	else if (!g->needsClean) return nullptr;

	uint16_t k = 0, count = g->componentCount;
	uint16_t mk = 0, mcount = count - g->firstMeta;
	const uint16_t cleanupType = cleanup_id;
	uint16_t* types = g->types();
	uint16_t* metatypes = g->metatypes();

	stack_array(uint16_t, dstTypes, count + 1);
	stack_array(uint16_t, dstMetaTypes, mcount);
	bool cleanAdded = false;
	forloop(i, 0, count)
	{
		auto type = (tagged_index)types[i];
		if (cleanupType < type && !cleanAdded)
		{
			dstTypes[k++] = cleanupType;
			cleanAdded = true;
		}
		if (type.is_internal())
			dstTypes[k++] = type;
	}
	forloop(i, 0, mcount)
	{
		auto type = (tagged_index)(types + g->firstMeta)[i];
		if (type.is_internal())
			dstMetaTypes[mk++] = metatypes[i];
	}

	auto dstKey = entity_type
	{
		typeset {dstTypes, k},
		metaset {{dstMetaTypes, mk}, types + k - mk}
	};

	return get_group(dstKey);
}

bool context::is_cleaned(const entity_type& type)
{
	return type.types.length == 1;
}

context::group* context::get_instatiation(group* g)
{
	if (g->cleaning) return nullptr;
	else if (!g->needsClean) return g;

	uint16_t k = 0, count = g->componentCount;
	uint16_t mk = 0, mcount = count - g->firstMeta;
	uint16_t* types = g->types();
	uint16_t* metatypes = g->metatypes();

	stack_array(uint16_t, dstTypes, count + 1);
	stack_array(uint16_t, dstMetaTypes, mcount);
	forloop(i, 0, count)
	{
		auto type = (tagged_index)types[i];
		if (!type.is_internal())
			dstTypes[k++] = type;
	}
	forloop(i, 0, mcount)
	{
		auto type = (tagged_index)(types + g->firstMeta)[i];
		if (!type.is_internal())
			dstMetaTypes[mk++] = metatypes[i];
	}

	auto dstKey = entity_type
	{
		typeset {dstTypes, k},
		metaset {{dstMetaTypes, mk}, types + k - mk}
	};

	return get_group(dstKey);
}

chunk* context::new_chunk(group* g)
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

void context::add_chunk(group* g, chunk* c)
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

void context::remove_chunk(group* g, chunk* c)
{
	chunk* h = g->firstChunk;
	if (c == g->firstFree) 
		g->firstFree = c->next;
	remove(g->firstChunk, g->lastChunk, c);
	c->type = nullptr;
	if (g->firstChunk == nullptr)
	{
		release_reference(g);
		groups.erase(g->get_type());
		::free(g);
	}
}

void context::mark_free(group* g, chunk* c)
{
	remove(g->firstChunk, g->lastChunk, c);
	g->lastChunk->link(c);
	g->lastChunk = c;
	if (g->firstFree == nullptr)
		g->firstFree = c;
}

void context::unmark_free(group* g, chunk* c)
{
	remove(g->firstFree, g->lastChunk, c);
	if (g->lastChunk == nullptr)
		g->lastChunk = c;
	if (c->next != g->firstFree)
		g->firstChunk->link(c);
}

void context::release_reference(group* g)
{
	uint16_t* metatypes = g->metatypes();
	uint16_t* types = g->types();
	forloop(i, g->firstMeta, g->componentCount)
	{
		auto key = metakey{ types[i], metatypes[i - g->firstMeta] };
		auto iter = metainfos.find(key);
		if (--iter->second.refCount == 0)
		{
			release_metatype(key);
			metainfos.erase(iter);
		}
	}
}

void context::serialize_type(group* g, serializer_i* s)
{
	entity_type type = g->get_type();
	uint16_t tlength = type.types.length, mlength = type.metatypes.length;
	s->write(&tlength, sizeof(uint16_t));
	forloop(i, 0, tlength)
		s->write(&infos[tagged_index(type.types[i]).index()].hash, sizeof(size_t));
	s->write(&mlength, sizeof(uint16_t));
	forloop(i, 0, mlength)
		s->writemeta({ type.types.data[tlength - mlength + i], type.metatypes.data[i] });
}

context::group* context::deserialize_group(deserializer_i* s, uint16_t tlength)
{
	stack_array(uint16_t, types, tlength);
	forloop(i, 0, tlength)
	{
		size_t hash;
		s->read(&hash, sizeof(size_t));
		//TODO: check validation
		types[i] = hash2type[hash];
	}
	uint16_t mlength;
	s->read(&mlength, sizeof(uint16_t));
	stack_array(uint16_t, metatypes, mlength);
	s->read(metatypes, mlength);
	forloop(i, 0, mlength)
		metatypes[i] = s->readmeta({ types[tlength - mlength + i] });
	std::sort(types, types + tlength - mlength);

	uint16_t* arr = types + tlength - mlength;
	forloop(i, 0, mlength)
	{
		uint16_t* min = std::min_element(arr + i, arr + mlength);
		std::swap(arr[i], *min);
		std::swap(metatypes[i], metatypes[min - arr]);
	}

	entity_type type = { {types, tlength}, {{metatypes, mlength}, types + (tlength - mlength)} };
	return get_group(type);
}

chunk_slice context::deserialize_slice(group* g, deserializer_i *stream, uint16_t count)
{
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

void context::destroy_chunk(group* g, chunk* c)
{

	remove_chunk(g, c);
	if (poolSize < kChunkPoolCapacity)
		chunkPool[poolSize++] = c;
	else
		::free(c);
}

void context::resize_chunk(chunk* c, uint16_t count)
{
	group* g = c->type;
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

chunk_slice context::allocate_slice(group* g, uint32_t count)
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

void context::cast_slice(chunk_slice src, group* g) 
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



ecs::memory_model::context::~context()
{
	for (auto& g : groups)
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

void context::allocate(const entity_type& type, entity* ret, uint32_t count)
{
	group *g = get_group(type);
	g = get_instatiation(g);
	uint32_t k = 0;
	while (k < count)
	{
		chunk_slice s = allocate_slice(g, count - k);
		chunk::construct(s);
		ents.new_entities(s);
		memcpy(ret + k, s.c->get_entities() + s.start, s.count * sizeof(entity));
		k += s.count;
	}
}

void context::instantiate(entity src, entity* ret, uint32_t count)
{
	const auto& data = ents.datas[src.id];
	group* g = get_instatiation(data.c->type);
	uint32_t k = 0;
	while (k < count)
	{
		chunk_slice s = allocate_slice(g, count - k);
		chunk::duplicate(s, data.c, data.i);
		ents.new_entities(s);
		if (ret != nullptr)
			memcpy(ret + k, s.c->get_entities() + s.start, s.count * sizeof(entity));
		k += s.count;
	}
}

context::batch_iterator context::batch(entity* ents, uint32_t count)
{
	return batch_iterator{ ents,count,this,0 };
}

context::chunk_iterator context::query(const entity_filter& type)
{
	auto iter = groups.begin();
	return { this,
		groups.empty() ? nullptr : iter->second->firstChunk,
		iter, type };
}

void context::destroy(chunk_slice s)
{
	group* g = s.c->type;
	if (g->cleaning)
		return;
	if (!g->needsClean)
	{
		ents.free_entities(s);
		free_slice(s);
	}
	else
	{
		g = get_cleaning(g);
		cast_slice(s, g);
	}
}

void context::extend(chunk_slice s, const entity_type& type)
{
	entity_type srcType = s.c->type->get_type();
	stack_array(uint16_t, newTypes, srcType.types.length + type.types.length);
	stack_array(uint16_t, newMetaTypes, srcType.metatypes.length + type.metatypes.length);
	cast(s, entity_type::merge(srcType, type, newTypes, newMetaTypes));
}

void context::shrink(chunk_slice s, const typeset& type)
{
	entity_type srcType = s.c->type->get_type();
	stack_array(uint16_t, newTypes, srcType.types.length);
	stack_array(uint16_t, newMetaTypes, srcType.metatypes.length );
	auto key = entity_type::substract(srcType, type, newTypes, newMetaTypes);
	if (s.c->type->cleaning && is_cleaned(key))
		free_slice(s);
	else
		cast(s, key);
}

void context::cast(chunk_slice s, const entity_type& type)
{
	group* g = get_group(type);
	group* srcG = s.c->type;
	if (srcG == g) 
		return;
	else if (s.full() && srcG->firstTag == g->firstTag)
	{
		remove_chunk(srcG, s.c);
		add_chunk(g, s.c);
	}
	else
		cast_slice(s, g);
}

const void* context::get_component_ro(entity e, uint16_t type)
{
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; group* g = c->type;
	uint16_t id = g->index(type);
	if (id == -1) return nullptr;
	return c->data() + g->offsets()[id] + data.i * g->sizes()[id];
}

void* context::get_component_rw(entity e, uint16_t type)
{
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; group* g = c->type;
	uint16_t id = g->index(type);
	if (id == -1) return nullptr;
	g->versions(c)[id] = version;
	return c->data() + g->offsets()[id] + data.i * g->sizes()[id];
}

const void* context::get_array_ro(chunk* c, uint16_t t) const noexcept
{
	uint16_t id = c->type->index(t);
	if (id == -1) return nullptr;
	return c->data() + c->type->offsets()[id];
}

void* context::get_array_rw(chunk* c, uint16_t t) noexcept
{
	uint16_t id = c->type->index(t);
	if (id == -1) return nullptr;
	c->type->versions(c)[id] = version;
	return c->data() + c->type->offsets()[id];
}

const entity* ecs::memory_model::context::get_entities(chunk* c) noexcept
{
	return c->get_entities();
}

uint16_t ecs::memory_model::context::get_element_size(chunk* c, uint16_t t) const noexcept
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

entity context::allocate_prefab(const entity_type& type)
{
	group* g = get_group(type);
	g = get_instatiation(g);
	chunk_slice s = allocate_slice(g);
	chunk::construct(s);
	entity newE = ents.new_prefab();
	*(entity*)s.c->data() = newE;
	auto& newData = ents.datas[newE.id];
	newData.c = s.c;
	newData.i = s.start;
	return newE;
}

prefab context::instantiate_as_prefab(entity* src, uint32_t count)
{
	if (count > 1)
	{
		stack_array(entity, pes, count);

		forloop(i, 0, count)
			pes[i] = ents.new_prefab();

		forloop(i, 0, count)
		{
			const auto& data = ents.datas[src[i].id];
			group* g = get_instatiation(data.c->type);
			chunk_slice s = allocate_slice(g, 1);
			chunk::duplicate(s, data.c, data.i);
			chunk::depatch(s, src, pes, count);

			((entity*)s.c->data())[s.start] = pes[i];
			auto& newData = ents.datas[pes[i].id];
			newData.c = s.c;
			newData.i = s.start;
		}

		return { this, pes[0], count };
	}
	else
	{
		const auto& data = ents.datas[src[0].id];
		group* g = get_instatiation(data.c->type);
		chunk_slice s = allocate_slice(g);
		chunk::duplicate(s, data.c, data.i);
		entity newE = ents.new_prefab();
		*(entity*)s.c->data() = newE;
		auto& newData = ents.datas[newE.id];
		newData.c = s.c;
		newData.i = s.start;

		return { this, newE, count };
	}
}

void context::instantiate_prefab(prefab p, entity* ret, uint32_t count)
{
	forloop(i, 0, p.count * count)
		ret[i] = ents.new_entity();
	forloop(i, 0, p.count)
	{
		const auto& data = ents.datas[p.start.id + i];
		group* g = get_instatiation(data.c->type);
		uint32_t k = 0;
		while (k < count)
		{
			chunk_slice s = allocate_slice(g, count - k);
			chunk::duplicate(s, data.c, data.i);
			entity* es = (entity*)s.c->data() + s.start;
			forloop(j, 0, s.count)
			{
				chunk::patch({ s.c,uint16_t(s.start + j),1 }, p.start.id, ret + (k + j) * p.count, p.count);
				es[j] = ret[(k + j) * p.count + i];
			}
			k += s.count;
		}
	}
}

void context::move_context(context& src, entity* patch, uint32_t count)
{
	for (auto& pair : src.groups)
	{
		group* g = pair.second;
		group* dstG = get_group(g->get_type());
		for (chunk* c = g->firstChunk; c;)
		{
			chunk* next = c->next;
			add_chunk(dstG, c);
			c = next;
		}
		release_reference(g);
		free(g);
	}
	src.groups.clear();
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
	group* dstG = get_group(c->type->get_type());

	src.ents.free_entities(c);
	src.remove_chunk(c->type, c);

	add_chunk(dstG, c);
	entity* es = (entity*)c->data();
	forloop(i, 0, c->count)
		if(es[i].id < count)
			patch[es[i].id] = ents.new_entity();
}

void context::patch_chunk(chunk* c, const entity* patch, uint32_t count)
{
	entity* es = (entity*)c->data();
	forloop(i, 0, c->count)
		es[i] = patch[es[i].id];
	chunk::patch(c, 0, patch, count);
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


void context::serialize(context& cont, serializer_i* s)
{
	const uint32_t start = 0;
	s->write(&start, sizeof(uint32_t));
	s->write(&cont.ents.size, sizeof(uint32_t));
	for (auto& pair : cont.groups)
	{
		group* g = pair.second;
		serialize_type(g, s);

		for (chunk* c = g->firstChunk; c; c= c->next)
			chunk::serialize({ c, 0, c->count }, s);

		s->write(0, sizeof(uint16_t));
	}
	s->write(0, sizeof(uint16_t));
}

void context::deserialize(context& cont, deserializer_i* s)
{
	uint32_t start, count;
	s->read(&start, sizeof(uint32_t));
	s->read(&count, sizeof(uint32_t));

	adaptive_array(entity, patch, count);

	forloop(i, 0, count)
		patch[i] = cont.ents.new_entity();

	uint16_t tlength;
	s->read(&tlength, sizeof(uint16_t));
	while (tlength != 0)
	{
		group* g = cont.deserialize_group(s, tlength);
		uint16_t count;
		s->read(&count, sizeof(uint16_t));

		while(count != 0)
		{
			chunk_slice slice = cont.deserialize_slice(g, s, count);
			chunk::patch(slice, start, patch, count);
			s->read(&count, sizeof(uint16_t));
		}
		s->read(&tlength, sizeof(uint16_t));
	}
}

prefab context::deserialize_prefab(context& cont, deserializer_i* s)
{
	uint32_t start, count;
	s->read(&start, sizeof(uint32_t));
	s->read(&count, sizeof(uint32_t));

	adaptive_array(entity, patch, count);

	forloop(i, 0, count)
		patch[i] = cont.ents.new_prefab();

	uint16_t tlength;
	s->read(&tlength, sizeof(uint16_t));
	stack_array(uint16_t, types, 128);
	stack_array(uint16_t, metatypes, 128);
	while (tlength != 0)
	{
		group* g = cont.deserialize_group(s, tlength);
		uint16_t count;
		s->read(&count, sizeof(uint16_t));

		while (count != 0)
		{
			chunk_slice slice = cont.deserialize_slice(g, s, count);
			chunk::patch(slice, start, patch, count);
			s->read(&count, sizeof(uint16_t));
		}
		s->read(&tlength, sizeof(uint16_t));
	}

	prefab ret{ &cont, patch[0], count };

	return ret;
}

void context::serialize_prefab(prefab p, serializer_i* s)
{
	s->write(&p.start.id, sizeof(uint32_t));
	s->write(&p.count, sizeof(uint32_t));
	forloop(i, 0, p.count)
	{
		auto& data = p.src->ents.datas[p.start.id];
		serialize_type(data.c->type, s);

		chunk::serialize({ data.c, (uint16_t)data.i, 1 }, s);
		s->write(0, sizeof(uint16_t));
	}
	s->write(0, sizeof(uint16_t));
}

uint16_t context::get_metatype(entity e, uint16_t t)
{
	const auto& data = ents.datas[e.id];
	group* type = data.c->type;
	return type->metatypes()[type->index(t) - type->firstMeta];
}

bool context::has_component(entity e, uint16_t t) const
{
	const auto& data = ents.datas[e.id];
	return data.c->type->index(t) != -1;
}

bool context::exist(entity e) const
{
	return e.id < ents.size && e.version == ents.datas[e.id].v;
}

uint16_t ecs::memory_model::context::get_metatype(chunk* c, uint16_t t)
{;
	group* type = c->type;
	return type->metatypes()[type->index(t) - type->firstMeta];
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
	auto valid_chunk = [&]()
	{
		return currc != nullptr && filter.match_chunk(currg->second->get_type(), currg->second->versions(currc));
	};
	if (currc != nullptr)
	{
		do
		{
			currc = currc->next;
		} while (!valid_chunk());
	}
	if (currc == nullptr)
	{
		do
		{
			currc = (++currg)->second->firstChunk;
			while (!valid_chunk())
				currc = currc->next;
		} while (currc == nullptr &&
			currg != cont->groups.end() &&
			!filter.match(currg->second->get_type(), currg->second->disabled));

		if (currc != nullptr)
			return currc;
		else
			return {};
	}
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
