#pragma once
#include "taskflow/taskflow.hpp"
#include "Context.hpp"

namespace ecs
{
	struct var_pool
	{
		struct var_value
		{
			size_t offset;
			using destructor_t = void(*)(char*);
			std::function<void(char*)> constructor;
			destructor_t destructor;
		};

		size_t totalSize = 0;
		char* data = nullptr;

		std::deque<var_value> vars;

		void realize(size_t id)
		{
			auto& var = vars[id];
			var.constructor(data + var.offset);
		}

		void finalize(size_t id)
		{
			auto& var = vars[id];
			var.destructor(data + var.offset);
		}

		template<typename T>
		T* get(size_t id)
		{
			return reinterpret_cast<T*>(data + vars[id].offset);
		}

		void prepare(size_t id)
		{
			auto& var = vars[id];
			size_t size = var.offset;
			var.offset = totalSize;
			totalSize += size;
		}

		void prepare_pool()
		{
			data = (char*)malloc(totalSize);
		}

		template<class T, typename... Ts>
		size_t allocate(Ts&& ... args)
		{
			vars.emplace_back(sizeof(T), [=](char* b)
				{
					new(b) T{ args... };
				},
				[](char* b)
				{
					T* ptr = reinterpret_cast<T*>(b);
					delete ptr;
				});
			return vars.size() - 1;
		}

		void reset()
		{
			if (data != nullptr)
				free(data);
			data = nullptr;
			vars.clear();
			totalSize = 0;
		}

		~var_pool()
		{
			reset();
		}
	};

	struct logic_var;

	struct pass
	{
		std::vector<size_t> reads;
		std::vector<size_t> writes;
		std::vector<size_t> dependency;

		bool isParallel;

		tf::Task task;
	};

	struct logic_var
	{
		//slice data accessor
		//array access or random access
		//implict created
		struct component_var
		{
			size_t mm;
			uint16_t type;
		};

		//transient resource
		//explict created
		struct transient_var
		{
			size_t size;
			using destructor_t = void(*)(char*);
			std::function<void(char*)> constructor;
			destructor_t destructor;
		};

		//external resource
		//explict created
		struct imported_var
		{
			void* ref;
		};

		std::variant<
			component_var,
			transient_var,
			imported_var
		> data;

		template<class T>
		bool is() { return std::holds_alternative<T>(data); }

		template<class T>
		T& as() { return std::get<T>(data); }
	};

	template<class T>
	struct var
	{
		size_t id;
	};

	struct execute_job
	{
		tf::Task task;
		std::vector<size_t> dependents;
	};

	struct execute_context
	{
		std::deque<logic_var> vars;
		std::deque<execute_job> jobs;
		char* buffer;
		tf::FlowBuilder flow;
	};

	struct par_t {} par;
	struct seq_t {} seq;

	template<class... Fs> struct overload : Fs... { using Fs::operator()...; };
	template<class... Fs> overload(Fs...)->overload<Fs...>;

	class graph_builder
	{
		//output
		std::deque<pass> passes;
		std::unique_ptr<execute_context> exe;

		//transient_var
		template<class T, class... Ts>
		var<T> create_var(Ts&& ... args)
		{
			vars.emplace_back(logic_var::transient_var{
				sizeof(T),
				[=](char* data)
				{
					new(data) T{args...};
				},
				[](char* data)
				{
					delete reinterpret_cast<T*>(data);
				}
				});
			return vars.size() - 1;
		}

		//context_var and imported_var
		template<class T>
		var<T> import_var(T& t)
		{
			vars.emplace_back(logic_var::imported_var{ &t });
			return vars.size() - 1;
		}

		template<class T>
		static T& realize(execute_context* exe, var<T> v)
		{
			return *std::visit(overload{
				[&](logic_var::imported_var& v) { return reinterpret_cast<T*>(v.ref); },
				[&](logic_var::transient_var& v) { return reinterpret_cast<T*>(exe->buffer + v.size); }
				}, exe->vars[v.id].data);
		}

		//component_var
		template<class F, class... Rs>
		size_t chunk_pass(par_t, var<context> c, filter& f, F&& action, Rs ... vars)
		{
			auto task = exe->flow.silent_emplace([exe = exe.get(), f, =](tf::SubflowBuilder& subflow)
				{
						context& cont = realize(exe, c);
						cont.for_filter_par(subflow, f, std::forward<F>(action), realize(vars)...);
				});
		}

		template<class F, class... Rs>
		size_t chunk_pass(seq_t, var<context> c, filter& f, F&& action, Rs ... vars)
		{

			auto task = exe->flow.silent_emplace([exe = exe.get(), f, =]
				{
					context& cont = realize(exe, c);
					cont.for_filter(f, std::forward<F>(action));
				});
		}

