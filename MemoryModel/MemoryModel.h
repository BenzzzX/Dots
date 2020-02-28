#pragma once
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <array>
#include <optional>
#include "Set.h"
#include "Type.h"
namespace ecs
{
	namespace memory_model
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

		//liner sequence of entity is prefab
		struct prefab
		{
			const context* src;
			//TODO: do we need version?
			entity start;
			uint32_t count;
		};

		class context
		{
			struct group
			{
				chunk* firstChunk;
				chunk* lastChunk;
				chunk* firstFree;
				uint16_t componentCount;
				uint16_t firstTag;
				uint16_t firstMeta;
				uint16_t firstManaged;
				uint16_t firstBuffer;
				uint16_t chunkCapacity;
				bool disabled;
				bool cleaning;
				bool needsClean;
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
				inline index_t* metatypes() noexcept { return (index_t*)(data() + componentCount * sizeof(index_t) + (firstTag + firstTag) * sizeof(uint16_t)); }
				inline uint32_t* versions(chunk* c) noexcept;
				inline uint16_t index(index_t type) noexcept;

				inline entity_type get_type();

				static size_t calculate_size(uint16_t componentCount, uint16_t firstTag, uint16_t firstMeta);
			};

			using groups_t = std::unordered_map<entity_type, group*, entity_type::hash>;

			struct batch_iterator
			{
				entity* ents;
				uint32_t count;
				context* cont;
				uint32_t i;

				std::optional<chunk_slice> next();
			};

			struct chunk_iterator
			{
				context* cont;
				chunk* currc;
				groups_t::iterator currg;
				entity_filter filter;

				std::optional<chunk*> next();
			};
			
			struct query_iterator
			{
				chunk* currc;
				std::vector<group*>::iterator currg;
				std::optional<chunk*> next();
			};

			struct query
			{
				std::unique_ptr<uint16_t> data;
				entity_filter filter;
				std::vector<group*> groups;
				query_iterator iter();
			};

			using queries_t = std::unordered_map<entity_filter, query, entity_filter::hash>;

			query* get_query(entity_filter& f);
			void update_queries(group* g, bool add);
			
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

			static constexpr size_t kChunkPoolCapacity = 8000;

			std::array<chunk*, kChunkPoolCapacity> chunkPool;
			uint16_t poolSize = 0;
			groups_t groups;
			queries_t queries;
			entities ents;

			static void remove(chunk*& head, chunk*& tail, chunk* toremove);

			group* get_group(const entity_type&);
			group* get_cleaning(group*);
			bool is_cleaned(const entity_type&);
			group* get_instatiation(group*);

			static void add_chunk(group* g, chunk* c);
			void remove_chunk(group* g, chunk* c);
			static void mark_free(group* g, chunk* c);
			static void unmark_free(group* g, chunk* c);

			chunk* new_chunk(group*);
			void destroy_chunk(group*, chunk*);
			void resize_chunk(chunk*, uint16_t);

			chunk_slice allocate_slice(group*, uint32_t = 1);
			void free_slice(chunk_slice);
			void cast_slice(chunk_slice, group*);

			static void release_reference(group* g);

			static void serialize_type(group* g, serializer_i* s);
			group* deserialize_group(deserializer_i* s, uint16_t tlength);
			chunk_slice deserialize_slice(group* g, deserializer_i* stream, uint16_t count);

			friend chunk;
			friend batch_iterator;
			friend chunk_iterator;
		public:
			~context();
			//create
			void allocate(const entity_type& type, entity* ret, uint32_t count = 1);
			void instantiate(entity src, entity* ret, uint32_t count = 1);

			//modify(batched)
			void destroy(chunk_slice);
			void extend(chunk_slice, const entity_type& type);
			void shrink(chunk_slice, const typeset& type);
			void cast(chunk_slice, const entity_type& type);
			void* get_component_rw(entity, index_t type);

			//query
			batch_iterator batch(entity* ents, uint32_t count);
			chunk_iterator query(const entity_filter& type);
			const void* get_component_ro(entity, index_t type);
			index_t get_metatype(entity, index_t type);
			bool has_component(entity, index_t type) const;
			bool exist(entity) const;
			index_t get_metatype(chunk* c, index_t type);
			const void* get_array_ro(chunk* c, index_t type) const noexcept;
			void* get_array_rw(chunk* c, index_t type) noexcept;
			const entity* get_entities(chunk* c) noexcept;
			uint16_t get_element_size(chunk* c, index_t type) const noexcept;
			entity_type get_type(entity) const noexcept;

			//multi context
			entity allocate_prefab(const entity_type& type);
			prefab instantiate_as_prefab(entity* src, uint32_t count);
			void instantiate_prefab(prefab p, entity* ret, uint32_t count);

			void move_context(context& src, entity* patch, uint32_t count);
			void move_chunk(context& src, chunk* c, entity* patch, uint32_t count);
			void patch_chunk(chunk* c, const entity* patch, uint32_t count);

			static void serialize(context& cont, serializer_i* s);
			static void deserialize(context& cont, deserializer_i* s);
			static void serialize_prefab(prefab p, serializer_i *s);
			static prefab deserialize_prefab(context& cont, deserializer_i* s);

			uint32_t version;
		};

		struct chunk
		{
		private:
			chunk *next, *prev;
			context::group* type;
			uint16_t count;
			/*
			entity entities[chunkCapacity];
			T1 component1[chunkCapacity];
			T2 component2[chunkCapacity];
				.
				.
				.
			uint32_t versions[firstTag];
			*/

			static void construct(chunk_slice) noexcept;
			static void destruct(chunk_slice) noexcept;
			static void move(chunk_slice dst, uint16_t srcIndex) noexcept;
			static void cast(chunk_slice dst, chunk* src, uint16_t srcIndex) noexcept;
			static void duplicate(chunk_slice dst, const chunk* src, uint16_t srcIndex) noexcept;
			static void patch(chunk_slice s, uint32_t start, const entity* target, uint32_t count) noexcept;
			static void depatch(chunk_slice dst, const entity *src, const entity *target, uint32_t count) noexcept;
			static void serialize(chunk_slice s, serializer_i *stream);
			static void deserialize(chunk_slice s, deserializer_i* stream);
			void link(chunk*) noexcept;
			void unlink() noexcept;
			char* data() { return (char*)(this + 1); }
			const char* data() const { return (char*)(this + 1); }
			friend class context;
			friend context::entities;
		public:
			uint16_t get_count() { return count; }
			const entity* get_entities() const { return (entity*)data(); }
			uint32_t get_version(index_t type) noexcept;
		};

		//system overhead
		static constexpr size_t kChunkSize = 16 * 1024 - 256;
		static constexpr size_t kChunkBufferSize = kChunkSize - sizeof(chunk);
	};
}