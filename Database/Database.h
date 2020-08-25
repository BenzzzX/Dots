#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <array>
#include <optional>
#include <functional>
#include "Set.h"
#include "Type.h"
#include <experimental/generator>
namespace core
{
	namespace database
	{
		struct chunk;
		class world;

		struct gallocator : public std::allocator<char>
		{
			using al = std::allocator<char>;
			using al::size_type;
			using al::value_type;
			using al::is_always_equal;
			using al::difference_type;
			using al::propagate_on_container_move_assignment;

			template <class U>
			struct rebind {
				typedef gallocator other;
			};

			[[nodiscard]] __declspec(allocator) char* allocate(_CRT_GUARDOVERFLOW const size_t count);
			void deallocate(char* const ptr, const size_t count);
		};

		template<class T>
		using generator = std::experimental::generator<T, gallocator>;

		//system overhead
		static constexpr size_t kFastBinSize = 64 * 1024 - 256;
		static constexpr size_t kSmallBinThreshold = 8;
		static constexpr size_t kSmallBinSize = 1024 - 256;
		static constexpr size_t kLargeBinSize = 1024 * 1024 - 256;

		static constexpr size_t kFastBinCapacity = 800;
		static constexpr size_t kSmallBinCapacity = 200;
		static constexpr size_t kLargeBinCapacity = 80;

		enum class alloc_type : uint8_t
		{
			smallbin, fastbin, largebin
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

		struct chunk_vector_base
		{
			size_t chunkSize = 0;
			size_t size = 0;
			void** data = nullptr;

			void grow();
			void shrink(size_t n);

			chunk_vector_base() = default;
			chunk_vector_base(chunk_vector_base&& r);
			chunk_vector_base(const chunk_vector_base& r);
			~chunk_vector_base();
		};

		template<class T>
		struct chunk_vector : chunk_vector_base
		{
			using chunk_vector_base::chunk_vector_base;
			static T* get(void** data, size_t i)
			{
				return &((T**)data)[i / kChunkCapacity][i % kChunkCapacity];
			}
			static constexpr size_t kChunkCapacity = kFastBinSize / sizeof(T);

			struct const_iterator
			{
				size_t i;
				void** c;
				
				const_iterator& operator++()
				{
					++i;
					return *this;
				}
				void operator++(int) { ++*this; }
				bool operator==(const const_iterator& right) const { return i == right.i && c == right.c; }
				bool operator!=(const const_iterator& right) const { return !(*this == right); };

				const T& operator*()
				{
					return *get(c, i);
				}
				const T* operator->()
				{
					return &**this;
				}
			};
			struct iterator : const_iterator
			{
				using const_iterator::operator++;
				using const_iterator::operator!=;
				T& operator*()
				{
					return const_cast<T&>(const_iterator::operator*());
				}
				T* operator->()
				{
					return &**this;
				}
			};

			iterator begin() { return iterator{ 0, data }; }
			iterator end() { return iterator{ size, data }; }

			const_iterator begin() const { return const_iterator{ 0, data }; }
			const_iterator end() const { return const_iterator{ size, data }; }
			template<class... Ts>
			void push(Ts&&... args)
			{
				if (size >= chunkSize * kChunkCapacity)
					grow();
				new(get(data, size)) T{ std::forward<Ts>(args)... };
				++size;
			}
			void pop() { --size; shrink(); }
			T& last() { return *get(data, size - 1); }
			const T& last() const { return *get(data, size - 1); }
			void shrink()
			{
				chunk_vector_base::shrink(chunkSize - (size + kChunkCapacity - 1) / kChunkCapacity);
			}
			void reserve(size_t n)
			{
				while (n < chunkSize * kChunkCapacity)
					grow();
			}
			T& operator[](size_t i) { return *get(data, i); }
			const T& operator[](size_t i) const { return *get(data, i); }
		};

		struct archetype
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
					uint32_t i;
					uint32_t v;
				};
				chunk_vector<data> datas;
				uint32_t free = 0;
				void clear();
				void new_entities(chunk_slice slice);
				entity new_prefab();
				entity new_entity();
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
			static void serialize_archetype(archetype* g, i_serializer* s);
			archetype* deserialize_archetype(i_serializer* s, i_patcher* patcher);
			std::optional<chunk_slice> deserialize_slice(archetype* g, i_serializer* s);

