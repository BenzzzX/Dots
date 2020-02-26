#pragma once
#include "../DenseEcsCore/MemoryModel.h"
#include <gsl/span>
#include <boost/hana.hpp>
#include <map>

#define constval static constexpr auto
#define forloop(i, z, n) for(auto i = decltype(n)(z); i<n; ++i)
#define foriter(c, iter) for (auto c = iter.next(); c.has_value(); c = iter.next())

namespace ecs
{
	template<class T>
	inline uint16_t typeof;

	template<class T>
	struct tid {};

	namespace hana = boost::hana;

	template <class F>
	struct lambda_trait
	{
		using impl = lambda_trait<decltype(&F::operator())>;
		constval return_type = impl::return_type;
		constval parameter_list = impl::parameter_list;
	};

	template <class F, class Ret, class... Args>
	struct lambda_trait<Ret(F::*)(Args...)>
	{
		constval return_type = hana::type_c<Ret>;
		constval parameter_list = hana::tuple_t<Args...>;
	};

	template <class F, class Ret, class... Args>
	struct lambda_trait<Ret(F::*)(Args...) const>
	{
		constval return_type = hana::type_c<Ret>;
		constval parameter_list = hana::tuple_t<Args...>;
	};

	template <class F, class Ret, class... Args>
	struct lambda_trait<Ret(F::*)(Args...) noexcept>
	{
		constval return_type = hana::type_c<Ret>;
		constval parameter_list = hana::tuple_t<Args...>;
	};

	template <class F, class Ret, class... Args>
	struct lambda_trait<Ret(F::*)(Args...) const noexcept>
	{
		constval return_type = hana::type_c<Ret>;
		constval parameter_list = hana::tuple_t<Args...>;
	};

	template <class Ret, class... Args>
	struct lambda_trait<Ret(*)(Args...)>
	{
		constval return_type = hana::type_c<Ret>;
		constval parameter_list = hana::tuple_t<Args...>;
	};

	template <class Ret, class... Args>
	struct lambda_trait<Ret(Args...)>
	{
		constval return_type = hana::type_c<Ret>;
		constval parameter_list = hana::tuple_t<Args...>;
	};

	template<class T>
	inline static std::unordered_map<T, uint16_t> ids;
	inline static std::vector<void*> values;

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

	template<class T>
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

	template<class T>
	class accessor
	{
		class context* cont;
	public:
		accessor(constext* c) :cont(c) {}
		decltype(auto) operator[](entity);
	};

	template<class ...Cs>
	struct typeset_builder
	{
		uint16_t arr[sizeof...(Cs)];
		typeset() : arr{ typeof<Cs>... }
		{
			std::sort(arr, arr + sizeof...(Cs));
		}
	};

	template<class ...Cs>
	struct metaset_builder
	{
		constval size = sizeof...(Cs);
		uint16_t metaarr[size];
		metaset(Cs&& ...ms) : metaarr{ meta::find(ms)... }
		{
			uint16_t arr[] = { typeof<Cs>... };
			forloop(i, 0, size)
			{
				uint16_t* min = std::min_element(arr + i, arr + size);
				std::swap(arr[i], *min);
				std::swap(metaarr[i], metaarr[min - arr]);
			}
		}
	};

	template<class T>
	class buffer_array
	{
		char* data;
		uint16_t stride;
	public:
		buffer<T> operator[](size_t i) { return { (memory_model::buffer*)(data + i * stride) }; }
	};

	class filter
	{
		struct type
		{
			uint16_t* c;
			std::unique_ptr<uint16_t> mc;
			uint16_t nc, nmc;
		};
		type all;
		type any;
		type none;

		uint16_t* changed;
		uint16_t nchanged;
		bool includeDisabled;
		size_t prevVersion;
	public:

		template<class ...Cs, class... MCs>
		void match_all(MCs&& ... mcs)
		{
			static typeset_builder<Cs..., MCs...> ts;
			all.c = ts.arr;
			if constexpr (sizeof...(MCs) > 0)
				all.mc = new metaset_builder<MCs...>{ std::forward<MCs>(mcs)... }->metaarr;
			else
				all.mc = nullptr;
			all.nc = sizeof...(Cs);
			all.nmc = sizeof...(MCs);
		}

		void match_disabled(bool b)
		{
			includeDisabled = b;
		}

		template<class ...Cs, class... MCs>
		void match_any(MCs&& ... mcs)
		{
			static typeset_builder<Cs..., MCs...> ts;
			any.c = ts.arr;
			if constexpr (sizeof...(MCs) > 0)
				any.mc = new metaset_builder<MCs...>{ std::forward<MCs>(mcs)... }->metaarr;
			else
				any.mc = nullptr;
			any.nc = sizeof...(Cs);
			any.nmc = sizeof...(MCs);
		}

