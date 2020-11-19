#include "Database.h"
#include <iostream>
#define cat(a, b) a##b
#define forloop(i, z, n) for(auto i = decltype(n)(z); i<n; ++i)

using namespace core;
using namespace database;

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

struct global_data
{
	std::vector<type_data> infos;
	std::vector<track_state> tracks;
	std::vector<intptr_t> entityRefs;
	std::unordered_map<size_t, size_t> hash2type;

	std::array<void*, kFastBinCapacity> fastbin{};
	std::array<void*, kSmallBinCapacity> smallbin{};
	std::array<void*, kLargeBinCapacity> largebin{};
	size_t fastbinSize = 0;
	size_t smallbinSize = 0;
	size_t largebinSize = 0;

	char* stackbuffer = nullptr;
	void* stacktop = nullptr;
	size_t stackSize = 0;
	
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
		desc.hash = std::numeric_limits<size_t>::max();
		desc.isElement = false;
		desc.name = "mask";
		desc.entityRefCount = 0;
		mask_id = register_type(desc);

		stackbuffer = (char*)::malloc(10000);
		stackSize = 10000;
		stacktop = stackbuffer;
	}

	void* stack_alloc(size_t size, size_t align = alignof(int))
	{
		auto offset = uint32_t((char*)stacktop - stackbuffer);
		stacktop = (uint32_t*)stacktop + 1;
		if (std::align(align, size, stacktop, stackSize))
		{
			*((uint32_t*)stacktop - 1) = offset;
			auto result = stacktop;
			stacktop = (char*)stacktop + size;
			stackSize -= size + sizeof(uint32_t);
			return result;
		}
		return nullptr;
	}

	void stack_free(void* ptr, size_t size)
	{
		auto offset = uint32_t((char*)stacktop - stackbuffer);
		auto oldOffset = *((uint32_t*)ptr - 1);
		stackSize += offset - oldOffset;
		stacktop = stackbuffer + oldOffset;
	}

	void free(alloc_type type, void* data)
	{
		switch (type)
		{
		case alloc_type::fastbin:
			if (fastbinSize < kLargeBinCapacity)
				fastbin[fastbinSize++] = data;
			else
				::free(data);
			break;
		case alloc_type::smallbin:
			if (smallbinSize < kSmallBinCapacity)
				smallbin[smallbinSize++] = data;
			else
				::free(data);
			break;
		case alloc_type::largebin:
			if (largebinSize < kLargeBinCapacity)
				largebin[largebinSize++] = data;
			else
				::free(data);
			break;
		}
	}

	void* malloc(alloc_type type)
	{
		switch (type)
		{
		case alloc_type::fastbin:
			if (fastbinSize == 0)
				return ::malloc(kFastBinSize);
			else
				return fastbin[--fastbinSize];
			break;
		case alloc_type::smallbin:
			if (smallbinSize == 0)
				return ::malloc(kSmallBinSize);
			else
				return smallbin[--smallbinSize];
			break;
		case alloc_type::largebin:
			if (largebinSize == 0)
				return ::malloc(kLargeBinSize);
			else
				return largebin[--largebinSize];
			break;
		}
		return nullptr;
	}
};

static global_data gd;

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
		self = gd.stack_alloc(size);
	}
	~stack_object()
	{
		gd.stack_free(self, size);
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
		if (gd.stackSize - size > 1024)
		{
			self = gd.stack_alloc(size);
			type = Stack;
		}
		else if (size < kFastBinCapacity)
		{
			self = gd.malloc(alloc_type::fastbin);
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
			gd.free(alloc_type::fastbin, self);
			break;
		case Heap:
			::free(self);
			break;
		case Stack:
			gd.stack_free(self, size);
			break;
		}
	}
};

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
	
	index_t id = (index_t)gd.infos.size();
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
		index_t id = (index_t)gd.infos.size();
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

