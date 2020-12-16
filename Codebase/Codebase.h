#pragma once
#include "Database.h"
#include "boost/hana.hpp"
#include "gsl/span"

#ifdef __EMSCRIPTEN__
#include "emscripten.h"
#define DLLEXPORT EMSCRIPTEN_KEEPALIVE
#define CODE_API EMSCRIPTEN_KEEPALIVE
#define DLLLOCAL __attribute__((visibility("hidden")))
#define __stdcall 
#elif defined(__PROSPERO__)
#define DLLEXPORT __declspec(dllexport)
#ifdef CODEBASE_DLL
#define CODE_API __declspec(dllexport)
#else
#define CODE_API __declspec(dllimport)
#endif
#elif defined(__GNUC__)
#define CODE_API __attribute__((visibility("default")))
#define DLLEXPORT __attribute__((visibility("default")))
#define DLLLOCAL __attribute__((visibility("hidden")))
#define __stdcall 
#else
#define DLLEXPORT __declspec(dllexport)
#ifdef CODEBASE_DLL
#define CODE_API __declspec(dllexport)
#else
#define CODE_API __declspec(dllimport)
#endif
#endif

#ifndef FORCEINLINE
#ifdef _MSC_VER
#define FORCEINLINE __forceinline
#else
#define FORCEINLINE inline
#endif
#endif

#define def static constexpr auto
namespace core
{
	namespace codebase
	{
		using core::entity;
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
			return (set[index / bits] & (1 << (index % bits))) != 0;
		}

#define DEFINE_GETTER(Name, Default) \
template<class T, class = void> \
struct get_##Name{ def value = Default; }; \
template<class T> \
struct get_##Name<T, std::void_t<decltype(T::Name)>> { def value = T::Name; }; \
template<class T> \
def get_##Name##_v = get_##Name<T>::value;

		DEFINE_GETTER(manual_clean, false);
		DEFINE_GETTER(manual_copy, false);
		DEFINE_GETTER(buffer_capacity, 1);
		DEFINE_GETTER(entity_refs, gsl::span<intptr_t>{});
		DEFINE_GETTER(vtable, component_vtable{});
#undef	DEFINE_GETTER

		template<template<class...> class TP, class T>
		struct is_template : std::false_type {};

		template<template<class...> class TP, class... T>
		struct is_template<TP, TP<T...>> : std::true_type {};

		template<template<class...> class TP, class... T>
		def is_template_v = is_template<TP, T...>{};

		template<class T, class = void>
		struct is_buffer : std::false_type {};

		template<class T>
		struct is_buffer<T, std::void_t<typename T::value_type>> : is_template<buffer_t, typename T::value_type> {};

		template<class T>
		inline index_t cid;

		template<class T>
		index_t register_component()
		{
			
			component_desc desc;
			desc.isElement = is_buffer<T>{};
			desc.manualClean = get_manual_clean_v<T>;
			desc.manualCopy = get_manual_copy_v<T>;
			desc.size = std::is_empty_v<T> ? 0 : get_buffer_capacity_v<T> *sizeof(T);
			desc.elementSize = std::is_empty_v<T> ? 0 : sizeof(T);
			desc.GUID = T::guid;
			def entityRefs = get_entity_refs_v<T>;
			desc.entityRefs = entityRefs.data();
			desc.entityRefCount = (uint16_t)entityRefs.size();
			desc.alignment = alignof(T);
			desc.name = typeid(T).name();
			desc.vtable = get_vtable_v<T>;
			return cid<T> = register_type(desc);
		}

		template<class ...Ts>
		void register_components()
		{
			std::initializer_list<int> _{ (register_component<Ts>(), 0)... };
		}

		CODE_API void initialize();

		template<class T, bool inRandomAccess = false>
		struct param_t
		{
			using TT = std::remove_const_t<T>;
			def comp_type = hana::type_c<TT>;
			def readonly = std::is_const_v<T>;
			def randomAccess = inRandomAccess;
		};
		template<class T, bool randomAccess = false>
		static constexpr param_t<T, randomAccess> param;

		template<class ...Ts>
		struct complist_t
		{
			operator core::database::typeset() const
			{
				static core::database::index_t list[] = { core::codebase::cid<Ts>... };
				return list;
			}
		};

		template<class ...T>
		static const complist_t<T...> complist;


		namespace detail
		{
			template<class T, class VT>
			struct array_type_ { using type = VT*; };

			template<class T, class VT>
			struct array_type_<T, buffer_t<VT>> { using type = buffer_pointer_t<VT, T::buffer_capacity * sizeof(VT)>; };
		}

		template<class T, class = void>
		struct array_type { using type = T*; };

		template<class T>
		struct array_type<T, std::void_t<typename T::value_type>> { using type = typename detail::array_type_<T, typename T::value_type>::type; };

		template<class T>
		using array_type_t = typename array_type<std::remove_const_t<T>>::type;


		namespace detail
		{
			template<class T>
			struct value_type_ { using type = T*; };

			template<class T>
			struct value_type_<buffer_t<T>> { using type = buffer_t<T>; };
		}

		template<class T, class = void>
		struct value_type { using type = T*; };

		template<class T>
		struct value_type<T, std::void_t<typename T::value_type>> { using type = typename  detail::value_type_<typename T::value_type>::type; };

