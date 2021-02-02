#include "Database.h"
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#define cat(a, b) a##b
#define forloop(i, z, n) for(auto i = std::decay_t<decltype(n)>(z); i<(n); ++i)
#define AO(type, name, size) \
adaptive_object _ao_##name{(size)*sizeof(type)}; \
type* name = (type*)_ao_##name.self;

using namespace core;
using namespace database;

index_t disable_id = 0;
index_t cleanup_id = 1;
index_t group_id = 2;
index_t mask_id = 3;
#ifdef ENABLE_GUID_COMPONENT
index_t guid_id = 4;
#endif

builtin_id core::database::get_builtin()
{
	return {
		disable_id,
		cleanup_id,
		group_id,
		mask_id,
#ifdef ENABLE_GUID_COMPONENT
		guid_id
#endif
	};
}

#define stack_array(type, name, size) \
stack_object __so_##name((size_t)(size)*sizeof(type)); \
type* name = (type*)__so_##name.self;

struct stack_object
{
	size_t size;
	void* self;
	stack_object(size_t size)
		: size(size), self(nullptr)
	{
		self = DotsContext->stack_alloc(size);
	}
	~stack_object()
	{
		DotsContext->stack_free(self, size);
	}
};

struct adaptive_object
{
	enum
	{
		Pool,
		Heap,
		Stack,
	} type;

	size_t size;
	void* self;
	adaptive_object(size_t size)
		:size(size), self(nullptr)
	{
		if (DotsContext->stack.stackSize - size > 1024)
		{
			self = DotsContext->stack_alloc(size);
			type = Stack;
		}
		else if (size < kFastBinCapacity)
		{
			self = DotsContext->malloc(alloc_type::fastbin);
			type = Pool;
		}
		else
		{
			self = ::malloc(size);
			type = Heap;
		}
	}
	~adaptive_object()
	{
		switch (type)
		{
		case Pool:
			DotsContext->free(alloc_type::fastbin, self);
			break;
		case Heap:
			::free(self);
			break;
		case Stack:
			DotsContext->stack_free(self, size);
			break;
		}
	}
};

component_vtable& set_vtable(index_t m)
{
	return DotsContext->infos[m].vtable;
}

index_t database::register_type(component_desc desc)
{
	return DotsContext->register_type(desc);
}

bool chunk_slice::full() { return c != nullptr && start == 0 && count == c->get_count(); }

chunk_slice::chunk_slice(chunk* c) : c(c), start(0), count(c->get_count()) {}

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

void chunk::clone(chunk* dst) noexcept
{
	memcpy(dst, this, get_size());
	uint32_t* offsets = type->offsets[(int)ct];
	uint16_t* sizes = type->sizes;
	forloop(i, type->firstBuffer, type->firstTag)
	{
		char* src = data() + offsets[i] + sizes[i];
		forloop(j, 0, count)
		{
			buffer* b = (buffer*)((size_t)j * sizes[i] + src);
			if (b->d != nullptr)
			{
				char* clonedData = (char*)buffer_malloc(b->size);
				memcpy(clonedData, b->d, b->size);
				b->d = clonedData;
			}
		}
	}
}

uint32_t chunk::get_timestamp(index_t t) noexcept
{
	tsize_t id = type->index(t);
	if (id == InvalidIndex) return 0;
	return type->timestamps(this)[id];
}

void chunk::move(chunk_slice dst, tsize_t srcIndex) noexcept
{
	chunk* src = dst.c;
	uint32_t* offsets = src->type->offsets[(int)src->ct];
	uint16_t* sizes = src->type->sizes;
	forloop(i, 0, src->type->firstTag)
		memcpy(
			src->data() + offsets[i] + (size_t)sizes[i] * dst.start,
			src->data() + offsets[i] + (size_t)sizes[i] * srcIndex,
			(size_t)dst.count * sizes[i]
		);
}

void chunk::move(chunk_slice dst, const chunk* src, uint32_t srcIndex) noexcept
{
	//assert(dst.c->type == src->type)
	uint32_t* offsets = dst.c->type->offsets[(int)dst.c->ct];
	uint16_t* sizes = dst.c->type->sizes;
	forloop(i, 0, dst.c->type->firstTag)
		memcpy(
			dst.c->data() + offsets[i] + (size_t)sizes[i] * dst.start,
			src->data() + offsets[i] + (size_t)sizes[i] * srcIndex,
			(size_t)dst.count * sizes[i]
		);
}

#define srcData (s.c->data() + (size_t)offsets[i] + (size_t)sizes[i] * s.start)
void chunk::construct(chunk_slice s) noexcept
{
	archetype* type = s.c->type;
	uint32_t* offsets = type->offsets[(int)s.c->ct];
	uint16_t* sizes = type->sizes;
#ifndef NOINITIALIZE
	forloop(i, 0, type->firstBuffer)
		memset(srcData, 0, (size_t)sizes[i] * s.count);
#endif
	forloop(i, type->firstBuffer, type->firstTag)
	{
		char* src = srcData;
		forloop(j, 0, s.count)
			new((size_t)j * sizes[i] + src) buffer{ 
				static_cast<uint16_t>( static_cast<size_t>(sizes[i]) - sizeof(buffer) )
			};
	}

	tsize_t maskId = type->index(mask_id);
	if (maskId != (tsize_t)-1)
	{
		auto i = maskId;
		memset(srcData, -1, (size_t)sizes[i] * s.count);
	}
}

