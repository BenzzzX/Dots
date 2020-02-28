#pragma once
#include "../MemoryModel/MemoryModel.h"
#include <gsl/span>
#include <boost/hana.hpp>
#include <map>
#include <concepts>
#include <assert.h>

#define constval static constexpr auto
#define forloop(i, z, n) for(auto i = decltype(n)(z); i<n; ++i)
#define foriter(c, iter) for (auto c = iter.next(); c.has_value(); c = iter.next())
#define x_v(name) template<class T> constexpr auto name##_v = name<T>::value;
#define x_t(name) template<class T> using name##_t = typename name<T>::type;

namespace ecs
{
	template<class T>
	struct component;
	//{
		//constval is_internal = false;
		//constval is_buffer = false;
		//constval is_meta = false;
		//constval is_tag = false;
		//constval hash = (size_t)-1;
		//constval size = (uint16_t)0;
		//constval elementSize = (uint16_t)0;
		//constval entityRefs[] = { (intptr)0 };
	//};

	template<class T>
	inline memory_model::index_t typeof = 0;

	template<class T>
	struct tid {};

	namespace hana = boost::hana;

	template <class F>
	struct lambda_trait
	{
		using impl = lambda_trait<decltype(&F::operator())>;
		constval return_type = impl::return_type;
		constval parameter_list = impl::parameter_list;
		using parameter_list_t = typename impl::parameter_list_t;
	};

#define lambda_trait_body \
	{ \
		constval return_type = hana::type_c<Ret>; \
		constval parameter_list = hana::tuple_t<Args...>; \
		using parameter_list_t = std::tuple<Args...>; \
	}

	template <class F, class Ret, class... Args>
	struct lambda_trait<Ret(F::*)(Args...)> lambda_trait_body;
	
	template <class F, class Ret, class... Args>
	struct lambda_trait<Ret(F::*)(Args...) const> lambda_trait_body;

	template <class F, class Ret, class... Args>
	struct lambda_trait<Ret(F::*)(Args...) noexcept> lambda_trait_body;

	template <class F, class Ret, class... Args>
	struct lambda_trait<Ret(F::*)(Args...) const noexcept> lambda_trait_body;

	template <class Ret, class... Args>
	struct lambda_trait<Ret(*)(Args...)> lambda_trait_body;

	template <class Ret, class... Args>
	struct lambda_trait<Ret(Args...)> lambda_trait_body;

	template<class F>
	using parameter_list_t = typename lambda_trait<F>::parameter_list_t;




	template<template<class...> class Tmp, class T>
	struct is_template_instance : std::false_type {};
	template<template<class...> class Tmp, class... Ts>
	struct is_template_instance<Tmp, Tmp<Ts...>> : std::true_type {};
	template<template<class...> class Tmp, class T>
	constexpr bool is_template_instance_v = is_template_instance<Tmp, T>::value;

	template<template<class...> class Tmp, class T>
	struct instantiate;
	template<template<class...> class Tmp, template<class...> class Tmp2, class... Ts>
	struct instantiate<Tmp, Tmp2<Ts...>> { using type = Tmp<Ts...>; };
	template<template<class...> class Tmp, class T>
	using instantiate_t = typename instantiate<Tmp, T>::type;

	template<class T>
	struct is_empty_instance : std::false_type {};
	template<template<class...> class Tmp>
	struct is_empty_instance<Tmp<>> : std::true_type {};
	template<class T>
	constexpr bool is_empty_instance_v = is_empty_instance<T>::value;

	template<class T>
	struct car;
	template<template<class...> class Tmp, class T, class... Ts>
	struct car<Tmp<T, Ts...>> { using type = T; };
	template<class T>
	using car_t = typename car<T>::type;

	template<class T>
	struct cdr;
	template<template<class...> class Tmp, class T, class... Ts>
	struct cdr<Tmp<T, Ts...>> { using type = Tmp<Ts...>; };
	template<class T>
	using cdr_t = typename cdr<T>::type;

#ifdef Cpp20
	template<class T>
	concept Component = requires
	{
		typename component<T>;
	};

	template<class T>
	concept AccessableComponent = Component<T> && !component<T>::is_buffer && !component<T>::is_meta;

	template<class T>
	concept DataComponent = Component<T> && !component<T>::is_meta;

	template<class T>
	concept BufferComponent = Component<T> && component<T>::is_buffer;

	template<class T>
	concept MetaComponent = Component<T> && component<T>::is_meta;

	namespace KernelConcepts
	{
		template<class T>
		using remove_pc = std::remove_const_t<std::remove_pointer_t<T>>;

		template<class T>
		using rcc = std::remove_const_t<car_t<T>>;

