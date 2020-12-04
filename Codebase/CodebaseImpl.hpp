#pragma once
#define forloop(i, z, n) for(auto i = std::decay_t<decltype(n)>(z); i<(n); ++i)
namespace core
{
	namespace codebase
	{
		template<class T>
		inline kernel* pipeline::create_kernel(const filters& v, T paramList)
		{
			auto paramCount = hana::length(paramList).value;
			auto archs = ctx.query(v.archetypeFilter);
			size_t bufferSize =
				sizeof(kernel) //自己
				+ archs.size * (sizeof(void*) + sizeof(mask)) // mask + archetype
				+ archs.size * sizeof(index_t)
				+ paramCount * sizeof(index_t) * (archs.size + 1) //type + local type list
				+ (paramCount / 4 + 1) * sizeof(index_t) * 2; //readonly + random access
			char* buffer = (char*)kernelStack.alloc(bufferSize, alignof(kernel));
			kernel* k = new(buffer) kernel{ ctx };
			kernels.push(k);
			buffer += sizeof(kernel);
			k->archetypeCount = (int)archs.size;
			k->paramCount = (int)paramCount;
			k->archetypes = allocate_inplace<archetype*>(buffer, archs.size);
			k->matched = allocate_inplace<mask>(buffer, archs.size);
			k->chunkCount = allocate_inplace<index_t>(buffer, archs.size);
			k->types = allocate_inplace<index_t>(buffer, paramCount);
			k->readonly = allocate_inplace<index_t>(buffer, paramCount / 4 + 1);
			k->randomAccess = allocate_inplace<index_t>(buffer, paramCount / 4 + 1);
			k->localType = allocate_inplace<index_t>(buffer, paramCount * archs.size);
			int t = 0;
			hana::for_each(paramList, [&](auto p)
				{
					using type = decltype(p);
					k->types[t] = cid<typename type::comp_type>;
					set_bit(k->readonly, type::readonly);
					set_bit(k->randomAccess, type::randomAccess);
					t++;
				});
			int counter = 0;
			for (auto i : archs)
			{
				k->chunkCount[counter] = 0;
				for (auto j : ctx.query(i.type, v.chunkFilter))
				{
					k->chunkCount[counter]++;
					k->chunks.push(j);
				}
				k->archetypes[counter] = i.type;
				v.entityFilter.apply(i);
				k->matched[counter] = i.matched;
				forloop(j, 0, paramCount)
					k->localType[j + counter * archs.size] = i.type->index(k->types[j]);
				counter++;
			}
			return k;
		}
	}
}
#undef forloop