void chunk::destruct(chunk_slice s) noexcept
{
	archetype* type = s.c->type;
	uint32_t* offsets = type->offsets[(int)s.c->ct];
	uint16_t* sizes = type->sizes;
	index_t* types = type->types;
	forloop(i, type->firstBuffer, type->firstTag)
	{
		char* src = srcData;
		forloop(j, 0, s.count)
			((buffer*)((size_t)j * sizes[i] + src))->~buffer();
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
	if (DotsContext->tracks[t.index()] == Copying)
		return t - 1;
	else
		return t;
}

#undef srcData
#define dstData (dst.c->data() + offsets[i] + (size_t)sizes[i] * dst.start)
#define srcData (src->data() + offsets[i] + (size_t)sizes[i] * srcIndex)
void chunk::duplicate(chunk_slice dst, const chunk* src, tsize_t srcIndex) noexcept
{
	archetype* type = src->type;
	archetype* dstType = dst.c->type;
	tsize_t dstI = 0;
	uint32_t* offsets = type->offsets[(int)src->ct];
	uint32_t* dstOffsets = dstType->offsets[(int)dst.c->ct];
	uint16_t* sizes = type->sizes;
	index_t* types = type->types;
	forloop(i, 0, type->firstBuffer)
	{
		tagged_index st = to_valid_type(type->types[i]);
		tagged_index dt = to_valid_type(dstType->types[dstI]);

		if (st != dt)
			continue;
#ifdef ENABLE_GUID_COMPONENT
		if (type->types[i] == guid_id)
		{
			auto dd = dstData;
			forloop(j, 0, dst.count)
				*((GUID*)dd + j) = new_guid();
		}
		else
#endif
			memdup(dstData, srcData, sizes[i], dst.count);
		dstI++;
	}
	forloop(i, type->firstBuffer, type->firstTag)
	{
		tagged_index st = to_valid_type(type->types[i]);
		tagged_index dt = to_valid_type(dstType->types[dstI]);

		if (st != dt)
			continue;
		const char* s = srcData;
		char* d = dstData;
		memdup(dstData, srcData, sizes[i], dst.count);
		forloop(j, 0, dst.count)
		{
			buffer* b = (buffer*)((size_t)j * sizes[i] + d);
			if (b->d != nullptr)
			{
				char* clonedData = (char*)buffer_malloc(b->size);
				memcpy(clonedData, b->d, b->size);
				b->d = clonedData;
			}
		}
		dstI++;
	}
}

//todo: will compiler inline patcher?
void chunk::patch(chunk_slice s, patcher_i* patcher) noexcept
{
	archetype* g = s.c->type;
	uint32_t* offsets = g->offsets[(int)s.c->ct];
	uint16_t* sizes = g->sizes;
	tagged_index* types = (tagged_index*)g->types;
	forloop(i, 0, g->firstBuffer)
	{
		const auto& t = DotsContext->infos[types[i].index()];
		char* arr = s.c->data() + offsets[i] + (size_t)sizes[i] * s.start;
		auto f = t.vtable.patch;
		if (f != nullptr)
		{
			forloop(j, 0, s.count)
			{
				char* data = arr + (size_t)sizes[i] * j;
				forloop(k, 0, t.entityRefCount)
				{
					entity& e = *(entity*)(data + DotsContext->entityRefs[(size_t)t.entityRefs + k]);
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
				char* data = arr + (size_t)sizes[i] * j;
				forloop(k, 0, t.entityRefCount)
				{
					entity& e = *(entity*)(data + DotsContext->entityRefs[(size_t)t.entityRefs + k]);
					e = patcher->patch(e);
				}
				patcher->move();
			}
		}
		patcher->reset();
	}

	forloop(i, g->firstBuffer, g->firstTag)
	{
		const auto& t = DotsContext->infos[types[i].index()];
		char* arr = s.c->data() + offsets[i] + (size_t)sizes[i] * s.start;
		forloop(j, 0, s.count)
		{
			buffer* b = (buffer*)(arr + (size_t)sizes[i] * j);
			uint16_t n = b->size / t.elementSize;
			auto f = t.vtable.patch;
			if (f != nullptr)
			{
				forloop(l, 0, n)
				{
					char* data = b->data() + (size_t)t.elementSize * l;
					forloop(k, 0, t.entityRefCount)
					{
						entity& e = *(entity*)(data + DotsContext->entityRefs[(size_t)t.entityRefs + k]);
						e = patcher->patch(e);
					}
					f(data, patcher);
				}
			}
			else
			{
				forloop(l, 0, n)
				{
					char* data = b->data() + (size_t)t.elementSize * l;
					forloop(k, 0, t.entityRefCount)
					{
						entity& e = *(entity*)(data + DotsContext->entityRefs[(size_t)t.entityRefs + k]);
						e = patcher->patch(e);
					}
				}
			}
			patcher->move();
		}
		patcher->reset();
	}
}

template<class T>
void archive(serializer_i* stream, const T& value)
{
	stream->stream(&value, sizeof(T));
}

template<class T>
void archive(serializer_i* stream, const T* value, size_t count)
{
	stream->stream(value, static_cast<uint32_t>(sizeof(T) * count));
}


//todo: handle transient data?
void chunk::serialize(chunk_slice s, serializer_i* stream, bool withEntities)
{
	archetype* type = s.c->type;
	uint32_t* offsets = type->offsets[(int)s.c->ct];
	uint16_t* sizes = type->sizes;
	tagged_index* types = (tagged_index*)type->types;
	if(withEntities)
		archive(stream, s.c->get_entities() + s.start, s.count);

	forloop(i, 0, type->firstTag)
	{
		char* arr = s.c->data() + offsets[i] + (size_t)sizes[i] * s.start;
		archive(stream, arr, sizes[i] * s.count);
	}
	
	if (stream->is_serialize())
	{
		forloop(i, type->firstBuffer, type->firstTag)
		{
			char* arr = s.c->data() + offsets[i] + (size_t)sizes[i] * s.start;
			forloop(j, 0, s.count)
			{
				buffer* b = (buffer*)(arr + (size_t)j * sizes[i]);
				archive(stream, b->data(), b->size);
			}
		}
	}
	else
	{
		forloop(i, type->firstBuffer, type->firstTag)
		{
			char* arr = s.c->data() + offsets[i] + (size_t)sizes[i] * s.start;
			forloop(j, 0, s.count)
			{
				buffer* b = (buffer*)(arr + (size_t)j * sizes[i]);
				if (b->d != nullptr)
					b->d = (char*)malloc(b->capacity);
				archive(stream, b->data(), b->size);
			}
		}
	}
}

size_t chunk::get_size()
{
	switch (ct)
	{
	case alloc_type::fastbin:
		return kFastBinSize;
	case alloc_type::smallbin:
		return kSmallBinSize;
	case alloc_type::largebin:
		return kLargeBinSize;
	}
	return 0;
}

void chunk::cast(chunk_slice dst, chunk* src, tsize_t srcIndex, bool destruct) noexcept
{
	archetype* srcType = src->type;
	archetype* dstType = dst.c->type;
	tsize_t dstI = 0;
	tsize_t srcI = 0;
	uint32_t count = dst.count;
	uint32_t* srcOffsets = srcType->offsets[(int)src->ct];
	uint32_t* dstOffsets = dstType->offsets[(int)dst.c->ct];
	uint16_t* srcSizes = srcType->sizes;
	uint16_t* dstSizes = dstType->sizes;
	index_t* srcTypes = srcType->types; index_t* dstTypes = dstType->types;

	//phase0: cast all components
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
			memset(d, 0, (size_t)dstSizes[dstI++] * count);
#else
			dstI++;
#endif
		else //move
			memcpy(d, s, (size_t)dstSizes[(srcI++, dstI++)] * count);
	}

	srcI = srcType->firstBuffer; // destruct
#ifndef NOINITIALIZE
	while (dstI < dstType->firstBuffer) //construct
	{
		char* d = dst.c->data() + dstOffsets[dstI] + (size_t)dstSizes[dstI] * dst.start;
		memset(d, 0, (size_t)dstSizes[dstI] * count);
		dstI++;
	}
#else
	dstI = dstType->firstBuffer;
#endif

	//phase1: cast all buffers
	while (srcI < srcType->firstTag && dstI < dstType->firstTag)
	{
		auto st = to_valid_type(srcTypes[srcI]);
		auto dt = to_valid_type(dstTypes[dstI]);
		char* s = src->data() + srcOffsets[srcI] + (size_t)srcSizes[srcI] * srcIndex;
		char* d = dst.c->data() + dstOffsets[dstI] + (size_t)dstSizes[dstI] * dst.start;
		if (st < dt) //destruct 
		{
			if (destruct)
				forloop(j, 0, count)
				((buffer*)((size_t)j * srcSizes[srcI] + s))->~buffer();
			srcI++;
		}
		else if (st > dt) //construct
		{
			forloop(j, 0, count)
				new((size_t)j * dstSizes[dstI] + d) buffer{ 
					static_cast<uint16_t>( static_cast<size_t>(dstSizes[dstI]) - sizeof(buffer) )
				};
			dstI++;
		}
		else //move
		{
			memcpy(d, s, (size_t)dstSizes[dstI] * count);
			dstI++; srcI++;
		}
	}

	if (destruct)
		while (srcI < srcType->firstTag) //destruct 
		{
			char* s = src->data() + srcOffsets[srcI] + (size_t)srcSizes[srcI] * srcIndex;
			forloop(j, 0, count)
				((buffer*)((size_t)j * srcSizes[srcI] + s))->~buffer();
			srcI++;
		}
	else
		srcI = srcType->firstTag;
	while (dstI < dstType->firstTag) //construct
	{
		char* d = dst.c->data() + dstOffsets[dstI] + (size_t)dstSizes[dstI] * dst.start;
		forloop(j, 0, count)
			new((size_t)j * dstSizes[dstI] + d) buffer{ 
				static_cast<uint16_t>( static_cast<size_t>(dstSizes[dstI]) - sizeof(buffer) )
			};
		dstI++;
	}


	//phase2: maintain mask for new archetype
	tsize_t srcMaskId = srcType->index(mask_id);
	tsize_t dstMaskId = dstType->index(mask_id);
	if (srcMaskId != InvalidIndex && dstMaskId != InvalidIndex)
	{
		mask* s = (mask*)(src->data() + srcOffsets[srcMaskId] + (size_t)srcSizes[srcMaskId] * srcIndex);
		mask* d = (mask*)(dst.c->data() + dstOffsets[dstMaskId] + (size_t)dstSizes[dstMaskId] * dst.start);
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
					if (s[i].test(srcI))
						d[i].set(dstI);
			}
			while (dstI < dstType->componentCount)
				d[i].set(dstI);
		}
	}
	else if (dstMaskId != InvalidIndex)
	{
		mask* d = (mask*)(dst.c->data() + dstOffsets[dstMaskId] + (size_t)dstSizes[dstMaskId] * dst.start);
		memset(d, -1, (size_t)dstSizes[dstMaskId] * count);
	}
}

uint32_t* archetype::timestamps(chunk* c) const noexcept { return (uint32_t*)((char*)c + c->get_size()) - firstTag; }

tsize_t archetype::index(index_t type) const noexcept
{
	index_t* ts = types;
	index_t* result = std::lower_bound(ts, ts + componentCount, type);
	if (result != ts + componentCount && *result == type)
		return tsize_t(result - ts);
	else
		return tsize_t(-1);
}

mask archetype::get_mask(const typeset& subtype) noexcept
{
	mask ret;
	auto ta = get_type().types;
	auto tb = subtype;
	tsize_t i = 0, j = 0;
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

entity_type archetype::get_type() const
{
	index_t* ts = types;
	entity* ms = metatypes;
	return entity_type
	{
		typeset { ts, componentCount },
		metaset { ms, metaCount }
	};

}

size_t archetype::get_size()
{
	return sizeof(index_t) * componentCount + // types
		sizeof(uint32_t) * firstTag * 3 + // offsets
		sizeof(uint32_t) * firstTag + // sizes
		sizeof(entity) * metaCount; // metatypes
}

world::query_cache& world::get_query_cache(const archetype_filter& f) const
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
				if ((DotsContext->tracks[type.index()] & Copying) != 0)
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


			tsize_t size = 0;
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
				mask m = g->get_mask(checking);
				cache.archetypes.push_back({ g, m });
			}
		}
		cache.includeClean = includeClean;
		cache.includeDisabled = includeDisabled;
		auto totalSize = f.get_size();
		cache.data.reset(new char[totalSize]);
		char* data = cache.data.get();
		cache.filter = f.clone(data);
		queries[cache.filter] = std::move(cache);
		return queries[cache.filter];
	}
}

void world::update_queries(archetype* g, bool add)
{
	if(on_archetype_update)
		on_archetype_update(g, add);
	tsize_t size = 0;
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
			if (!add)
			{
				int j = 0;
				auto count = gs.size();
				for (; j < count && gs[j].type != g; ++j);
				if(j == count)
					continue;
				if (j != (count - 1))
					std::swap(gs[j], gs[count - 1]);
				gs.pop_back();
			}
			else
				gs.push_back({ g });
		}
	}
}

void world::remove(chunk*& h, chunk*& t, chunk* c)
{
	if (c == t)
		t = t->prev;
	if (h == c)
		h = h->next;
	c->unlink();
}

template<class T>
void allocate_inline(T*& target, char*& buf, int count)
{
	target = (T*)buf;
	buf += count * sizeof(T);
}

archetype* world::get_archetype(const entity_type& key)
{
	auto iter = archetypes.find(key);
	if (iter != archetypes.end())
		return iter->second;
	auto g = construct_archetype(key);
	add_archetype(g);
	return g;
}

archetype* world::find_archetype(const entity_type& key)
{
	auto iter = archetypes.find(key);
	if (iter != archetypes.end())
		return iter->second;
	return nullptr;
}

archetype* world::get_archetype(chunk_slice s) const noexcept
{
	return s.c->type;
}

