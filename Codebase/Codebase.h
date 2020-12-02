#pragma once
#include "Database.h"

namespace core
{
	namespace codebase
	{
		using namespace database;
		struct param
		{
			index_t typeIndex;
			bool readonly;
			bool randomAccess;
		};

		struct view
		{
			archetype_filter af;
			chunk_filter cf;
			entity_filter ef;
			param* params;
			int paramCount;
		};

		struct task
		{
			const kernel& ctx;
			mask matched;
			int chunkIndex;
			void** values;
		};

		struct task_group
		{
			const kernel& ctx;
			mask matched;
			int startIndex;
			int numTasks;
			void** values;
			task_group** dependencies;
		};

		struct kernel
		{
			world& ctx;

			archetype** archetypes;
			mask* matched;
			index_t* localType;
			int archetypeCount;
			chunk_vector<chunk*> chunks;
			index_t* types;
			index_t* readonly;
			index_t* randomAccess;
			int paramCount;
			bool withEntityFilter;
		};

		class pipeline //计算管线，处于两个 sync point 之间
		{
			//std::map<archetype*,std::vector<task_group*>> ownership;
			stack_allocator kernelStack;
			stack_allocator taskStack;
			kernel* create_kernel(world& w, const view& v);
			task* create_task(const kernel& k, int index);
		};
	}
}