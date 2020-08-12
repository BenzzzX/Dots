#pragma once
#include <cstdint>
#include <climits>
#include <memory>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <bitset>
#include "Set.h"

namespace core
{

	namespace database
	{
		class tagged_index
		{
			index_t id;

			static constexpr size_t offset = sizeof(id) * CHAR_BIT - 2;
			static constexpr index_t mask = (index_t(1) << offset) - 1;
		public:
			constexpr index_t index() const noexcept { return id & mask; }
			constexpr bool is_buffer() const noexcept { return (id >> (offset)) & 1; }
			constexpr bool is_tag() const noexcept { return (id >> (offset + 1)) & 1; }

			constexpr tagged_index(index_t value = 0) noexcept :id(value) { }
			constexpr tagged_index(index_t a, bool b, bool c) noexcept
				: id(a | ((index_t)b << offset) |
					((index_t)c << (offset + 1))) { }

			constexpr operator index_t() const { return id; }
		};

		template<class... Ts>
		struct soa
		{
			static constexpr int sizes[sizeof...(Ts)] = { sizeof(Ts)... };
			uint16_t dims[sizeof...(Ts)];
			uint32_t get_offset(int dim)
			{
				uint32_t size = 0;
				for (int i = 0; i < dim; ++i)
					size += dims[i] * sizes[i];
				return size;
			}
		};

		extern uint32_t metaTimestamp;

		struct i_serializer
		{
			virtual void stream(const void* data, uint32_t bytes) = 0;
			virtual bool is_serialize() = 0;
		};

		struct i_patcher
		{
			virtual entity patch(entity e) = 0;
			virtual void move() {}
			virtual void reset() {}
		};

		struct group
		{
			entity e;
		};

		using mask = std::bitset<32>;

		struct component_vtable
		{
			void(*patch)(char* data, i_patcher* stream) = nullptr;
		};

		enum track_state : uint8_t
		{
			Valid = 0,
			Copying = 1,
			NeedCopying = 2,
			NeedCleaning = 4,
			NeedCC = 6
		};

		struct component_desc
		{
			bool isElement = false;
			bool need_copy = false;
			bool need_clean = false;
			size_t hash = 0; 
			uint16_t size = 0; 
			uint16_t elementSize = 0; 
			intptr_t* entityRefs = nullptr; 
			uint16_t entityRefCount = 0;
			component_vtable vtable;
			const char* name = nullptr;
		};

		index_t register_type(component_desc desc);

		extern index_t disable_id;
		extern index_t cleanup_id;
		extern index_t group_id;
		extern index_t mask_id;

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

			static entity_type merge(const entity_type& lhs, const entity_type& rhs, index_t* dst, entity* metaDst)
			{
				typeset ts = typeset::merge(lhs.types, rhs.types, dst);
				metaset ms = metaset::merge(lhs.metatypes, rhs.metatypes, metaDst);
				return { ts, ms };
			}

			static entity_type substract(const entity_type& lhs, const entity_type& rhs, index_t* dst, entity* metaDst)
			{
				typeset ts = typeset::substract(lhs.types, rhs.types, dst);
				metaset ms = metaset::substract(lhs.metatypes, rhs.metatypes, metaDst);
				return { ts, ms };
			}
		};

		static const entity_type EmptyType;

		struct archetype_filter
		{
			entity_type all;
			entity_type any;
			entity_type none;
			typeset shared;
			typeset owned;


			struct hash
			{
				size_t operator()(const archetype_filter& key) const
				{
					size_t hash = hash_array(key.all.types.data, key.all.types.length);
					hash = hash_array(key.all.metatypes.data, key.all.metatypes.length, hash);
					hash = hash_array(key.any.types.data, key.any.types.length, hash);
					hash = hash_array(key.any.metatypes.data, key.any.metatypes.length, hash);
					hash = hash_array(key.none.types.data, key.none.types.length, hash);
					hash = hash_array(key.none.metatypes.data, key.none.metatypes.length, hash);
					hash = hash_array(key.shared.data, key.shared.length, hash);
					hash = hash_array(key.owned.data, key.owned.length, hash);
					return hash;
				}
			};

			bool operator==(const archetype_filter& other) const
			{
				return all == other.all && any == other.any &&
					none == other.any && all.metatypes == other.all.metatypes &&
					any.metatypes == other.any.metatypes && none.metatypes == other.none.metatypes&&
					shared == other.shared && owned == other.owned;
			}

			bool match(const entity_type& t, const typeset& sharedT) const;
		};

		struct chunk_filter
		{
			typeset changed;
			size_t prevTimestamp;

			struct hash
			{
				size_t operator()(const chunk_filter& key) const
				{
					size_t hash = hash_array(key.changed.data, key.changed.length);
					hash = hash_array(&key.prevTimestamp, 1, hash);
					return hash;
				}
			};

			bool operator==(const chunk_filter& other) const
			{
				return changed == other.changed && prevTimestamp == other.prevTimestamp;
			}

			bool match(const entity_type& t, uint32_t* timestamps) const
			{
				uint16_t i = 0, j = 0;
				while (i < changed.length && j < t.types.length)
				{
					if (changed[i] > t.types[j])
						j++;
					else if (changed[i] < t.types[j])
						return false;
					else if (timestamps[j] >= prevTimestamp)
						(j++, i++);
					else
						return false;
				}
				return i == changed.length;
			}
		};


		//todo: should mask support none filter?
		struct entity_filter
		{
			typeset disabeld;

			struct hash
			{
				size_t operator()(const entity_filter& key) const
				{
					size_t hash = hash_array(key.disabeld.data, key.disabeld.length);
					return hash;
				}
			};

			bool operator==(const entity_filter& other) const
			{
				return disabeld == other.disabeld;
			}
		};
	}
	
}
#undef stack_array