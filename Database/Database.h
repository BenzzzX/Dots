#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <array>
#include <optional>
#include <functional>
#include "Set.h"
#include "Type.h"

namespace core
{
	namespace database
	{
		struct chunk;
		class world;

		enum class alloc_type : uint8_t
		{
			smallbin, fastbin, largebin
		};

		struct ECS_API chunk_slice
		{
			chunk* c;
			uint32_t start;
			uint32_t count;
			bool full();
			chunk_slice(chunk* c);
			chunk_slice(chunk* c, uint32_t s, uint32_t count)
				: c(c), start(s), count(count) {}
		};

		struct ECS_API archetype
		{
			chunk* firstChunk;
			chunk* lastChunk;
			chunk* firstFree;
			tsize_t componentCount;
			tsize_t firstTag;
			tsize_t metaCount;
			tsize_t firstBuffer;
			uint16_t chunkCount;
			uint32_t chunkCapacity[3];
			uint32_t timestamp;
			uint32_t size;
			uint32_t entitySize;
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
			*/
			using data_t = soa<index_t, uint32_t, uint32_t, uint32_t, uint16_t, entity>;
			inline data_t accessor() noexcept { return { componentCount, firstTag, firstTag, firstTag, firstTag, metaCount }; }
			inline char* data() noexcept { return (char*)(this + 1); };
			inline index_t* types() noexcept { return (index_t*)data(); }
			inline uint32_t* offsets(alloc_type type) noexcept { return  (uint32_t*)(data() + accessor().get_offset(1 + (int)type)); }
			inline uint16_t* sizes() noexcept { return (uint16_t*)(data() + accessor().get_offset(4)); }
			inline entity* metatypes() noexcept { return (entity*)(data() + accessor().get_offset(5)); }
			inline uint32_t* timestamps(chunk* c) noexcept;
			inline tsize_t index(index_t type) noexcept;
			inline mask get_mask(const typeset& subtype) noexcept;

			inline entity_type get_type();

			static size_t alloc_size(tsize_t componentCount, tsize_t firstTag, tsize_t firstMeta);
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

		class world
		{
		private:
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
			queries_t queries;
			entities ents;
			uint32_t* typeTimestamps;
			index_t typeCapacity;

			//query behavior
			query_cache& get_query_cache(const archetype_filter& f);
			void update_queries(archetype* g, bool add);
			archetype_filter cache_query(const archetype_filter& type);

			//archetype behavior, lifetime
			archetype* get_archetype(const entity_type&);
			void add_archetype(archetype*);
			archetype* get_cleaning(archetype*);
			bool is_cleaned(const entity_type&);
			archetype* get_casted(archetype*, type_diff diff, bool inst = false);
			void structural_change(archetype* g, chunk* c);

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

			//entity behavior
			chunk_slice allocate_slice(archetype*, uint32_t = 1);
			void free_slice(chunk_slice);
			chunk_vector<chunk_slice> cast_slice(chunk_slice, archetype*);
			chunk_vector<chunk_slice> cast(chunk_slice, archetype* g);

			//serialize behavior
			static void serialize_archetype(archetype* g, serializer_i* s);
			archetype* deserialize_archetype(serializer_i* s, patcher_i* patcher);
			std::optional<chunk_slice> deserialize_slice(archetype* g, serializer_i* s);

			//group behavior
			void group_to_prefab(entity* src, uint32_t size, bool keepExternal = true);
			void prefab_to_group(entity* src, uint32_t count);
			chunk_vector<chunk_slice> instantiate_prefab(entity* src, uint32_t size, uint32_t count);
			chunk_vector<chunk_slice> instantiate_single(entity src, uint32_t count);
			void serialize_single(serializer_i* s, entity);
			chunk_slice deserialize_single(serializer_i* s, patcher_i* patcher);
			void destroy_single(chunk_slice);

			//ownership utils
			void estimate_shared_size(tsize_t& size, archetype* t) const;
			void get_shared_type(typeset& type, archetype* t, typeset& buffer) const;
			void release_reference(archetype* g);

			friend chunk;
		public:
			ECS_API world(index_t typeCapacity = 4096u);
			ECS_API world(const world& other/*todo: ,archetype_filter*/);
			ECS_API ~world();
			

			/*** per chunk slice ***/
			//create
			ECS_API chunk_vector<chunk_slice> allocate(const entity_type& type, uint32_t count = 1);
			ECS_API chunk_vector<chunk_slice> instantiate(entity src, uint32_t count = 1);

			//stuctural change
			ECS_API void destroy(chunk_slice);
			/* note: return null if trigger chunk move or chunk clean up */
			ECS_API chunk_vector<chunk_slice> cast(chunk_slice, type_diff);
			ECS_API chunk_vector<chunk_slice> cast(chunk_slice, const entity_type& type);

			//query iterators
			ECS_API chunk_vector<chunk_slice> batch(entity* ents, uint32_t count);
			ECS_API chunk_vector<matched_archetype> query(const archetype_filter& filter);
			ECS_API chunk_vector<chunk*> query(archetype*, const chunk_filter& filter);


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
			ECS_API const void* get_component_ro(chunk* c, index_t type) const noexcept;
			ECS_API const void* get_owned_ro(chunk* c, index_t type) const noexcept;
			ECS_API const void* get_shared_ro(chunk* c, index_t type) const noexcept;
			ECS_API void* get_owned_rw(chunk* c, index_t type) noexcept;
			ECS_API const void* get_owned_ro_local(chunk* c, index_t type) const noexcept;
			ECS_API void* get_owned_rw_local(chunk* c, index_t type) noexcept;
			ECS_API const entity* get_entities(chunk* c) noexcept;
			ECS_API uint16_t get_size(chunk* c, index_t type) const noexcept;
			ECS_API const void* get_shared_ro(archetype *g, index_t type) const;
			ECS_API bool share_component(archetype* g, const typeset& type) const;
			ECS_API bool own_component(archetype* g, const typeset& type) const;
			ECS_API bool has_component(archetype* g, const typeset& type) const;


			/*** per world ***/
			ECS_API void move_context(world& src);
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
		};

		struct ECS_API chunk
		{
		private:
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
			static void serialize(chunk_slice s, serializer_i *stream);
			size_t get_size();
			void link(chunk*) noexcept;
			void unlink() noexcept;
			void clone(chunk*) noexcept;
			char* data() { return (char*)(this + 1); }
			const char* data() const { return (char*)(this + 1); }
			friend world; 
			friend archetype;
			friend world::entities;
		public:
			uint32_t get_count() { return count; }
			mask get_mask(const typeset& ts) { return type->get_mask(ts); }
			const entity* get_entities() const { return (entity*)data(); }
			uint32_t get_timestamp(index_t type) noexcept;
		};
		
		ECS_API void initialize();
	};
}