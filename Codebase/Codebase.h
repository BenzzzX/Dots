#pragma once
#include "Database.h"

namespace core
{
	namespace codebase
	{
		using namespace database;
		template<class T>
		void set_bit(T* set, int index)//, bool value)
		{
			constexpr size_t bits = std::numeric_limits<T>::digits;
			//if(value)
				set[index / bits] |= 1 << (index % bits);
			//else
			//	set[index / bits] &= ~(1 << (index % bits));
		}
		template<class T>
		bool check_bit(T* set, int index)
		{
			constexpr size_t bits = std::numeric_limits<T>::digits;
			return (set[index / bits] & (1 << (index % bits)))!=0;
		}
		struct param
		{
			index_t typeIndex;
			bool readonly;
			bool randomAccess;
		};
		
		struct view
		{
			archetype_filter archetypeFilter;
			chunk_filter chunkFilter;
			entity_filter entityFilter;

			param* params;
			int paramCount;
		};

		struct task
		{
			int matched;
			int indexInKernel;
			chunk_slice slice;
		};
		struct kernel;
		struct operation //用于简化api
		{
			operation(const kernel& k, task& t)
				:ctx(k), matched(t.matched), slice(t.slice), indexInKernel(t.indexInKernel) {}
			template<class T>
			T* get_component(int paramId) { return (T*)get(paramId) + slice.start; }
			ECS_API const entity* get_entities();
			ECS_API mask get_mask();
			uint32_t get_count() { return slice.count; }
			uint32_t get_index() { return indexInKernel; }
		private:
			const kernel& ctx;
			int matched;
			chunk_slice slice;
			int indexInKernel;
			ECS_API void* get(int paramId);
		};

		struct kernel
		{
			world& ctx;
			//int* archetypeIndex;
			archetype** archetypes;
			mask* matched;
			index_t* localType;
			index_t* chunkCount;
			int archetypeCount;
			chunk_vector<chunk*> chunks;
			index_t* types;
			index_t* readonly;
			index_t* randomAccess;
			int paramCount;
		};

		class pipeline //计算管线，处于两个 sync point 之间
		{
			//std::vector<std::pair<archetype*, int>> archetypeIndices;
			//std::vector<std::vector<task_group*>> ownership;
			stack_allocator kernelStack;
		public:
			ECS_API pipeline();
			ECS_API kernel* create_kernel(world& w, const view& v);
			ECS_API chunk_vector<task> create_tasks(kernel& k, int maxSlice = -1);
		};
	}

	/*
	chunk_vector<task> tasks = create_tasks(kernel, -1);
	auto handle = xxx::parallel_for(tasks, [&kernel](task& curr)
	{
		operation o{kernel, curr}
		auto counters = o.get_component<counter>(0);
		forloop(i, 0, o.get_count())
			counters[i]++;
	});
	setup_dependencies(handle, kernel);
	*/
}