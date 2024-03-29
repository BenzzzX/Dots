#pragma once
#ifdef __EMSCRIPTEN__
#include "emscripten.h"
#define DLLEXPORT EMSCRIPTEN_KEEPALIVE
#define ECS_RT_API EMSCRIPTEN_KEEPALIVE
#define DLLLOCAL __attribute__((visibility("hidden")))
#define __stdcall 
#elif defined(__PROSPERO__)
#define DLLEXPORT __declspec(dllexport)
#ifdef DOTSRUNTIME_EXPORTS
#define ECS_RT_API __declspec(dllexport)
#else
#define ECS_RT_API __declspec(dllimport)
#endif
#elif defined(__GNUC__)
#define ECS_RT_API __attribute__((visibility("default")))
#define DLLEXPORT __attribute__((visibility("default")))
#define DLLLOCAL __attribute__((visibility("hidden")))
#define __stdcall 
#else
#define DLLEXPORT __declspec(dllexport)
#ifdef DOTSRUNTIME_EXPORTS
#define ECS_RT_API __declspec(dllexport)
#elif defined(FULL_STATIC)
	#ifdef ECS_RT_API
	#undef ECS_RT_API
	#endif
	#define ECS_RT_API 
#else
#define ECS_RT_API __declspec(dllimport)
#endif
#endif

#ifndef FORCEINLINE
#ifdef _MSC_VER
#define FORCEINLINE __forceinline
#else
#define FORCEINLINE inline
#endif
#endif

#define CACHE_LINE_SIZE 64
#ifdef _DEBUG
#define ECS_ENABLE_ASSERTIONS true
#else
#define ECS_ENABLE_ASSERTIONS false
#endif

#include <vector>
#include <string>
#include <assert.h>
#include <array>
#include <map>
#include <bitset>
#include "concurrentqueue.h"
#include "Set.h"

namespace core
{
	template<size_t A, size_t B, class T = uint32_t>
	struct handle
	{
		using underlying_type = T;
		static_assert(A + B == sizeof(T) * 8, "size does not fit into T");
		union
		{
			T value;
			struct
			{
				T version : B;
				T id : A;
			};
		};
		constexpr static T Null = std::numeric_limits<T>::max();
		constexpr static T TransientMagicNumber = ((1 << B) - 1);
		constexpr operator T() const { return value; }
		constexpr handle() = default;
		constexpr handle(nullptr_t) : value(Null) {};
		constexpr handle(T t) : value(t) { }
		constexpr handle(T i, T v) : id(i), version(v) { }
		constexpr bool is_transient() { return version == ((1 << B) - 1); }
		constexpr static T make_transient(T i) { return handle{ i, TransientMagicNumber }.value; }
		constexpr static T recycle(T version)
		{
			return (version + 1) == TransientMagicNumber ? (version + 2) : (version + 1);
		}
	};

	struct entity : handle<24, 8>
	{
		using ut = handle<24, 8>;
		using ut::handle;
	};
	constexpr entity NullEntity = entity::Null;

	struct GUID {
		struct
		{
			uint32_t Data1;
			uint16_t Data2;
			uint16_t Data3;
			uint8_t Data4[8];
		};

		bool operator<(const GUID& rhs) const
		{
			using value_type = std::array<std::byte, 16>;
			return reinterpret_cast<const value_type&>(*this) < reinterpret_cast<const value_type&>(rhs);
		}
	};
#ifdef __EMSCRIPTEN__
	constexpr size_t _FNV_offset_basis = 2166136261U;
	constexpr size_t _FNV_prime = 16777619U;
#else
	constexpr size_t _FNV_offset_basis = sizeof(size_t) == sizeof(uint32_t) ? 2166136261U : 14695981039346656037ULL;
	constexpr size_t _FNV_prime = sizeof(uint32_t) ? 16777619U : 1099511628211ULL;
#endif

	inline size_t hash_append(size_t val, const unsigned char* const data, const size_t length) noexcept
	{ // accumulate range [data, data + length) into partial FNV-1a uuid val
		for (size_t i = 0; i < length; ++i) {
			val ^= static_cast<size_t>(data[i]);
			val *= _FNV_prime;
		}

		return val;
	}