archetype* world::construct_archetype(const entity_type& key)
{
	//字典序分割
	const tsize_t count = key.types.length;
	tsize_t firstTag = 0;
	tsize_t firstBuffer = 0;
	tsize_t metaCount = key.metatypes.length;
	tsize_t c = 0;
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

	//计算基本数据和 flag
	archetype proto;
	proto.componentCount = count;
	proto.firstTag = firstTag;
	proto.metaCount = metaCount;
	proto.componentCount = count;
	proto.metaCount = metaCount;
	proto.firstBuffer = firstBuffer;
	proto.firstTag = firstTag;
	proto.cleaning = false;
	proto.copying = false;
	proto.disabled = false;
	proto.withMask = false;
	proto.withTracked = false;
	proto.zerosize = false;
	proto.chunkCount = 0;
	proto.size = 0;
	proto.timestamp = timestamp;
	proto.lastChunk = proto.firstChunk = proto.firstFree = nullptr;

	const index_t disableType = disable_id;
	const index_t cleanupType = cleanup_id;
	const index_t maskType = mask_id;
	uint16_t entitySize = sizeof(entity);
	forloop(i, 0, count)
	{
		auto type = (tagged_index)key.types[i];
		if (type == disableType)
			proto.disabled = true;
		else if (type == cleanupType)
			proto.cleaning = true;
		else if (type == maskType)
			proto.withMask = true;
		if ((DotsContext->tracks[type.index()] & NeedCC) != 0)
			proto.withTracked = true;
		if ((DotsContext->tracks[type.index()] & Copying) != 0)
			proto.copying = true;
	}

	//生成内敛数组
	archetype* g = new(malloc(proto.get_size() + sizeof(archetype))) archetype(proto);
	char* buffer = (char*)(g + 1);
	allocate_inline(g->types, buffer, count);
	allocate_inline(g->offsets[0], buffer, firstTag);
	allocate_inline(g->offsets[1], buffer, firstTag);
	allocate_inline(g->offsets[2], buffer, firstTag);
	allocate_inline(g->sizes, buffer, firstTag);
	allocate_inline(g->metatypes, buffer, metaCount);
	index_t* types = g->types;
	entity* metatypes = g->metatypes;
	memcpy(types, key.types.data, count * sizeof(index_t));
	memcpy(metatypes, key.metatypes.data, key.metatypes.length * sizeof(entity));
	uint16_t* sizes = g->sizes;

	stack_array(size_t, align, firstTag);
	stack_array(core::GUID, hash, firstTag);
	stack_array(tsize_t, stableOrder, firstTag);
	forloop(i, 0, firstTag)
	{
		auto type = (tagged_index)key.types[i];
		auto& info = DotsContext->infos[type.index()];
		sizes[i] = info.size;
		hash[i] = info.GUID;
		align[i] = info.alignment;
		stableOrder[i] = i;
		entitySize += info.size;
	}
	if (entitySize == sizeof(entity))
		g->zerosize = true;
	g->entitySize = entitySize;
	size_t Caps[] = { kSmallBinSize, kFastBinSize, kLargeBinSize };
	std::sort(stableOrder, stableOrder + firstTag, [&](tsize_t lhs, tsize_t rhs)
		{
			return hash[lhs] < hash[rhs];
		});
	forloop(i, 0, 3)
	{
		uint32_t* offsets = g->offsets[(int)(alloc_type)i];
		g->chunkCapacity[i] = (uint32_t)(Caps[i] - sizeof(chunk) - sizeof(uint32_t) * firstTag) / entitySize;
		if (g->chunkCapacity[i] == 0)
			continue;
		uint32_t offset = sizeof(entity) * g->chunkCapacity[i];
		forloop(j, 0, firstTag)
		{
			tsize_t id = stableOrder[j];
			offset = static_cast<uint32_t>(
				align[id] * ((offset + align[id] - 1) / align[id]));
			offsets[id] = offset;
			offset += sizes[id] * g->chunkCapacity[i];
		}
	}
	return g;
}

void world::add_archetype(archetype* g)
{
	update_queries(g, true);
	archetypes.insert({ g->get_type(), g });
}

archetype* world::get_cleaning(archetype* g)
{
	if (g->cleaning) return g;
	else if (!g->withTracked) return nullptr;

	tsize_t k = 0, count = g->componentCount;
	const index_t cleanupType = cleanup_id;
	index_t* types = g->types;
	entity* metatypes = g->metatypes;

	stack_array(index_t, dstTypes, count + 1);
	dstTypes[k++] = cleanupType;
	forloop(i, 0, count)
	{
		auto type = (tagged_index)types[i];
		auto stage = DotsContext->tracks[type.index()];
		if ((stage & ManualCleaning) != 0)
			dstTypes[k++] = type;
	}
	std::sort(dstTypes, dstTypes + k);
	if (k == 1)
		return nullptr;

	auto dstKey = entity_type
	{
		typeset {dstTypes, k},
		metaset {metatypes, g->metaCount}
	};

	return get_archetype(dstKey);
}

bool world::is_cleaned(const entity_type& type)
{
	return type.types.length == 1;
}

archetype* world::get_casted(archetype* g, type_diff diff, bool inst)
{
	if (inst && g->cleaning)
		return nullptr;

	entity_type srcType = g->get_type();
	const index_t* srcTypes = srcType.types.data;
	const entity* srcMetaTypes = srcType.metatypes.data;
	const index_t* shrTypes = diff.shrink.types.data;
	index_t* dstTypes;
	entity* dstMetaTypes;
	tsize_t srcSize = srcType.types.length;
	tsize_t srcMetaSize = srcType.metatypes.length;
	tsize_t dstSize = srcType.types.length;
	tsize_t dstMetaSize = srcType.metatypes.length;
	tsize_t shrSize = diff.shrink.types.length;
	std::optional<stack_object> _so_phase0;
	std::optional<stack_object> _so_phase1_1;
	std::optional<stack_object> _so_phase1_2;
	std::optional<stack_object> _so_phase2;
	std::optional<stack_object> _so_phase3;
	std::optional<stack_object> _so_phase4_1;
	std::optional<stack_object> _so_phase4_2;

	//phase 0 : start copying state duto instantiate
	if (inst && g->withTracked)
	{
		_so_phase0.emplace(sizeof(index_t) * dstSize);
		dstTypes = (index_t*)_so_phase0->self;

		forloop(i, 0, srcSize)
		{
			auto type = (tagged_index)srcTypes[i];
			auto stage = DotsContext->tracks[type.index()];
			if ((stage & ManualCopying) != 0)
				dstTypes[i] = type + 1;
			else
				dstTypes[i] = type;
		}

		srcTypes = dstTypes;
	}

	//phase 1 : extend
	if (diff.extend != EmptyType)
	{
		dstSize = srcSize + diff.extend.types.length;
		dstMetaSize = srcMetaSize + diff.extend.metatypes.length;
		_so_phase1_1.emplace(sizeof(index_t) * dstSize);
		dstTypes = (index_t*)_so_phase1_1->self;
		_so_phase1_2.emplace(sizeof(entity) * dstMetaSize);
		dstMetaTypes = (entity*)_so_phase1_2->self;
		auto key = entity_type::merge(
			{ {srcTypes, srcSize}, {srcMetaTypes, srcMetaSize} },
			diff.extend, dstTypes, dstMetaTypes);
		srcTypes = dstTypes; srcMetaTypes = dstMetaTypes;
		srcSize = key.types.length; srcMetaSize = key.metatypes.length;
	}

	//phase 2 : complete copying duto extend
	if (g->copying && diff.extend != EmptyType)
	{
		_so_phase2.emplace(sizeof(index_t) * srcSize);
		tsize_t k = 0, mk = 0;
		dstTypes = (index_t*)_so_phase2->self;
		auto can_zip = [&](int i)
		{
			auto type = (tagged_index)srcTypes[i];
			auto stage = DotsContext->tracks[type.index()];
			if (((stage & ManualCopying) != 0) && (srcTypes[i + 1] == type + 1))
				return true;
			return false;
		};
		forloop(i, 0, srcSize)
		{
			auto type = (tagged_index)srcTypes[i];
			dstTypes[k++] = type;
			if (i < srcSize - 1 && can_zip(i))
				i++;
		}
		srcTypes = dstTypes;
		srcSize = k;
	}

	//phase 3 : interupt copying duto shrink
	if (g->copying && diff.shrink != EmptyType)
	{
		_so_phase3.emplace(sizeof(index_t) * diff.shrink.types.length * 2);
		index_t* newShrTypes = (index_t*)_so_phase3->self;
		tsize_t k = 0;
		forloop(i, 0, shrSize)
		{
			auto type = (tagged_index)shrTypes[i];
			newShrTypes[k++] = type;
			auto stage = DotsContext->tracks[type.index()];
			if ((stage & ManualCopying) != 0)
				newShrTypes[k++] = type + 1;
		}
		shrTypes = newShrTypes;
		shrSize = k;
	}

	//phase 4 : shrink
	if (diff.shrink != EmptyType)
	{
		_so_phase4_1.emplace(sizeof(index_t) * srcSize);
		dstTypes = (index_t*)_so_phase4_1->self;
		_so_phase4_2.emplace(sizeof(entity) * srcMetaSize);
		dstMetaTypes = (entity*)_so_phase4_2->self;
		auto key = entity_type::substract(
			{ {srcTypes, srcSize}, {srcMetaTypes, srcMetaSize} },
			{ {shrTypes, shrSize}, diff.shrink.metatypes }, dstTypes, dstMetaTypes);
		srcTypes = dstTypes; srcMetaTypes = dstMetaTypes;
		srcSize = key.types.length; srcMetaSize = key.metatypes.length;
	}

	//phase 5 : check cleaning
	if (g->cleaning)
	{
		entity_type key = { {srcTypes, srcSize}, {srcMetaTypes, srcMetaSize} };
		if (is_cleaned(key))
			return nullptr;
	}

	return get_archetype({ {srcTypes, srcSize}, {srcMetaTypes, srcMetaSize} });
}

chunk* world::malloc_chunk(alloc_type type)
{
	chunk* c = (chunk*)DotsContext->malloc(type);
	c->ct = type;
	c->count = 0;
	c->prev = c->next = nullptr;
	return c;
}

chunk* world::new_chunk(archetype* g, uint32_t hint)
{
	chunk* c = nullptr;
	auto size = hint * g->entitySize;
	if (g->chunkCount < kSmallBinThreshold && size < kSmallBinSize)
		c = malloc_chunk(alloc_type::smallbin);
	else if (size > kFastBinSize * 8u)
		c = malloc_chunk(alloc_type::largebin);
	else
		c = malloc_chunk(alloc_type::fastbin);
	add_chunk(g, c);

	return c;
}

