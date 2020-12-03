#pragma once
#include "Database.h"

namespace ecs
{
	using index_t = core::database::index_t;
	enum flags : uint32_t
	{
		none = 0x00,
		need_cpy = 0x01,
		need_clean = 0x02,
		is_element = 0x04
	};
	template<typename T> core::database::index_t cid;
	template <typename, typename = void>
	struct has_flags : std::false_type {};
	template <typename T>
	struct has_flags<T, std::void_t<decltype(&T::flags)>>
		: std::is_same<const flags, decltype(std::declval<T>().flags)> {};
	
	template<typename T>
	struct register_ {
		using Type = std::decay_t<T>;
		register_() {
			// To make MSVC Happy.
			if constexpr (has_flags<Type>::value)
			{
				result = core::database::register_type({
					(Type::flags & is_element) != 0, (Type::flags & need_cpy) != 0,
					(Type::flags & need_clean) != 0,
					typeid(Type).hash_code(), sizeof(Type)
				});
			}
			else
			{
				result = core::database::register_type({
					false, false, false,
					typeid(Type).hash_code(), sizeof(Type)
				});
			}
		}
		FORCEINLINE operator core::database::index_t() const { return result; }
		core::database::index_t result;
	};


	
	struct world : public core::database::world {
		using base = core::database::world;
		
		template<typename... Args>
		FORCEINLINE auto allocate(uint32_t count = 1) ->
			core::database::chunk_vector<core::database::chunk_slice>
		{
			using namespace core::database;
			const index_t t[] = { ecs::cid<Args>... };
			const entity_type type{ {t, sizeof...(Args)} };
			return base::allocate(type, count);
		}

		template<typename... Args>
		FORCEINLINE bool has_component(const core::entity e) const
		{
			using namespace core::database;
			index_t t[] = { ecs::cid<Args>... };
			return base::has_component(e, { t, sizeof...(Args) });
		}

	};
}

