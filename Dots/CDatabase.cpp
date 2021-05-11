#include "CDatabase.h"
#include "Database.h"

#define forloop(i, z, n) for(auto i = std::decay_t<decltype(n)>(z); i<(n); ++i)
namespace db = core::database;
/************************************chunk vector****************************************************/
void* get_vec(chunk_vector* vector, uint32_t index, size_t stride)
{
	const size_t kFastBinSize = core::database::chunk_vector_base::kChunkSize;
	const size_t kChunkCapacity = kFastBinSize / stride;
	return &((char*)vector->data[index / kChunkCapacity])[index % kChunkCapacity * stride];
}

void release_vec(chunk_vector* vector)
{
	db::chunk_vector_base cv;
	cv.chunkSize = vector->chunkSize;
	cv.size = vector->size; 
	cv.chunkCapacity = vector->chunkCapacity; 
	cv.data = vector->data;
	cv.reset();
}

chunk_vector move(db::chunk_vector_base&& cv)
{
	chunk_vector r{ cv.chunkSize, cv.size, cv.chunkCapacity, cv.data };
	cv.data = nullptr;
	cv.chunkCapacity = cv.chunkSize = cv.size = 0;
	return r;
}

/************************************batch iter******************************************************/
batch_iter iter_batch(world* wrd, const entity* ents, uint32_t count)
{
	auto iter = ((db::world*)wrd)->iter((const core::entity*)ents, count);
	return *(batch_iter*)&iter;
}

bool next_batch(world* wrd, batch_iter* iter)
{
	return ((db::world*)wrd)->next(*(db::batch_iter*)iter);
}

chunk_slice get_batch(const batch_iter* iter)
{
	return iter->s;
}

/************************************casters**************************************************************
* 
* 注意这里假设了两边的数据布局完全一致，修改时务必同步两边的修改
* 
********************************************************************************************************/
#define DEF_CASTER(type) \
db::type& cast(type& o) { static_assert(sizeof(db::type) == sizeof(type)); return *(db::type*)&o; } \
const db::type& cast(const type& o) { static_assert(sizeof(db::type) == sizeof(type)); return *(const db::type*)&o; } \
type& cast(db::type& o) { static_assert(sizeof(db::type) == sizeof(type)); return *(type*)&o; } \
const type& cast(const db::type& o) { static_assert(sizeof(db::type) == sizeof(type)); return *(const type*)&o; }

DEF_CASTER(chunk_slice)
DEF_CASTER(type_diff)
DEF_CASTER(entity_type)
DEF_CASTER(archetype_filter)
DEF_CASTER(chunk_filter)
DEF_CASTER(entity_filter)
DEF_CASTER(typeset)

core::entity cast(entity e)
{
	return (core::entity)e.value;
}
entity cast(core::entity e)
{
	entity r;
	r.value = e.value;
	return r;
}

#define DEF_POINTER_CASTER(type) \
db::type* cast(type* o) { return (db::type*)o; } \
type* cast(db::type* o) { return (type*)o; } \
const db::type* cast(const type* o) { return (db::type*)o; } \
const type* cast(const db::type* o) { return (type*)o; }

DEF_POINTER_CASTER(world)
DEF_POINTER_CASTER(chunk)
DEF_POINTER_CASTER(archetype)

struct serializer_wrapper final : db::serializer_i
{
	::serializer_i s;
	serializer_wrapper(::serializer_i s) : s(s) {}
	void stream(const void* data, uint32_t size) override { return s.vtable->stream(s.self, data, size); };
	bool is_serialize() override { return s.vtable->is_serialize(s.self); };
};

serializer_wrapper cast(::serializer_i s)
{
	return { s };
}

struct patcher_wrapper final : db::patcher_i
{
	::patcher_i s;
	patcher_wrapper(::patcher_i s) : s(s) {}
	core::entity patch(core::entity e) override { return cast(s.vtable->patch(s.self, cast(e))); }
	void move() override { s.vtable->move(s.self); }
	void reset() override { s.vtable->reset(s.self); }
};