void chunk::clone(chunk* dst) noexcept
{
	memcpy(dst, this, get_size());
	uint32_t* offsets = type->offsets(ct);
	uint16_t* sizes = type->sizes();
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
	uint32_t* offsets = src->type->offsets(src->ct);
	uint16_t* sizes = src->type->sizes();
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
	uint32_t* offsets = dst.c->type->offsets(dst.c->ct);
	uint16_t* sizes = dst.c->type->sizes();
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
	uint32_t* offsets = type->offsets(s.c->ct);
	uint16_t* sizes = type->sizes();
#ifndef NOINITIALIZE
	forloop(i, 0, type->firstBuffer)
		memset(srcData, 0, (size_t)sizes[i] * s.count);
#endif
	forloop(i, type->firstBuffer, type->firstTag)
	{
		char* src = srcData;
		forloop(j, 0, s.count)
			new((size_t)j * sizes[i] + src) buffer{ sizes[i] - sizeof(buffer) };
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
	uint32_t* offsets = type->offsets(s.c->ct);
	uint16_t* sizes = type->sizes();
	index_t* types = type->types();
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
	if (gd.tracks[t.index()] == Copying)
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
	archetype *dstType = dst.c->type;
	tsize_t dstI = 0;
	uint32_t* offsets = type->offsets(src->ct);
	uint32_t* dstOffsets = dstType->offsets(dst.c->ct);
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
			new(d + (size_t)sizes[i]*j) buffer{ *(buffer*)s };
		dstI++;
	}
}