		template<class T>
		concept ComponentParameter = std::is_pointer_v<T> && AccessableComponent<remove_pc<T>>;

		template<class T>
		concept MetaParameter = std::is_const_v<T> && std::is_pointer_v<T> && MetaComponent<remove_pc<T>>;

		template<class T>
		concept BufferParameter = is_template_instance_v<buffer, T> && BufferComponent<rcc<T>>;

		template<class T>
		concept AccessorParameter = is_template_instance_v<accessor, T> && Component<rcc<T>> && !component<rcc<T>>::is_meta;

		template<class T>
		concept Parameter = ComponentParameter<T> || MetaParameter<T> || BufferParameter<T> || AccessorParameter<T> || std::same_as<int, T> || std::same_as<const entity*>;

		template<class T>
		struct is_parameter_list : std::false_type {};
		template<template<class...> class Tmp, class... Ts>
		struct is_parameter_list<Tmp<Ts...>>
		{
			static constexpr bool value = Parameter<Ts> &&...;
		};

		template<class T>
		concept ParameterList = is_parameter_list<T>::value;
	}

	template<class F>
	concept KernelFunction = KernelConcepts::ParameterList<paramter_list_t<F>>;
#define Require(expr) requires expr
#endif
#define Requires(expr)

	template<class T> Requires(MetaComponent<T>)
	class meta_handle
	{
		uint16_t id;
		bool is_valid()
		{
			return id != -1;
		}
		operator bool()
		{
			return is_valid();
		}
		const T& operator*()
		{
			return meta::get<T>(id);
		}
		const T* operator->()
		{
			return &meta::get<T>(id);
		}
	};

	namespace meta
	{
		namespace detail
		{
			struct data
			{
				size_t refCount;
				const void* value;
				using destructor_t = void(*)(const void*);
				destructor_t destructor;
			};
			template<class T>
			inline std::map<T, size_t> ids;
			inline std::vector<data> values;
			inline size_t freeIndex = 0;
		}

		template<class T>
		static const T& get(size_t id)
		{
			using namespace detail;
			return *(const T*)values[id].value;
		}

		template<class T>
		static size_t find(T&& value)
		{
			using namespace detail;
			auto iter = ids<T>.find(value);
			if (iter == ids<T>.end())
				return -1;
			else
				return iter->second;
		}

		template<class T>
		static size_t map(T&& value)
		{
			using namespace detail;
			size_t index;
			if (freeIndex != 0)
				index = freeIndex;
			else
				index = values.size();

			auto pair = ids<T>.insert({ value, index });
			if (!pair.second)
				return pair.first->second;
			else
			{
				const void* ptr = (const void*)&pair.first->first;
				data d{ 0, ptr, [](const void* ptr) {
					ids<T>.erase(*(const T*)(ptr));
				} };
				if (freeIndex != 0)
				{
					freeIndex = values[index].refCount;
					values[index] = d;
				}
				else
					values.push_back(d);
				return index;
			}
		}

		void release(size_t id)
		{
			using namespace detail;
			values[id].refCount -= 1;
			if (values[id].refCount > 0) return;
			values[id].refCount = freeIndex;
			freeIndex = id;
			values[id].destructor(values[id].value);
		}

		void reference(size_t id)
		{
			using namespace detail;
			values[id].refCount += 1;
		}
	};

	template<class T> Requires(BufferComponent<T>)
	class buffer_array
	{
		char* data;
		uint16_t stride;
	public:
		buffer<T> operator[](size_t i) { return { (memory_model::buffer*)(data + i * stride) }; }
	};

	template<class T> Requires(BufferComponent<T>)
	class buffer
	{
		memory_model::buffer* header;
	public:
		buffer(memory_model::buffer* h) :header(h) {}

		uint16_t size() const { return header->size / sizeof(T); }
		uint16_t capacity() const { return header->size / sizeof(T); }
		void push(const T& value) { header->push(&value, sizeof(T)); }
		T pop() { return *(T*)header->pop(); }
		T* data() { return (T*)header->data(); }
		T& operator[](uint16_t i) { return data()[i]; }
	};

	template<class T> Requires(AccessableComponent<T>)
	class accessor
	{
		class context* cont;
	public:
		accessor(context* c) :cont(c) {}
		T& operator[](entity);
	};

	template<class T> Requires(BufferComponent<T>)
	class buffer_accessor
	{
		class context* cont;
	public:
		buffer_accessor(context* c) :cont(c) {}
		buffer<T> operator[](entity);
	};

	using entity_type = memory_model::entity_type;
	using entity_filter = memory_model::entity_filter;

	class context
	{
		memory_model::context cont;