patcher_wrapper cast(::patcher_i s)
{
	return { s };
}

/*********************************Database API*********************************************************/

chunk_vector allocate(world* wrd, entity_type* type, uint32_t count)
{
	return move(cast(wrd)->allocate(*(const db::entity_type*)type, count));
}

chunk_vector allocate_arch(world* wrd, archetype* g, uint32_t count)
{
	return move(cast(wrd)->allocate((db::archetype*)g, count));
}

chunk_vector instantiate(world* wrd, entity src, uint32_t count)
{
	return move(cast(wrd)->instantiate(cast(src), count));
}

//stuctural change
void destroy(world* wrd, chunk_slice s)
{
	cast(wrd)->destroy(cast(s));
}

/* note: return null if trigger chunk move or chunk clean up */
chunk_vector cast_diff(world* wrd, chunk_slice s, const type_diff* diff)
{
	return move(cast(wrd)->cast(cast(s), cast(*diff)));
}

chunk_vector cast_type(world* wrd, chunk_slice s, const entity_type* type)
{
	return move(cast(wrd)->cast(cast(s), cast(*type)));
}

//archetype behavior, lifetime
archetype* get_archetype_type(world* wrd, const entity_type* type)
{
	return cast(cast(wrd)->get_archetype(cast(*type)));
}

archetype* get_archetype_slice(world* wrd, chunk_slice s)
{
	return cast(cast(wrd)->get_archetype(cast(s)));
}

archetype* get_cleaning(world* wrd, archetype* g)
{
	return cast(cast(wrd)->get_cleaning(cast(g)));
}

bool is_cleaned(world* wrd, const entity_type* type)
{
	return cast(wrd)->is_cleaned(cast(*type));
}

archetype* get_casted_arch(world* wrd, archetype* g, const type_diff* diff, bool inst)
{
	return cast(cast(wrd)->get_casted(cast(g), cast(*diff), inst ));
}

chunk_vector cast_arch(world* wrd, chunk_slice s, archetype* g)
{
	return move(cast(wrd)->cast(cast(s), cast(g)));
}


//query iterators
chunk_vector query_arch(world* wrd, const archetype_filter* filter)
{
	return move(cast(wrd)->query(cast(*filter)));
}

chunk_vector query_chunk(world* wrd, const archetype* g, const chunk_filter* filter)
{
	return move(cast(wrd)->query(cast(g), cast(*filter)));
}

chunk_vector get_archetypes(world* wrd)
{
	return move(cast(wrd)->get_archetypes());
}

/*** per entity ***/
//query
const void* get_component_ro_entity(world* wrd, entity e, index_t type)
{
	return cast(wrd)->get_component_ro(cast(e), type);
}

const void* get_owned_ro_entity(world* wrd, entity e, index_t type)
{
	return cast(wrd)->get_owned_ro(cast(e), type);
}

const void* get_shared_ro_entity(world* wrd, entity e, index_t type)
{
	return cast(wrd)->get_shared_ro(cast(e), type);
}

bool is_a(world* wrd, entity e, const entity_type* type)
{
	return cast(wrd)->is_a(cast(e), cast(*type));
}

bool share_component(world* wrd, entity e, const typeset* type)
{
	return cast(wrd)->share_component(cast(e), cast(*type));
}

bool has_component(world* wrd, entity e, const typeset* type)
{
	return cast(wrd)->has_component(cast(e), cast(*type));
}

bool own_component(world* wrd, entity e, const typeset* type)
{
	return cast(wrd)->own_component(cast(e), cast(*type));
}

bool is_component_enabled(world* wrd, entity e, const typeset* type)
{
	return cast(wrd)->is_component_enabled(cast(e), cast(*type));
}

bool exist(world* wrd, entity e)
{
	return cast(wrd)->exist(cast(e));
}

archetype* get_archetype_entity(world* wrd, entity e)
{
	return cast(cast(wrd)->get_archetype(cast(e)));
}

