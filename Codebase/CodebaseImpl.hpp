#include "Codebase.h"
#pragma once
#define forloop(i, z, n) for(auto i = std::decay_t<decltype(n)>(z); i<(n); ++i)
namespace core
{
	namespace codebase
	{
		template<class T>
		kernel* pipeline::create_kernel(const filters& v, T paramList)
		{
			auto paramCount = hana::length(paramList).value;
			auto archs = ctx.query(v.archetypeFilter);
			chunk_vector<chunk*> chunks;
			for (auto i : archs)
				for (auto j : ctx.query(i.type, v.chunkFilter))
					chunks.push(j);
			size_t bufferSize =
				sizeof(kernel) //自己
				+ archs.size * (sizeof(void*) + sizeof(mask)) // mask + archetype
				+ chunks.size * sizeof(chunk*) // chunks
				+ paramCount * sizeof(index_t) * (archs.size + 1) //type + local type list
				+ (paramCount / 4 + 1) * sizeof(index_t) * 2; //readonly + random access
			char* buffer = (char*)kernelStack.alloc(bufferSize, alignof(kernel));
			kernel* k = new(buffer) kernel{ ctx };
			kernels.push(k);
			buffer += sizeof(kernel);
			k->kernelIndex = kernelIndex++;
			k->archetypeCount = (int)archs.size;
			k->chunkCount = (int)chunks.size;
			k->paramCount = (int)paramCount;
			k->archetypes = allocate_inplace<archetype*>(buffer, archs.size);
			k->chunks = allocate_inplace<chunk*>(buffer, chunks.size);
			k->matched = allocate_inplace<mask>(buffer, archs.size);
			k->types = allocate_inplace<index_t>(buffer, paramCount);
			k->readonly = allocate_inplace<index_t>(buffer, paramCount / 4 + 1);
			k->randomAccess = allocate_inplace<index_t>(buffer, paramCount / 4 + 1);
			k->localType = allocate_inplace<index_t>(buffer, paramCount * archs.size);
			int t = 0;
			hana::for_each(paramList, [&](auto p)
				{
					using type = decltype(p);
					k->types[t] = cid<decltype(p.comp_type)::type>;
					set_bit(k->readonly, type::readonly);
					set_bit(k->randomAccess, type::randomAccess);
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
					k->localType[j + counter * archs.size] = i.type->index(k->types[j]);
				counter++;
			}
			setup_kernel_dependency(*k);
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
			if (localType >= ctx.archetypes[matched]->firstTag)
				return (value_type*)nullptr;
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
			return (ptr && operation_base::is_owned(paramId)) ? (value_type*)ptr + slice.start : (value_type*)ptr;
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
			//if (localType >= ctx.archetypes[matched]->firstTag)
			//	return (value_type*)nullptr;
			if constexpr (param.readonly)
				ptr = const_cast<void*>(ctx.ctx.get_owned_ro_local(slice.c, localType));
			else
				ptr = const_cast<void*>(ctx.ctx.get_owned_rw_local(slice.c, localType));
			return (ptr && operation_base::is_owned(paramId)) ? (value_type*)ptr + slice.start : (value_type*)ptr;
		}

		template<class ...params>
		template<class T>
		auto operation<params...>::get_parameter(entity e)
		{
			using value_type = component_value_type_t<T>;
			auto paramId_c = param_id<T>();
			int paramId = paramId_c.value;
			auto param = hana::at(paramList, paramId_c);
			if constexpr (!param::randomAccess)
				return nullptr;
			if constexpr (param.readonly)
				return (value_type*)const_cast<void*>(ctx.ctx.get_component_ro(e, ctx.types[paramId]));
			else
				return (value_type*)const_cast<void*>(ctx.ctx.get_owned_rw(e, ctx.types[paramId]));
		}

		template<class ...params>
		template<class T>
		auto operation<params...>::get_parameter_owned(entity e)
		{
			using value_type = component_value_type_t<T>;
			auto paramId_c = param_id<T>();
			int paramId = paramId_c.value;
			auto param = hana::at(paramList, paramId_c);
			if constexpr (!param::randomAccess)
				return nullptr;
			if constexpr (param.readonly)
				return (value_type*)const_cast<void*>(ctx.ctx.get_owned_ro(e, ctx.types[paramId]));
			else
				return (value_type*)const_cast<void*>(ctx.ctx.get_owned_rw(e, ctx.types[paramId]));
		}
	}
}
#undef forloop