	public:
		void create(gsl::span<entity> es, entity_type type)
		{
#define parm_slice(es) es.data(), (uint32_t)es.size()
			cont.allocate(type, parm_slice(es));
		}

		void instantiate(gsl::span<entity> es, entity proto)
		{
			cont.instantiate(proto, parm_slice(es));
		}

		void destroy(gsl::span<entity> es)
		{
			auto iter = cont.batch(parm_slice(es));
			foriter(s, iter)
				cont.destroy(*s);
		}

		template<class ...Cs, class... MCs>
		void extend(gsl::span<entity> es, MCs&& ... mcs)
		{
			deftype;
			auto iter = cont.batch(parm_slice(es));
			foriter(s, iter)
				cont.extend(*s, type);
		}

		template<class ...Cs>
		void shrink(gsl::span<entity> es)
		{
			static typeset<Cs...> ts;
			auto iter = cont.batch(parm_slice(es));
			foriter(s, iter)
				cont.shrink(*s, ts);
		}

		void destroy(const entity_filter& g)
		{
			auto iter = cont.query(g);
			foriter(s, iter)
				cont.destroy(*s);
		}

		void extend(const entity_filter& g, entity_type type)
		{
			auto iter = cont.query(g);
			foriter(s, iter)
				cont.extend(*s, type);
		}

		void shrink(const entity_filter& g, memory_model::typeset ts)
		{
			auto iter = cont.query(g);
			foriter(s, iter)
				cont.shrink(*s, ts);
		}

		template<class T> Requires(AccessableComponent<T>)
		const T& read_component(entity e)
		{
			return *(const T*)cont.get_component_ro(entity, typeof<T>);
		}

		template<class T> Requires(BufferComponent<T>)
		buffer<const T> read_buffer(entity e)
		{
			return { (memory_model::buffer*)cont.get_component_ro(entity, typeof<T>) };
		}

		template<class T> Requires(MetaComponent<T>)
		const T& read_meta(entity e)
		{
			return meta::get<T>(cont.get_metatype(e, typeof<T>));
		}

		template<class T> Requires(AccessableComponent<T>)
		T& write_component(entity e)
		{
			return *(T*)cont.get_component_rw(entity, typeof<T>);
		}

		template<class T> Requires(BufferComponent<T>)
		buffer<T> write_buffer(entity e)
		{
			return { (memory_model::buffer*)cont.get_component_rw(entity, typeof<T>) };
		}

		bool exist(entity e)
		{
			return cont.exist(e);
		}

		template<class T> Requires(Component<T>)
		bool has(entity e) 
		{
			return cont.has_component(e, typeof<T>);
		}

	private:
		int get_array_param(memory_model::chunk* c, tid<int>)
		{
			return c->get_count();
		}

		template<class T> 
		const T* get_array_param(memory_model::chunk* c, tid<const T*>)
		{
			if constexpr (component<T>::is_meta)
			{
				uint16_t id = cont.get_metatype(c, typeof<T>);
				if (id == -1)
					return  nullptr;
				return meta::get<T>(id);
			}
			else
				return (const T*)cont.get_array_ro(c, typeof<T>);
		}

		template<class T> 
		T* get_array_param(memory_model::chunk* c, tid<T*>)
		{
			return (T*)cont.get_array_rw(c, typeof<T>);
		}

		template<class T> 
		buffer_array<T> get_array_param(memory_model::chunk* c, tid<buffer_array<T>>)
		{
			return { cont.get_array_rw(c, typeof<T>), cont.get_element_size(c, typeof<T>) };
		}

		template<class T>
		buffer_array<const T> get_array_param(memory_model::chunk* c, tid<buffer_array<const T>>)
		{
			return { cont.get_array_ro(c, typeof<T>), cont.get_element_size(c, typeof<T>) };
		}

		template<class T>
		accessor<T> get_array_param(memory_model::chunk* c, tid<accessor<T>>)
		{
			return { &cont };
		}

		template<class T>
		accessor<const T> get_array_param(memory_model::chunk* c, tid<accessor<const T>>)
		{
			return { &cont };
		}

		const entity* get_array_param(memory_model::chunk* c, tid<const entity*>)
		{
			return { cont.get_entities(c) };
		}

		template<class T>
		const T get_param_type(tid<const T&>);

		template<class T>
		T get_param_type(tid<buffer<T>>);

		template<class T>
		const T get_param_type(tid<buffer<const T>>);

		template<class T>
		T get_param_type(tid<accessor<T>>);

		template<class T>
		const T get_param_type(tid<accessor<const T>>);

		template<class T>
		T get_param_type(tid<buffer_accessor<T>>);

