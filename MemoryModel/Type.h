#pragma once
#include <cstdint>
#include <climits>
#include <memory>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include "Set.h"
namespace ecs
{
	struct entity
	{
		uint32_t id;
		uint32_t version;

		bool operator==(const entity& e) const
		{
			return id == e.id && version == e.version;
		}
	};

	namespace memory_model
	{
		class tagged_index
		{
			index_t id;

			static constexpr size_t offset = sizeof(id) * CHAR_BIT - 5;
			static constexpr index_t mask = (index_t(2) << offset) - 1;
		public:
			constexpr index_t index() const noexcept { return id & mask; }
			constexpr bool is_internal() const noexcept { return (id >> offset) & 1; }
			constexpr bool is_managed() const noexcept { return (id >> (offset + 1)) & 1; }
			constexpr bool is_buffer() const noexcept { return (id >> (offset + 2)) & 1; }
			constexpr bool is_tag() const noexcept { return (id >> (offset + 3)) & 1; }
			constexpr bool is_meta() const noexcept { return (id >> (offset + 4)) & 1; }

			constexpr tagged_index(index_t value = 0) noexcept :id(value) { }
			constexpr tagged_index(index_t a, bool b, bool c, bool d, bool e, bool f) noexcept
				: id(a | ((index_t)b << offset) |
					((index_t)c << (offset + 1)) |
					((index_t)d << (offset + 2)) |
					((index_t)e << (offset + 3)) |
					((index_t)e << (offset + 4))) { }

			constexpr operator index_t() const { return id; }
		};

		struct metainfo
		{
			uint32_t refCount = 0;
		};

		struct metakey
		{
			index_t type;
			index_t metatype;
			bool operator==(const metakey& other) const
			{
				return type == other.type && metatype == other.metatype;
			}

			struct hash
			{
				size_t operator()(const metakey& key) const
				{
					return (size_t(key.type) << 16) & key.metatype;
				}
			};
		};

		struct serializer_i
		{
			virtual void write(const void* data, uint32_t bytes) = 0;
			virtual void writemeta(metakey metatype) = 0;
		};

		struct deserializer_i
		{
			virtual void read(void* data, uint32_t bytes) = 0;
			virtual index_t readmeta(index_t type) = 0;
		};

		struct managed_rtti
		{
			void(*copy_constructor)(char*) = nullptr;
			void(*destructor)(char*) = nullptr;
			void(*serialize)(serializer_i* stream) = nullptr;
			void(*deserialize)(deserializer_i* stream) = nullptr;
		};

		struct component_desc
		{
			bool isInsternal = false;
			bool isElement = false;
			bool isMeta = false; 
			bool isManaged = false;
			size_t hash = 0; 
			uint16_t size = 0; 
			uint16_t elementSize = 0; 
			intptr_t* entityRefs = nullptr; 
			uint16_t entityRefCount = 0;
			managed_rtti rtti;
		};

		using metatype_release_callback_t = void(*)(metakey);
		index_t register_type(component_desc desc);

		extern index_t disable_id;
		extern index_t cleanup_id;
		void register_metatype_release_callback(metatype_release_callback_t callback);

		struct buffer
		{
			char* d;
			uint16_t size;
			uint16_t capacity;

			buffer(uint16_t cap)
			{
				d = nullptr;
				capacity = cap;
				size = 0;
			}

			char* data()
			{
				if (d == nullptr) return (char*)(this + 1);
				else return d;
			}

			char* data() const
			{
				if (d == nullptr) return (char*)(this + 1);
				else return d;
			}

			void push(const void* dd, uint16_t length)
			{
				if (size + length >= capacity)
					grow();
				memcpy(data() + size, dd, length);
				size += length;
			}

			void* pop(uint16_t length)
			{
				return data() + (size -= length);
			}

			void grow()
			{
				uint16_t newCap = capacity * 2;
				char* newBuffer = new char[newCap];
				memcpy(newBuffer, data(), size);
				if (d != nullptr)
					free(d);
				d = newBuffer;
				capacity = newCap;
			}

			~buffer()
			{
				if (d != nullptr)
					free(d);
			}
		};