			//group behavior
			void group_to_prefab(entity* src, uint32_t size, bool keepExternal = true);
			void prefab_to_group(entity* src, uint32_t count);
			chunk_vector<chunk_slice> instantiate_prefab(entity* src, uint32_t size, uint32_t count);
			chunk_vector<chunk_slice> instantiate_single(entity src, uint32_t count);
			void serialize_single(i_serializer* s, entity);
			chunk_slice deserialize_single(i_serializer* s, i_patcher* patcher);
			void destroy_single(chunk_slice);

			//ownership utils
			void estimate_shared_size(tsize_t& size, archetype* t) const;
			void get_shared_type(typeset& type, archetype* t, typeset& buffer) const;
			void release_reference(archetype* g);

			friend chunk;
		public:
			world(index_t typeCapacity = 4096u);
			world(const world& other/*todo: ,archetype_filter*/);
			~world();
			//create
			chunk_vector<chunk_slice> allocate(const entity_type& type, uint32_t count = 1);
			chunk_vector<chunk_slice> instantiate(entity src, uint32_t count = 1);

			//batched stuctural change
			void destroy(chunk_slice);
			/* note: return null if trigger chunk move or chunk clean up */
			chunk_vector<chunk_slice> cast(chunk_slice, type_diff);
			chunk_vector<chunk_slice> cast(chunk_slice, const entity_type& type);

			//query iterators
			chunk_vector<chunk_slice> batch(entity* ents, uint32_t count);
			chunk_vector<matched_archetype> query(const archetype_filter& filter);
			chunk_vector<chunk*> query(archetype*, const chunk_filter& filter);

			//update
			void* get_owned_rw(entity, index_t type) const noexcept;
			void enable_component(entity, const typeset& type) const noexcept;
			void disable_component(entity, const typeset& type) const noexcept;

			//per entity query
			const void* get_component_ro(entity, index_t type) const noexcept;
			const void* get_owned_ro(entity, index_t type) const noexcept;
			const void* get_shared_ro(entity, index_t type) const noexcept;
			bool is_a(entity, const entity_type& type) const noexcept;
			bool share_component(entity, const typeset& type) const;
			bool has_component(entity, const typeset& type) const noexcept;
			bool own_component(entity, const typeset& type) const noexcept;
			bool is_component_enabled(entity, const typeset& type) const noexcept;
			bool exist(entity) const noexcept;
			archetype* get_archetype(entity) const noexcept;
			/* note: only owned */
			entity_type get_type(entity) const noexcept; 
			//per chunk or archetype
			const void* get_component_ro(chunk* c, index_t type) const noexcept;
			const void* get_owned_ro(chunk* c, index_t type) const noexcept;
			const void* get_shared_ro(chunk* c, index_t type) const noexcept;
			void* get_owned_rw(chunk* c, index_t type) noexcept;
			const void* get_owned_ro_local(chunk* c, index_t type) const noexcept;
			void* get_owned_rw_local(chunk* c, index_t type) noexcept;
			const void* get_shared_ro(archetype *g, index_t type) const;
			bool share_component(archetype* g, const typeset& type) const;
			bool own_component(archetype* g, const typeset& type) const;
			bool has_component(archetype* g, const typeset& type) const;
			const entity* get_entities(chunk* c) noexcept;
			uint16_t get_size(chunk* c, index_t type) const noexcept;

			//entity/group serialize
			void gather_reference(entity, std::pmr::vector<entity>& entities);
			void serialize(i_serializer* s, entity);
			entity deserialize(i_serializer* s, i_patcher* patcher);

			//multi world
			void move_context(world& src);
			void patch_chunk(chunk* c, i_patcher* patcher);

			//world serialize
			void serialize(i_serializer* s);
			void deserialize(i_serializer* s);

			//clear
			void clear();
			void gc_meta();
			void merge_chunks();

			//timestamp
			uint32_t timestamp;
		};

		struct chunk
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
			static void patch(chunk_slice s, i_patcher* patcher) noexcept;
			static void serialize(chunk_slice s, i_serializer *stream);
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

};
}