void world::add_chunk(archetype* g, chunk* c)
{
	structural_change(g, c);
	g->size += c->count;
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

void world::remove_chunk(archetype* g, chunk* c)
{
	structural_change(g, c);
	g->size -= c->count;
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

void world::mark_free(archetype* g, chunk* c)
{
	remove(g->firstChunk, g->lastChunk, c);
	if(g->lastChunk)
		g->lastChunk->link(c);
	g->lastChunk = c;
	if (g->firstFree == nullptr)
		g->firstFree = c;
	if (g->firstChunk == nullptr)
		g->firstChunk = c;
}

void world::unmark_free(archetype* g, chunk* c)
{
	if (g->firstFree == c)
	{
		g->firstFree = c->next;
		return;
	}
	remove(g->firstChunk, g->lastChunk, c);
	if (g->firstChunk)
	{
		c->next = g->firstChunk->next;
		c->link(g->firstChunk);
	}
	g->firstChunk = c;
	if (g->lastChunk == nullptr)
		g->lastChunk = c;
}

void world::release_reference(archetype* g)
{
	//todo: does this make sense?
}

void world::serialize_archetype(archetype* g, serializer_i* s)
{
	archive(s, g->size);
	entity_type type = g->get_type();
	tsize_t tlength = type.types.length, mlength = type.metatypes.length;
	archive(s, tlength);
	forloop(i, 0, tlength)
		archive(s, DotsContext->infos[tagged_index(type.types[i]).index()].GUID);
	archive(s, mlength);
	archive(s, type.metatypes.data, mlength);
}

archetype* world::deserialize_archetype(serializer_i* s, patcher_i* patcher, bool createNew)
{
	uint32_t size = 0;
	archive(s, size);
	if (size == 0)
		return nullptr;
	tsize_t tlength;
	archive(s, tlength);
	stack_array(index_t, types, tlength);
	forloop(i, 0, tlength)
	{
		core::GUID uu;
		archive(s, uu);
		//todo: check validation
		types[i] = (index_t)DotsContext->hash2type[uu];
	}
	tsize_t mlength;
	archive(s, mlength);
	stack_array(entity, metatypes, mlength);
	archive(s, metatypes, mlength);
	if (patcher)
		forloop(i, 0, mlength)
			metatypes[i] = patcher->patch(metatypes[i]);
	if (!createNew)
	{
		for (tsize_t i = 0; i < mlength;) //remove invalid metas
			if (!exist(metatypes[i]))
				std::swap(metatypes[i], metatypes[--mlength]);
			else
				++i;
	}
	std::sort(types, types + tlength);
	std::sort(metatypes, metatypes + mlength);
	entity_type type = { {types, tlength}, {metatypes, mlength} };
	auto g = createNew ? construct_archetype(type) : get_archetype(type);
	g->size += size;
	return g;
}

bool world::deserialize_slice(archetype* g, serializer_i* s, chunk_slice& slice)
{
	uint32_t count;
	archive(s, count);
	if (count == 0)
		return false;
	chunk* c;
	c = g->firstFree;
	while (c && (c->count + count) > g->chunkCapacity[(int)c->ct])
		c = c->next;
	if (c == nullptr)
	{
		auto size = count * g->entitySize;
		if (g->chunkCount < kSmallBinThreshold && size < kSmallBinSize)
			c = malloc_chunk(alloc_type::smallbin);
		else if (size > kFastBinSize)
			c = malloc_chunk(alloc_type::largebin);
		else
			c = malloc_chunk(alloc_type::fastbin);
		add_chunk(g, c);
	}
	uint32_t start = c->count;
	resize_chunk(c, start + count);
	slice = { c, start, count };
	chunk::serialize(slice, s);
	return true;
}

void world::serialize_slice(const chunk_slice& slice, serializer_i* s)
{
	archive(s, slice.count);
	chunk::serialize(slice, s);
}

void world::group_to_prefab(entity* src, uint32_t size, bool keepExternal)
{
	//TODO: should we patch meta?
	struct patcher final : patcher_i
	{
		entity* source;
		uint32_t count;
		bool keepExternal;
		entity patch(entity e) override
		{
			forloop(i, 0, count)
				if (e == source[i])
					return entity::make_transient(i);
			return keepExternal ? e : NullEntity;
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

void world::prefab_to_group(entity* members, uint32_t size)
{
	struct patcher final : patcher_i
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

chunk_vector<chunk_slice> world::instantiate_group(buffer* group, uint32_t count)
{
	uint32_t size = group->size / sizeof(entity);
	AO(entity, members, group->size);
	memcpy(members, group->data(), group->size);
	group_to_prefab(members, size);
	auto result = instantiate_prefab(members, size, count);
	prefab_to_group(members, size);
	return result;
}

chunk_vector<chunk_slice> world::instantiate_prefab(entity* src, uint32_t size, uint32_t count)
{
	chunk_vector<chunk_slice> allSlices;
	
	AO(entity, ret, size * count);
	AO(uint32_t, scount, size);
	memset(scount, 0, sizeof(uint32_t) * size);

	forloop(i, 0, size)
	{
		auto e = src[i];
		auto g = instantiate_single(e, count);
		uint32_t k = 0;
		for (auto s : g)
		{
			allSlices.push(s);
			scount[i]++;
			forloop(j, 0, s.count)
				ret[k++ * size + i] = s.c->get_entities()[s.start + j];
		}
	}

	struct patcher final : patcher_i
	{
		entity* base;
		entity* curr;
		uint32_t count;
		void move() override { curr += count; }
		void reset() override { curr = base; }
		entity patch(entity e) override
		{
			if (e.id > count || !e.is_transient())
				return e;
			else
				return curr[e.id];
		}
	} p;
	p.count = size;
	uint32_t x = 0;
	//todo: multithread?
	forloop(i, 0, size)
	{
		uint32_t k = 0;
		forloop(j, 0, scount[i])
		{
			auto s = allSlices[x++];
			p.curr = p.base = &ret[k * size];
			chunk::patch(s, &p);
			k += s.count;
		}
	}
	return allSlices;
}

chunk_vector<chunk_slice> world::instantiate_single(entity src, uint32_t count)
{
	chunk_vector<chunk_slice> result;
	const auto& data = ents.datas[src.id];
	archetype* g = get_casted(data.c->type, {}, true);
	uint32_t k = 0;
	while (k < count)
	{
		chunk_slice s = allocate_slice(g, count - k);
		chunk::duplicate(s, data.c, data.i);
		ents.new_entities(s);
		k += s.count;
		result.push(s);
	}
	return result;
}

void world::serialize_single(serializer_i* s, entity src)
{
	const auto& data = ents.datas[src.id];
	serialize_archetype(data.c->type, s);
	serialize_slice({ data.c, data.i, 1 }, s);
}

void world::serialize_group(serializer_i* s, buffer* group)
{
	uint32_t size = group->size / sizeof(entity);
	stack_array(entity, members, size);
	memcpy(members, group->data(), group->size);
	group_to_prefab(members, size, false);
	forloop(i, 0, size)
	{
		serialize_single(s, members[i]);
	}
	prefab_to_group(members, size);
}

void world::structural_change(archetype* g, chunk* c)
{
	entity_type t = g->get_type();

	if (g->timestamp != timestamp)
	{
		g->timestamp = timestamp;
		forloop(i, 0, t.types.length)
		{
			auto type = (tagged_index)t.types[i];
			typeTimestamps[type.index()] = timestamp;
		}
	}
	auto timestamps = g->timestamps(c);
	forloop(i, 0, t.types.length)
		timestamps[i] = timestamp;
}

chunk_slice world::deserialize_single(serializer_i* s, patcher_i* patcher)
{
	auto* g = deserialize_archetype(s, patcher, false);
	chunk_slice slice;
	deserialize_slice(g, s, slice);
	ents.new_entities(slice);
	if(patcher)
		chunk::patch(slice, patcher);
	return slice;
}

void world::destroy_single(chunk_slice s)
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



void world::destroy_chunk(archetype* g, chunk* c)
{
	remove_chunk(g, c);
	recycle_chunk(c);
}

void world::recycle_chunk(chunk* c)
{
	DotsContext->free(c->ct, c);
}

void world::resize_chunk(chunk* c, uint32_t count)
{
	archetype* g = c->type;
	if (count == 0)
		destroy_chunk(g, c);
	else
	{
		if (count == g->chunkCapacity[(int)c->ct])
			unmark_free(g, c);
		else if (c->count == g->chunkCapacity[(int)c->ct])
			mark_free(g, c);
		c->count = count;
	}
}

void world::merge_chunks(archetype* g)
{
	//zero or one chunk
	if (g->firstChunk == nullptr || g->firstChunk->next == nullptr)
		return;

	//sort by capacity and fill rate
	auto unsorted = g->firstChunk->next;
	chunk* toSort = unsorted;
	while (unsorted != nullptr)
	{
		toSort = unsorted;
		unsorted = unsorted->next;
		auto iter = g->firstChunk;
		while (iter != unsorted)
		{
			if ((int)iter->ct < (int)toSort->ct || iter->count < toSort->count)
			{
				toSort->unlink();
				toSort->prev = iter->prev;
				toSort->next = iter;
				if (iter->prev != nullptr)
				{
					iter->prev->next = toSort;
					iter->prev = toSort;
				}
				else
				{
					iter->prev = toSort;
					g->firstChunk = toSort;
				}
				break;
			}
			iter = iter->next;
		}
	}
	if (toSort != nullptr)
		g->lastChunk = toSort;

	//merge into large chunk if can
	auto mergeableSize = g->size;
	auto iter = g->firstChunk;
	auto largeSize = g->chunkCapacity[(int)alloc_type::largebin];
	while (iter->ct == alloc_type::largebin)
	{
		mergeableSize = mergeableSize > largeSize ?
			mergeableSize - largeSize : 0u;
	}
	while (mergeableSize > largeSize)
	{
		auto c = malloc_chunk(alloc_type::largebin);
		g->firstChunk = c;
		mergeableSize -= largeSize;
	}

	//merge smallest chunk into largest chunk
	auto dst = g->firstChunk;
	while (dst != g->lastChunk)
	{
		auto src = g->lastChunk;
		auto fullSize = g->chunkCapacity[(int)dst->ct];
		auto freeSize = fullSize - dst->count;
		auto moveSize = std::min(freeSize, src->count);
		dst->move({ dst, dst->count, moveSize }, src, src->count - moveSize);
		ents.move_entities({ dst, dst->count, moveSize }, src, src->count - moveSize);
		dst->count += moveSize;
		src->count -= moveSize;
		if (dst->count == fullSize)
			dst = dst->next;
		if (src->count == 0)
			destroy_chunk(g, src);
	}

	//maintain firstFree
	if (dst->count != g->chunkCapacity[(int)dst->ct])
		g->firstFree = dst;
	else
		g->firstFree = nullptr;
}

chunk_slice world::allocate_slice(archetype* g, uint32_t count)
{
	chunk* c = g->firstFree;
	if (c == nullptr)
		c = new_chunk(g, count);
	structural_change(g, c);
	uint32_t start = c->count;
	uint32_t allocated = std::min(count, g->chunkCapacity[(int)c->ct] - start);
	g->size += allocated;
	resize_chunk(c, start + allocated);
	return { c, start, allocated };
}

void world::free_slice(chunk_slice s)
{
	archetype* g = s.c->type;
	structural_change(g, s.c);
	g->size -= s.count;
	uint32_t toMoveCount = std::min(s.count, s.c->count - s.start - s.count);
	if (toMoveCount > 0)
	{
		chunk_slice moveSlice{ s.c, s.start, toMoveCount };
		chunk::move(moveSlice, s.c->count - toMoveCount);
		ents.fill_entities(moveSlice, s.c->count - toMoveCount);
	}
	resize_chunk(s.c, s.c->count - s.count);
}

chunk_vector<chunk_slice> world::cast_slice(chunk_slice src, archetype* g)
{
	chunk_vector<chunk_slice> result;
	archetype* srcG = src.c->type;
	structural_change(srcG, src.c);
	srcG->size -= src.count;
	uint32_t k = 0;
	while (k < src.count)
	{
		chunk_slice s = allocate_slice(g, src.count - k);
		chunk::cast(s, src.c, src.start + k);
		ents.move_entities(s, src.c, src.start + k);
		k += s.count;
		result.push(s);
	}
	free_slice(src);
	return result;
}

bool static_castable(const entity_type& typeA, const entity_type& typeB)
{
	int size = std::min(typeA.types.length, typeB.types.length);
	int i = 0;
	for (; i < size; ++i)
	{
		tagged_index st = typeA.types[i];
		tagged_index dt = typeB.types[i];
		if (st.is_tag() && dt.is_tag())
			return true;
		if (to_valid_type(st) != to_valid_type(dt))
			return false;
	}
	if (typeA.types.length == typeB.types.length)
		return true;
	else if (i >= typeA.types.length)
		return tagged_index(typeB.types[i]).is_tag();
	else if (i >= typeB.types.length)
		return tagged_index(typeA.types[i]).is_tag();
	else
		return false;
}

chunk_vector<chunk_slice> world::cast(chunk_slice s, archetype* g)
{
	if (g == nullptr)
	{
		ents.free_entities(s);
		free_slice(s);
		return {};
	}
	archetype* srcG = s.c->type;
	entity_type srcT = srcG->get_type();
	if (srcG == g)
		return {};
	else if (s.full() && static_castable(srcT, g->get_type()))
	{
		remove_chunk(srcG, s.c);
		add_chunk(g, s.c);
		return {};
	}
	else
	{
		return cast_slice(s, g);
	}
}

world::world(index_t typeCapacity)
	:typeCapacity(typeCapacity)
{
	typeTimestamps = (uint32_t*)::malloc(typeCapacity * sizeof(uint32_t));
	memset(typeTimestamps, 0, typeCapacity * sizeof(uint32_t));
}

world::world(const world& other)
{
	auto& src = const_cast<world&>(other);
	timestamp = src.timestamp;
	typeCapacity = src.typeCapacity;
	typeTimestamps = (uint32_t*)::malloc(typeCapacity * sizeof(uint32_t));
	std::fill(typeTimestamps, typeTimestamps + typeCapacity, 0);
	src.ents.clone(&ents);
	for (auto& iter : src.archetypes)
	{
		auto g = iter.second;
		if (g->cleaning) // skip dead entities
			continue;
		auto size = g->get_size() + sizeof(archetype);
		archetype* newG = (archetype*)::malloc(size);
		memcpy(newG, g, size);

		// mark copying stage
		forloop(i, 0, newG->componentCount)
		{
			auto type = (tagged_index)newG->types[i];
			if ((DotsContext->tracks[type.index()] & ManualCopying) != 0)
				newG->types[i] = index_t(type) + 1;
		}

		//clone chunks
		newG->firstChunk = newG->firstFree = newG->lastChunk = nullptr;
		for (auto c = g->firstChunk; c != nullptr; c = c->next)
		{
			auto newC = malloc_chunk(c->ct);
			c->clone(newC);
			add_chunk(newG, c);
		}
		add_archetype(newG);
	}
}

world::world(world&& other)
	:archetypes(std::move(other.archetypes)),
	queries(std::move(other.queries)),
	ents(std::move(other.ents)),
	typeTimestamps(other.typeTimestamps),
	typeCapacity(other.typeCapacity),
	timestamp(other.timestamp)
{
	other.typeTimestamps = nullptr;
}

world::~world()
{
	clear();
	if(typeTimestamps != nullptr)
		free(typeTimestamps);
}

void world::operator=(world&& other)
{
	archetypes = std::move(other.archetypes);
	queries = std::move(other.queries);
	ents = std::move(other.ents);
	typeTimestamps = other.typeTimestamps;
	other.typeTimestamps = nullptr;
	typeCapacity = other.typeCapacity;
	timestamp = other.timestamp;
}

chunk_vector<chunk_slice> world::allocate(const entity_type& type, uint32_t count)
{
	archetype* g = get_archetype(type);
	return allocate(g, count);
}

chunk_vector<chunk_slice> world::allocate(archetype* g, uint32_t count)
{
	chunk_vector<chunk_slice> result;
	uint32_t k = 0;

	while (k < count)
	{
		chunk_slice s = allocate_slice(g, count - k);
		chunk::construct(s);
		ents.new_entities(s);
		k += s.count;
		result.push(s);
	}
	return result;
}

chunk_vector<chunk_slice> world::instantiate(entity src, uint32_t count)
{
	auto group_data = (buffer*)get_component_ro(src, group_id);
	if (group_data == nullptr)
	{
		return instantiate_single(src, count);
	}
	else
	{
		return instantiate_group(group_data, count);
	}
}

batch_range world::batch(const entity* ents, uint32_t count) const
{
	return {*this, ents, count};
}

batch_iter world::iter(const entity* es, uint32_t count) const
{
	batch_iter iter{ es,count,0 };
	next(iter);
	return iter;
}

bool world::next(batch_iter& iter) const
{
	const auto& datas = ents.datas;
	const auto& start = datas[iter.es[iter.i++].id];
	chunk_slice ns{ start.c, start.i, 1 };
	while (iter.i < iter.count)
	{
		const auto& curr = datas[iter.es[iter.i].id];
		if (curr.i != start.i + ns.count || curr.c != start.c)
		{
			iter.s = ns;
			return true;
		}
		iter.i++; ns.count++;
	}
	return false;
}

archetype_filter world::cache_query(const archetype_filter& type)
{
	auto& cache = get_query_cache(type);
	return cache.filter;
}

void world::estimate_shared_size(tsize_t& size, archetype* t) const
{
	entity* metas = t->metatypes;
	forloop(i, 0, t->metaCount)
	{
		auto g = get_archetype(metas[i]);
		size += g->componentCount;
		estimate_shared_size(size, g);
	}
}

void world::get_shared_type(typeset& type, archetype* t, typeset& buffer) const
{
	entity* metas = t->metatypes;
	forloop(i, 0, t->metaCount)
	{
		auto g = get_archetype(metas[i]);
		std::swap(type, buffer);
		type = typeset::merge(g->get_type().types, buffer, (index_t*)type.data);
		get_shared_type(type, g, buffer);
	}
}

void world::destroy(chunk_slice s)
{
	archetype* g = s.c->type;
	tsize_t id = g->index(group_id);
	if (id != InvalidIndex)
	{
		uint16_t* sizes = g->sizes;
		char* src = (s.c->data() + g->offsets[(int)s.c->ct][id]);
		forloop(i, 0, s.count)
		{
			auto* group_data = (buffer*)(src + (size_t)i * sizes[i]);
			uint16_t size = group_data->size / sizeof(entity);
			forloop(j, 1, size)
			{
				entity e = ((entity*)group_data->data())[i];
				auto& data = ents.datas[i];
				//todo: we could batch instantiated prefab group
				destroy_single({ data.c, data.i, 1 });
			}
		}
	}
	destroy_single(s);
}

chunk_vector<chunk_slice> world::cast(chunk_slice s, type_diff diff)
{
	archetype* g = get_casted(s.c->type, diff);
	return cast(s, g);
}

chunk_vector<chunk_slice> world::cast(chunk_slice s, const entity_type& type)
{
	archetype* g = get_archetype(type);
	return cast(s, g);
}

const void* world::get_component_ro(entity e, index_t type) const noexcept
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
	return c->data() + (size_t)g->offsets[(int)c->ct][id] + (size_t)data.i * g->sizes[id];
}

const void* world::get_owned_ro(entity e, index_t type) const noexcept
{
	if (!exist(e))
		return nullptr;
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	tsize_t id = g->index(type);
	if (id == InvalidIndex || id >= g->firstTag)
		return nullptr;
	return c->data() + g->offsets[(int)c->ct][id] + (size_t)data.i * g->sizes[id];
}

const void* world::get_shared_ro(entity e, index_t type) const noexcept
{
	if (!exist(e))
		return nullptr;
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	return get_shared_ro(g, type);
}

void* world::get_owned_rw(entity e, index_t type) const noexcept
{
	if (!exist(e))
		return nullptr;
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	tsize_t id = g->index(type);
	if (id == InvalidIndex || id >= g->firstTag)
		return nullptr;
	g->timestamps(c)[id] = timestamp;
	return c->data() + g->offsets[(int)c->ct][id] + (size_t)data.i * g->sizes[id];
}


const void* world::get_component_ro(chunk* c, index_t t) const noexcept
{
	archetype* g = c->type;
	tsize_t id = g->index(t);
	if (id >= g->firstTag)
		return nullptr;
	if (id == InvalidIndex)
		return get_shared_ro(g, t);
	return c->data() + c->type->offsets[(int)c->ct][id];
}

const void* world::get_owned_ro(chunk* c, index_t t) const noexcept
{
	tsize_t id = c->type->index(t);
	if (id == InvalidIndex || id >= c->type->firstTag)
		return nullptr;
	return c->data() + c->type->offsets[(int)c->ct][id];
}

const void* world::get_shared_ro(chunk* c, index_t type) const noexcept
{
	archetype* g = c->type;
	return get_shared_ro(g, type);
}

void* world::get_owned_rw(chunk* c, index_t t) noexcept
{
	tsize_t id = c->type->index(t);
	if (id == InvalidIndex || id >= c->type->firstTag) 
		return nullptr;
	c->type->timestamps(c)[id] = timestamp;
	return c->data() + c->type->offsets[(int)c->ct][id];
}

const void* world::get_owned_ro_local(chunk* c, index_t type) const noexcept
{
	return c->data() + c->type->offsets[(int)c->ct][type];
}

void* world::get_owned_rw_local(chunk* c, index_t type) noexcept
{
	c->type->timestamps(c)[type] = timestamp;
	return c->data() + c->type->offsets[(int)c->ct][type];
}

const void* world::get_shared_ro(archetype* g, index_t type) const
{
	entity* metas = g->metatypes;
	forloop(i, 0, g->metaCount)
		if (const void* shared = get_component_ro(metas[i], type))
			return shared;
	return nullptr;
}

bool world::share_component(archetype* g, const typeset& type) const
{
	entity* metas = g->metatypes;
	forloop(i, 0, g->metaCount)
		if (has_component(metas[i], type))
			return true;
	return false;
}

bool world::own_component(archetype* g, const typeset& type) const
{
	return g->get_type().types.all(type);
}

bool world::has_component(archetype* g, const typeset& type) const
{
	if (own_component(g, type))
		return true;
	else
		return share_component(g, type);
}

archetype* world::get_archetype(entity e) const noexcept
{
	if (!exist(e))
		return nullptr;
	const auto& data = ents.datas[e.id];
	return data.c->type;
}

const entity* world::get_entities(chunk* c) noexcept
{
	return c->get_entities();
}

uint16_t world::get_size(chunk* c, index_t t) const noexcept
{
	tsize_t id = c->type->index(t);
	if (id == InvalidIndex || id > c->type->firstTag) 
		return 0;
	return c->type->sizes[id];
}

entity_type world::get_type(entity e) const noexcept
{
	const auto& data = ents.datas[e.id];
	return data.c->type->get_type();
}

chunk_vector<entity> world::gather_reference(entity e)
{
	chunk_vector<entity> result;
	auto group_data = (buffer*)get_component_ro(e, group_id);
	if (group_data == nullptr)
	{
		struct gather final : patcher_i
		{
			entity source;
			chunk_vector<entity>* ents;
			entity patch(entity e) override
			{
				if (e != source)
					ents->push(e);
				return e;
			}
		} p;
		p.source = e;
		p.ents = &result;
		auto& data = ents.datas[e.id];
		auto g = data.c->type;
		auto mt = g->metatypes;
		forloop(i, 0, g->metaCount)
			p.patch(mt[i]);
		chunk::patch({ data.c, data.i, 1 }, &p);
	}
	else
	{
		uint32_t size = group_data->size / sizeof(entity);
		stack_array(entity, members, size);
		memcpy(members, group_data->data(), group_data->size);
		struct gather final : patcher_i
		{
			entity* source;
			uint32_t count;
			chunk_vector<entity>* ents;
			entity patch(entity e) override
			{
				forloop(i, 0, count)
					if (e == source[i])
						return e;
				ents->push(e);
				return e;
			}
		} p;
		p.source = members;
		p.count = size;
		p.ents = &result;
		forloop(i, 0, size)
		{
			e = members[i];
			auto& data = ents.datas[e.id];
			auto g = data.c->type;
			auto mt = g->metatypes;
			forloop(j, 0, g->metaCount)
				p.patch(mt[j]);
			chunk::patch({ data.c, data.i, 1 }, &p);
		}
	}
	return result;
}

void world::serialize(serializer_i* s, entity src)
{
	auto group_data = (buffer*)get_component_ro(src, group_id);
	if (group_data == nullptr)
		serialize_single(s, src);
	else
	{
		serialize_group(s, group_data);
	}
}

entity first_entity(chunk_slice s)
{
	return s.c->get_entities()[s.start];
}

entity world::deserialize(serializer_i* s, patcher_i* patcher)
{
	auto slice = deserialize_single(s, patcher);
	entity src = first_entity(slice);
	auto group_data = (buffer*)get_component_ro(src, group_id);
	if (group_data != nullptr)
	{
		uint32_t size = group_data->size / sizeof(entity);
		stack_array(entity, members, size);
		stack_array(chunk_slice, chunks, size);
		members[0] = src;
		chunks[0] = slice;
		forloop(i, 1, size)
		{
			chunks[i] = deserialize_single(s, patcher);
			members[i] = first_entity(chunks[i]);
		}
		prefab_to_group(members, size);

	}
	return src;
}

void world::move_context(world& src)
{
	auto& sents = src.ents;
	uint32_t count = (uint32_t)sents.datas.size;
	AO(entity, patch, count);
	int validCount = 0;
	forloop(i, 0, count)
		if (sents.datas[i].c != nullptr)
			validCount++;
	AO(entity, entities, validCount);
	//batch new to trigger fast path
	ents.new_entities(entities, validCount);
	validCount = 0;
	forloop(i, 0, count)
		if (sents.datas[i].c != nullptr)
			patch[i] = entities[validCount++];
	sents.clear();

	struct patcher final : patcher_i
	{
		uint32_t start;
		entity* target;
		uint32_t count;
		entity patch(entity e) override
		{
			if (e.id < start || e.id > start + count) return NullEntity;
			e = target[e.id - start];
			return e;
		}
	} p;
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
			patch_chunk(c, &p);
			c = next;
		}
		src.update_queries(g, false);
		src.release_reference(g);
		free(g);
	}
	src.archetypes.clear();
}

#ifdef ENABLE_GUID_COMPONENT
struct stack_buffer : serializer_i
{
	std::vector<char> buf;

	void stream(const void* data, uint32_t bytes) override { write((char*)data, bytes); };
	bool is_serialize() override { return true; }

	template<class T>
	local_span<T> write(const T* value, size_t size)
	{
		auto dst = buf.data() + buf.size() * sizeof(T);
		buf.resize(buf.size() + size);
		memcpy(dst, value, size);
		return { dst - buf.data(), size };
	};

	char* allocate(size_t size)
	{
		auto dst = buf.data() + buf.size();
		buf.resize(buf.size() + size);
		return dst;
	};

	intptr_t top()
	{
		return  buf.size();
	};
};

world_delta::array_delta diff_array(const char* baseData, const char* data, size_t count, size_t stride, stack_buffer& buf)
{
	struct { uint32_t start, count; } range{ 0, 0 };

	world_delta::array_delta result;

	size_t remainToDiff = count;
	while (remainToDiff != 0)
	{
		auto start = stride * (count - remainToDiff);
		auto mresult = std::mismatch(data + start, data + stride * count, baseData + start);
		auto offset = mresult.first - data;
		if (offset == 0)
			break;
		auto index = (offset - 1) / stride;
		if (index == range.start + range.count)
			range.count++;
		else
		{
			result.push_back(buf.write(data + stride * range.start, stride* range.count));
			range.start = static_cast<uint32_t>(index);
			range.count = 1;
		}
		remainToDiff = remainToDiff - index - 1;
	}
	if (range.count != 0)
	{
		result.push_back(buf.write(data + stride * range.start, stride* range.count));
	}
	return result;
}

world_delta world::diff_context(world& base)
{
	world_delta wd{};
	stack_buffer buf;
	std::map<GUID, entity> entityMap;
	for (auto& pair : base.archetypes)
	{
		auto g = pair.second;

		auto guid_l = g->index(guid_id);
		if (guid_l == InvalidIndex)
			continue;

		chunk* c = g->firstChunk;
		while (c != nullptr)
		{
			auto guids = (GUID*)(c->data() + g->offsets[(int)c->ct][guid_l]);
			auto ents = c->get_entities();
			forloop(i, 0, c->count)
				entityMap.insert({ guids[i], ents[i] });
			c = c->next;
		}
	}
	for (auto& pair : archetypes)
	{
		auto g = pair.second;
		auto guid_l = g->index(guid_id);
		if (guid_l == InvalidIndex)
			continue;
		chunk* c = g->firstChunk;
		auto type = g->get_type();
		
		while (c != nullptr)
		{
			//entity operations are batched, thus diff could be batched too
			chunk_slice slice{ c, 0, 0 };
			while (slice.start != c->count)
			{
				auto guids = (GUID*)(c->data() + g->offsets[(int)c->ct][guid_l]);
				auto iter = entityMap.find(guids[slice.start]);
				if (iter != entityMap.end())
				{
					entityMap.erase(iter);
					auto& baseE = base.ents.datas[iter->second];
					chunk* baseC = baseE.c;
					chunk_slice baseSlice{ baseC, baseE.i, 0 };
					uint32_t i = baseSlice.start + 1, j = slice.start + 1;
					while (i < baseC->count && j < c->count)
					{
						auto it = entityMap.find(guids[j]);
						if (it == entityMap.end() || baseC->get_entities()[i] != it->second)
							break;
						entityMap.erase(it);
						(i++, j++);
					}
					baseSlice.count = i - baseSlice.start;
					slice.count = j - baseSlice.start;
					auto baseG = baseC->type;
					auto baseType = baseG->get_type();
					world_delta::slice_delta delta;
					auto data = buf.allocate(type.get_size());
					delta.type = type.clone(data);
					delta.diffs = world_delta::component_delta{ new world_delta::array_delta[g->firstBuffer] };
					delta.bufferDiffs = world_delta::buffer_delta{ new std::vector<world_delta::vector_delta>[g->firstTag-g->firstBuffer] };

					delta.ents = buf.write(guids + slice.start, slice.count);
					forloop(i, 0, g->firstBuffer)
					{
						auto t = type.types[i];
						if (t == guid_id)
							continue;
						auto blid = baseG->index(t);
						if (blid == InvalidIndex)
						{
							char* data = c->data() + g->sizes[i] * slice.start + g->offsets[(int)c->ct][i];
							delta.diffs[i].push_back(buf.write(data, g->sizes[i] * slice.count));
						}
						else
						{
							auto size = g->sizes[i];
							char* baseData = baseC->data() + size * baseSlice.start + baseG->offsets[(int)baseC->ct][blid];
							char* data = c->data() + size * slice.start + g->offsets[(int)c->ct][i];
							auto adiffs = diff_array(baseData, data, slice.count, size, buf);
							delta.diffs[i].insert(delta.diffs[i].end(), adiffs.begin(), adiffs.end());
						}
					}
					forloop(i, g->firstBuffer, g->firstTag)
					{
						tagged_index* types = (tagged_index*)g->types;
						const auto& t = DotsContext->infos[types[i].index()];
						auto blid = baseG->index(type.types[i]);
						if (blid == InvalidIndex)
						{
							char* data = c->data() + g->sizes[i] * slice.start + g->offsets[(int)c->ct][i];
							forloop(j, 0, slice.count)
							{
								world_delta::vector_delta dt;
								buffer* b = (buffer*)(data + (size_t)g->sizes[i] * j);
								uint16_t n = b->size / t.elementSize;
								dt.length = n;
								dt.content.push_back(buf.write(b->data(), b->size));
								delta.bufferDiffs[i].emplace_back(std::move(dt));
							}
						}
						else
						{
							auto size = g->sizes[i];
							char* baseData = baseC->data() + size * baseSlice.start + baseG->offsets[(int)baseC->ct][blid];
							char* data = c->data() + size * slice.start + g->offsets[(int)c->ct][i];
							forloop(j, 0, slice.count)
							{
								world_delta::vector_delta dt;
								buffer* baseB = (buffer*)(data + (size_t)g->sizes[i] * j); 
								buffer* b = (buffer*)(data + (size_t)g->sizes[i] * j);
								uint16_t baseN = baseB->size / t.elementSize;
								uint16_t n = b->size / t.elementSize;
								dt.length = n;
								uint32_t diffed = std::min(baseB->size, b->size);
								dt.content = diff_array(baseB->data(), b->data(), diffed, t.elementSize, buf);
								if (diffed < b->size)
									dt.content.push_back(buf.write(b->data() + diffed, b->size - diffed));

								delta.bufferDiffs[i].emplace_back(std::move(dt));
							}
						}
					}
					wd.changed.push_back(std::move(delta));
				}
				else
				{
					int i = slice.start;
					while (entityMap.find(guids[++i]) == entityMap.end());
					slice.count = i - slice.start;
					world_delta::slice_data delta;
					auto data = buf.allocate(type.get_size());
					delta.type = type.clone(data);
					delta.offset = buf.top();
					chunk::serialize(slice, &buf, false);
					wd.created.push_back(std::move(delta));
				}

				slice.start += slice.count;
				slice.count = 0;
			}
			c = c->next;
		}
	}
	{
		for (auto& pair : entityMap)
			wd.destroyed.push_back(pair.second);
	}
	wd.store = std::move(buf.buf);
	return wd;
}
#endif

void world::patch_chunk(chunk* c, patcher_i* patcher)
{
	entity* es = (entity*)c->data();
	forloop(i, 0, c->count)
		es[i] = patcher->patch(es[i]);
	chunk::patch(c, patcher);
}

const int ZeroValue = 0;
void world::serialize(serializer_i* s)
{
	archive(s, ents.datas.size);

	std::vector<archetype*> ats;
	for (auto& pair : archetypes)
	{
		archetype* g = pair.second;
		if (g->cleaning)
			continue;
		serialize_archetype(g, s);
		ats.push_back(g);
	}
	archive(s, ZeroValue);
	for (archetype* g : ats)
	{
		for (chunk* c = g->firstChunk; c; c = c->next)
			serialize_slice({ c, 0, c->count }, s);

		archive(s, ZeroValue);
	}
}

void world::deserialize(serializer_i* s)
{
	clear();

	archive(s, ents.datas.size);

	//reallocate entity data buffer
	ents.datas.reserve(ents.datas.size);
	ents.datas.zero();
	chunk_slice slice;
	std::vector<archetype*> ats;
	while (archetype* g = deserialize_archetype(s, nullptr, true))
		ats.push_back(g);

	//todo: multithread?
	for (archetype* g : ats)
	{
		while (deserialize_slice(g, s, slice))
		{
			//reinitialize entity data
			auto ref = get_entities(slice.c);
			forloop(i, 0, slice.count)
			{
				auto index = i + slice.start;
				auto e = ref[index];
				auto& data = ents.datas[e.id];
				data.c = slice.c;
				data.i = index;
				data.v = e.version;
			}
		}
	}

	//reinitialize entity free list
	forloop(i, 0, ents.datas.size)
	{
		auto& data = ents.datas[i];
		if (data.c == nullptr)
		{
			data.nextFree = ents.free;
			ents.free = static_cast<uint32_t>(i);
		}
	}

	//update query till all meta entity is complete
	for (archetype* g : ats)
		add_archetype(g);
}

void world::clear()
{
	if(typeTimestamps)
		std::fill(typeTimestamps, typeTimestamps + typeCapacity, 0);
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
		if(on_archetype_update)
			on_archetype_update(g.second, false);
		release_reference(g.second);
		free(g.second);
	}
	ents.clear();
	queries.clear();
	archetypes.clear();
}

void world::gc_meta()
{
	for (auto& gi : archetypes)
	{
		auto g = gi.second;
		auto mt = g->metatypes;
		bool invalid = false;
		forloop(i, 0, g->metaCount)
		{
			if (!exist(mt[i]))
			{
				invalid = true;
				break;
			}
		}
		if (invalid)
		{
			//note: remove from map before we edit the type
			//or key will be broken
			archetypes.erase(g->get_type());
			forloop(i, 0, g->metaCount)
			{
				if (!exist(mt[i]))
				{
					if (i + 1 != g->metaCount)
						std::swap(mt[i], mt[g->metaCount - 1]);
					g->metaCount--;
				}
			}
			archetypes.insert({ g->get_type(), g });
		}
	}
}

void world::merge_chunks()
{
	for (auto& pair : archetypes)
	{
		archetype* g = pair.second;
		merge_chunks(g);
	}
}

bool world::is_a(entity e, const entity_type& type) const noexcept
{
	if (!exist(e))
		return false;
	const auto& data = ents.datas[e.id];
	auto et = data.c->type->get_type();
	return et.types.all(type.types) && et.metatypes.all(type.metatypes);
}

bool world::share_component(entity e, const typeset& type) const
{
	if (!exist(e))
		return false;
	const auto& data = ents.datas[e.id];
	return share_component(data.c->type, type);
}

bool world::has_component(entity e, const typeset& type) const noexcept
{
	if (!exist(e))
		return false;
	const auto& data = ents.datas[e.id];
	return has_component(data.c->type, type);
}

bool world::own_component(entity e, const typeset& type) const noexcept
{
	if (!exist(e))
		return false;
	const auto& data = ents.datas[e.id];
	return own_component(data.c->type, type);
}

void world::enable_component(entity e, const typeset& type) const noexcept
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
	auto& m = *(mask*)(c->data() + g->offsets[(int)c->ct][id] + (size_t)data.i * g->sizes[id]);
	m |= mm;
}

void world::disable_component(entity e, const typeset& type) const noexcept
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
	auto& m = *(mask*)(c->data() + g->offsets[(int)c->ct][id] + (size_t)data.i * g->sizes[id]);
	m &= ~mm;
}