		struct entity_type
		{
			typeset types;
			metaset metatypes;

			bool operator==(const entity_type& other) const
			{
				return types == other.types && metatypes == other.metatypes;
			}

			struct hash
			{
				size_t operator()(const entity_type& key) const
				{
					size_t hash = hash_array(key.types.data, key.types.length);
					hash = hash_array(key.metatypes.data, key.metatypes.length, hash);
					return hash;
				}
			};

			bool valid_for_filter() const
			{
				typeset mt = { metatypes.metaData,metatypes.length };
				return types.all(mt);
			}

			bool valid_for_entity() const
			{
				if (types.length == 0)
					return false;
				return memcmp(metatypes.metaData, types.data + types.length - metatypes.length, metatypes.length) == 0;
			}

			static const index_t* get_meta(typeset ts)
			{
				return std::find_if(ts.data, ts.data + ts.length, [](index_t v) { return ((tagged_index)v).is_meta(); });
			}

			static entity_type merge(const entity_type& lhs, const entity_type& rhs, index_t* dst, index_t* metaDst)
			{
				typeset ts = typeset::merge(lhs.types, rhs.types, dst);
				const index_t* meta = get_meta(ts);
				if (meta != ts.data + ts.length)
				{
					metaset ms = metaset::merge(lhs.metatypes, rhs.metatypes, meta, metaDst);
					return { ts, ms };
				}
				else
					return { ts, {{nullptr, 0}, nullptr} };
			}

			static entity_type substract(const entity_type& lhs, const typeset& rhs, index_t* dst, index_t* metaDst)
			{
				typeset ts = typeset::substract(lhs.types, rhs, dst);
				const index_t* meta = get_meta(ts);
				if (meta != ts.data + ts.length)
				{
					metaset ms = metaset::substract(lhs.metatypes, rhs, meta, metaDst);
					return { ts, ms };
				}
				else
					return { ts, {{nullptr, 0}, nullptr} };
			}

		};


		struct entity_filter
		{
			entity_type all;
			entity_type any;
			entity_type none;

			typeset changed;
			size_t prevVersion;
			bool includeDisabled;

			struct hash
			{
				size_t operator()(const entity_filter& key) const
				{
					size_t hash = hash_array(key.all.types.data, key.all.types.length);
					hash = hash_array(key.any.types.data, key.any.types.length, hash);
					hash = hash_array(key.none.types.data, key.none.types.length, hash);
					hash = hash_array(key.all.metatypes.data, key.all.metatypes.length, hash);
					hash = hash_array(key.any.metatypes.data, key.any.metatypes.length, hash);
					hash = hash_array(key.none.metatypes.data, key.none.metatypes.length, hash);
					hash = hash_array(key.changed.data, key.changed.length, hash);
					hash = hash_array(&key.prevVersion, 1, hash);
					hash ^= key.includeDisabled;
					return hash;
				}
			};

			bool operator==(const entity_filter& other) const
			{
				return all == other.all && any == other.any && 
					none == other.any && all.metatypes == other.all.metatypes &&
					any.metatypes == other.any.metatypes && none.metatypes == other.none.metatypes &&
					changed == other.changed && prevVersion == prevVersion &&
					includeDisabled == other.includeDisabled;
			}

			bool match_chunk(const entity_type& t, uint32_t* versions) const
			{
				uint16_t i = 0, j = 0;
				while (i < changed.length && j < t.types.length)
				{
					if (changed[i] > t.types[j])
						j++;
					else if (changed[i] < t.types[j])
						i++;
					else if (versions[j] >= prevVersion)
						(j++, i++);
				}
				return i == changed.length;
			}

			bool match(const entity_type& t, bool disabled) const
			{
				if (disabled > includeDisabled)
					return false;

				if (!t.types.all(all.types))
					return false;
				if (any.types.length > 0 && !t.types.any(any.types))
					return false;
				if (t.types.any(none.types))
					return false;

				if (!t.metatypes.all(all.metatypes))
					return false;
				if (any.metatypes.length > 0 && !t.metatypes.any(any.metatypes))
					return false;
				if (t.metatypes.any(none.metatypes))
					return false;

				return true;
			}
		};
	}
	
}