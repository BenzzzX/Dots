#pragma once
#include "Database.h"
#include "boost/hana.hpp"
#include "gsl/span"

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
			def managed = !std::is_pod_v<T>;
			if constexpr(managed)
			{
				desc.isManaged = true;
				if (std::is_constructible_v<T>)
					desc.vtable.constructor = +[](char* data, size_t count) {
						for (int i = 0; i < count; ++i)
							new(((T*)data) + i) T();
					};
				if (std::is_destructible_v<T>)
					desc.vtable.destructor = +[](char* data, size_t count) {
						for (int i = 0; i < count; ++i)
							((T*)data)[i].~T();
					};
				if (std::is_copy_assignable_v<T>)
					desc.vtable.destructor = +[](char* dst, const char* src, size_t count)
					{
						for (int i = 0; i < count; ++i)
							((T*)dst)[i] = ((const T*)src)[i];
					};
			}
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
		void declare_components()
		{
			std::initializer_list<int> _{ (register_component<Ts>(), 0)... };
		}

#ifdef ENABLE_GUID_COMPONENT
		ECS_API void initialize(core::GUID(*new_guid_func)());
#else
		ECS_API void initialize();
#endif

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

		template<class... Ts>
		std::tuple<array_type_t<Ts>...> init_components(core::database::world& ctx, core::database::chunk_slice c)
		{
			return std::make_tuple(init_component<Ts>(ctx, c)...);
		}

		struct filters
		{
			archetype_filter archetypeFilter;
			chunk_filter chunkFilter;
			entity_filter entityFilter;
			ECS_API int get_size() const;
			ECS_API filters clone(char*& buffer) const;
		};

		struct task
		{
			int gid;
			int indexInKernel;
			chunk_slice slice;
		};

		struct task_group
		{
			int begin;
			int end;
		};
		struct pass;

		template<class P, class... params>
		struct operation //用于简化api
		{
			static constexpr hana::tuple<params...> paramList;
			static constexpr auto compList = hana::transform(paramList, [](const auto p) { return p.comp_type; });
			operation(hana::tuple<params...> ps, const P& k, const task& t)
			:ctx(k), gid(t.gid), slice(t.slice), indexInKernel(t.indexInKernel){}
			template<class T>
			constexpr auto param_id();
			template<class T>
			bool is_owned()
			{
				def paramId = param_id<T>();
				return is_owned(paramId.value);
			}
			template<class T>
			detail::array_ret_t<T> get_parameter();
			template<class... Ts>
			std::tuple<detail::array_ret_t<Ts>...> get_parameters();
			template<class T>
			detail::array_ret_t<T> get_parameter_owned();
			template<class... Ts>
			std::tuple<detail::array_ret_t<Ts>...> get_parameters_owned();
			template<class T>
			detail::value_ret_t<T> get_parameter(entity e);
			template<class... Ts>
			std::tuple<detail::array_ret_t<Ts>...> get_parameters(entity e);
			template<class T>
			detail::value_ret_t<T> get_parameter_owned(entity e);
			template<class... Ts>
			std::tuple<detail::array_ret_t<Ts>...> get_parameters_owned(entity e);
			mask get_mask() { return ctx.matched[gid]; }
			bool is_owned(int paramId)
			{
				constexpr uint16_t InvalidIndex = (uint16_t)-1;
				return ctx.localType[gid * ctx.paramCount + paramId] != InvalidIndex;
			}
			template<class T>
			bool has_component(entity e) { return ctx.ctx.has_component(e, complist<T>); }
			const entity* get_entities() { return ctx.ctx.get_entities(slice.c) + slice.start; }
			const P& ctx;
			int gid;
			chunk_slice slice;
			int indexInKernel;
			uint32_t get_count() { return slice.count; }
			uint32_t get_index() { return indexInKernel; }
		};
		template<class P, class... params>
		operation(hana::tuple<params...> ps, const P& k, task& t)->operation<P, params...>;

		struct custom_pass
		{
			class pipeline& ctx;
			int passIndex;
			std::weak_ptr<custom_pass>* dependencies;
			int dependencyCount;
			ECS_API void release_dependencies();
			ECS_API ~custom_pass();
		};

		template<class base = custom_pass>
		struct pass_t : base
		{
			archetype** archetypes;
			mask* matched;
			index_t* localType;
			int archetypeCount;
			index_t* types;
			index_t* readonly;
			index_t* randomAccess;
			int paramCount;
			bool hasRandomWrite;
			filters filter;
			uint32_t calc_size() const;
		};

		struct pass : pass_t<> {};

		template<class T>
		T* allocate_inplace(char*& buffer, size_t size)
		{
			char* allocated = buffer;
			buffer += size * sizeof(T);
			return (T*)allocated;
		}

		struct dependency_entry
		{
			std::weak_ptr<custom_pass> owned;
			std::vector<std::weak_ptr<custom_pass>> shared;
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

		class pipeline : protected world //计算管线，Database 的多线程交互封装
		{
		protected:
			//std::vector<std::pair<archetype*, int>> archetypeIndices;
			//std::vector<std::vector<task_group*>> ownership;
			std::unordered_map<archetype*, std::unique_ptr<dependency_entry[]>> dependencyEntries;
			template<class P>
			void setup_pass_dependency(std::shared_ptr<P>& k, gsl::span<shared_entry> sharedEntries = {});
			template<class P>
			void setup_custom_pass_dependency(std::shared_ptr<P>& k, gsl::span<shared_entry> sharedEntries = {});
			void update_archetype(archetype* at, bool add);
			int passIndex;
			virtual void sync_dependencies(gsl::span<std::weak_ptr<custom_pass>> dependencies) const {}
			friend class world;
		public:
			ECS_API pipeline(world&& ctx);
			ECS_API ~pipeline();
			ECS_API world release();
			ECS_API void sync_archetype(archetype* at) const;
			ECS_API void sync_entry(archetype* at, index_t type) const;

			virtual void sync_all() const {}
			ECS_API void sync_all_ro() const;

			template<class P = pass, class T>
			std::shared_ptr<P> create_pass(const filters& v, T paramList, gsl::span<shared_entry> sharedEntries = {});
			template<class P = custom_pass>
			std::shared_ptr<P> create_custom_pass(gsl::span<shared_entry> sharedEntries = {});
			template<class P = pass>
			std::pair<chunk_vector<task>, chunk_vector<task_group>> create_tasks(P& k, int batchCount);

#define forloop(i, z, n) for(auto i = std::decay_t<decltype(n)>(z); i<(n); ++i)
			/*** per chunk slice ***/
			//create
			ECS_API chunk_vector<chunk_slice> allocate(const entity_type& type, uint32_t count = 1);
			ECS_API chunk_vector<chunk_slice> allocate(archetype* g, uint32_t count = 1);
			ECS_API chunk_vector<chunk_slice> instantiate(entity src, uint32_t count = 1);

			//stuctural change
			ECS_API void destroy(chunk_slice s);
			/* note: return null if trigger chunk move or chunk clean up */
			ECS_API chunk_vector<chunk_slice> cast(chunk_slice s, type_diff diff);
			ECS_API chunk_vector<chunk_slice> cast(chunk_slice s, const entity_type& type);

			//archetype behavior, lifetime
			using world::find_archetype;
			using world::get_archetype;
			using world::get_cleaning;
			using world::is_cleaned;
			using world::get_casted;
			ECS_API chunk_vector<chunk_slice> cast(chunk_slice s, archetype* g);

			//query iterators
			using world::batch;
			using world::query;
			ECS_API chunk_vector<chunk*> query(archetype* g, const chunk_filter& filter = {});
			using world::get_archetypes;


			/*** per entity ***/
			//query
			ECS_API const void* get_component_ro(entity e, index_t type) const noexcept;
			ECS_API const void* get_owned_ro(entity e, index_t type) const noexcept;
			ECS_API const void* get_shared_ro(entity e, index_t type) const noexcept;
			using world::is_a;
			using world::share_component;
			using world::has_component;
			using world::own_component;
			ECS_API bool is_component_enabled(entity e, const typeset& type) const noexcept;
			using world::exist;
			//update
			ECS_API void* get_owned_rw(entity e, index_t type) const noexcept;
			ECS_API void enable_component(entity e, const typeset& type) const noexcept;
			ECS_API void disable_component(entity e, const typeset& type) const noexcept;
			using world::get_type; /* note: only owned */
			//entity/group serialize
			ECS_API chunk_vector<entity> gather_reference(entity e);
			ECS_API void serialize(serializer_i* s, entity e);
			ECS_API chunk_slice deserialize_single(serializer_i* s, patcher_i* patcher);
			ECS_API entity deserialize(serializer_i* s, patcher_i* patcher);

			/*** per chunk or archetype ***/
			//query
			ECS_API const void* get_component_ro(chunk* c, index_t t) const noexcept;
			ECS_API const void* get_owned_ro(chunk* c, index_t t) const noexcept;
			ECS_API const void* get_shared_ro(chunk* c, index_t t) const noexcept;
			ECS_API void* get_owned_rw(chunk* c, index_t t) noexcept;
			using world::get_entities;
			using world::get_size;
			ECS_API const void* get_shared_ro(archetype* g, index_t type) const;

			/*** per world ***/
			ECS_API void move_context(world& src);
			ECS_API void patch_chunk(chunk* c, patcher_i* patcher);
			//serialize
			ECS_API void serialize(serializer_i* s);
			ECS_API void deserialize(serializer_i* s);
			//clear
			using world::gc_meta;
			ECS_API void merge_chunks();
			//query
			using world::get_timestamp;
			using world::inc_timestamp;
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