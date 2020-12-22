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
#include "Set.h"

namespace core
{
	struct GUID {
		uint32_t Data1;
		uint16_t Data2;
		uint16_t Data3;
		uint8_t Data4[8];
		bool operator<(const GUID& rhs) const
		{
			using value_type = std::array<std::byte, 16>;
			return reinterpret_cast<const value_type&>(*this) < reinterpret_cast<const value_type&>(rhs);
		}
	};
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

		// Internal Components
		struct group
		{
			entity e;
		};
		using mask = std::bitset<32>;
		struct disable {};
		struct cleanup {};

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

			std::array<void*, kFastBinCapacity + 10> fastbin{};
			std::array<void*, kSmallBinCapacity + 10> smallbin{};
			std::array<void*, kLargeBinCapacity + 10> largebin{};
			size_t fastbinSize = 0;
			size_t smallbinSize = 0;
			size_t largebinSize = 0;
			index_t disable_id;
			index_t cleanup_id;
			index_t group_id;
			index_t mask_id;

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

			void free(alloc_type type, void* data)
			{
				switch (type)
				{
				case alloc_type::fastbin:
					if (fastbinSize < kLargeBinCapacity)
						fastbin[fastbinSize++] = data;
					else
						::free(data);
					break;
				case alloc_type::smallbin:
					if (smallbinSize < kSmallBinCapacity)
						smallbin[smallbinSize++] = data;
					else
						::free(data);
					break;
				case alloc_type::largebin:
					if (largebinSize < kLargeBinCapacity)
						largebin[largebinSize++] = data;
					else
						::free(data);
					break;
				}
			}

			void* malloc(alloc_type type)
			{
				switch (type)
				{
				case alloc_type::fastbin:
					if (fastbinSize == 0)
						return ::malloc(kFastBinSize);
					else
						return fastbin[--fastbinSize];
					break;
				case alloc_type::smallbin:
					if (smallbinSize == 0)
						return ::malloc(kSmallBinSize);
					else
						return smallbin[--smallbinSize];
					break;
				case alloc_type::largebin:
					if (largebinSize == 0)
						return ::malloc(kLargeBinSize);
					else
						return largebin[--largebinSize];
					break;
				}
				return nullptr;
			}

			index_t register_type(component_desc desc)
			{
				{
					auto i = hash2type.find(desc.GUID);
					if (i != hash2type.end())
						return i->second;
				}
				uint32_t rid = -1;
				if (desc.entityRefs != nullptr)
				{
					rid = (uint32_t)entityRefs.size();
					for(int i = 0; i < desc.entityRefCount; ++i)
						entityRefs.push_back(desc.entityRefs[i]);
				}

				index_t id = (index_t)infos.size();
				id = tagged_index{ id, desc.isElement, desc.size == 0 };
				type_registry i{ desc.GUID, desc.size, desc.elementSize, desc.alignment, rid, desc.entityRefCount, desc.name, desc.vtable };
				infos.push_back(i);
				uint8_t s = 0;
				if (desc.manualClean)
					s = s | ManualCleaning;
				if (desc.manualCopy)
					s = s | ManualCopying;
				tracks.push_back((track_state)s);
				hash2type.insert({ desc.GUID, id });

				if (desc.manualCopy)
				{
					index_t id2 = (index_t)infos.size();
					id2 = tagged_index{ id2, desc.isElement, desc.size == 0 };
					type_registry i2{ desc.GUID, desc.size, desc.elementSize, desc.alignment, rid, desc.entityRefCount, desc.name, desc.vtable };
					tracks.push_back(Copying);
					infos.push_back(i2);
				}

				return id;
			}
		private:
			context();
			void init();
		};
	}
}