	template <class T>
	inline size_t hash_array(const T* const data, const size_t length, const size_t basis = _FNV_offset_basis) noexcept
	{ // bitwise hashes the representation of an array
		static_assert(std::is_trivial_v<T>, "Only trivial types can be directly hashed.");
		return hash_append(
			basis, reinterpret_cast<const unsigned char*>(data), length * sizeof(T));
	}

	namespace guid_parse
	{
		namespace details
		{
			constexpr const size_t short_guid_form_length = 36;	// XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX
			constexpr const size_t long_guid_form_length = 38;	// {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX}

			//
			constexpr int parse_hex_digit(const char c)
			{
				using namespace std::string_literals;
				if ('0' <= c && c <= '9')
					return c - '0';
				else if ('a' <= c && c <= 'f')
					return 10 + c - 'a';
				else if ('A' <= c && c <= 'F')
					return 10 + c - 'A';
				else
					assert(0 && "invalid character in GUID");
				return -1;
			}

			template<class T>
			constexpr T parse_hex(const char* ptr)
			{
				constexpr size_t digits = sizeof(T) * 2;
				T result{};
				for (size_t i = 0; i < digits; ++i)
					result |= parse_hex_digit(ptr[i]) << (4 * (digits - i - 1));
				return result;
			}

			constexpr GUID make_guid_helper(const char* begin)
			{
				GUID result{};
				result.Data1 = parse_hex<uint32_t>(begin);
				begin += 8 + 1;
				result.Data2 = parse_hex<uint16_t>(begin);
				begin += 4 + 1;
				result.Data3 = parse_hex<uint16_t>(begin);
				begin += 4 + 1;
				result.Data4[0] = parse_hex<uint8_t>(begin);
				begin += 2;
				result.Data4[1] = parse_hex<uint8_t>(begin);
				begin += 2 + 1;
				for (size_t i = 0; i < 6; ++i)
					result.Data4[i + 2] = parse_hex<uint8_t>(begin + i * 2);
				return result;
			}

			template<size_t N>
			constexpr GUID make_guid(const char(&str)[N])
			{
				using namespace std::string_literals;
				static_assert(N == (long_guid_form_length + 1) || N == (short_guid_form_length + 1), "String GUID of the form {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX} or XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX is expected");

				if constexpr (N == (long_guid_form_length + 1))
				{
					if (str[0] != '{' || str[long_guid_form_length - 1] != '}')
						assert(0 && "Missing opening or closing brace");
				}

				return make_guid_helper(str + (N == (long_guid_form_length + 1) ? 1 : 0));
			}
		}
		using details::make_guid;

		namespace literals
		{
			constexpr GUID operator "" _guid(const char* str, size_t N)
			{
				using namespace std::string_literals;
				using namespace details;

				if (!(N == long_guid_form_length || N == short_guid_form_length))
					assert(0 && "String GUID of the form {XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX} or XXXXXXXX-XXXX-XXXX-XXXX-XXXXXXXXXXXX is expected");
				if (N == long_guid_form_length && (str[0] != '{' || str[long_guid_form_length - 1] != '}'))
					assert(0 && "Missing opening or closing brace");

				return make_guid_helper(str + (N == long_guid_form_length ? 1 : 0));
			}
		}

	}

	namespace database
	{
		using index_t = uint32_t;
		enum component_type : index_t
		{
			ct_pod = 0,
			ct_buffer = 0b1,
			ct_tag = 0b11,
			ct_managed = 0b10,
		};
		class type_index
		{
			static_assert(sizeof(index_t) * 8 == 32, "index_t should be 32 bits");
			union
			{
				index_t value;
				struct
				{
					index_t id : 30;
					component_type type : 2;
				};
			};

		public:
			constexpr index_t index() const noexcept { return id; }
			constexpr bool is_pod() const noexcept { return type == ct_pod; }
			constexpr bool is_buffer() const noexcept { return type == ct_buffer; }
			constexpr bool is_tag() const noexcept { return type == ct_tag; }
			constexpr bool is_managed() const noexcept { return type == ct_managed; }

			constexpr type_index() noexcept : value(0) {}
			constexpr type_index(index_t value) noexcept : value(value) { }
			constexpr type_index(index_t a, component_type t) noexcept
				: id(a), type(t) { }

			constexpr operator index_t() const { return value; }
		};

		constexpr uint16_t InvalidIndex = (uint16_t)-1;