bool world::is_component_enabled(entity e, const typeset& type) const noexcept
{
	if (!exist(e))
		return false;
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	if (!g->withMask)
		return true;
	mask mm = g->get_mask(type);
	auto id = g->index(mask_id);
	auto& m = *(mask*)(c->data() + g->offsets[(int)c->ct][id] + (size_t)data.i * g->sizes[id]);
	return (m & mm) == mm;
}

bool world::exist(entity e) const noexcept
{
	return e.id < ents.datas.size&& e.version == ents.datas[e.id].v;
}

void world::entities::clear()
{
	free = 0;
	datas.reset();
}

void world::entities::new_entities(chunk_slice s)
{
	entity* dst = (entity*)s.c->data() + s.start;
	uint32_t i = 0;
	while (i < s.count && free != 0)
	{
		uint32_t id = free;
		free = datas[free].nextFree;
		entity newE = { id, datas[id].v };
		dst[i] = newE;
		datas[newE.id].c = s.c;
		datas[newE.id].i = s.start + i;
		i++;
	}
	if (i == s.count)
		return;
	//fast path
	size_t newId = datas.size;
	datas.resize(datas.size + s.count - i + 1);
	while (i < s.count)
	{
		entity newE(
			static_cast<uint32_t>(newId),
			static_cast<uint32_t>(datas[newId].v)
		);
		dst[i] = newE;
		datas[newE.id].c = s.c;
		datas[newE.id].i = s.start + i;
		i++;
		newId++;
	}
}

