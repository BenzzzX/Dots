#pragma once
#include <inttypes.h>

extern "C"
{
typedef struct world world;
struct entity
{
	union
	{
		uint32_t value;
		struct
		{
			uint32_t version : 8;
			uint32_t id : 24;
		};
	};
};
typedef struct chunk chunk;
typedef struct archetype archetype;
typedef uint32_t index_t;
typedef uint16_t tsize_t;
typedef struct chunk_slice
{
	chunk* c;
	uint32_t start;
	uint32_t count;
} chunk_slice;
typedef struct typeset
{
	const index_t* data;
	tsize_t length;
} typeset;
typedef struct metaset
{
	const entity* data;
	tsize_t length;
} metaset;
typedef struct entity_type
{
	typeset types;
	metaset metatypes;
} entity_type;
typedef struct type_diff
{
	entity_type extend;
	entity_type shrink;
} type_diff;
typedef struct archetype_filter
{
	entity_type all;
	entity_type any;
	entity_type none;
	typeset shared;
	typeset owned;
} archetype_filter;
typedef struct chunk_filter
{
	typeset changed;
	size_t prevTimestamp;
} chunk_filter;
typedef struct entity_filter
{
	typeset inverseMask;
} entity_filter;
typedef struct serializer_vtable
{
	void(*stream)(void* self, const void* data, uint32_t size);
	bool(*is_serialize)(void* self);
} serializer_vtable;
typedef struct serializer_i
{
	serializer_vtable* vtable;
	void* self;
} serializer_i;
typedef struct patcher_vtable
{
	entity(*patch)(void* self, entity e);
	void(*move)(void* self);
	void(*reset)(void* self);
} patcher_vtable;
typedef struct patcher_i
{
	patcher_vtable* vtable;
	void* self;
} patcher_i;

typedef struct chunk_vector
{
	size_t chunkSize;
	size_t size;
	size_t chunkCapacity;
	void** data;
} chunk_vector;
void* get_vec(const chunk_vector* vector, uint32_t index, size_t stride);
void release_vec(chunk_vector* vector);


struct batch_iter
{
	const entity* es;
	uint32_t count;
	uint32_t i;
	chunk_slice s;
};
batch_iter iter_batch(world* wrd, const entity* ents, uint32_t count);
bool next_batch(world* wrd, batch_iter* iter);
chunk_slice get_batch(const batch_iter* iter);
chunk_vector batch(world* wrd, const entity* ents, uint32_t count);


chunk_vector allocate(world* wrd, entity_type* type, uint32_t count = 1);
chunk_vector allocate_arch(world* wrd, archetype* g, uint32_t count = 1);
chunk_vector instantiate(world* wrd, entity src, uint32_t count = 1);

//stuctural change
void destroy(world* wrd, chunk_slice);
/* note: return null if trigger chunk move or chunk clean up */
chunk_vector cast_diff(world* wrd, chunk_slice, const type_diff*);
chunk_vector cast_type(world* wrd, chunk_slice, const entity_type* type);


//archetype behavior, lifetime
archetype* get_archetype_type(world* wrd, const entity_type*);
archetype* get_archetype_slice(world* wrd, chunk_slice);
archetype* get_cleaning(world* wrd, archetype*);
bool is_cleaned(world* wrd, const entity_type*);
archetype* get_casted_arch(world* wrd, archetype*, const type_diff* diff, bool inst = false);
chunk_vector cast_arch(world* wrd, chunk_slice, archetype* g);

//query iterators
chunk_vector query_arch(world* wrd, const archetype_filter* filter);
chunk_vector query_chunk(world* wrd, const archetype*, const chunk_filter* filter);
chunk_vector get_archetypes(world* wrd);


/*** per entity ***/
//query
const void* get_component_ro_entity(world* wrd, entity, index_t type);
const void* get_owned_ro_entity(world* wrd, entity, index_t type);
const void* get_shared_ro_entity(world* wrd, entity, index_t type);
bool is_a(world* wrd, entity, const entity_type* type);
bool share_component(world* wrd, entity, const typeset* type);
bool has_component(world* wrd, entity, const typeset* type);
bool own_component(world* wrd, entity, const typeset* type);
bool is_component_enabled(world* wrd, entity, const typeset* type);
bool exist(world* wrd, entity);
archetype* get_archetype_entity(world* wrd, entity);
//update
void* get_owned_rw_entity(world* wrd, entity, index_t type);
void enable_component(world* wrd, entity, const typeset* type);
void disable_component(world* wrd, entity, const typeset* type);
entity_type get_type(world* wrd, entity); /* note: only owned */
//entity/group serialize
chunk_vector gather_reference(world* wrd, entity);
void serialize_entity(world* wrd, serializer_i s, entity);
entity deserialize_entity(world* wrd, serializer_i s, patcher_i patcher);

/*** per chunk or archetype ***/
//query
const void* get_component_ro(world* wrd, chunk* c, index_t type);
const void* get_owned_ro(world* wrd, chunk* c, index_t type);
const void* get_shared_ro(world* wrd, chunk* c, index_t type);
void* get_owned_rw(world* wrd, chunk* c, index_t type);
const void* get_owned_ro_local(world* wrd, chunk* c, index_t type);
void* get_owned_rw_local(world* wrd, chunk* c, index_t type);
const entity* get_entities(world* wrd, chunk* c);
uint16_t get_size(world* wrd, chunk* c, index_t type);
const void* get_shared_ro_arch(world* wrd, archetype* g, index_t type);
bool share_component_arch(world* wrd, archetype* g, const typeset* type);
bool own_component_arch(world* wrd, archetype* g, const typeset* type);
bool has_component_arch(world* wrd, archetype* g, const typeset* type);


/*** per world ***/
void move_context(world* wrd, world* src);
//TODO：world_delta 使用了大量的 std::vector，如何标准化。。。
//#ifdef ENABLE_GUID_COMPONENT
//world_delta diff_context(world* wrd, world* base);
//#endif
void patch_chunk(world* wrd, chunk* c, patcher_i patcher);
//serialize
void serialize_world(world* wrd, serializer_i s);
void deserialize_world(world* wrd, serializer_i s);
//clear
void clear_world(world* wrd);
void gc_meta(world* wrd);
void merge_chunks(world* wrd);
//query
int get_timestamp(world* wrd);
void inc_timestamp(world* wrd);
}