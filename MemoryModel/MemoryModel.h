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
		class context;
		struct chunk;

		struct chunk_slice
		{
			chunk* c;
			uint16_t start;
			uint16_t count;
			bool full();
			chunk_slice(chunk* c);
			chunk_slice(chunk* c, uint16_t s, uint16_t count)
				: c(c), start(s), count(count) {}
		};

		class context
		{
			struct archetype
			{
				chunk* firstChunk;
				chunk* lastChunk;
				chunk* firstFree;
				uint16_t componentCount;
				uint16_t firstTag;
				uint16_t metaCount;
				uint16_t firstBuffer;
				uint16_t chunkCapacity;
				uint32_t timestamp;
				uint32_t size;
				bool disabled;
				bool cleaning;
				bool withMask;
				bool withTracked;
				bool zerosize;

				/*
				index_t types[componentCount];
				uint16_t offsets[firstTag];
				uint16_t sizes[firstTag];
				index_t metatypes[componentCount - firstMeta];
				*/

				inline char* data() noexcept { return (char*)(this + 1); };
				inline index_t* types() noexcept { return (index_t*)data(); }
				inline uint16_t* offsets() noexcept { return  (uint16_t*)(data() + componentCount ); }
				inline uint16_t* sizes() noexcept { return (uint16_t*)(data() + componentCount * sizeof(index_t) + firstTag * sizeof(uint16_t)); }
				inline entity* metatypes() noexcept { return (entity*)(data() + componentCount * sizeof(index_t) + (firstTag + firstTag) * sizeof(uint16_t)); }
				inline uint32_t* timestamps(chunk* c) noexcept;
				inline uint16_t index(index_t type) noexcept;
				inline mask get_mask(const entity_type& subtype) noexcept;

				inline entity_type get_type();

				static size_t calculate_size(uint16_t componentCount, uint16_t firstTag, uint16_t firstMeta);
			};

			struct entities
			{
				struct data
				{
					chunk* c;
					uint32_t i;
					uint32_t v;
				} *datas = nullptr;
				uint32_t free = 0;
				uint32_t size = 0;
				uint32_t capacity = 0;
				~entities();
				void new_entities(chunk_slice slice);
				entity new_prefab();
				entity new_entity();
				void free_entities(chunk_slice slice);
				void move_entities(chunk_slice dst, const chunk* src, uint16_t srcIndex);
				void fill_entities(chunk_slice dst, uint16_t srcIndex);
			};

			using archetypes_t = std::unordered_map<entity_type, archetype*, entity_type::hash>;

			struct batch_iterator
			{
				entity* ents;
				uint32_t count;
				context* cont;
				uint32_t i;

				std::optional<chunk_slice> next();
			};

			struct alloc_iterator
			{
				context* cont;
				archetype* g;
				entity* ret;
				uint32_t count;
				uint32_t k;

				std::optional<chunk_slice> next();
			};

			struct query_cache
			{
				std::vector<char> data;
				bool includeDisabled;
				bool includeClean;
				entity_filter filter;
				std::vector<archetype*> archetypes;
			};

			struct structural_change
			{
				void* extend;
				void* shrink;
			};

			using queries_t = std::unordered_map<entity_filter, query_cache, entity_filter::hash>;

			query_cache& get_query_cache(const entity_filter& f);
			void update_queries(archetype* g, bool add);

			static constexpr size_t kChunkPoolCapacity = 8000;

			std::array<chunk*, kChunkPoolCapacity> chunkPool;
			uint16_t poolSize = 0;
			archetypes_t archetypes;
			queries_t queries;
			entities ents;

			static void remove(chunk*& head, chunk*& tail, chunk* toremove);

			archetype* get_archetype(const entity_type&);
			archetype* get_cleaning(archetype*);
			bool is_cleaned(const entity_type&);
			archetype* get_instatiation(archetype*);
			archetype* get_extending(archetype*, const entity_type&);
			archetype* get_shrinking(archetype*, const entity_type&);

			void add_chunk(archetype* g, chunk* c);
			void remove_chunk(archetype* g, chunk* c);
			static void mark_free(archetype* g, chunk* c);
			static void unmark_free(archetype* g, chunk* c);

			chunk* new_chunk(archetype*);
			void destroy_chunk(archetype*, chunk*);
			void resize_chunk(chunk*, uint16_t);

			chunk_slice allocate_slice(archetype*, uint32_t = 1);
			void free_slice(chunk_slice);
			void cast_slice(chunk_slice, archetype*);

			void release_reference(archetype* g);

			static void serialize_archetype(archetype* g, serializer_i* s);
			archetype* deserialize_archetype(serializer_i* s, patcher_i* patcher);
			std::optional<chunk_slice> deserialize_slice(archetype* g, serializer_i* s);

			void group_to_prefab(entity* src, uint32_t size, bool keepExternal = true);
			void prefab_to_group(entity* src, uint32_t count);
			void instantiate_prefab(entity* src, uint32_t size, entity* ret, uint32_t count);
			void instantiate_single(entity src, entity* ret, uint32_t count, std::vector<chunk_slice>* = nullptr, int32_t stride = 1);
			void serialize_single(serializer_i* s, entity);
			entity deserialize_single(serializer_i* s, patcher_i* patcher);
			void destroy_single(chunk_slice);
			void structural_change(archetype* g, chunk* c, int32_t count);

			friend chunk;
			friend batch_iterator;
		public:
			context(index_t typeCapacity = 4096u);
			~context();
			//create
			alloc_iterator allocate(const entity_type& type, entity* ret, uint32_t count = 1);
			void instantiate(entity src, entity* ret, uint32_t count = 1);

			//batched stuctural change
			void destroy(chunk_slice);
			void extend(chunk_slice, const entity_type& type);
			void shrink(chunk_slice, const entity_type& type);
			void cast(chunk_slice, const entity_type& type);
			void cast(chunk_slice, archetype* g);

			//stuctural change
			void destroy(entity* es, int32_t count);
			void destroy(const entity_filter& filter);
			void extend(entity* es, int32_t count, const entity_type& type);
			void extend(const entity_filter& filter, const entity_type& type);
			void shrink(entity* es, int32_t count, const entity_type& type);
			void shrink(const entity_filter& filter, const entity_type& type);
			void cast(entity* es, int32_t count, const entity_type& type);
			void cast(const entity_filter& filter, const entity_type& type);
			//update
			void* get_owned_rw(entity, index_t type);

			//query
			batch_iterator batch(entity* ents, uint32_t count);
			entity_filter cache_query(const entity_filter& type);
			const void* get_component_ro(entity, index_t type) const noexcept;
			const void* get_owned_ro(entity, index_t type) const noexcept;
			const void* get_shared_ro(entity, index_t type) const noexcept;
			bool has_component(entity, const entity_type& type) const noexcept;
			void enable_component(entity, const entity_type& type) const noexcept;
			void disable_component(entity, const entity_type& type) const noexcept;
			bool is_component_enabled(entity, const entity_type& type) const noexcept;
			bool exist(entity) const noexcept;
			const void* get_owned_ro(chunk* c, index_t type) const noexcept;
			const void* get_shared_ro(chunk* c, index_t type) const noexcept;
			void* get_owned_rw(chunk* c, index_t type) noexcept;
			const void* get_shared_ro(archetype *g, index_t type) const;
			const entity* get_entities(chunk* c) noexcept;
			uint16_t get_size(chunk* c, index_t type) const noexcept;
			entity_type get_type(entity) const noexcept;

			//as prefab
			void gather_reference(entity, std::pmr::vector<entity>& entities);
			void serialize(serializer_i* s, entity);
			void deserialize(serializer_i* s, patcher_i* patcher, entity*, uint32_t times = 1);

			//multi context
			void move_context(context& src);
			void patch_chunk(chunk* c, patcher_i* patcher);

			void create_snapshot(serializer_i* s);
			void load_snapshot(serializer_i* s);
			void append_snapshot(serializer_i* s, entity* ret);

			//clear
			void clear();
			void gc_meta();

			uint32_t *typeTimestamps;
			index_t typeCapacity;
			uint32_t timestamp;
		};

		struct chunk
		{
		private:
			chunk *next, *prev;
			context::archetype* type;
			uint16_t count;
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
			static void move(chunk_slice dst, uint16_t srcIndex) noexcept;
			static void cast(chunk_slice dst, chunk* src, uint16_t srcIndex) noexcept;
			static void duplicate(chunk_slice dst, const chunk* src, uint16_t srcIndex) noexcept;
			static void patch(chunk_slice s, patcher_i* patcher) noexcept;
			static void serialize(chunk_slice s, serializer_i *stream);
			void link(chunk*) noexcept;
			void unlink() noexcept;
			char* data() { return (char*)(this + 1); }
			const char* data() const { return (char*)(this + 1); }
			friend class context;
			friend context::entities;
		public:
			uint16_t get_count() { return count; }
			const entity* get_entities() const { return (entity*)data(); }
			uint32_t get_timestamp(index_t type) noexcept;
		};

		//system overhead
		static constexpr size_t kChunkSize = 16 * 1024 - 256;
		static constexpr size_t kChunkBufferSize = kChunkSize - sizeof(chunk);
	};
}