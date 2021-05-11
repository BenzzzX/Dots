#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <array>
#include <optional>
#include <functional>
#include "DotsRuntime.h"
#define ECS_API
#include "Type.h"

namespace core
{
	namespace database
	{
		struct chunk;
		class world;

		ECS_API index_t register_type(component_desc desc);
		struct builtin_id
		{
			index_t disable_id;
			index_t cleanup_id;
			index_t group_id;
			index_t mask_id;
#ifdef ENABLE_GUID_COMPONENT
			index_t guid_id;
#endif
		};
#ifdef ENABLE_GUID_COMPONENT
		ECS_API core::GUID new_guid();
#endif

		ECS_API builtin_id get_builtin();
#include "ChunkVector.h"
#include "Buffer.h"
		template<class T>
		struct local_span
		{
			intptr_t offset;
			size_t count;
			T* resolve(void* store) { return (T*)((char*)store + offset); }
		};
		struct world_delta
		{
			using array_delta = std::vector<local_span<char>>;
			struct vector_delta
			{
				size_t length;
				array_delta content;
			};
			using component_delta = std::unique_ptr<array_delta[]>;
			using buffer_delta = std::unique_ptr<std::vector<vector_delta>[]>;
			struct slice_delta
			{
				entity_type type;
				local_span<GUID> ents;
				component_delta diffs;
				buffer_delta bufferDiffs;
			};

			struct slice_data
			{
				entity_type type;
				intptr_t offset;
			};

			std::vector<slice_delta> changed;
			std::vector<slice_data> created;
			std::vector<entity> destroyed;
			std::vector<char> store;
		};

		struct ECS_API chunk_slice
		{
			chunk* c;
			uint32_t start;
			uint32_t count;
			bool full();
			chunk_slice() :c(nullptr), start(0), count(0) {}
			chunk_slice(chunk* c);
			chunk_slice(chunk* c, uint32_t s, uint32_t count)
				: c(c), start(s), count(count) {}
		};

		struct managed_func
		{
			void(*copy)(char* dst, const char* src, size_t n) = nullptr;
			void(*constructor)(char* data, size_t n) = nullptr;
			void(*destructor)(char* data, size_t n) = nullptr;
		};

		struct ECS_API archetype
		{
			chunk* firstChunk;
			chunk* lastChunk;
			chunk* firstFree;
			tsize_t componentCount;
			tsize_t firstTag;
			tsize_t firstManaged;
			tsize_t metaCount;
			tsize_t firstBuffer;
			uint16_t chunkCount;
			uint32_t chunkCapacity[3];
			uint32_t timestamp;
			uint32_t size;
			uint32_t entitySize;
			index_t* types;
			uint32_t* offsets[3];
			uint16_t* sizes;
			entity* metatypes;
			managed_func* managedFuncs;

			bool disabled;
			bool cleaning;
			bool copying;
			bool withMask;
			bool withTracked;
			bool zerosize;

			/*
			index_t types[componentCount];
			uint32_t offsets[3][firstTag];
			uint16_t sizes[firstTag];
			entity metatypes[metaCount];
			managed_func managedFuncs[firstTag - firstManaged]
			*/
			inline char* data() noexcept { return (char*)(this + 1); };
			uint32_t* timestamps(chunk* c) const  noexcept;
			tsize_t index(index_t type) const  noexcept;
			mask get_mask(const typeset& subtype) noexcept;

			inline entity_type get_type() const;

			size_t get_size();
		};

		struct matched_archetype
		{
			archetype* type;
			mask matched;
		};

		struct type_diff
		{
			entity_type extend = EmptyType;
			entity_type shrink = EmptyType;
		};

		struct batch_iter
		{
			const entity* es;
			uint32_t count;
			uint32_t i;
			chunk_slice s;
		};

		struct batch_range
		{
			const world& ctx;
			const entity* es;
			uint32_t count;
			struct iterator_end {};
			struct iterator
			{
				const world& ctx;
				batch_iter iter;
				bool valid;
				iterator& operator++();
				chunk_slice operator*() { return iter.s; }
				bool operator!=(const iterator_end& other) { return valid; }
			};
			iterator begin() const;
			iterator_end end() const { return {}; }
		};

		class world
		{
		public:
			//entity allocator
			struct entities
			{
				struct data
				{
					union
					{
						chunk* c;
						uint32_t nextFree;
					};
					uint32_t i : 24;
					uint32_t v : 8;
				};
				chunk_vector<data> datas;
				uint32_t free = 0;
				void clear();
				void new_entities(chunk_slice slice);
				void new_entities(entity* dst, uint32_t count);
				entity new_prefab(int sizeHint);
				entity new_entity(int sizeHint);
				void free_entities(chunk_slice slice);
				void move_entities(chunk_slice dst, const chunk* src, uint32_t srcIndex);
				void fill_entities(chunk_slice dst, uint32_t srcIndex);
				void clone(entities*);
			};