		template<class T>
		using value_type_t = typename value_type<std::remove_const_t<T>>::type;

		namespace detail
		{
			template<class T>
			using array_ret_t = std::conditional_t<std::is_const_v<T>, std::add_const_t<array_type_t<T>>, array_type_t<T>>;


			template<class T>
			using value_ret_t = std::conditional_t<std::is_const_v<T>, std::add_const_t<value_type_t<T>>, value_type_t<T>>;
		}



		template<class T>
		array_type_t<T> init_component(core::database::world& ctx, core::database::chunk_slice c)
		{
			using namespace core::codebase;
			return (array_type_t<T>)const_cast<void*>(ctx.get_component_ro(c.c, cid<T>)) + (size_t)c.start;
		}
		
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
			CODE_API const entity* get_entities();
		protected:
			operation_base(const pass& k, const task& t)
				:ctx(k), matched(t.matched), slice(t.slice), indexInKernel(t.indexInKernel) {}
			const pass& ctx;
			int matched;
			chunk_slice slice;
			int indexInKernel;
			CODE_API mask get_mask();
			CODE_API bool is_owned(int paramId);
		};
		template<class... params>
		struct operation : operation_base //用于简化api
		{
			static constexpr hana::tuple<params...> paramList;
			operation(hana::tuple<params...> ps, const pass& k, const task& t)
				:operation_base(k, t) {}
			template<class T>
			constexpr auto param_id();
			template<class T>
			bool is_owned();
			template<class T>
			detail::array_ret_t<T> get_parameter();
			template<class T>
			detail::array_ret_t<T> get_parameter_owned();
			template<class T>
			detail::value_ret_t<T> get_parameter(entity e);
			template<class T>
			detail::value_ret_t<T> get_parameter_owned(entity e);
			template<class T>
			bool has_component(entity e);
			uint32_t get_count() { return slice.count; }
			uint32_t get_index() { return indexInKernel; }
		};
		template<class... params>
		operation(hana::tuple<params...> ps, const pass& k, task& t)->operation<params...>;

		struct custom_pass
		{
			world& ctx;
			int passIndex;
			custom_pass** dependencies;
			int dependencyCount;
		};

		struct pass : custom_pass
		{
			archetype** archetypes;
			mask* matched;
			index_t* localType;
			int archetypeCount;
			chunk** chunks;
			int chunkCount;
			index_t* types;
			index_t* readonly;
			index_t* randomAccess;
			int paramCount;
			int entityCount;
			bool hasRandomWrite;
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
			custom_pass* owned = nullptr;
			std::vector<custom_pass*> shared;
		};

		template<class T>
		struct shared_resource
		{
			struct inner
			{
				T value;
				dependency_entry entry;
			};
			std::shared_ptr<inner> ptr;
			T* operator->()
			{
				return &ptr->value;
			}
			const T* operator->() const
			{
				return &ptr->value;
			}
			T& operator*()
			{
				return ptr->value;
			}
			const T& operator*() const
			{
				return ptr->value;
			}
		};

		template<class T, class ...TArgs>
		shared_resource<T> make_resource(TArgs&&... args)
		{
			return { std::make_shared<typename shared_resource<T>::inner>(std::forward<TArgs>(args)...) };
		}

		struct shared_entry
		{
			bool readonly;
			dependency_entry& entry;
		};

		template<class T>
		shared_entry write(shared_resource<T>& target)
		{
			return { false, target.ptr->entry };
		}

		template<class T>
		shared_entry read(shared_resource<T>& target)
		{
			return { true, target.ptr->entry };
		}

		class pipeline //计算管线，Database 的多线程交互封装
		{
		protected:
			//std::vector<std::pair<archetype*, int>> archetypeIndices;
			//std::vector<std::vector<task_group*>> ownership;
			stack_allocator passStack; //TODO: 改成 ring buffer
			std::unordered_map<archetype*, std::unique_ptr<dependency_entry[]>> dependencyEntries;
			CODE_API void setup_pass_dependency(pass& k, gsl::span<shared_entry> sharedEntries = {});
			CODE_API void setup_pass_dependency(custom_pass& k, gsl::span<shared_entry> sharedEntries = {});
			void update_archetype(archetype* at, bool add);
			world& ctx;
			int passIndex;
			void sync_archetype(archetype* at);
			void sync_entry(archetype* at, index_t type);
		public:
			CODE_API pipeline(world& ctx);
			CODE_API ~pipeline();
			template<class T>
			pass* create_pass(const filters& v, T paramList, gsl::span<shared_entry> sharedEntries = {});
			CODE_API custom_pass* create_custom_pass(gsl::span<shared_entry> sharedEntries = {});
			CODE_API chunk_vector<task> create_tasks(pass& k, int maxSlice = -1);
			CODE_API int get_timestamp() { return ctx.timestamp; }
			CODE_API void inc_timestamp() { ++ctx.timestamp; }
			std::function<void(gsl::span<custom_pass*> dependencies)> on_sync;
		};
}
	/*
	auto params = make_params(param<counter>, param<const material>, param<fuck, access::random_acesss>>);
	auto refs = make_refs(write(fishTree));

	auto pass = create_pass(ctx, params, refs);
	chunk_vector<task> tasks = create_tasks(pass, -1);
	auto handle = xxx::parallel_for(tasks, [&pass, &fishTree](task& curr)
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