//update
void* get_owned_rw_entity(world* wrd, entity e, index_t type)
{
	return cast(wrd)->get_owned_rw(cast(e), type);
}

void enable_component(world* wrd, entity e, const typeset* type)
{
	cast(wrd)->enable_component(cast(e), cast(*type));
}

void disable_component(world* wrd, entity e, const typeset* type)
{
	cast(wrd)->disable_component(cast(e), cast(*type));
}

entity_type get_type(world* wrd, entity e) /* note: only owned */
{
	return cast(cast(wrd)->get_type(cast(e)));
}

//entity/group serialize
chunk_vector gather_reference(world* wrd, entity e)
{
	return move(cast(wrd)->gather_reference(cast(e)));
}

void serialize_entity(world* wrd, serializer_i s, entity e)
{
	auto sw = cast(s);
	cast(wrd)->serialize(&sw, cast(e));
}

entity deserialize_entity(world* wrd, serializer_i s, patcher_i patcher)
{
	auto sw = cast(s);
	auto sp = cast(patcher);
	return cast(cast(wrd)->deserialize(&sw, &sp));
}


/*** per chunk or archetype ***/
//query
const void* get_component_ro(world* wrd, chunk* c, index_t type)
{
	return cast(wrd)->get_component_ro(cast(c), type);
}

const void* get_owned_ro(world* wrd, chunk* c, index_t type)
{
	return cast(wrd)->get_owned_ro(cast(c), type);
}

const void* get_shared_ro(world* wrd, chunk* c, index_t type)
{
	return cast(wrd)->get_shared_ro(cast(c), type);
}

void* get_owned_rw(world* wrd, chunk* c, index_t type)
{
	return cast(wrd)->get_owned_rw(cast(c), type);
}

const void* get_owned_ro_local(world* wrd, chunk* c, index_t type)
{
	return cast(wrd)->get_owned_ro_local(cast(c), type);
}

void* get_owned_rw_local(world* wrd, chunk* c, index_t type)
{
	return cast(wrd)->get_owned_rw_local(cast(c), type);
}

const entity* get_entities(world* wrd, chunk* c)
{
	return (const entity*)cast(wrd)->get_entities(cast(c));
}

uint16_t get_size(world* wrd, chunk* c, index_t type)
{
	return cast(wrd)->get_size(cast(c), type);
}

const void* get_shared_ro_arch(world* wrd, archetype* g, index_t type)
{
	return cast(wrd)->get_shared_ro(cast(g), type);
}

bool share_component_arch(world* wrd, archetype* g, const typeset* type)
{
	return cast(wrd)->share_component(cast(g), cast(*type));
}

bool own_component_arch(world* wrd, archetype* g, const typeset* type)
{
	return cast(wrd)->own_component(cast(g), cast(*type));
}

bool has_component_arch(world* wrd, archetype* g, const typeset* type)
{
	return cast(wrd)->has_component(cast(g), cast(*type));
}



/*** per world ***/
void move_context(world* wrd, world* src)
{
	return cast(wrd)->move_context(*cast(src));
}

void patch_chunk(world* wrd, chunk* c, patcher_i patcher)
{
	auto pw = cast(patcher);
	return cast(wrd)->patch_chunk(cast(c), &pw);
}

//serialize
void serialize_world(world* wrd, serializer_i s)
{
	auto sw = cast(s);
	return cast(wrd)->serialize(&sw);
}

void deserialize_world(world* wrd, serializer_i s)
{
	auto sw = cast(s);
	return cast(wrd)->deserialize(&sw);
}

//clear
void clear_world(world* wrd)
{
	return cast(wrd)->clear();
}

void gc_meta(world* wrd)
{
	return cast(wrd)->gc_meta();
}

void merge_chunks(world* wrd)
{
	return cast(wrd)->merge_chunks();
}

//query
int get_timestamp(world* wrd)
{
	return cast(wrd)->get_timestamp();
}

void inc_timestamp(world* wrd)
{
	return cast(wrd)->inc_timestamp();
}