			struct query_cache
			{
				std::unique_ptr<char[]> data;
				bool includeDisabled;
				bool includeClean;
				archetype_filter filter;
				//TODO: use fixed_vector
				std::vector<matched_archetype> archetypes;
				using iterator = std::vector<matched_archetype>::iterator;
			};

			using queries_t = std::unordered_map<archetype_filter, query_cache, archetype_filter::hash>;
			using archetypes_t = std::unordered_map<entity_type, archetype*, entity_type::hash>;

			archetypes_t archetypes;
			mutable queries_t queries;
			entities ents;
			uint32_t* typeTimestamps;
			index_t typeCapacity;

			//query behavior
			query_cache& get_query_cache(const archetype_filter& f) const;
			void update_queries(archetype* g, bool add);
			archetype_filter cache_query(const archetype_filter& type);

			//archetype-chunk behavior
			static void remove(chunk*& head, chunk*& tail, chunk* toremove);
			void add_chunk(archetype* g, chunk* c);
			void remove_chunk(archetype* g, chunk* c);
			static void mark_free(archetype* g, chunk* c);
			static void unmark_free(archetype* g, chunk* c);
			static chunk* malloc_chunk(alloc_type type);
			chunk* new_chunk(archetype*, uint32_t hint);
			void destroy_chunk(archetype*, chunk*);
			void recycle_chunk(chunk*);
			void resize_chunk(chunk*, uint32_t);
			void merge_chunks(archetype*);

			//archetype behavior
			archetype* get_archetype(const entity_type&);
			archetype* construct_archetype(const entity_type& key);
			void add_archetype(archetype*);
			void structural_change(archetype* g, chunk* c);

			//entity behavior
			chunk_slice allocate_slice(archetype*, uint32_t = 1);
			void free_slice(chunk_slice);
			chunk_slice as_slice(entity) const;
			chunk_vector<chunk_slice> cast_slice(chunk_slice, archetype*);

			//serialize behavior
			static void serialize_archetype(archetype* g, serializer_i* s);
			archetype* deserialize_archetype(serializer_i* s, patcher_i* patcher, bool createNew);
			void serialize_slice(const chunk_slice& slice, serializer_i* s);
			bool deserialize_slice(archetype* g, serializer_i* s, chunk_slice& slice);

			//group behavior
			void group_to_prefab(entity* src, uint32_t size, bool keepExternal = true);
			void prefab_to_group(entity* src, uint32_t count);
			chunk_vector<chunk_slice> instantiate_group(buffer* group, uint32_t count);
			chunk_vector<chunk_slice> instantiate_prefab(entity* src, uint32_t size, uint32_t count);
			chunk_vector<chunk_slice> instantiate_single(entity src, uint32_t count);
			void serialize_single(serializer_i* s, entity);
			void serialize_group(serializer_i* s, buffer* g);
			void deserialize_prefab(serializer_i* s, patcher_i* patcher, buffer* group, chunk_vector<entity>& result);
			chunk_slice deserialize_single(serializer_i* s, patcher_i* patcher);
			void destroy_single(chunk_slice);
			void estimate_group_size(uint32_t& size, buffer* root);
			void flatten_group(entity* data, uint32_t& i, buffer* root);

			//ownership utils
			void estimate_shared_size(tsize_t& size, archetype* t) const;
			void get_shared_type(typeset& type, archetype* t, typeset& buffer) const;
			void release_reference(archetype* g);

			friend chunk;
		public:
			ECS_API world(index_t typeCapacity = 4096u);
			ECS_API world(const world& other/*todo: ,archetype_filter*/);
			ECS_API world(world&& other);
			ECS_API ~world();
			ECS_API void operator=(world&& other);

			/*** per chunk slice ***/
			//create
			ECS_API chunk_vector<chunk_slice> allocate(const entity_type& type, uint32_t count = 1);
			ECS_API chunk_vector<chunk_slice> allocate(archetype* g, uint32_t count = 1);
			ECS_API chunk_vector<chunk_slice> instantiate(entity src, uint32_t count = 1);

			//stuctural change
			ECS_API void destroy(chunk_slice);
			/* note: return null if trigger chunk move or chunk clean up */
			ECS_API chunk_vector<chunk_slice> cast(chunk_slice, type_diff);
			ECS_API chunk_vector<chunk_slice> cast(chunk_slice, const entity_type& type);


			//archetype behavior, lifetime
			ECS_API archetype* find_archetype(const entity_type&);
			ECS_API archetype* get_archetype(chunk_slice) const noexcept;
			ECS_API archetype* get_cleaning(archetype*);
			ECS_API bool is_cleaned(const entity_type&);
			ECS_API archetype* get_casted(archetype*, type_diff diff, bool inst = false);
			ECS_API chunk_vector<chunk_slice> cast(chunk_slice, archetype* g);

			//query iterators
			ECS_API batch_iter iter(const entity* ents, uint32_t count) const;
			ECS_API batch_range batch(const entity* ents, uint32_t count) const;
			ECS_API bool next(batch_iter& iter) const;
			ECS_API chunk_vector<matched_archetype> query(const archetype_filter& filter) const;
			ECS_API chunk_vector<chunk*> query(const archetype*, const chunk_filter& filter = {}) const;
			ECS_API chunk_vector<archetype*> get_archetypes();


