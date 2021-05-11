#include "Codebase.h"
#include <set>
#pragma once
#define forloop(i, z, n) for(auto i = std::decay_t<decltype(n)>(z); i<(n); ++i)


namespace boost::hana
{
	template <typename Iterable, typename T>
	constexpr auto index_of(Iterable const& iterable, T const& element)
	{
		auto size = decltype(hana::size(iterable)){};
		auto dropped = decltype(hana::size(
			hana::drop_while(iterable, hana::not_equal.to(element))
		)){};
		return size - dropped;
	}
}
namespace core
{
	namespace codebase
	{
		
		template<class T>
		std::shared_ptr<pass> pipeline::create_pass(const filters& v, T paramList, gsl::span<shared_entry> sharedEntries)
		{
			static_assert(hana::is_a<hana::tuple_tag>(paramList), "parameter list should be a hana::list");

			auto paramCount = hana::length(paramList).value;
			auto archs = query(v.archetypeFilter);
			constexpr size_t bits = std::numeric_limits<index_t>::digits;
			const auto bal = paramCount / bits + 1;
			
			size_t bufferSize =
				sizeof(pass) //自己
				+ v.get_size()
				+ archs.size * (sizeof(void*) + sizeof(mask)) // mask + archetype
				+ paramCount * sizeof(index_t) * (archs.size + 1) //type + local type list
				+ bal * sizeof(index_t) * 2; //readonly + random access
			char* buffer = (char*)::malloc(bufferSize);
			pass* k = new(buffer) pass{ *this };
			std::shared_ptr<pass> ret{ k };
			buffer += sizeof(pass);
			k->passIndex = passIndex++;
			k->archetypeCount = (int)archs.size;
			k->paramCount = (int)paramCount;
			k->archetypes = allocate_inplace<archetype*>(buffer, archs.size);
			k->matched = allocate_inplace<mask>(buffer, archs.size);
			k->types = allocate_inplace<index_t>(buffer, paramCount);
			k->readonly = allocate_inplace<index_t>(buffer, bal);
			k->randomAccess = allocate_inplace<index_t>(buffer, bal);
			memset(k->readonly, 0, sizeof(index_t) * bal);
			memset(k->randomAccess, 0, sizeof(index_t) * bal);
			k->localType = allocate_inplace<index_t>(buffer, paramCount * archs.size);
			k->filter = v.clone(buffer);
			k->hasRandomWrite = false;
			int t = 0;
			hana::for_each(paramList, [&](auto p)
				{
					using type = decltype(p);
					k->types[t] = cid<typename decltype(p.comp_type)::type>;
					if(type::readonly)
						set_bit(k->readonly, t);
					if(type::randomAccess)
						set_bit(k->randomAccess, t);
					k->hasRandomWrite |= (type::randomAccess && !type::readonly);
					t++;
				});
			int counter = 0;
			for (auto i : archs)
			{
				k->archetypes[counter] = i.type;
				v.entityFilter.apply(i);
				k->matched[counter] = i.matched;
				forloop(j, 0, paramCount)
					k->localType[j + counter * paramCount] = i.type->index(k->types[j]);
				counter++;
			}
			setup_pass_dependency(ret, sharedEntries);
			setup_pass(ret);
			return ret;
		}

		namespace detail
		{
			struct weak_ptr_compare
			{
				template<class T>
				bool operator() (const std::weak_ptr<T>& lhs, const std::weak_ptr<T>& rhs)const
				{
					return lhs.owner_before(rhs);
				}
			};
			using weak_ptr_set = std::set<std::weak_ptr<custom_pass>, weak_ptr_compare>;
		}

		template<class ...params>
		template<class T>
		inline constexpr auto operation<params...>::param_id()
		{
			return hana::index_of(compList, hana::type_c<T>);
		}

		template<class ...params>
		template<class T>
		detail::array_ret_t<T> operation<params...>::get_parameter()
		{
			constexpr uint16_t InvalidIndex = (uint16_t)-1;
			using DT = std::remove_const_t<T>;
			//using value_type = component_value_type_t<DT>;
			auto paramId_c = param_id<DT>();
			auto param = hana::at(paramList, paramId_c);
			using return_type = detail::array_ret_t<T>;
			//if (localType >= ctx.archetypes[gid]->firstTag)
			//	return (return_type)nullptr;
			void* ptr = nullptr;
			int paramId = paramId_c.value;
			auto localType = ctx.localType[gid * ctx.paramCount + paramId];
			auto& wrd = (world&)ctx.ctx;
			if constexpr (param.readonly)
			{
				static_assert(std::is_const_v<T>, "Can only perform const-get for readonly params.");
				if (localType == InvalidIndex)
					ptr = const_cast<void*>(wrd.get_shared_ro(ctx.archetypes[gid], ctx.types[paramId]));
				else
					ptr = const_cast<void*>(wrd.get_owned_ro_local(slice.c, localType));
			}
			else
			{
				if (localType == InvalidIndex)
					ptr = nullptr; // 不允许非 owner 修改 share 的值
				else
					ptr = const_cast<void*>(wrd.get_owned_rw_local(slice.c, localType));
			}
			return (ptr && localType != InvalidIndex) ? (return_type)ptr + slice.start : (return_type)ptr;
		}