		template<class ...Cs, class... MCs>
		void match_none(MCs&& ... mcs)
		{
			static typeset_builder<Cs..., MCs...> ts;
			none.c = ts.arr;
			if constexpr (sizeof...(MCs) > 0)
				none.mc = new metaset_builder<MCs...>{ std::forward<MCs>(mcs)... }->metaarr;
			else
				none.mc = nullptr;
			none.nc = sizeof...(Cs);
			none.nmc = sizeof...(MCs);
		}

		template<class ...Cs>
		void match_changed(size_t version)
		{
			static typeset_builder<Cs...> ts;
			changed = ts.arr;
			nchanged = sizeof...(Cs);
			prevVersion = version;
		}

		const memory_model::entity_filter to_raw() const
		{
			using namespace memory_model;
			typeset allt{ all.c,all.nc };
			typeset anyt{ any.c,any.nc };
			typeset nonet{ none.c,none.nc };
			metaset allMetat{ all.mc.get() ,all.nmc, all.c + all.nc };
			metaset anyMetat{ any.mc.get(), any.nmc, any.c + any.nc };
			metaset noneMetat{ none.mc.get(), none.nmc, none.c + none.nc };
			typeset changedt{ changed,nchanged };
			return
			{
				allt,
				anyt,
				nonet,
				allMetat,
				anyMetat,
				noneMetat,
				changedt,
				prevVersion,
				includeDisabled
			};
		}
	};

	class context
	{
		memory_model::context cont;

		template<class ...Cs, class... MCs>
		void create(gsl::span<entity> es, MCs&& ... mcs)
		{
#define deftype \
			static typeset_builder<Cs..., MCs...> ts; \
			metaset_builder<MCs...> ms{ std::forward<MCs>(mcs)... }; \
			memory_model::entity_type type{ {ts.arr, sizeof...(Cs)}, {ms.metaarr, sizeof...(MCs), ts.arr + sizeof...(Cs)} }

#define parm_slice(es) es.data(), (uint32_t)es.size()


			deftype;
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

		void destroy(const filter& g)
		{
			auto iter = cont.query(g.to_raw());
			foriter(s, iter)
				cont.destroy(*s);
		}

		template<class ...Cs, class... MCs>
		void extend(const filter& g, MCs&& ... mcs)
		{
			deftype;
			auto iter = cont.query(g.to_raw());
			foriter(s, iter)
				cont.extend(*s, type);
		}

		template<class ...Cs>
		void shrink(const filter& g)
		{
			static typeset_builder<Cs...> ts;
			auto iter = cont.query(g.to_raw());
			foriter(s, iter)
				cont.shrink(*s, ts);
		}

#undef deftype

		template<class T>
		buffer<const T> read_buffer(entity e)
		{
			return { (memory_model::buffer*)cont.get_component_ro(entity, typeof<T>) };
		}

		template<class T>
		const T& read_component(entity e)
		{
			return *(const T*)cont.get_component_ro(entity, typeof<T>);
		}

		template<class T>
		const T& read_meta(entity e)
		{
			return meta::get<T>(cont.get_metatype(e, typeof<T>));
		}

		template<class T>
		T& write_component(entity e)
		{
			return *(T*)cont.get_component_rw(entity, typeof<T>);
		}

		template<class T>
		buffer<T> write_buffer(entity e)
		{
			return { (memory_model::buffer*)cont.get_component_rw(entity, typeof<T>) };
		}

		bool exist(entity e)
		{
			return cont.exist(e);
		}

		template<class T>
		bool has(entity e)
		{
			return cont.has_component(e, typeof<T>);
		}

		template<class T>
		const T& get_array_param(memory_model::chunk* c, tid<const T&>)
		{
			return meta::get<T>(cont.get_metatype(c, typeof<T>));
		}

		template<class T>
		const T* get_array_param(memory_model::chunk* c, tid<const T*>)
		{
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

		template<class F, class ...Ts>
		void for_filter(filter& f, F&& action, Ts&... args)
		{
			constval parameter_list = lambda_trait<std::decay_t<F>>::parameter_list;
			auto iter = cont.query(f.to_raw());
			foriter(s, iter)
			{
				memory_model::chunk* c = *s;
				auto arrays = hana::transform(parameter_list, [&](auto arg)
					{
						using raw_parameter = typename decltype(arg)::type;
						return get_array_param(c, tid<raw_parameter>{});
					});
				hana::unpack(hana::make_tuple(args...) | std::move(arrays), std::forward<F>(action));
			}
		}
	};
}