void world::entities::new_entities(entity* dst, uint32_t count)
{
	uint32_t i = 0;
	while (i < count && free != 0)
	{
		uint32_t id = free;
		free = datas[free].nextFree;
		dst[i] = { id, datas[id].v };
		i++;
	}
	if (i == count)
		return;
	//fast path
	uint32_t newId = static_cast<uint32_t>(datas.size);
	datas.resize(datas.size + count - i);
	while (i < count)
	{
		dst[i] = { newId, datas[newId].v };
		i++;
		newId++;
	}
}

entity world::entities::new_prefab(int sizeHint)
{
	datas.reserve(datas.size + sizeHint);
	uint32_t id = (uint32_t)datas.size;
	datas.push();
	return { id, datas[id].v };
}

entity world::entities::new_entity(int sizeHint)
{
	if (free == 0)
		return new_prefab(sizeHint);
	else
	{
		uint32_t id = free;
		free = datas[free].nextFree;
		return { id, datas[id].v };
	}
}

void world::entities::free_entities(chunk_slice s)
{
	entity* toFree = (entity*)s.c->data() + s.start;
	forloop(i, 0, s.count - 1)
	{
		data& freeData = datas[toFree[i].id];
		freeData = { nullptr, 0, entity::recycle(freeData.v) };
		freeData.nextFree = toFree[i + 1].id;
	}
	data& freeData = datas[toFree[s.count - 1].id];
	freeData = { nullptr, 0, entity::recycle(freeData.v) };
	freeData.nextFree = free;
	free = toFree[0].id;
	//shrink
	while (datas.size > 0 && datas.last().c == nullptr)
		datas.pop();
}

