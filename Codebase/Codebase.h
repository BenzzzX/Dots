#pragma once
#include "Database.h"
#include "boost/hana.hpp"

#define def static constexpr auto
namespace core
{
	namespace codebase
	{
		namespace hana = boost::hana;
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

		template<class T, class = void>
		struct component_value_type { using type = T; };
		template<class T>
		struct component_value_type<T, std::void_t<typename T::value_type>>
		{
			using type = typename T::value_type;
		};
		template<class T>
		using component_value_type_t = typename component_value_type<T>::type;

		template<class T>
		inline index_t cid;

		template<class T, bool inRandomAccess = false>
		struct param_t
		{
			using TT = std::remove_const_t<T>;
			def comp_type = hana::type_c<TT>;
			def value_type = hana::type_c< component_value_type_t<TT>>;
			def readonly = std::is_const_v<T>;
			def randomAccess = inRandomAccess;
		};
		template<class T, bool randomAccess = false>
		static constexpr param_t<T, randomAccess> param;

		
		struct filters
		{
			archetype_filter archetypeFilter;
			chunk_filter chunkFilter;
			entity_filter entityFilter;
		};

		struct task
		{
			int matched;
			int indexInKernel;
			chunk_slice slice;
		};
		struct pass;

		struct operation_base
		{
			ECS_API const entity* get_entities();
		protected:
			operation_base(const pass& k, task& t)
				:ctx(k), matched(t.matched), slice(t.slice), indexInKernel(t.indexInKernel) {}
			const pass& ctx;
			int matched;
			chunk_slice slice;
			int indexInKernel;
			ECS_API mask get_mask();
			ECS_API bool is_owned(int paramId);
		};
		template<class... params>
		struct operation : operation_base //用于简化api
		{
			static constexpr hana::tuple<params...> paramList;
			operation(hana::tuple<params...> ps, const pass& k, task& t)
				:operation_base(k, t) {}
			template<class T>
			constexpr auto param_id();
			template<class T>
			bool is_owned();
			template<class T>
			auto get_parameter();
			template<class T>
			auto get_parameter_owned();
			template<class T>
			auto get_parameter(entity e);
			template<class T>
			auto get_parameter_owned(entity e);
			uint32_t get_count() { return slice.count; }
			uint32_t get_index() { return indexInKernel; }
		};
		template<class... params>
		operation(hana::tuple<params...> ps, const pass& k, task& t)->operation<params...>;

		struct pass
		{
			world& ctx;
			int passIndex;
			//int* archetypeIndex;
			archetype** archetypes;
			int* archetypeIndices;
			mask* matched;
			index_t* localType;
			int archetypeCount;
			chunk** chunks;
			int chunkCount;
			index_t* types;
			index_t* readonly;
			index_t* randomAccess;
			int paramCount;
			pass** dependencies;
			int dependencyCount;
		};

		template<class T>
		T* allocate_inplace(char*& buffer, size_t size)
		{
			char* allocated = buffer;
			buffer += size * sizeof(T);
			return (T*)allocated;
		}

		struct dependency_entry
		{
			pass* owned = nullptr;
			std::vector<pass*> shared;
		};

		class pipeline //计算管线，处于两个 sync point 之间
		{
			//std::vector<std::pair<archetype*, int>> archetypeIndices;
			//std::vector<std::vector<task_group*>> ownership;
			stack_allocator kernelStack;
			chunk_vector<pass*> kernels;
			chunk_vector<archetype*> archetypes;
			std::unique_ptr<dependency_entry[]> denpendencyEntries;
			ECS_API void setup_kernel_dependency(pass& k);
			world& ctx;
			int passIndex;
		public:
			ECS_API pipeline(world& ctx);
			ECS_API ~pipeline();
			template<class T>
			pass* create_pass(const filters& v, T paramList);
			ECS_API chunk_vector<task> create_tasks(pass& k, int maxSlice = -1);
		};
}
	/*
	auto params = make_params(param<counter>, param<const material>, param<fuck, access::random_acesss>>);
	auto pass = create_pass(ctx, params);
	chunk_vector<task> tasks = create_tasks(pass, -1);
	auto handle = xxx::parallel_for(tasks, [&pass](task& curr)
	{
		operation o{params, pass, curr}
		auto counters = o.get_parameter<counter>(); // component
		auto mat = o.get_parameter<material>(); // component may shared
		if(!o.is_owned<material>())
			forloop(i, 0, o.get_count())
				set_material(o.get_index()+i, *mat);
		else
			forloop(i, 0, o.get_count())
				set_material(o.get_index()+i, mat[i]);

		forloop(i, 0, o.get_count())
			counters[i]++;
		o.get_parameter<fuck>(e); //random access
	});
	setup_dependencies(handle, pass);
	*/
}
#include "CodebaseImpl.hpp"
#undef def