		static constexpr size_t kFastBinSize = 64 * 1024;
		static constexpr size_t kSmallBinThreshold = 8;
		static constexpr size_t kSmallBinSize = 1024;
		static constexpr size_t kLargeBinSize = 1024 * 1024;

		static constexpr size_t kFastBinCapacity = 800;
		static constexpr size_t kSmallBinCapacity = 200;
		static constexpr size_t kLargeBinCapacity = 80;

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

		struct component_vtable
		{
			void(*patch)(char* data, patcher_i* stream) = nullptr;
			void(*copy)(char* dst, const char* src, size_t n) = nullptr;
			void(*constructor)(char* data, size_t n) = nullptr;
			void(*destructor)(char* data, size_t n) = nullptr;
			//todo: move?
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
			bool isManaged = false;
			bool manualCopy = false;
			bool manualClean = false;
			core::GUID GUID = core::GUID();
			uint16_t size = 0;
			uint16_t elementSize = 0;
			uint16_t alignment = alignof(long long);
			intptr_t* entityRefs = nullptr;
			uint16_t entityRefCount = 0;
			component_vtable vtable;
			const char* name = nullptr;
		};

		struct stack_allocator
		{
			char* stackbuffer = nullptr;
			void* stacktop = nullptr;
			size_t stackSize = 0;
			void* alloc(size_t size, size_t align = alignof(int))
			{
				auto offset = uint32_t((char*)stacktop - stackbuffer);
				stacktop = (uint32_t*)stacktop + 1;
				if (std::align(align, size, stacktop, stackSize))
				{
					*((uint32_t*)stacktop - 1) = offset;
					auto result = stacktop;
					stacktop = (char*)stacktop + size;
					stackSize -= size + sizeof(uint32_t);
					return result;
				}
				return nullptr;
			}
			void free(void* ptr, size_t size)
			{
				auto offset = uint32_t((char*)stacktop - stackbuffer);
				auto oldOffset = *((uint32_t*)ptr - 1);
				stackSize += offset - oldOffset;
				stacktop = stackbuffer + oldOffset;
			}
			void init(size_t size)
			{
				stackSize = size;
				stacktop = stackbuffer = (char*)::malloc(size);
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

		enum class alloc_type : uint8_t
		{
			smallbin, fastbin, largebin
		};

		struct type_registry
		{
			core::GUID GUID = core::GUID();
			uint16_t size;
			uint16_t elementSize;
			uint16_t alignment;
			uint32_t entityRefs;
			uint16_t entityRefCount;
			const char* name;
			component_vtable vtable;
		};

		struct context
		{
			std::vector<type_registry> infos;
			std::vector<track_state> tracks;
			std::vector<intptr_t> entityRefs;
			std::map<core::GUID, size_t> hash2type;

			moodycamel::ConcurrentQueue<void*> fastbin{ kFastBinCapacity };
			moodycamel::ConcurrentQueue<void*> smallbin{ kSmallBinCapacity };
			moodycamel::ConcurrentQueue<void*> largebin{ kLargeBinCapacity };
			type_index disable_id;
			type_index cleanup_id;
			type_index group_id;
			type_index mask_id;

#ifdef ENABLE_GUID_COMPONENT
			type_index guid_id;
#endif

			stack_allocator stack;

			ECS_RT_API static context& get();

			void* stack_alloc(size_t size, size_t align = alignof(int))
			{
				return stack.alloc(size, align);
			}

			void stack_free(void* ptr, size_t size)
			{
				return stack.free(ptr, size);
			}

			ECS_RT_API void free(alloc_type type, void* data);

			ECS_RT_API void* malloc(alloc_type type);

			ECS_RT_API type_index register_type(component_desc desc);
		private:
			context();
		};
	}

	inline size_t hash_array(const database::type_index* const data, const size_t length, const size_t basis = _FNV_offset_basis) noexcept
	{
		return hash_append(
			basis, reinterpret_cast<const unsigned char*>(data), length * sizeof(database::type_index));
	}

	namespace database
	{
		using typeset = set<type_index>;
		using metaset = set<entity>;

		// Internal Components
		struct group
		{
			entity e;
		};
		using mask = std::bitset<32>; //TODO: atomic api
		struct disable {};
		struct cleanup {};
	}
}