		template<class T>
		const T get_param_type(tid<buffer_accessor<const T>>);

	public:
		template<class F> Requires(KernelFunction<F>)
		void for_each_chunk(entity_filter& f, F&& action)
		{
			constval parameter_list = lambda_trait<std::decay_t<F>>::parameter_list;
			auto get_array = [&](auto arg)
			{
				using raw_parameter = typename decltype(arg)::type;
				return get_array_param(c, tid<raw_parameter>{});
			};
			auto iter = cont.query(f);
			foriter(s, iter)
			{
				memory_model::chunk* c = *s;
				auto arrays = hana::transform(parameter_list, get_array);
				hana::unpack(std::move(arrays), std::forward<F>(action))
			}
		}
	};

	template<class... Cs, class... Ms> Requires((DataComponent<Cs>&&...) && (MetaComponent<Ms>&&...))
	auto make_entity_type(Ms&&... metas)
	{
		struct type_t
		{
			memory_model::index_t arr[sizeof...(Cs) + sizeof...(Ms)];
			memory_model::index_t metaarr[sizeof...(Ms)];
			entity_type get()
			{
				memory_model::typeset ts{ arr, sizeof...(Cs) + sizeof...(Ms) };
				memory_model::metaset ms{ {metaarr, sizeof...(Ms)}, arr + sizeof(Cs) };
				return { ts, ms };
			}
		};
		type_t type{ {typeof<Cs>..., typeof<Ms>... },{meta::map(std::forward<Ms>(metas)...} };
		memory_model::index_t* marr = type.arr + sizeof...(Cs);
		forloop(i, 0, sizeof...(Ms))
		{
			uint16_t* min = std::min_element(marr + i, marr + sizeof...(Ms));
			std::swap(marr[i], *min);
			std::swap(type.metaarr[i], type.metaarr[min - marr]);
		}
		std::sort(type.arr, type.arr + sizeof...(Cs));
		memory_model::index_t* dup = std::adjacent_find(type.arr, type.arr + sizeof...(Cs) + sizeof...(Ms));
		assert(dup == type.arr + sizeof...(Cs) + sizeof...(Ms));
		return type;
	}

	template<class... Cs, class... Ms> Requires((Component<Cs>&&...) && (MetaComponent<Ms>&&...))
	auto make_filter_type(Ms&&... metas)
	{
		struct type_t
		{
			memory_model::index_t arr[sizeof...(Cs) + sizeof...(Ms)];
			memory_model::index_t metas[sizeof...(Ms)];
			memory_model::index_t metaarr[sizeof...(Ms)];
			entity_type get()
			{
				memory_model::typeset ts{ arr, sizeof...(Cs) + sizeof...(Ms) };
				memory_model::metaset ms{ {metaarr, sizeof...(Ms)}, metas };
				return { ts, ms };
			}
		};
		type_t type{ {typeof<Cs>..., typeof<Ms>...}, {typeof<Ms>...},{meta::find(std::forward<Ms>(metas)...} };
		forloop(i, 0, sizeof...(Ms))
		{
			uint16_t* min = std::min_element(type.metas + i, type.metas + sizeof...(Ms));
			std::swap(type.metas[i], *min);
			std::swap(type.metaarr[i], type.metaarr[min - type.metas]);
		}
		std::sort(type.arr, type.arr + sizeof...(Cs) + sizeof...(Ms));
		memory_model::index_t* dup = std::adjacent_find(type.arr, type.arr + sizeof...(Cs) + sizeof...(Ms));
		assert(dup == type.arr + sizeof...(Cs) + sizeof...(Ms));
		return type;
	}

	struct Kernel
	{
		entity_filter filter;
		virtual void Run(context*) = 0;
	};

	template<class F>
	struct KernelImpl : Kernel
	{
		F f;
		template<class T>
		KernelImpl(T&& t) : f(std::forward<T>(t)) {}
		void Run(context* cont)
		{
			cont->for_each_chunk(filter, f);
		}
	};

	template<class F> Requires(KernelFunction<std::decay_t<F>>)
	Kernel* for_each_chunk(F&& action)
	{
		return new KernelImpl<std::decay_t<F>>{std::forward<F>(action)};
	}

	template<class T>
	T& accessor<T>::operator[](entity e)
	{
		if constexpr (std::is_const_v<T>)
			return cont->read_component<T>(e);
		else
			return cont->write_component<T>(e);
	}

	template<class T>
	buffer<T> buffer_accessor<T>::operator[](entity e)
	{
		if constexpr (std::is_const_v<T>)
			return cont->read_buffer<T>(e);
		else
			return cont->write_buffer<T>(e);
	}
}