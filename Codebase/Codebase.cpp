#include "Codebase.h"
using namespace core::codebase;
#define forloop(i, z, n) for(auto i = std::decay_t<decltype(n)>(z); i<(n); ++i)

template<class T>
T* allocate_inplace(char*& buffer, size_t size)
{
	char* allocated = buffer;
	buffer += size * sizeof(T);
	return allocated;
}

kernel* pipeline::create_kernel(world& w, const view& v)
{
	auto archs = w.query(v.af);
	size_t bufferSize = 
		sizeof(kernel) //自己
		+ archs.size * (sizeof(void*) + sizeof(mask)) // mask + archetype
		+ v.paramCount * sizeof(index_t) * (archs.size + 1) //type + local type list
		+ (v.paramCount / 4 + 1) * sizeof(index_t) * 2; //readonly + random access
	char* buffer = (char*)kernelStack.alloc(bufferSize, alignof(kernel));
	kernel* k = new(buffer) kernel{ w };
	k->archetypeCount = archs.size;
	k->paramCount = v.paramCount;
	k->archetypes = allocate_inplace<archetype*>(buffer, archs.size);
	k->matched = allocate_inplace<mask>(buffer, archs.size);
	k->types = allocate_inplace<index_t>(buffer, v.paramCount);
	k->readonly = allocate_inplace<index_t>(buffer, v.paramCount / 4 + 1);
	k->randomAccess = allocate_inplace<index_t>(buffer, v.paramCount / 4 + 1);
	k->localType = allocate_inplace<index_t>(buffer, v.paramCount * archs.size);
	forloop(t, 0, v.paramCount)
	{
		k->types[t] = v.params[t].typeIndex;
		k->readonly[t / 4] |= v.params[t].readonly ? 1 << (t % 4) : 0;
		k->randomAccess[t / 4] |= v.params[t].randomAccess ? 1 << (t % 4) : 0;
	}
	int counter = 0;
	for (auto i : archs)
	{
		for (auto j : w.query(i.type, v.cf))
			k->chunks.push(j);
		k->archetypes[counter] = i.type;
		v.ef.apply(i);
		k->matched[counter] = i.matched;
		forloop(t, 0, v.paramCount)
			k->localType[t + counter*archs.size] = i.type->index(k->types[t]);
		counter++;
	}
	return k;
}

constexpr uint16_t InvalidIndex = (uint16_t)-1;
task* pipeline::create_task(const kernel& k, int index)
{
	size_t bufferSize = sizeof(void*) * k.paramCount + sizeof(task);
	char* buffer = (char*)taskStack.alloc(bufferSize, alignof(task));
	task* t = new(buffer) task{ k };
	t->chunkIndex = index;
	chunk* c = k.chunks[index];
	int archIndex = 0;
	for (; k.archetypes[archIndex] != c->get_type(); ++archIndex);
	t->matched = k.matched[archIndex];
	t->values = allocate_inplace<void*>(buffer, k.paramCount);
	for (int i = 0; i < k.paramCount; ++i)
	{
		auto localType = k.localType[archIndex * k.paramCount + i];
		if ((k.readonly[i / 4] & (1 << (i % 4))) != 0)
		{
			if (localType == InvalidIndex)
				t->values[i] = const_cast<void*>(k.ctx.get_shared_ro(k.archetypes[archIndex], k.types[i]));
			else
				t->values[i] = const_cast<void*>(k.ctx.get_owned_ro_local(c, localType));
		}
		else
		{
			if (localType == InvalidIndex) 
				t->values[i] = nullptr; // 不允许非 owner 修改 share 的值
			else
				t->values[i] = const_cast<void*>(k.ctx.get_owned_rw_local(c, localType));
		}
	}
	return t;
}