void world::entities::move_entities(chunk_slice dst, const chunk* src, uint32_t srcIndex)
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

void world::entities::fill_entities(chunk_slice dst, uint32_t srcIndex)
{
	const entity* toMove = (entity*)dst.c->data() + srcIndex;
	forloop(i, 0, dst.count)
		datas[toMove[i].id].i = dst.start + i;
	memcpy((entity*)dst.c->data() + dst.start, toMove, dst.count * sizeof(entity));
}

void world::entities::clone(entities* dst)
{
	dst->clear();
	new(&dst->datas) chunk_vector<data>(datas);
	dst->free = free;
}

chunk_vector<matched_archetype> world::query(const archetype_filter& filter) const
{
	chunk_vector<matched_archetype> result;
	auto& cache = get_query_cache(filter);
	for (auto& type : cache.archetypes)
		result.push(type);
	return result;
}

chunk_vector<chunk*> world::query(const archetype* type, const chunk_filter& filter) const
{
	chunk_vector<chunk*> result;
	auto iter = type->firstChunk;
	if (filter.changed.length == 0)
	{
		while(iter != nullptr)
		{
			result.push(iter);
			iter = iter->next;
		}
		return result;
	}

	while (iter != nullptr)
	{
		if (filter.match(type->get_type(), type->timestamps(iter)))
			result.push(iter);
		iter = iter->next;
	}
	return result;
}