			/*** per entity ***/
			//query
			ECS_API const void* get_component_ro(entity, index_t type) const noexcept;
			ECS_API const void* get_owned_ro(entity, index_t type) const noexcept;
			ECS_API const void* get_shared_ro(entity, index_t type) const noexcept;
			ECS_API bool is_a(entity, const entity_type& type) const noexcept;
			ECS_API bool share_component(entity, const typeset& type) const;
			ECS_API bool has_component(entity, const typeset& type) const noexcept;
			ECS_API bool own_component(entity, const typeset& type) const noexcept;
			ECS_API bool is_component_enabled(entity, const typeset& type) const noexcept;
			ECS_API bool exist(entity) const noexcept;
			ECS_API archetype* get_archetype(entity) const noexcept;
			//update
			ECS_API void* get_owned_rw(entity, index_t type) const noexcept;
			ECS_API void enable_component(entity, const typeset& type) const noexcept;
			ECS_API void disable_component(entity, const typeset& type) const noexcept;
			ECS_API entity_type get_type(entity) const noexcept; /* note: only owned */
			//entity/group serialize
			ECS_API chunk_vector<entity> gather_reference(entity);
			ECS_API void serialize(serializer_i* s, entity);
			ECS_API entity deserialize(serializer_i* s, patcher_i* patcher);

			/*** per chunk or archetype ***/
			//query
			ECS_API const void* get_component_ro(chunk_slice c, index_t type) const noexcept;
			ECS_API const void* get_owned_ro(chunk_slice c, index_t type) const noexcept;
			ECS_API const void* get_shared_ro(chunk_slice c, index_t type) const noexcept;
			ECS_API void* get_owned_rw(chunk_slice c, index_t type) const noexcept;
			ECS_API const void* get_owned_ro_local(chunk_slice c, index_t type) const noexcept;
			ECS_API void* get_owned_rw_local(chunk_slice c, index_t type) noexcept;
			ECS_API const entity* get_entities(chunk_slice c) noexcept;
			ECS_API uint16_t get_size(chunk_slice c, index_t type) const noexcept;
			ECS_API const void* get_shared_ro(archetype *g, index_t type) const;
			ECS_API bool share_component(archetype* g, const typeset& type) const;
			ECS_API bool own_component(archetype* g, const typeset& type) const;
			ECS_API bool has_component(archetype* g, const typeset& type) const;


			/*** per world ***/
			ECS_API void move_context(world& src);
#ifdef ENABLE_GUID_COMPONENT
			ECS_API world_delta diff_context(world& base);
#endif
			ECS_API void patch_chunk(chunk* c, patcher_i* patcher);
			//serialize
			ECS_API void serialize(serializer_i* s);
			ECS_API void deserialize(serializer_i* s);
			//clear
			ECS_API void clear();
			ECS_API void gc_meta();
			ECS_API void merge_chunks();
			//query
			uint32_t timestamp;
			ECS_API int get_timestamp() { return timestamp; }
			ECS_API void inc_timestamp() { ++timestamp; }

			std::function<void(archetype*, bool)> on_archetype_update;
		};

		struct chunk
		{
		public:
			chunk *next, *prev;
			archetype* type;
			uint32_t count;
			alloc_type ct;
			/*
			entity entities[chunkCapacity];
			T1 component1[chunkCapacity];
			T2 component2[chunkCapacity];
				.
				.
				.
			uint32_t timestamps[firstTag];
			*/

			static void construct(chunk_slice) noexcept;
			static void destruct(chunk_slice) noexcept;
			static void move(chunk_slice dst, tsize_t srcIndex) noexcept;
			static void move(chunk_slice dst, const chunk* src, uint32_t srcIndex) noexcept;
			static void cast(chunk_slice dst, chunk* src, tsize_t srcIndex, bool destruct = true) noexcept;
			static void duplicate(chunk_slice dst, const chunk* src, tsize_t srcIndex) noexcept;
			static void patch(chunk_slice s, patcher_i* patcher) noexcept;
			static void serialize(chunk_slice s, serializer_i *stream, bool withEntities = true);
			size_t get_size();
			void link(chunk*) noexcept;
			void unlink() noexcept;
			void clone(chunk*) noexcept;
			char* data() { return (char*)(this + 1); }
			const char* data() const { return (char*)(this + 1); }
		public:
			ECS_API uint32_t get_count() { return count; }
			ECS_API mask get_mask(const typeset& ts) { return type->get_mask(ts); }
			ECS_API const entity* get_entities() const { return (entity*)data(); }
			ECS_API uint32_t get_timestamp(index_t type) noexcept;
			ECS_API archetype* get_type() noexcept { return type; }
		};

#ifdef ENABLE_GUID_COMPONENT
		ECS_API void initialize(core::GUID(*guid_generator)());
#else
		ECS_API void initialize();
#endif 
	};
}