		template<class ...params>
		template<class... Ts>
		std::tuple<detail::array_ret_t<Ts>...> operation<params...>::get_parameters()
		{
			return std::make_tuple(get_parameter<Ts>()...);
		}

		template<class ...params>
		template<class T>
		detail::array_ret_t<T> operation<params...>::get_parameter_owned()
		{
			constexpr uint16_t InvalidIndex = (uint16_t)-1;
			//using value_type = component_value_type_t<std::decay_t<T>>;
			auto paramId_c = param_id<std::decay_t<T>>();
			auto param = hana::at(paramList, paramId_c);
			using return_type = detail::array_ret_t<T>;
			//if (localType >= ctx.archetypes[gid]->firstTag)
			//	return (return_type)nullptr;
			void* ptr = nullptr;
			int paramId = paramId_c.value;
			auto localType = ctx.localType[gid * ctx.paramCount + paramId];
			auto& wrd = (world&)ctx.ctx;
			if constexpr (param.readonly)
			{ 
				static_assert(std::is_const_v<T>, "Can only perform const-get for readonly params.");
				ptr = const_cast<void*>(wrd.get_owned_ro_local(slice.c, localType));
			}
			else
				ptr = const_cast<void*>(wrd.get_owned_rw_local(slice.c, localType));
			return (ptr && localType != InvalidIndex) ? (return_type)ptr + slice.start : (return_type)ptr;
		}

		template<class ...params>
		template<class... Ts>
		std::tuple<detail::array_ret_t<Ts>...> operation<params...>::get_parameters_owned()
		{
			return std::make_tuple(get_parameter_owned<Ts>()...);
		}

		template<class ...params>
		template<class T>
		detail::value_ret_t<T> operation<params...>::get_parameter(entity e)
		{
			//using value_type = component_value_type_t<std::decay_t<T>>;
			auto paramId_c = param_id<std::decay_t<T>>();
			int paramId = paramId_c.value;
			auto param = hana::at(paramList, paramId_c);
			static_assert(param.randomAccess, "only random access parameter can be accessed by entity");
			using return_type = detail::value_ret_t<T>;
			auto& wrd = (world&)ctx.ctx;
			if constexpr (param.readonly)
			{
				static_assert(std::is_const_v<T>, "Can only perform const-get for readonly params.");
				return (return_type)const_cast<void*>(wrd.get_component_ro(e, ctx.types[paramId]));
			}
			else
				return (return_type)const_cast<void*>(wrd.get_owned_rw(e, ctx.types[paramId]));
		}
		
		template<class ...params>
		template<class... Ts>
		std::tuple<detail::array_ret_t<Ts>...> operation<params...>::get_parameters(entity e)
		{
			return std::make_tuple(get_parameter<Ts>(e)...);
		}

		template<class ...params>
		template<class T>
		detail::value_ret_t<T> operation<params...>::get_parameter_owned(entity e)
		{
			//using value_type = component_value_type_t<std::decay_t<T>>;
			auto paramId_c = param_id<std::decay_t<T>>();
			int paramId = paramId_c.value;
			auto param = hana::at(paramList, paramId_c);
			static_assert(param.randomAccess, "only random access parameter can be accessed by entity");
			using return_type = detail::value_ret_t<T>;
			auto& wrd = (world&)ctx.ctx;
			if constexpr (param.readonly)
			{
				static_assert(std::is_const_v<T>, "Can only perform const-get for readonly params.");
				return (return_type)const_cast<void*>(wrd.get_owned_ro(e, ctx.types[paramId]));
			}
			else
				return (return_type)const_cast<void*>(wrd.get_owned_rw(e, ctx.types[paramId]));
		}

		template<class ...params>
		template<class... Ts>
		std::tuple<detail::array_ret_t<Ts>...> operation<params...>::get_parameters_owned(entity e)
		{
			return std::make_tuple(get_parameter_owned<Ts>(e)...);
		}
	}
}
#undef forloop