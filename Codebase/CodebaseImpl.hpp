#include "Codebase.h"
#pragma once
#define forloop(i, z, n) for(auto i = std::decay_t<decltype(n)>(z); i<(n); ++i)
namespace core
{
	namespace codebase
	{
		template<class T>
		pass* pipeline::create_pass(const filters& v, T paramList)
		{
			auto paramCount = hana::length(paramList).value;
			auto archs = ctx.query(v.archetypeFilter);
			chunk_vector<chunk*> chunks;
			for (auto i : archs)
				for (auto j : ctx.query(i.type, v.chunkFilter))
					chunks.push(j);
			size_t bufferSize =
				sizeof(pass) //自己
				+ archs.size * (sizeof(void*) + sizeof(mask)) // mask + archetype
				+ chunks.size * sizeof(chunk*) // chunks
				+ paramCount * sizeof(index_t) * (archs.size + 1) //type + local type list
				+ (paramCount / 4 + 1) * sizeof(index_t) * 2; //readonly + random access
			char* buffer = (char*)passStack.alloc(bufferSize, alignof(pass));
			pass* k = new(buffer) pass{ ctx };
			passes.push(k);
			buffer += sizeof(pass);
			k->passIndex = passIndex++;
			k->archetypeCount = (int)archs.size;
			k->chunkCount = (int)chunks.size;
			k->paramCount = (int)paramCount;
			k->archetypes = allocate_inplace<archetype*>(buffer, archs.size);
			k->chunks = allocate_inplace<chunk*>(buffer, chunks.size);
			k->matched = allocate_inplace<mask>(buffer, archs.size);
			k->types = allocate_inplace<index_t>(buffer, paramCount);
			constexpr size_t bits = std::numeric_limits<index_t>::digits;
			const auto bal = paramCount / bits + 1;
			k->readonly = allocate_inplace<index_t>(buffer, bal);
			k->randomAccess = allocate_inplace<index_t>(buffer, bal);
			memset(k->readonly, 0, sizeof(index_t) * bal);
			memset(k->randomAccess, 0, sizeof(index_t) * bal);
			k->localType = allocate_inplace<index_t>(buffer, paramCount * archs.size);
			k->hasRandomWrite = false;
			int t = 0;
			hana::for_each(paramList, [&](auto p)
				{
					using type = decltype(p);
					k->types[t] = cid<decltype(p.comp_type)::type>;
					if(type::readonly)
						set_bit(k->readonly, t);
					if(type::randomAccess)
						set_bit(k->randomAccess, t);
					k->hasRandomWrite |= (type::randomAccess && !type::readonly);
					t++;
				});
			int counter = 0;
			chunks.flatten(k->chunks);
			for (auto i : archs)
			{
				k->archetypes[counter] = i.type;
				v.entityFilter.apply(i);
				k->matched[counter] = i.matched;
				forloop(j, 0, paramCount)
					k->localType[j + counter * paramCount] = i.type->index(k->types[j]);
				counter++;
			}
			setup_pass_dependency(*k);
			return k;
		}


		template<class ...params>
		template<class T>
		inline constexpr auto operation<params...>::param_id()
		{
			def compList = hana::transform(paramList, [](const auto p) { return p.comp_type; });
			return *hana::index_if(compList, hana::_ == hana::type_c<T>);
		}

		template<class ...params>
		template<class T>
		inline bool operation<params...>::is_owned()
		{
			def paramId = param_id<T>();
			return is_owned(paramId.value);
		}

		template<class ...params>
		template<class T>
		auto operation<params...>::get_parameter()
		{
			constexpr uint16_t InvalidIndex = (uint16_t)-1;
			using value_type = component_value_type_t<T>;
			auto paramId_c = param_id<T>();
			int paramId = paramId_c.value;
			auto param = hana::at(paramList, paramId_c);
			void* ptr = nullptr;
			auto localType = ctx.localType[matched * ctx.paramCount + paramId];
			using return_type = std::conditional_t<param.readonly, std::add_const_t<value_type>, value_type>;
			if (localType >= ctx.archetypes[matched]->firstTag)
				return (return_type*)nullptr;
			if constexpr (param.readonly)
			{
				if (localType == InvalidIndex)
					ptr = const_cast<void*>(ctx.ctx.get_shared_ro(ctx.archetypes[matched], ctx.types[paramId]));
				else
					ptr = const_cast<void*>(ctx.ctx.get_owned_ro_local(slice.c, localType));
			}
			else
			{
				if (localType == InvalidIndex)
					ptr = nullptr; // 不允许非 owner 修改 share 的值
				else
					ptr = const_cast<void*>(ctx.ctx.get_owned_rw_local(slice.c, localType));
			}
			return (ptr && operation_base::is_owned(paramId)) ? (return_type*)ptr + slice.start : (return_type*)ptr;
		}

		template<class ...params>
		template<class T>
		auto operation<params...>::get_parameter_owned()
		{
			constexpr uint16_t InvalidIndex = (uint16_t)-1;
			using value_type = component_value_type_t<T>;
			auto paramId_c = param_id<T>();
			int paramId = paramId_c.value;
			auto param = hana::at(paramList, paramId_c);
			void* ptr = nullptr;
			auto localType = ctx.localType[matched * ctx.paramCount + paramId];
			using return_type = std::conditional_t<param.readonly, std::add_const_t<value_type>, value_type>;
			//if (localType >= ctx.archetypes[matched]->firstTag)
			//	return (value_type*)nullptr;
			if constexpr (param.readonly)
				ptr = const_cast<void*>(ctx.ctx.get_owned_ro_local(slice.c, localType));
			else
				ptr = const_cast<void*>(ctx.ctx.get_owned_rw_local(slice.c, localType));
			return (ptr && operation_base::is_owned(paramId)) ? (return_type*)ptr + slice.start : (return_type*)ptr;
		}

		template<class ...params>
		template<class T>
		auto operation<params...>::get_parameter(entity e)
		{
			static_assert(param::randomAccess, "only random access parameter can be accessed by entity");
			using value_type = component_value_type_t<T>;
			auto paramId_c = param_id<T>();
			int paramId = paramId_c.value;
			auto param = hana::at(paramList, paramId_c);
			using return_type = std::conditional_t<param.readonly, std::add_const_t<value_type>, value_type>;
			if constexpr (param.readonly)
				return (return_type*)const_cast<void*>(ctx.ctx.get_component_ro(e, ctx.types[paramId]));
			else
				return (return_type*)const_cast<void*>(ctx.ctx.get_owned_rw(e, ctx.types[paramId]));
		}

		template<class ...params>
		template<class T>
		auto operation<params...>::get_parameter_owned(entity e)
		{
			static_assert(param::randomAccess, "only random access parameter can be accessed by entity");
			using value_type = component_value_type_t<T>;
			auto paramId_c = param_id<T>();
			int paramId = paramId_c.value;
			auto param = hana::at(paramList, paramId_c);
			using return_type = std::conditional_t<param.readonly, std::add_const_t<value_type>, value_type>;
			if constexpr (param.readonly)
				return (return_type*)const_cast<void*>(ctx.ctx.get_owned_ro(e, ctx.types[paramId]));
			else
				return (return_type*)const_cast<void*>(ctx.ctx.get_owned_rw(e, ctx.types[paramId]));
		}
	}
}
#undef forloop