		template<class R, class F, class... Rs>
		size_t for_pass(par_t, var<R> r, F&& action, Rs ... vars)
		{
			auto task = exe->flow.silent_emplace([=](tf::SubflowBuilder& subflow)
				{
					R& r = realize(exe, r);
					subflow.parallel_for(r.begin(), r.end(), [action, chunks, =](memory_model::chunk* c)
						{
						}
				});
		}

		template<class R, class F, class... Rs>
		size_t for_pass(seq_t, var<R> r, F&& action, Rs ... vars)
		{

		}

		template<class R, class F, class... Rs>
		size_t data_pass(F&& action, Rs ... vars)
		{

		}

		//ensure dep >= pass
		void add_dependency(size_t pass, gsl::span<size_t> deps)
		{

		}
	};

	class execute_compiler
	{
		//input
		std::deque<pass> passes;

		//output
		std::unique_ptr<execute_context> exe;

		//transient
		std::vector<size_t> refCount;
		std::vector<bool> constructed;
		std::vector<std::vector<size_t>> readerFence;
		std::vector<size_t> writerFence;
		size_t bufferSize;

		void compile()
		{
			bufferSize = 0;
			refCount.resize(exe->vars.size());
			readerFence.resize(exe->vars.size());
			writerFence.resize(exe->vars.size());
			constructed.resize(exe->vars.size());
			memset(writerFence.data(), -1, exe->vars.size() * sizeof(size_t));
			exe->jobs.resize(exe->vars.size());

			//collect ref
			for (auto& pass : passes)
			{
				for (auto& r : pass.reads)
					refCount[r]++;
				for (auto& r : pass.writes)
					refCount[r]++;
			}

			//calculate buffer
			for (size_t r = 0; r < exe->vars.size(); ++r)
			{
				if (exe->vars[r].is<logic_var::transient_var>() && refCount[r] > 0)
				{
					auto& var = exe->vars[r].as<logic_var::transient_var>();
					size_t size = var.size;
					var.size = bufferSize;
					bufferSize += size;
				}
				else
				{
					//TODO: Unused variable
				}
			}

			exe->buffer = (char*)malloc(bufferSize);

			size_t i = 0;

			for (auto& pass : passes)
			{
				exe->jobs[i].task = std::move(pass.task);
				++i;
			}

			i = 0;
			for (auto& pass : passes)
			{
				auto writeVar = [&](size_t i, size_t r)
				{
					if (writerFence[r] != -1)
						exe->jobs[i].dependents.push_back(writerFence[r]);
					for (auto& d : readerFence[r])
						exe->jobs[i].dependents.push_back(d);
					readerFence[r].clear();
					writerFence[r] = i;
				};

				auto constructVar = [&](size_t r)
				{
					if (exe->vars[r].is<logic_var::transient_var>() && !constructed[r])
					{
						exe->jobs.emplace_back();
						writeVar(exe->jobs.size() - 1, r);

						auto& var = exe->vars[r].as<logic_var::transient_var>();
						exe->jobs.back().task = exe->flow.silent_emplace([data = exe->buffer + var.size, constructor = std::move(var.constructor)]()
						{
							constructor(data);
						});
						constructed[r] = true;
					}
				};

				auto destructVar = [&](size_t r)
				{
					if (exe->vars[r].is<logic_var::transient_var>() && --refCount[r] == 0 && constructed[r])
					{
						exe->jobs.emplace_back();
						writeVar(exe->jobs.size() - 1, r);

						auto& var = exe->vars[r].as<logic_var::transient_var>();
						exe->jobs.back().task = exe->flow.silent_emplace([data = exe->buffer + var.size, destructor = std::move(var.destructor)](tf::FlowBuilder& flow)
						{
							destructor(data);
						});
					}
				};

				//(1. check and apply construct
				for (auto& r : pass.reads)
					constructVar(r);
				for (auto& r : pass.writes)
					constructVar(r);


				//(2. check and apply read/write
				for (auto& r : pass.reads)
				{
					if (writerFence[r] != -1)
						exe->jobs[i].dependents.push_back(writerFence[r]);
					readerFence[r].push_back(i);
				}
				for (auto& r : pass.writes)
					writeVar(i, r);

				//(3. apply additional dependency
				for (auto& r : pass.dependency)
					exe->jobs[i].dependents.push_back(r);

				//(4. check and apply destruct
				for (auto& r : pass.reads)
					destructVar(r);
				for (auto& r : pass.writes)
					destructVar(r);

				++i;
			}
		}
	};

	class executor
	{
		//input
		std::deque<logic_var> vars;
		std::deque<execute_job> jobs;
		std::unique_ptr<execute_context> exe;

		void execute();
	};

	class play_graph
	{

	};
}
