#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <array>
#include <optional>
#include <functional>
#include "Set.h"
#include "Type.h"
#undef small
namespace core
{
	namespace database
	{
		class context;
		struct chunk;

		enum class chunk_type : uint8_t
		{
			fast,small,large
		};

		struct chunk_slice
		{
			chunk* c;
			uint32_t start;
			uint32_t count;
			bool full();
			chunk_slice(chunk* c);
			chunk_slice(chunk* c, uint32_t s, uint32_t count)
				: c(c), start(s), count(count) {}
		};

		struct archetype
		{
			chunk* firstChunk;
			chunk* lastChunk;
			chunk* firstFree;
			uint16_t componentCount;
			uint16_t firstTag;
			uint16_t metaCount;
			uint16_t firstBuffer;
			uint16_t chunkCount;
			//todo: use 32 bit capacity
			uint32_t chunkCapacity[3];
			uint32_t timestamp;
			uint32_t size;
			bool disabled;
			bool cleaning;
			bool copying;
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
			inline uint32_t* offsets(chunk_type type) noexcept { return  (uint32_t*)(data() + componentCount * sizeof(index_t) + firstTag * sizeof(uint32_t) * (int)type); }
			inline uint16_t* sizes() noexcept { return (uint16_t*)(data() + componentCount * sizeof(index_t) + firstTag * 3 * sizeof(uint32_t)); }
			inline entity* metatypes() noexcept { return (entity*)(data() + componentCount * sizeof(index_t) + firstTag * 3 * sizeof(uint32_t) + firstTag * sizeof(uint16_t)); }
			inline uint32_t* timestamps(chunk* c) noexcept;
			inline uint16_t index(index_t type) noexcept;
			inline mask get_mask(const typeset& subtype) noexcept;

			inline entity_type get_type();

			static size_t calculate_size(uint16_t componentCount, uint16_t firstTag, uint16_t firstMeta);
		};

		class context
		{

			struct entities
			{
				struct data
				{
					union
					{
						chunk* c;
						uint32_t nextFree;
					};
					uint16_t i;
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
				struct mached_archetype
				{
					archetype* type;
					mask matched;
				};
				std::unique_ptr<char[]> data;
				bool includeDisabled;
				bool includeClean;
				archetype_filter filter;
				std::vector<mached_archetype> archetypes;
				using iterator = std::vector<mached_archetype>::iterator;
			};

			struct archetype_iterator
			{
				query_cache::iterator curr;
				query_cache::iterator iter;
				query_cache::iterator end;
				std::optional<archetype*> next();
				mask get_mask() const;
			};

			struct chunk_iterator
			{
				chunk_filter filter;
				archetype* type;
				chunk* iter;

				std::optional<chunk*> next();
			};

			struct entity_iterator
			{
				entity_filter filter;
				uint16_t size;
				mask* masks;
				uint16_t index;

				std::optional<uint16_t> next();
			};

			using queries_t = std::unordered_map<archetype_filter, query_cache, archetype_filter::hash>;

			query_cache& get_query_cache(const archetype_filter& f);
			void update_queries(archetype* g, bool add);

			static constexpr size_t kFastBinCapacity = 800;
			static constexpr size_t kSmallBinCapacity = 200;
			static constexpr size_t kLargeBinCapacity = 80;

			std::array<chunk*, kFastBinCapacity> fastbin;
			std::array<chunk*, kSmallBinCapacity> smallbin;
			std::array<chunk*, kLargeBinCapacity> largebin;
			uint16_t fastbinSize = 0;
			uint16_t smallbinSize = 0;
			uint16_t largebinSize = 0;
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

			chunk* malloc_chunk(chunk_type type);
			chunk* new_chunk(archetype*, uint32_t hint);
			void destroy_chunk(archetype*, chunk*);
			void recycle_chunk(chunk*);
			void resize_chunk(chunk*, uint32_t);

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
			void structural_change(archetype* g, chunk* c);
			archetype_filter cache_query(const archetype_filter& type);
			void estimate_shared_size(uint16_t& size, archetype* t) const;
			void get_shared_type(typeset& type, archetype* t, typeset& buffer) const;

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
			void extend(archetype*, const entity_type& type);
			void shrink(archetype*, const entity_type& type);


			//stuctural change
			void destroy(entity* es, int32_t count);
			void extend(entity* es, int32_t count, const entity_type& type);
			void shrink(entity* es, int32_t count, const entity_type& type);
			void cast(entity* es, int32_t count, const entity_type& type);
			//update
			void* get_owned_rw(entity, index_t type) const noexcept;
			void enable_component(entity, const typeset& type) const noexcept;
			void disable_component(entity, const typeset& type) const noexcept;

			//query
			batch_iterator batch(entity* ents, uint32_t count);
			archetype_iterator query(const archetype_filter& filter);
			chunk_iterator query(archetype*, const chunk_filter& filter);
			entity_iterator query(chunk*, const entity_filter& filter);

			const void* get_component_ro(entity, index_t type) const noexcept;
			const void* get_owned_ro(entity, index_t type) const noexcept;
			const void* get_shared_ro(entity, index_t type) const noexcept;
			bool is(entity, const entity_type& type) const noexcept;
			bool share_component(entity, const typeset& type) const;
			bool has_component(entity, const typeset& type) const noexcept;
			bool own_component(entity, const typeset& type) const noexcept;
			bool is_component_enabled(entity, const typeset& type) const noexcept;
			bool exist(entity) const noexcept;
			const void* get_component_ro(chunk* c, index_t type) const noexcept;
			const void* get_owned_ro(chunk* c, index_t type) const noexcept;
			const void* get_shared_ro(chunk* c, index_t type) const noexcept;
			void* get_owned_rw(chunk* c, index_t type) noexcept;
			const void* get_shared_ro(archetype *g, index_t type) const;
			bool share_component(archetype* g, const typeset& type) const;
			bool own_component(archetype* g, const typeset& type) const;
			bool has_component(archetype* g, const typeset& type) const;
			archetype* get_archetype(entity) const noexcept;
			const entity* get_entities(chunk* c) noexcept;
			uint16_t get_size(chunk* c, index_t type) const noexcept;
			//note: only owned
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
			archetype* type;
			uint32_t count;
			chunk_type ct;
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
			size_t get_size();
			void link(chunk*) noexcept;
			void unlink() noexcept;
			char* data() { return (char*)(this + 1); }
			const char* data() const { return (char*)(this + 1); }
			friend context; 
			friend archetype;
			friend context::entities;
		public:
			uint16_t get_count() { return count; }
			const entity* get_entities() const { return (entity*)data(); }
			uint32_t get_timestamp(index_t type) noexcept;
		};

		//system overhead
		static constexpr size_t kFastBinSize = 64 * 1024 - 256;
		static constexpr size_t kSmallBinThreshold = 3;
		static constexpr size_t kSmallBinSize = 1024 - 256;
		static constexpr size_t kLargeBinSize = 1024 * 1024 - 256;
	};
}