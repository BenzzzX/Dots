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

		class kernel
		{

			struct match_info
			{
				int chunkSize;
				mask matched;
				index_t* localType;
			};
			world& ctx;
			chunk_vector<match_info> matcheds;
			chunk_vector<chunk*> chunks;
			index_t* types;
			index_t* readonly;
			index_t* randomAccess;
			int paramCount;
			bool withEntityFilter;

			task get(int i);
		};
	}
}