#pragma once
#ifdef MAKEDLL
#  define ECS_API __declspec(dllexport)
#else
#  define ECS_API __declspec(dllimport)
#endif
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
			static_assert(sizeof(index_t) * 8 == 32, "index_t should be 32 bits");
			union
			{
				index_t value;
				struct
				{
					index_t id : 30;
					index_t buffer : 1;
					index_t tag : 1;
				};
			};

		public:
			constexpr index_t index() const noexcept { return id; }
			constexpr bool is_buffer() const noexcept { return buffer; }
			constexpr bool is_tag() const noexcept { return tag; }

			constexpr tagged_index(index_t value = 0) noexcept : value(value) { }
			constexpr tagged_index(index_t a, bool b, bool c) noexcept
				: id(a), buffer(b), tag(c) { }

			constexpr operator index_t() const { return value; }
		};

		static constexpr size_t kFastBinSize = 64 * 1024;
		static constexpr size_t kSmallBinThreshold = 8;
		static constexpr size_t kSmallBinSize = 1024;
		static constexpr size_t kLargeBinSize = 1024 * 1024;

		static constexpr size_t kFastBinCapacity = 800;
		static constexpr size_t kSmallBinCapacity = 200;
		static constexpr size_t kLargeBinCapacity = 80;

		struct ECS_API chunk_vector_base
		{
			size_t chunkSize = 0;
			size_t size = 0;
			void** data = nullptr;

			void grow();
			void shrink(size_t n);

			chunk_vector_base() = default;
			chunk_vector_base(chunk_vector_base&& r) noexcept;
			chunk_vector_base(const chunk_vector_base& r) noexcept;
			~chunk_vector_base();
		};

		namespace chunk_vector_detail
		{
			template<class V>
			struct const_iterator
			{
				using iterator_category = std::random_access_iterator_tag;

				using value_type = typename V::value_type;
				using difference_type = typename V::difference_type;
				using pointer = typename V::pointer;
				using reference = typename V::reference;


				size_t i;
				void** c;

				const_iterator& operator++() noexcept { ++i; return *this; }
				void operator++(int) noexcept { ++* this; }
				bool operator==(const const_iterator& right) const noexcept { return i == right.i && c == right.c; }
				bool operator!=(const const_iterator& right) const noexcept { return !(*this == right); };
				const_iterator& operator+=(difference_type d) { i += d; return *this; }
				const_iterator& operator-=(difference_type d) { i -= d; return *this; }
				friend const_iterator operator+(const const_iterator& lhs, difference_type rhs) { auto result = lhs; return result += rhs; }
				friend const_iterator operator+(difference_type lhs, const const_iterator& rhs) { auto result = rhs; return result += lhs; }
				friend const_iterator operator-(const const_iterator& lhs, difference_type rhs) { auto result = lhs; return result -= rhs; }
				friend const_iterator operator-(difference_type lhs, const const_iterator& rhs) { auto result = rhs; return result -= lhs; }
				friend difference_type operator-(const const_iterator& lhs, const const_iterator& rhs) { return lhs.i - rhs.i; }
				const value_type& operator[](difference_type d) { return *(*this + d); }
				bool operator<(const const_iterator& rhs) const { return i < rhs.i; }
				bool operator>(const const_iterator& rhs) const { return i > rhs.i; }
				bool operator<=(const const_iterator& rhs) const { return i <= rhs.i; }
				bool operator>=(const const_iterator& rhs) const { return i >= rhs.i; }


				const value_type& operator*()
				{
					return *V::get(c, i);
				}
				const value_type* operator->()
				{
					return &**this;
				}
			};

			template<class V>
			struct iterator : const_iterator<V>
			{
				iterator() {}
				iterator(size_t size, void** data)
					:const_iterator<V>{ size, data } {}
				using iterator_category = std::random_access_iterator_tag;

				using value_type = typename V::value_type;
				using difference_type = typename V::difference_type;
				using pointer = typename V::pointer;
				using reference = typename V::reference;

				iterator& operator++() noexcept { ++this->i; return *this; }
				void operator++(int) noexcept { ++* this; }
				bool operator==(const iterator& right) const noexcept { return this->i == right.i && this->c == right.c; }
				bool operator!=(const iterator& right) const noexcept { return !(*this == right); };
				iterator& operator+=(difference_type d) { this->i += d; return *this; }
				iterator& operator-=(difference_type d) { this->i -= d; return *this; }
				friend iterator operator+(const iterator& lhs, difference_type rhs) { auto result = lhs; return result += rhs; }
				friend iterator operator+(difference_type lhs, const iterator& rhs) { auto result = rhs; return result += lhs; }
				friend iterator operator-(const iterator& lhs, difference_type rhs) { auto result = lhs; return result -= rhs; }
				friend iterator operator-(difference_type lhs, const iterator& rhs) { auto result = rhs; return result -= lhs; }
				friend difference_type operator-(const iterator& lhs, const iterator& rhs) { return lhs.i - rhs.i; }
				bool operator<(const iterator& rhs) const { return this->i < rhs.i; }
				bool operator>(const iterator& rhs) const { return this->i > rhs.i; }
				bool operator<=(const iterator& rhs) const { return this->i <= rhs.i; }
				bool operator>=(const iterator& rhs) const { return this->i >= rhs.i; }
				value_type& operator[](difference_type d) { return *(*this + d); }
				value_type& operator*()
				{
					return *V::get(this->c, this->i);
				}
				value_type* operator->()
				{
					return &**this;
				}
			};
		}
		//for transient lightweight data storage
		template<class T>
		struct chunk_vector : chunk_vector_base
		{
			using value_type = T;
			using pointer = T*;
			using const_pointer = const T*;
			using reference = T&;
			using const_reference = const T&;
			using size_type = size_t;
			using difference_type = size_t;

			using chunk_vector_base::chunk_vector_base;
			static T* get(void** data, size_t i)
			{
				return &((T**)data)[i / kChunkCapacity][i % kChunkCapacity];
			}
			static constexpr size_t kChunkCapacity = kFastBinSize / sizeof(T);

			using const_iterator = chunk_vector_detail::const_iterator<chunk_vector>;
			using iterator = chunk_vector_detail::iterator<chunk_vector>;
			iterator begin() noexcept { return iterator{ 0, data }; }
			iterator end() noexcept { return iterator{ size, data }; }

			const_iterator begin() const noexcept { return const_iterator{ 0, data }; }
			const_iterator end() const noexcept { return const_iterator{ size, data }; }
			void resize(int newSize)
			{
				size = newSize;
				if (newSize > chunkSize * kChunkCapacity)
					reserve(newSize);
				else
					shrink();
			}
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
			void shrink() noexcept
			{
				chunk_vector_base::shrink(chunkSize - (size + kChunkCapacity - 1) / kChunkCapacity);
			}
			void reserve(size_t n)
			{
				while (n > chunkSize * kChunkCapacity)
					grow();
			}
			T& operator[](size_t i) noexcept { return *get(data, i); }
			const T& operator[](size_t i) const noexcept { return *get(data, i); }
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

		ECS_API extern uint32_t metaTimestamp;

		struct serializer_i
		{
			virtual void stream(const void* data, uint32_t bytes) = 0;
			virtual bool is_serialize() = 0;
		};

		struct patcher_i
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
			void(*patch)(char* data, patcher_i* stream) = nullptr;
		};

		enum track_state : uint8_t
		{
			Valid = 0,
			Copying = 1,
			ManualCopying = 2,
			ManualCleaning = 4,
			NeedCC = 6
		};

		struct component_desc
		{
			bool isElement = false;
			bool manualCopy = false;
			bool manualClean = false;
			size_t hash = 0; 
			uint16_t size = 0; 
			uint16_t elementSize = 0; 
			intptr_t* entityRefs = nullptr; 
			uint16_t entityRefCount = 0;
			component_vtable vtable;
			const char* name = nullptr;
		};

		ECS_API index_t register_type(component_desc desc);

		ECS_API extern index_t disable_id;
		ECS_API extern index_t cleanup_id;
		ECS_API extern index_t group_id;
		ECS_API extern index_t mask_id;
		constexpr uint32_t GroupBufferSize = sizeof(entity) * 5;

		inline void* buffer_malloc(size_t size)
		{
			return ::malloc(size);
		}

		inline void buffer_free(void* ptr)
		{
			return ::free(ptr);
		}

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

			void grow(uint16_t hint = 0)
			{
				uint16_t newCap = (uint16_t)std::max((int)hint, capacity * 2);
				char* newBuffer = (char*)buffer_malloc(newCap);
				memcpy(newBuffer, data(), size);
				if (d != nullptr)
					buffer_free(d);
				d = newBuffer;
				capacity = newCap;
			}

			void shrink(uint16_t inlineSize)
			{
				if (size < inlineSize)
				{
					buffer_free(d);
					d = nullptr;
					return;
				}
				uint16_t newCap = capacity;
				while (newCap > size * 2)
					newCap /= 2;
				if (newCap != capacity)
				{
					char* newBuffer = (char*)buffer_malloc(newCap);
					memcpy(newBuffer, data(), size);
					if (d != nullptr)
						buffer_free(d);
					d = newBuffer;
					capacity = newCap;
					return;
				}
			}

			~buffer()
			{
				if (d != nullptr)
					buffer_free(d);
			}
		};

		namespace buffer_detail
		{
			template<class V>
			struct const_iterator
			{
				using iterator_category = std::random_access_iterator_tag;

				using value_type = typename V::value_type;
				using difference_type = typename V::difference_type;
				using pointer = typename V::pointer;
				using reference = typename V::reference;

				value_type* ptr;

				const_iterator& operator++() noexcept { ++ptr; return *this; }
				void operator++(int) noexcept { ++* this; }
				bool operator==(const const_iterator& right) const noexcept { return ptr == right.ptr; }
				bool operator!=(const const_iterator& right) const noexcept { return !(*this == right); };
				const_iterator& operator+=(difference_type d) { ptr += d; return *this; }
				const_iterator& operator-=(difference_type d) { ptr -= d; return *this; }
				friend const_iterator operator+(const const_iterator& lhs, difference_type rhs) { auto result = lhs; return result += rhs; }
				friend const_iterator operator+(difference_type lhs, const const_iterator& rhs) { auto result = rhs; return result += lhs; }
				friend const_iterator operator-(const const_iterator& lhs, difference_type rhs) { auto result = lhs; return result -= rhs; }
				friend const_iterator operator-(difference_type lhs, const const_iterator& rhs) { auto result = rhs; return result -= lhs; }
				friend difference_type operator-(const const_iterator& lhs, const const_iterator& rhs) { return lhs.ptr - rhs.ptr; }
				const value_type& operator[](difference_type d) { return *(*this + d); }
				bool operator<(const const_iterator& rhs) const { return ptr < rhs.ptr; }
				bool operator>(const const_iterator& rhs) const { return ptr > rhs.ptr; }
				bool operator<=(const const_iterator& rhs) const { return ptr <= rhs.ptr; }
				bool operator>=(const const_iterator& rhs) const { return ptr >= rhs.ptr; }


				const value_type& operator*()
				{
					return *ptr;
				}
				const value_type* operator->()
				{
					return ptr;
				}
			};

			template<class V>
			struct iterator : const_iterator<V>
			{
				using iterator_category = std::random_access_iterator_tag;

				using value_type = typename V::value_type;
				using difference_type = typename V::difference_type;
				using pointer = typename V::pointer;
				using reference = typename V::reference;
				iterator() {}
				iterator(pointer ptr)
					:const_iterator<V>{ ptr } {}

				iterator& operator++() noexcept { ++this->ptr; return *this; }
				void operator++(int) noexcept { ++* this; }
				bool operator==(const iterator& right) const noexcept { return this->ptr == right.ptr; }
				bool operator!=(const iterator& right) const noexcept { return !(*this == right); };
				iterator& operator+=(difference_type d) { this->ptr += d; return *this; }
				iterator& operator-=(difference_type d) { this->ptr -= d; return *this; }
				friend iterator operator+(const iterator& lhs, difference_type rhs) { auto result = lhs; return result += rhs; }
				friend iterator operator+(difference_type lhs, const iterator& rhs) { auto result = rhs; return result += lhs; }
				friend iterator operator-(const iterator& lhs, difference_type rhs) { auto result = lhs; return result -= rhs; }
				friend iterator operator-(difference_type lhs, const iterator& rhs) { auto result = rhs; return result -= lhs; }
				friend difference_type operator-(const iterator& lhs, const iterator& rhs) { return lhs.ptr - rhs.ptr; }
				bool operator<(const iterator& rhs) const { return this->ptr < rhs.ptr; }
				bool operator>(const iterator& rhs) const { return this->ptr > rhs.ptr; }
				bool operator<=(const iterator& rhs) const { return this->ptr <= rhs.ptr; }
				bool operator>=(const iterator& rhs) const { return this->ptr >= rhs.ptr; }
				value_type& operator[](difference_type d) { return *(*this + d); }
				value_type& operator*()
				{
					return *this->ptr;
				}
				value_type* operator->()
				{
					return this->ptr;
				}
			};
		}

		template<class T>
		class buffer_t
		{
			buffer* _data;

		public:
			using value_type = T;
			using pointer = T*;
			using const_pointer = const T*;
			using reference = T&;
			using const_reference = const T&;
			using size_type = size_t;
			using difference_type = size_t;

			buffer_t(const void* inData)
				:_data((buffer*)inData) {}
			T& operator[](int i)
			{
				return ((T*)_data->data())[i];
			}
			const T& operator[](int i) const
			{
				return ((const T*)_data->data())[i];
			}
			uint16_t size() const
			{
				return _data->size / sizeof(T);
			}
			using const_iterator = buffer_detail::const_iterator<buffer_t>;
			using iterator = buffer_detail::iterator<buffer_t>;
			iterator begin() noexcept
			{
				return iterator{ (T*)_data->data() };
			}
			iterator end() noexcept
			{
				return iterator{ (T*)_data->data() + (_data->size / sizeof(T)) };
			}

			const_iterator begin() const noexcept
			{
				return const_iterator{ (T*)_data->data() };
			}
			const_iterator end() const noexcept
			{
				return const_iterator{ (T*)_data->data() + (_data->size / sizeof(T)) };
			}

			template<class P>
			void push(P&& p)
			{
				_data->push(&p, sizeof(T));
			}
			void pop()
			{
				_data->pop(sizeof(T));
			}
			T& last()
			{
				((T*)_data->data())[size()];
			}
			const T& last() const
			{
				((const T*)_data->data())[size()];
			}
			void shrink(uint16_t inlineSize) noexcept
			{
				_data->shrink(inlineSize);
			}
			void reserve(uint16_t newSize) noexcept
			{
				if (newSize * sizeof(T) > _data->capacity)
					_data->grow(newSize * sizeof(T));
			}
			T* data()
			{
				return (T*)_data->data();
			}
			void resize(uint16_t newSize) noexcept
			{
				reserve(newSize);
				_data->size = newSize*sizeof(T);
			}
		};

		struct ECS_API stack_allocator
		{
			char* stackbuffer = nullptr;
			void* stacktop = nullptr;
			size_t stackSize = 0;
			void* alloc(size_t size, size_t align = alignof(int));
			void free(void* ptr, size_t size);
			void init(size_t size)
			{
				stackSize = size;
				stacktop = stackbuffer = (char*)::malloc(10000);
			}
			void reset()
			{
				if (stackbuffer != nullptr)
					::free(stackbuffer);
				stacktop = stackbuffer = nullptr;
				stackSize = 0;
			}
			~stack_allocator()
			{
				reset();
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

			bool operator!=(const entity_type& other) const
			{
				return !(*this == other);
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

		ECS_API extern const entity_type EmptyType;

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
			typeset inverseMask;

			struct hash
			{
				size_t operator()(const entity_filter& key) const
				{
					size_t hash = hash_array(key.inverseMask.data, key.inverseMask.length);
					return hash;
				}
			};

			void ECS_API apply(struct matched_archetype& ma) const;

			bool operator==(const entity_filter& other) const
			{
				return inverseMask == other.inverseMask;
			}
		};
	}
	
}