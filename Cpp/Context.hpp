#pragma once
#include "../MemoryModel/MemoryModel.h"
#include <gsl/span>
#include <boost/hana.hpp>
#include <map>

#define constval static constexpr auto
#define forloop(i, z, n) for(auto i = decltype(n)(z); i<n; ++i)
#define foriter(c, iter) for (auto c = iter.next(); c.has_value(); c = iter.next())
#define x_v(name) template<class T> constexpr auto name##_v = name<T>::value;
#define x_t(name) template<class T> using name##_t = typename name<T>::type;

namespace ecs
{
	template<class T>
	inline uint16_t typeof = 0;

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
		accessor(context* c) :cont(c) {}
		T& operator[](entity);
	};

	template<class T>
	class buffer_accessor
	{
		class context* cont;
	public:
		buffer_accessor(context* c) :cont(c) {}
		buffer<T> operator[](entity);
	};

	template<class T>
	struct is_buffer : std::false_type {};
	template<class T>
	struct is_buffer<buffer<T>> : std::true_type {};

	template<class T>
	struct is_accessor : std::false_type {};
	template<class T>
	struct is_accessor<accessor<T>> : std::true_type {};

	template<class T>
	struct is_buffer_accessor : std::false_type {};
	template<class T>
	struct is_buffer_accessor<buffer_accessor<T>> : std::true_type {};

	x_v(is_buffer);
	x_v(is_accessor);
	x_v(is_buffer_accessor);

	template<class T>
	struct element_type;
	template<class T>
	struct element_type<buffer<T>> { using type = std::remove_const_t<T>; };
	template<class T>
	struct element_type<accessor<T>> { using type = std::remove_const_t<T>; };
	template<class T>
	struct element_type<buffer_accessor<T>> { using type = std::remove_const_t<T>; };

	x_t(element_type);

	template<class ...Cs>
	struct typeset
	{
		consteval size = sizeof...(Cs);
		uint16_t arr[size];
		memory_model::typeset to_raw()
		{
			return { arr, size };
		}
		typeset() : arr{ typeof<Cs>... }
		{
			std::sort(arr, arr + sizeof...(Cs));
		}
	};

	template<class ...Ms>
	struct metaset
	{
		constval size = sizeof...(Ms);
		uint16_t arr[size];
		uint16_t metaarr[size];

		memory_model::metaset to_raw()
		{
			return { arr, metaarr, size };
		}

		metaset(Ms&& ...ms) : arr{ typeof<Ms>... }, metaarr { meta::find(ms)... }
		{
			forloop(i, 0, size)
			{
				uint16_t* min = std::min_element(arr + i, arr + size);
				std::swap(arr[i], *min);
				std::swap(metaarr[i], metaarr[min - arr]);
			}
		}
	};
	template<class ...Ms>
	metaset(Ms&& ...ms)->metaset<Ms...>;

	template<class ...Cs>
	inline static typeset<Cs...> typeset_v;

	template<class T>
	class buffer_array
	{
		char* data;
		uint16_t stride;
	public:
		buffer<T> operator[](size_t i) { return { (memory_model::buffer*)(data + i * stride) }; }
	};

	using entity_type = memory_model::entity_type;
	using entity_filter = memory_model::entity_filter;

	class context
	{
		memory_model::context cont;

		template<class ...Cs, class... MCs>
		void create(gsl::span<entity> es, MCs&& ... mcs)
		{
#define deftype \
			static typeset<Cs..., MCs...> ts; \
			metaset<MCs...> ms{ std::forward<MCs>(mcs)... }; \
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

		void destroy(const entity_filter& g)
		{
			auto iter = cont.query(g);
			foriter(s, iter)
				cont.destroy(*s);
		}

		template<class ...Cs, class... MCs>
		void extend(const entity_filter& g, MCs&& ... mcs)
		{
			deftype;
			auto iter = cont.query(g);
			foriter(s, iter)
				cont.extend(*s, type);
		}

		template<class ...Cs>
		void shrink(const entity_filter& g)
		{
			static typeset<Cs...> ts;
			auto iter = cont.query(g);
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
		int get_array_param(memory_model::chunk* c, tid<int>)
		{
			return c->get_count();
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
		void for_filter(entity_filter& f, F&& action)
		{
			constval parameter_list = lambda_trait<std::decay_t<F>>::parameter_list;
			bool valid = true;
			auto check_parameter = [&](auto arg)
			{
				using raw_parameter = typename decltype(arg)::type;
				using raw_type = std::decay_t<raw_parameter>;
				using tagged_index = memory_model::tagged_index;
				if constexpr (is_buffer_v<raw_parameter>)
				{
					using element = std::decay_t<element_type_t<raw_parameter>>;
					auto type = (tagged_index)typeof<element>;
					if (!type.is_buffer())
						valid = false;
				}
				else if constexpr (is_accessor_v<raw_parameter>)
				{
					using element = std::decay_t<element_type_t<raw_parameter>>;
					auto type = (tagged_index)typeof<element>;
					if (type.is_meta())
						valid = false;
				}
				else if constexpr (is_buffer_accessor_v<raw_parameter>)
				{
					using element = std::decay_t<element_type_t<raw_parameter>>;
					auto type = (tagged_index)typeof<element>;
					if (!type.is_buffer())
						valid = false;
				}
				else if constexpr (std::is_same<raw_parameter, int>)
				{
				}
				else if constexpr (std::is_pointer_v<raw_parameter>)
				{
					auto type = (tagged_index)typeof<raw_type>;
					if (type == 0)
						valid = false;
				}
				else
				{
					auto type = (tagged_index)typeof<raw_type>;
					if (type == 0)
						valid = false;
					if (!type.is_meta())
						valid = false;
				}
			};
			auto get_array = [&](auto arg)
			{
				using raw_parameter = typename decltype(arg)::type;
				return get_array_param(c, tid<raw_parameter>{});
			};
			hana::for_each(parameter_list, check_parameter);
			if (!valid)
				return;
			auto iter = cont.query(f);
			foriter(s, iter)
			{
				memory_model::chunk* c = *s;
				auto arrays = hana::transform(parameter_list, get_array);
				hana::unpack(std::move(arrays), std::forward<F>(action))
			}
		}
	};

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