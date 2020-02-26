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
			uint16_t id;

			static constexpr size_t offset = sizeof(id) * CHAR_BIT - 4;
			static constexpr uint16_t mask = (uint16_t(2) << offset) - 1;
		public:
			constexpr uint16_t index() const noexcept { return id & mask; }
			constexpr bool is_internal() const noexcept { return (id >> offset) & 1; }
			constexpr bool is_buffer() const noexcept { return (id >> (offset + 1)) & 1; }
			constexpr bool is_tag() const noexcept { return (id >> (offset + 2)) & 1; }
			constexpr bool is_meta() const noexcept { return (id >> (offset + 3)) & 1; }

			constexpr tagged_index(uint16_t value = 0) noexcept :id(value) { }
			constexpr tagged_index(uint16_t a, bool b, bool c, bool d, bool e) noexcept
				: id(a | ((uint16_t)b << offset) |
				((uint16_t)c << (offset + 1)) |
					((uint16_t)d << (offset + 2)) |
					((uint16_t)e << (offset + 3))) { }

			constexpr operator uint16_t() const { return id; }
		};

		struct metainfo
		{
			uint32_t refCount = 0;
		};

		struct metakey
		{
			uint16_t type;
			uint16_t metatype;
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

		struct component_desc
		{
			bool isInsternal = false;
			bool isElement = false;
			bool isMeta = false; 
			size_t hash = 0; 
			uint16_t size = 0; 
			uint16_t elementSize = 0; 
			intptr_t* entityRefs = nullptr; 
			uint16_t entityRefCount = 0;
		};

		using metatype_release_callback_t = void(*)(metakey);
		uint16_t register_type(component_desc desc);
		uint16_t register_disable();
		uint16_t register_cleanup();
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
				size_t newCap = capacity * 2;
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

			static const uint16_t* get_meta(typeset ts)
			{
				return std::find_if(ts.data, ts.data + ts.length, [](uint16_t v) { return ((tagged_index)v).is_meta(); });
			}

			static entity_type merge(const entity_type& lhs, const entity_type& rhs, uint16_t* dst, uint16_t* metaDst)
			{
				typeset ts = typeset::merge(lhs.types, rhs.types, dst);
				const uint16_t* meta = get_meta(ts);
				if (meta != ts.data + ts.length)
				{
					metaset ms = metaset::merge(lhs.metatypes, rhs.metatypes, meta, metaDst);
					return { ts, ms };
				}
				else
					return { ts, {{nullptr, 0}, nullptr} };
			}

			static entity_type substract(const entity_type& lhs, const typeset& rhs, uint16_t* dst, uint16_t* metaDst)
			{
				typeset ts = typeset::substract(lhs.types, rhs, dst);
				const uint16_t* meta = get_meta(ts);
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
			typeset all;
			typeset any;
			typeset none;
			metaset allMeta;
			metaset anyMeta;
			metaset noneMeta;

			typeset changed;
			size_t prevVersion;
			bool includeDisabled;

			struct hash
			{
				size_t operator()(const entity_filter& key) const
				{
					size_t hash = hash_array(key.all.data, key.all.length);
					hash = hash_array(key.any.data, key.any.length, hash);
					hash = hash_array(key.none.data, key.none.length, hash);
					hash = hash_array(key.allMeta.data, key.allMeta.length, hash);
					hash = hash_array(key.anyMeta.data, key.anyMeta.length, hash);
					hash = hash_array(key.noneMeta.data, key.noneMeta.length, hash);
					hash = hash_array(key.noneMeta.data, key.noneMeta.length, hash);
					hash = hash_array(key.changed.data, key.changed.length, hash);
					hash = hash_array(&key.prevVersion, 1, hash);
					hash |= key.includeDisabled;
					return hash;
				}
			};

			bool operator==(const entity_filter& other) const
			{
				return all == other.all && any == other.any && 
					none == other.any && allMeta == other.allMeta &&
					anyMeta == other.anyMeta && noneMeta == other.noneMeta &&
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

				if (!t.types.all(all))
					return false;
				if (!t.types.any(any))
					return false;
				if (t.types.any(none))
					return false;

				if (!t.metatypes.all(allMeta))
					return false;
				if (!t.metatypes.any(anyMeta))
					return false;
				if (t.metatypes.any(noneMeta))
					return false;

				return true;
			}
		};
	}
	
}