//todo: will compiler inline patcher?
void chunk::patch(chunk_slice s, patcher_i* patcher) noexcept
{
	archetype* g = s.c->type;
	uint32_t* offsets = g->offsets(s.c->ct);
	uint16_t* sizes = g->sizes();
	tagged_index* types = (tagged_index*)g->types();
	forloop(i, 0, g->firstBuffer)
	{
		const auto& t = gd.infos[types[i].index()];
		char* arr = s.c->data() + offsets[i] + (size_t)sizes[i]*s.start;
		auto f = t.vtable.patch;
		if (f != nullptr)
		{
			forloop(j, 0, s.count)
			{
				char* data = arr + (size_t)sizes[i] * j;
				forloop(k, 0, t.entityRefCount)
				{
					entity& e = *(entity*)(data + gd.entityRefs[(size_t)t.entityRefs + k]);
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
					entity& e = *(entity*)(data + gd.entityRefs[(size_t)t.entityRefs + k]);
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
		char* arr = s.c->data() + offsets[i] + (size_t)sizes[i] * s.start;
		forloop(j, 0, s.count)
		{
			buffer *b =(buffer*)(arr + (size_t)sizes[i] * j);
			uint16_t n = b->size / t.elementSize;
			auto f = t.vtable.patch;
			if (f != nullptr)
			{
				forloop(l, 0, n)
				{
					char* data = b->data() + (size_t)t.elementSize * l;
					forloop(k, 0, t.entityRefCount)
					{
						entity& e = *(entity*)(data + gd.entityRefs[(size_t)t.entityRefs + k]);
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
					char* data = b->data() + (size_t)t.elementSize * l;
					forloop(k, 0, t.entityRefCount)
					{
						entity& e = *(entity*)(data + gd.entityRefs[(size_t)t.entityRefs + k]);
						e = patcher->patch(e);
					}
					patcher->move();
				}
			}
		}
		patcher->reset();
	}
}

//todo: handle transient data?
void chunk::serialize(chunk_slice s, serializer_i* stream)
{
	archetype* type = s.c->type;
	uint32_t* offsets = type->offsets(s.c->ct);
	uint16_t* sizes = type->sizes();
	tagged_index* types = (tagged_index*)type->types();
	stream->stream(&s.count, sizeof(uint32_t));
	stream->stream(s.c->get_entities()+s.start, sizeof(entity) * s.count);

	forloop(i, 0, type->firstTag)
	{
		char* arr = s.c->data() + offsets[i] + (size_t)sizes[i] * s.start;
		stream->stream(arr, sizes[i] * s.count);
	}

	forloop(i, type->firstBuffer, type->firstTag)
	{
		char* arr = s.c->data() + offsets[i] + (size_t)sizes[i] * s.start;
		forloop(j, 0, s.count)
		{
			buffer* b = (buffer*)(arr + (size_t)j * sizes[i]);
			stream->stream(b->data(), b->size);
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
	uint32_t* srcOffsets = srcType->offsets(src->ct);
	uint32_t* dstOffsets = dstType->offsets(dst.c->ct);
	uint16_t* srcSizes = srcType->sizes();
	uint16_t* dstSizes = dstType->sizes();
	index_t* srcTypes = srcType->types(); index_t* dstTypes = dstType->types();

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
			if(destruct)
				forloop(j, 0, count)
					((buffer*)((size_t)j * srcSizes[srcI] + s))->~buffer();
			srcI++;
		}
		else if (st > dt) //construct
		{
			forloop(j, 0, count)
				new((size_t)j * dstSizes[dstI] + d) buffer{ dstSizes[dstI] - sizeof(buffer) };
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
			new((size_t)j * dstSizes[dstI] + d) buffer{ dstSizes[dstI] - sizeof(buffer) };
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
					if(s[i].test(srcI))
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

inline uint32_t* archetype::timestamps(chunk* c) noexcept { return (uint32_t*)((char*)c + c->get_size()) - firstTag; }

tsize_t archetype::index(index_t type) noexcept
{
	index_t* ts = types();
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

size_t archetype::alloc_size(tsize_t componentCount, tsize_t firstTag, tsize_t metaCount)
{
	data_t acc{ componentCount, firstTag, firstTag, firstTag, firstTag, metaCount };
	return acc.get_offset(6) + sizeof(archetype);
}

size_t get_filter_size(const archetype_filter& f)
{
	auto totalSize = f.all.types.length * sizeof(index_t) + f.all.metatypes.length * sizeof(entity) +
		f.any.types.length * sizeof(index_t) + f.any.metatypes.length * sizeof(entity) +
		f.none.types.length * sizeof(index_t) + f.none.metatypes.length * sizeof(entity);
	return totalSize;
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

world::query_cache& world::get_query_cache(const archetype_filter& f)
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

void world::update_queries(archetype* g, bool add)
{
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

void world::remove(chunk*& h, chunk*& t, chunk* c)
{
	if (c == t)
		t = t->prev;
	if (h == c)
		h = h->next;
	c->unlink();
}


archetype* world::get_archetype(const entity_type& key)
{
	auto iter = archetypes.find(key);
	if (iter != archetypes.end())
		return iter->second;

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
	void* data = malloc(archetype::alloc_size(count, firstTag, metaCount));
	archetype* g = (archetype*)data;
	g->componentCount = count;
	g->metaCount = metaCount;
	g->firstBuffer = firstBuffer;
	g->firstTag = firstTag;
	g->cleaning = false;
	g->copying = false;
	g->disabled = false;
	g->withMask = false;
	g->withTracked = false;
	g->zerosize = false;
	g->chunkCount = 0;
	g->size = 0;
	g->timestamp = timestamp;
	g->lastChunk = g->firstChunk = g->firstFree = nullptr;
	index_t* types = g->types();
	entity* metatypes = g->metatypes();
	memcpy(types, key.types.data, count * sizeof(index_t));
	memcpy(metatypes, key.metatypes.data, key.metatypes.length * sizeof(entity));

	const index_t disableType = disable_id;
	const index_t cleanupType = cleanup_id;
	const index_t maskType = mask_id;

	uint16_t* sizes = g->sizes();
	stack_array(size_t, hash, firstTag);
	stack_array(tsize_t, stableOrder, firstTag);
	uint16_t entitySize = sizeof(entity);
	forloop(i, 0, count)
	{
		auto type = (tagged_index)key.types[i];
		if (type == disableType)
			g->disabled = true;
		else if (type == cleanupType)
			g->cleaning = true;
		else if (type == maskType)
			g->withMask = true;
		if ((gd.tracks[type.index()] & NeedCC) != 0)
			g->withTracked = true;
		if ((gd.tracks[type.index()] & Copying) != 0)
			g->copying = true;
	}
	forloop(i, 0, firstTag)
	{
		auto type = (tagged_index)key.types[i];
		auto& info = gd.infos[type.index()];
		sizes[i] = info.size;
		hash[i] = info.hash;
		stableOrder[i] = i;
		entitySize += info.size;
	}
	if (entitySize == sizeof(entity))
		g->zerosize = true;
	g->entitySize = entitySize;
	size_t Caps[] = {kSmallBinSize, kFastBinSize, kLargeBinSize};
	std::sort(stableOrder, stableOrder + firstTag, [&](tsize_t lhs, tsize_t rhs)
		{
			return hash[lhs] < hash[rhs];
		});
	forloop(i, 0, 3)
	{
		uint32_t* offsets = g->offsets((alloc_type)i);
		g->chunkCapacity[i] = (uint32_t)(Caps[i] - sizeof(chunk) - sizeof(uint32_t) * firstTag) / entitySize;
		if (g->chunkCapacity[i] == 0)
			continue;
		uint32_t offset = sizeof(entity) * g->chunkCapacity[i];
		forloop(j, 0, firstTag)
		{
			tsize_t id = stableOrder[j];
			offsets[id] = offset;
			offset += sizes[id] * g->chunkCapacity[i];
		}
	}
	add_archetype(g);
	return g;
}

void world::add_archetype(archetype* g)
{
	archetypes.insert({ g->get_type(), g });
	update_queries(g, true);
}

archetype* world::get_cleaning(archetype* g)
{
	if (g->cleaning) return g;
	else if (!g->withTracked) return nullptr;

	tsize_t k = 0, count = g->componentCount;
	const index_t cleanupType = cleanup_id;
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
			auto stage = gd.tracks[type.index()];
			if ((stage & NeedCopying) != 0)
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
			{ .types = {srcTypes, srcSize}, .metatypes = {srcMetaTypes, srcMetaSize} },
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
			auto stage = gd.tracks[type.index()];
			if (((stage & NeedCopying) != 0) && (srcTypes[i + 1] == type + 1))
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
			auto stage = gd.tracks[type.index()];
			if ((stage & NeedCopying) != 0)
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
			{ .types = {srcTypes, srcSize}, .metatypes = {srcMetaTypes, srcMetaSize} },
			{ .types = {shrTypes, shrSize}, .metatypes = diff.shrink.metatypes }, dstTypes, dstMetaTypes);
		srcTypes = dstTypes; srcMetaTypes = dstMetaTypes;
		srcSize = key.types.length; srcMetaSize = key.metatypes.length;
	}

	//phase 5 : check cleaning
	if (g->cleaning)
	{
		entity_type key = { .types = {srcTypes, srcSize}, .metatypes = {srcMetaTypes, srcMetaSize} };
		if (is_cleaned(key))
			return nullptr;
	}

	return get_archetype({ .types = {srcTypes, srcSize}, .metatypes = {srcMetaTypes, srcMetaSize} });
}

chunk* world::malloc_chunk(alloc_type type)
{
	chunk* c = (chunk*)gd.malloc(type);
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
	g->lastChunk->link(c);
	g->lastChunk = c;
	if (g->firstFree == nullptr)
		g->firstFree = c;
}

void world::unmark_free(archetype* g, chunk* c)
{
	remove(g->firstFree, g->lastChunk, c);
	if (g->lastChunk == nullptr)
		g->lastChunk = c;
	if (c->next != g->firstFree)
		g->firstChunk->link(c);
}

void world::release_reference(archetype* g)
{
	//todo: does this make sense?
}

void world::serialize_archetype(archetype* g, serializer_i* s)
{
	entity_type type = g->get_type();
	tsize_t tlength = type.types.length, mlength = type.metatypes.length;
	s->stream(&tlength, sizeof(tsize_t));
	forloop(i, 0, tlength)
		s->stream(&gd.infos[tagged_index(type.types[i]).index()].hash, sizeof(size_t));
	s->stream(&mlength, sizeof(tsize_t));
	s->stream(type.metatypes.data, mlength * sizeof(entity));
}

archetype* world::deserialize_archetype(serializer_i* s, patcher_i* patcher)
{
	tsize_t tlength;
	s->stream(&tlength, sizeof(tsize_t));
	stack_array(index_t, types, tlength);
	if (tlength == 0)
		return nullptr;
	forloop(i, 0, tlength)
	{
		size_t hash;
		s->stream(&hash, sizeof(size_t));
		//todo: check validation
		types[i] = (index_t)gd.hash2type[hash];
	}
	tsize_t mlength;
	s->stream(&mlength, sizeof(tsize_t));
	stack_array(entity, metatypes, mlength);
	s->stream(metatypes, mlength);
	if(patcher)
		forloop(i, 0, mlength)
			metatypes[i] = patcher->patch(metatypes[i]);
	std::sort(types, types + tlength);
	std::sort(metatypes, metatypes + mlength);
	entity_type type = { {types, tlength}, {metatypes, mlength} };
	return get_archetype(type);
}

std::optional<chunk_slice> world::deserialize_slice(archetype* g, serializer_i* s)
{
	uint32_t count;
	s->stream(&count, sizeof(uint32_t));
	g->size += count;
	if (count == 0)
		return {};
	chunk* c;
	c = g->firstFree;
	while (c && c->count + count > g->chunkCapacity[(int)c->ct])
		c = c->next;
	if (c == nullptr)
		c = new_chunk(g, count);
	uint32_t start = c->count;
	resize_chunk(c, start + count);
	chunk_slice slice = { c, start, count };
	chunk::serialize(slice, s);
	return slice;
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

chunk_vector<chunk_slice> world::instantiate_prefab(entity* src, uint32_t size, uint32_t count)
{
	chunk_vector<chunk_slice> allSlices;

	adaptive_object object(sizeof(entity) * size * count);
	auto ret = (entity*)object.self;

	forloop(i, 0, size)
	{
		auto e = src[i];
		auto g = instantiate_single(e, count);
		uint32_t k = 0;
		for (auto s : g)
		{
			allSlices.push(s);
			forloop(i, 0, s.count)
				ret[k++ * size] = s.c->get_entities()[s.start + i];
		}
	}

	struct patcher final : patcher_i
	{
		entity* base;
		entity* curr;
		uint32_t count;
		void move() override { curr += count; }
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
		p.curr = p.base = &ret[(k % count) * size + (k / count)];
		chunk::patch(s, &p);
		k += s.count;
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
	chunk::serialize({ data.c, data.i, 1 }, s);
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
	auto *g = deserialize_archetype(s, patcher);
	auto slice = deserialize_slice(g, s);
	ents.new_entities(*slice);
	chunk::patch(*slice, patcher);
	return *slice;
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
	gd.free(c->ct, c);
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
		else if(c->count == g->chunkCapacity[(int)c->ct])
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
	if(toSort != nullptr)
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
	g->size += count;
	uint32_t start = c->count;
	uint32_t allocated = std::min(count, g->chunkCapacity[(int)c->ct] - start);
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
	g->size -= src.count;
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
	memset(typeTimestamps, 0, typeCapacity * sizeof(uint32_t));
	src.ents.clone(&ents);
	for (auto& iter : src.archetypes)
	{
		auto g = iter.second;
		if (g->cleaning) // skip dead entities
			continue;
		auto size = archetype::alloc_size(g->componentCount, g->firstTag, g->metaCount);
		archetype* newG = (archetype*)::malloc(size);
		memcpy(newG, g, size);

		// mark copying stage
		forloop(i, 0, newG->componentCount)
		{
			auto type = (tagged_index)newG->types()[i];
			if ((gd.tracks[type.index()] & NeedCopying) != 0)
				newG->types()[i] = index_t(type) + 1;
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

world::~world()
{
	clear();
	free(typeTimestamps);
}

chunk_vector<chunk_slice> world::allocate(const entity_type& type, uint32_t count)
{
	chunk_vector<chunk_slice> result;
	archetype* g = get_archetype(type);
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
		uint32_t size = group_data->size / sizeof(entity);
		adaptive_object _ao_members(group_data->size);
		entity* members = (entity*)_ao_members.self;
		memcpy(members, group_data->data(), group_data->size);
		group_to_prefab(members, size);
		return instantiate_prefab(members, size, count);
		prefab_to_group(members, size);
	}
}

chunk_vector<chunk_slice> world::batch(entity* es, uint32_t count)
{
	chunk_vector<chunk_slice> result;
	uint32_t i = 0;
	while (i < count)
	{
		const auto& datas = ents.datas;
		const auto& start = datas[es[i++].id];
		chunk_slice s{ start.c, start.i, 1 };
		while (i < count)
		{
			const auto& curr = datas[es[i].id];
			if (curr.i != start.i + s.count || curr.c != start.c)
				break;
			i++; s.count++;
		}
		result.push(s);
	}
	return result;
}

archetype_filter world::cache_query(const archetype_filter& type)
{
	auto& cache = get_query_cache(type);
	return cache.filter;
}

void world::estimate_shared_size(tsize_t& size, archetype* t) const
{
	entity* metas = t->metatypes();
	forloop(i, 0, t->metaCount)
	{
		auto g = get_archetype(metas[i]);
		size += g->componentCount;
		estimate_shared_size(size, g);
	}
}

void world::get_shared_type(typeset& type, archetype* t, typeset& buffer) const
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

void world::destroy(chunk_slice s)
{
	archetype* g = s.c->type;
	tsize_t id = g->index(group_id);
	if (id != InvalidIndex)
	{
		uint16_t* sizes = g->sizes();
		char* src = (s.c->data() + g->offsets(s.c->ct)[id]);
		forloop(i, 0, s.count)
		{
			auto* group_data = (buffer*)(src + (size_t)i * sizes[i]);
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
	if (id == InvalidIndex)
		return get_shared_ro(g, type);
	return c->data() + (size_t)g->offsets(c->ct)[id] + (size_t)data.i * g->sizes()[id];
}

const void* world::get_owned_ro(entity e, index_t type) const noexcept
{
	if (!exist(e))
		return nullptr;
	const auto& data = ents.datas[e.id];
	chunk* c = data.c; archetype* g = c->type;
	tsize_t id = g->index(type);
	if (id == InvalidIndex) 
		return nullptr;
	return c->data() + g->offsets(c->ct)[id] + (size_t)data.i * g->sizes()[id];
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
	if (id == InvalidIndex) return nullptr;
	g->timestamps(c)[id] = timestamp;
	return c->data() + g->offsets(c->ct)[id] + (size_t)data.i * g->sizes()[id];
}


const void* world::get_component_ro(chunk* c, index_t t) const noexcept
{
	archetype* g = c->type;
	tsize_t id = g->index(t);
	if (id == InvalidIndex) 
		return get_shared_ro(g, t);
	return c->data() + c->type->offsets(c->ct)[id];
}

const void* world::get_owned_ro(chunk* c, index_t t) const noexcept
{
	tsize_t id = c->type->index(t);
	if (id == InvalidIndex) return nullptr;
	return c->data() + c->type->offsets(c->ct)[id];
}

const void* world::get_shared_ro(chunk* c, index_t type) const noexcept
{
	archetype* g = c->type;
	return get_shared_ro(g, type);
}

void* world::get_owned_rw(chunk* c, index_t t) noexcept
{
	tsize_t id = c->type->index(t);
	if (id == InvalidIndex) return nullptr;
	c->type->timestamps(c)[id] = timestamp;
	return c->data() + c->type->offsets(c->ct)[id];
}

const void* world::get_owned_ro_local(chunk* c, index_t type) const noexcept
{
	return c->data() + c->type->offsets(c->ct)[type];
}

void* world::get_owned_rw_local(chunk* c, index_t type) noexcept
{
	return c->data() + c->type->offsets(c->ct)[type];
}

const void* world::get_shared_ro(archetype* g, index_t type) const
{
	entity* metas = g->metatypes();
	forloop(i, 0, g->metaCount)
		if (const void* shared = get_component_ro(metas[i], type))
			return shared;
	return nullptr;
}

bool world::share_component(archetype* g, const typeset& type) const
{
	entity* metas = g->metatypes();
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
	if (id == InvalidIndex) return 0;
	return c->type->sizes()[id];
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
				if(e!=source)
					ents->push(e);
				return e;
			}
		} p;
		p.source = e;
		p.ents = &result;
		auto& data = ents.datas[e.id];
		auto g = data.c->type;
		auto mt = g->metatypes();
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
			auto mt = g->metatypes();
			forloop(i, 0, g->metaCount)
				p.patch(mt[i]);
			chunk::patch({ data.c, data.i, 1 }, &p);
		}
	}
}

void world::serialize(serializer_i* s, entity src)
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
	uint32_t count = sents.datas.size;
	adaptive_object _ao_patch(sizeof(entity) * count);
	entity* patch = (entity*)_ao_patch.self;
	forloop(i, 0, count)
		if (sents.datas[i].c != nullptr)
			patch[i] = ents.new_entity();
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
			c = next;
			patch_chunk(c, &p);
		}
		release_reference(g);
		free(g);
	}
	src.archetypes.clear();
}

void world::patch_chunk(chunk* c, patcher_i* patcher)
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

void world::serialize(serializer_i* s)
{
	s->stream(&ents.datas.size, sizeof(uint32_t));

	for (auto& pair : archetypes)
	{
		archetype* g = pair.second;
		if (g->cleaning)
			continue;
		serialize_archetype(g, s);
		for (chunk* c = g->firstChunk; c; c= c->next)
			chunk::serialize({ c, 0, c->count }, s);

		s->stream(0, sizeof(tsize_t));
	}
	s->stream(0, sizeof(tsize_t));
}

void world::deserialize(serializer_i* s)
{
	clear();

	s->stream(&ents.datas.size, sizeof(uint32_t));

	//reallocate entity data buffer
	ents.datas.reserve(ents.datas.size);

	//todo: multithread?
	for (archetype* g = deserialize_archetype(s, nullptr); g != nullptr; g = deserialize_archetype(s, nullptr))
		for (auto slice = deserialize_slice(g, s); slice; slice = deserialize_slice(g, s))
		{
			//reinitialize entity data
			auto ref = get_entities(slice->c);
			forloop(i, 0, slice->count)
			{
				auto index = i + slice->start;
				auto e = ref[index];
				auto& data = ents.datas[e.id];
				data.c = slice->c;
				data.i = index;
				data.v = e.version;
			}
		}

	//reinitialize entity free list
	forloop(i, 0, ents.datas.size)
	{
		auto& data = ents.datas[i];
		if (data.c == nullptr)
		{
			data.nextFree = ents.free;
			ents.free = data.i;
		}
	}
}

void world::clear()
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
	ents.clear();
	queries.clear();
	archetypes.clear();
}

void world::gc_meta()
{
	for (auto& gi : archetypes)
	{
		auto g = gi.second;
		auto mt = g->metatypes();
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
	auto& m = *(mask*)(c->data() + g->offsets(c->ct)[id] + (size_t)data.i * g->sizes()[id]);
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
	auto& m = *(mask*)(c->data() + g->offsets(c->ct)[id] + (size_t)data.i * g->sizes()[id]);
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
	g->timestamps(c)[id] = timestamp;
	auto& m = *(mask*)(c->data() + g->offsets(c->ct)[id] + (size_t)data.i * g->sizes()[id]);
	return (m & mm) == mm;
}

bool world::exist(entity e) const noexcept
{
	return e.id < ents.datas.size && e.version == ents.datas[e.id].v;
}

void world::entities::clear()
{
	free = 0;
	datas.~chunk_vector<data>();
}

void world::entities::new_entities(chunk_slice s)
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

entity world::entities::new_prefab()
{
	uint32_t id = (uint32_t)datas.size;
	datas.push();
	return { id, datas[id].v };
}

entity world::entities::new_entity()
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

chunk_vector<matched_archetype> world::query(const archetype_filter& filter)
{
	chunk_vector<matched_archetype> result;
	auto& cache = get_query_cache(filter);
	for (auto& type : cache.archetypes)
		result.push(type);
	return result;
}

chunk_vector<chunk*> world::query(archetype* type , const chunk_filter& filter)
{
	chunk_vector<chunk*> result;
	auto iter = type->firstChunk;

	while (iter != nullptr)
	{
		if (filter.match(type->get_type(), type->timestamps(iter)))
		{
			chunk* c = iter;
			iter = c->next;
			result.push(c);
		}
		else
			iter = iter->next;
	}
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

void chunk_vector_base::grow()
{
	if (data == nullptr)
		data = (void**)gd.malloc(alloc_type::fastbin);
	data[chunkSize++] = gd.malloc(alloc_type::fastbin);
}

void chunk_vector_base::shrink(size_t n)
{
	forloop(i, 0, n)
		gd.free(alloc_type::fastbin, data[--chunkSize]);
	if (data && chunkSize == 0)
	{
		gd.free(alloc_type::fastbin, data);
		data = nullptr;
	}
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
		data = (void**)gd.malloc(alloc_type::fastbin);
	forloop(i, 0, r.chunkSize)
	{
		void* newChunk = gd.malloc(alloc_type::fastbin);
		memcpy(newChunk, r.data[i], kFastBinSize);
		data[i] = newChunk;
	}
	size = r.size;
	chunkSize = r.chunkSize;
}

chunk_vector_base::~chunk_vector_base()
{
	shrink(chunkSize);
}