chunk_vector<archetype*> world::get_archetypes()
{
	chunk_vector<archetype*> result;
	for (auto& pair : archetypes)
		result.push(pair.second);
	return result;
}

bool archetype_filter::match(const entity_type& t, const typeset& sharedT) const
{
	//todo: cache these things?
	stack_array(index_t, _components, t.types.length + sharedT.length);
	auto components = typeset::merge(t.types, sharedT, _components);

	if (!components.all(all.types))
		return false;
	if (any.types.length > 0 && !components.any(any.types))
		return false;

	stack_array(index_t, _nonOwned, none.types.length);
	stack_array(index_t, _nonShared, none.types.length);
	auto nonOwned = typeset::substract(none.types, shared, _nonOwned);
	auto nonShared = typeset::substract(none.types, owned, _nonShared);
	if (t.types.any(nonOwned))
		return false;
	if (sharedT.any(nonShared))
		return false;

	if (!t.types.all(owned))
		return false;
	if (!sharedT.all(shared))
		return false;

	if (!t.metatypes.all(all.metatypes))
		return false;
	if (any.metatypes.length > 0 && !t.metatypes.any(any.metatypes))
		return false;
	if (t.metatypes.any(none.metatypes))
		return false;

	return true;
}

namespace core::database::chunk_vector_pool
{
	constexpr size_t kThreadBinCapacity = 40;
	const std::thread::id kMainThreadId = std::this_thread::get_id();
	thread_local std::array<void*, kThreadBinCapacity> threadbin{};
	thread_local size_t threadbinSize = 0;
	constexpr size_t kChunkSize = chunk_vector_base::kChunkSize;

	void free(void* data)
	{
		if (std::this_thread::get_id() == kMainThreadId)
		{
			DotsContext->free(alloc_type::fastbin, data);
			return;
		}
		if (threadbinSize < kThreadBinCapacity)
			threadbin[threadbinSize++] = data;
		else
			::free(data);
	}

	void* malloc()
	{
		if (std::this_thread::get_id() == kMainThreadId)
			return DotsContext->malloc(alloc_type::fastbin);
		if (threadbinSize == 0)
			return ::malloc(kChunkSize);
		else
			return threadbin[--threadbinSize];
	}
}
namespace chunk_vector_pool = core::database::chunk_vector_pool;

void chunk_vector_base::grow()
{
	if (data == nullptr)
		data = (void**)chunk_vector_pool::malloc();
	if(chunkSize * sizeof(void*) >= chunkCapacity)
	{
		auto oldCap = chunkCapacity;
		chunkCapacity *= 2;
		auto newData = ::malloc(chunkCapacity);
		memcpy(newData, data, oldCap);
		if (oldCap == kChunkSize)
			chunk_vector_pool::free(data);
		else
			::free(data);
		data = (void**)newData;
	}
	data[chunkSize++] = chunk_vector_pool::malloc();
}

void chunk_vector_base::shrink(size_t n)
{
	forloop(i, 0, n)
		chunk_vector_pool::free(data[--chunkSize]);
	if (data && chunkSize == 0)
	{
		if (chunkCapacity == kChunkSize)
			chunk_vector_pool::free(data);
		else
			::free(data);
		data = nullptr;
	}
}

void chunk_vector_base::flatten(void* dst, size_t eleSize)
{
	auto remainSize = size * eleSize;
	forloop(i, 0, chunkSize)
	{
		auto sizeToCopy = std::min(kChunkSize, remainSize);
		memcpy(dst, data[i], sizeToCopy);
		dst = (char*)dst + sizeToCopy;
		remainSize -= sizeToCopy;
	}
}

void chunk_vector_base::zero(size_t eleSize)
{
	auto remainSize = size * eleSize;
	forloop(i, 0, chunkSize)
	{
		auto sizeToCopy = std::min(kChunkSize, remainSize);
		memset(data[i], 0, sizeToCopy);
		remainSize -= sizeToCopy;
	}
}

void chunk_vector_base::reset()
{
	shrink(chunkSize);
	size = 0;
}

chunk_vector_base::chunk_vector_base(chunk_vector_base&& r) noexcept
{
	data = r.data;
	size = r.size;
	chunkSize = r.chunkSize;
	r.data = nullptr;
	r.size = r.chunkSize = 0;
}

chunk_vector_base::chunk_vector_base(const chunk_vector_base& r) noexcept
{
	if (r.chunkSize > 0)
	{
		if (r.chunkCapacity == kChunkSize)
			data = (void**)chunk_vector_pool::malloc();
		else
			data = (void**)::malloc(r.chunkCapacity);
	}
	forloop(i, 0, r.chunkSize)
	{
		void* newChunk = chunk_vector_pool::malloc();
		memcpy(newChunk, r.data[i], kChunkSize);
		data[i] = newChunk;
	}
	size = r.size;
	chunkSize = r.chunkSize;
}

chunk_vector_base::~chunk_vector_base()
{
	reset();
}


chunk_vector_base& chunk_vector_base::operator=(chunk_vector_base&& r) noexcept
{
	reset();
	data = r.data;
	size = r.size;
	chunkSize = r.chunkSize;
	r.data = nullptr;
	r.size = r.chunkSize = 0;
	return *this;
}



// converts a single hex char to a number (0 - 15)
unsigned char hexDigitToChar(char ch)
{
	// 0-9
	if (ch > 47 && ch < 58)
		return ch - 48;
	// a-f
	if (ch > 96 && ch < 103)
		return ch - 87;
	// A-F
	if (ch > 64 && ch < 71)
		return ch - 55;
	return 0;
}

bool isValidHexChar(char ch)
{
	// 0-9
	if (ch > 47 && ch < 58)
		return true;
	// a-f
	if (ch > 96 && ch < 103)
		return true;
	// A-F
	if (ch > 64 && ch < 71)
		return true;
	return false;
}
// converts the two hexadecimal characters to an unsigned char (a byte)
std::byte hexPairToChar(char a, char b)
{
	return static_cast<std::byte>(hexDigitToChar(a) * 16 + hexDigitToChar(b));
}

void core::database::initialize()
{
	context::initialize();
	disable_id = DotsContext->disable_id;
	cleanup_id = DotsContext->cleanup_id;
	group_id = DotsContext->group_id;
	mask_id = DotsContext->mask_id;
#ifdef ENABLE_GUID_COMPONENT
	guid_id = DotsContext->guid_id;
#endif
}

void entity_filter::apply(core::database::matched_archetype& ma) const
{
	ma.matched &= ~ma.type->get_mask(inverseMask);
}

bool chunk_filter::match(const entity_type& t, uint32_t* timestamps) const
{
	uint16_t i = 0, j = 0;
	if (changed.length == 0)
		return true;
	while (i < changed.length && j < t.types.length)
	{
		if (changed[i] > t.types[j])
			j++;
		else if (changed[i] < t.types[j])
			i++;
		else if (timestamps[j] >= prevTimestamp)
			return true;
		else
			(j++, i++);
	}
	return false;
}

int archetype_filter::get_size() const
{
	return all.get_size() +
		any.get_size() +
		none.get_size() +
		shared.get_size() +
		owned.get_size();
}

archetype_filter archetype_filter::clone(char*& buffer) const
{
	return {
		all.clone(buffer),
		any.clone(buffer),
		none.clone(buffer),
		shared.clone(buffer),
		owned.clone(buffer)
	};
}

int chunk_filter::get_size() const
{
	return changed.get_size();
}

chunk_filter chunk_filter::clone(char*& buffer) const
{
	return {
		changed.clone(buffer),
		prevTimestamp
	};
}

int entity_filter::get_size() const
{
	return 0;
}

entity_filter entity_filter::clone(char*& buffer) const
{
	return { inverseMask };
}

batch_range::iterator batch_range::begin() const
{
	return { ctx, ctx.iter(es, count) };
}

batch_range::iterator& batch_range::iterator::operator++()
{
	valid = ctx.next(iter);
	return *this;
}