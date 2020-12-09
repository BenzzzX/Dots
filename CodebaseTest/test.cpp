#include "pch.h"
#include "Codebase.h"

#include "taskflow.hpp"

#include "marl/defer.h"
#include "marl/event.h"
#include "marl/scheduler.h"
#include "marl/waitgroup.h"

#define forloop(i, z, n) for(auto i = std::decay_t<decltype(n)>(z); i<(n); ++i)
#define def static constexpr auto

struct test
{
	using value_type = int;
	int v;
};
struct test2
{
	using value_type = int;
	int v;
};

void install_test_components()
{
	using namespace core::database;
	core::codebase::cid<group> = group_id;
	core::codebase::cid<disable> = disable_id;
	core::codebase::cid<cleanup> = cleanup_id;
	core::codebase::cid<mask> = mask_id;
	core::codebase::cid<test> = register_type({ false, false, false, typeid(test).hash_code(), sizeof(test) });
	core::codebase::cid<test2> = register_type({ false, false, false,  typeid(test2).hash_code(), sizeof(test2) });
}

class CodebaseTest : public ::testing::Test
{
protected:
	CodebaseTest()
		:ctx() {}
	void SetUp() override
	{}

	core::entity pick(const core::database::chunk_vector<core::database::chunk_slice>& vector)
	{
		auto c = vector[0];
		return ctx.get_entities(c.c)[c.start];
	}
	core::database::world ctx;
};


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

TEST_F(CodebaseTest, CreatePass) {
	using namespace core::codebase;
	entity_type type = { complist<test> };
	ctx.allocate(type);

	core::codebase::pipeline ppl(ctx);
	filters filter;
	filter.archetypeFilter = { type };
	auto params = hana::make_tuple(param<test>);
	auto k = ppl.create_pass(filter, params);
	EXPECT_EQ(k->chunkCount, 1);
}

template<class T>
auto init_component(core::database::world& ctx, core::database::chunk_slice c)
{
	using namespace core::codebase;
	using value_type = component_value_type_t<T>;
	return (value_type*)ctx.get_component_ro(c.c, cid<T>) + c.start;
}

TEST_F(CodebaseTest, TaskSingleThread)
{
	using namespace core::codebase;
	entity_type type = { complist<test> }; //定义 entity 类型
	{
		int counter = 1;
		for (auto c : ctx.allocate(type, 100000)) // 生产 10w 个 entity
		{
			//返回创建的 slice，在 slice 中就地初始化生成的 entity 的数据
			auto components = init_component<test>(ctx, c);
			forloop(i, 0, c.count)
				components[i] = counter++;
		}
	}

	long long counter = 0;
	{
		//创建一个运算管线，运算管线生命周期内不应该直接操作 world
		pipeline ppl(ctx); 
		filters filter;
		filter.archetypeFilter = { type }; //筛选所有的 test
		def params = hana::make_tuple(param<test>); //定义 pass 的参数
		auto k = ppl.create_pass(filter, params); //创建 pass
		auto tasks = ppl.create_tasks(*k); //从 pass 提取 task
		std::for_each(tasks.begin(), tasks.end(), [k, &counter](task& tk)
			{
				//使用 operation 封装 task 的操作，通过先前定义的参数来保证类型安全
				auto o = operation{ params, *k, tk };
				//以 slice 为粒度执行具体的逻辑
				int* tests = o.get_parameter<test>();
				forloop(i, 0, o.get_count())
					counter += tests[i];
			});
	}
	EXPECT_EQ(counter, 5000050000);
}
#if defined(_WIN32) || defined(_WIN64)
// Only MSVC support this.
#include <execution>
TEST_F(CodebaseTest, TaskMultiThreadStd)
{
	using namespace core::codebase;
	entity_type type = { complist<test> }; //定义 entity 类型
	{
		int counter = 1;
		for (auto c : ctx.allocate(type, 100000)) // 生产 10w 个 entity
		{
			//返回创建的 slice，在 slice 中就地初始化生成的 entity 的数据
			auto components = init_component<test>(ctx, c);
			forloop(i, 0, c.count)
				components[i] = counter++;
		}
	}

	std::atomic<long long> counter = 0;
	{
		//创建一个运算管线，运算管线生命周期内不应该直接操作 world
		pipeline ppl(ctx);
		filters filter;
		filter.archetypeFilter = { type }; //筛选所有的 test
		def params = hana::make_tuple(param<test>); //定义 pass 的参数
		auto k = ppl.create_pass(filter, params); //创建 pass
		auto tasks = ppl.create_tasks(*k); //从 pass 提取 task
		std::for_each(std::execution::parallel_unsequenced_policy{}, //使用 std 的多线程调度
			tasks.begin(), tasks.end(), [k, &counter](task& tk)
			{
				//使用 operation 封装 task 的操作，通过先前定义的参数来保证类型安全
				auto o = operation{ params, *k, tk };
				//以 slice 为粒度执行具体的逻辑
				int* tests = o.get_parameter<test>();
				forloop(i, 0, o.get_count())
					counter.fetch_add(tests[i]);
			});
	}
	EXPECT_EQ(counter, 5000050000);
}
#endif

TEST_F(CodebaseTest, TaskflowIntergration)
{
	using namespace core::codebase;
	entity_type type = { complist<test> }; //定义 entity 类型
	{
		int counter = 1;
		for (auto c : ctx.allocate(type, 100000)) // 生产 10w 个 entity
		{
			//返回创建的 slice，在 slice 中就地初始化生成的 entity 的数据
			auto tests = init_component<test>(ctx, c);
			forloop(i, 0, c.count)
				tests[i] = counter++;
		}
	}

	std::atomic<long long> counter = 0;
	{
		tf::Executor executer;
		tf::Taskflow taskflow;
		std::vector<tf::Task> tfTasks;
		//创建一个运算管线，运算管线生命周期内不应该直接操作 world
		pipeline ppl(ctx);
		ppl.on_sync = [&](pass** dependencies, int dependencyCount)
		{
			forloop(i, 0, dependencyCount)
				;// executer.wait(tkTasks[dependencies[dependencyCount]->passId]);
		};

		filters filter;
		filter.archetypeFilter = { type }; //筛选所有的 test
		def params = hana::make_tuple(param<const test>); //定义 pass 的参数
		auto k = ppl.create_pass(filter, params); //创建 pass
		auto tasks = ppl.create_tasks(*k); //从 pass 提取 task
		auto tk = taskflow.for_each(//使用 taskflow 的多线程调度
			tasks.begin(), tasks.end(), [k, &counter](task& tk)
			{
				//使用 operation 封装 task 的操作，通过先前定义的参数来保证类型安全
				auto o = operation{ params, *k, tk };
				//以 slice 为粒度执行具体的逻辑
				const int* tests = o.get_parameter<test>();
				const core::entity* es = o.get_entities();
				forloop(i, 0, o.get_count())
					counter.fetch_add(tests[i]);
			});

		tfTasks.push_back(tk);
		forloop(i, 0, k->dependencyCount) //转换 pass 的依赖到 taskflow
			tk.precede(tfTasks[k->dependencies[i]->passIndex]);

		executer.run(taskflow).wait();
	}
	EXPECT_EQ(counter, 5000050000);
}


namespace ecs
{
	using namespace core::database;
	using core::entity;
	//using namespace core::codebase;
	using pass = core::codebase::pass;
	using filters = core::codebase::filters;
	using task = core::codebase::task;

	using core::codebase::component_value_type_t;
	using core::codebase::cid;
	using core::codebase::param;
	using core::codebase::operation;

	struct pipeline final : public core::codebase::pipeline
	{
		using base_t = core::codebase::pipeline;
		pipeline(world& ctx) :base_t(ctx) {};
		template<class T>
		pass* create_pass(const filters& v, T paramList)
		{
			pass_events.emplace_back(marl::Event::Mode::Manual);
			return static_cast<pass*>(base_t::create_pass(v, paramList));
		}
		void wait()
		{
			forloop(i, 0u, pass_events.size())
				pass_events[i].wait();
		}
		std::vector<marl::Event> pass_events;
	};

	template<typename F, bool ForceParallel = false, bool ForceNoParallel = false>
	FORCEINLINE marl::Event schedule(ecs::pipeline& pipeline, ecs::pass& pass, F&& f)
	{
		static_assert(std::is_invocable_v<std::decay_t<F>, ecs::pipeline&, ecs::pass&, ecs::task&>,
			"F must be an invokable of void(ecs::pipeline&, ecs::pass&, ecs::task&)>");
		static_assert(!(ForceParallel & ForceNoParallel),
			"A schedule can not force both parallel and not parallel!");
		marl::schedule([&, f]
			{
				auto tasks = pipeline.create_tasks(pass); //从 pass 提取 task
				defer(pipeline.pass_events[pass.passIndex].wait());
				for (auto i = 0u; i < pass.dependencyCount; i++)
					pipeline.pass_events[pass.dependencies[i]->passIndex].wait();

				constexpr auto MinParallelTask = 10u;
				const bool recommandParallel = !pass.hasRandomWrite && tasks.size > MinParallelTask;
				if ((recommandParallel & !ForceNoParallel) || ForceParallel) // task交付task_system
				{
					marl::WaitGroup tasksGroup(tasks.size);
					forloop(tsk, 0, tasks.size)
					{
						auto& tk = tasks[tsk];
						marl::schedule([&, tasksGroup] {
							// Decrement the WaitGroup counter when the task has finished.
							defer(tasksGroup.done());
							f(pipeline, pass, tk);
							});
					}
					tasksGroup.wait();
				}
				else // 串行
				{
					std::for_each(
						tasks.begin(), tasks.end(), [&, f](auto& tk)
						{
							f(pipeline, pass, tk);
						});
				}
				pipeline.pass_events[pass.passIndex].signal();
			});
		return pipeline.pass_events[pass.passIndex];
	}
}


TEST_F(CodebaseTest, MarlIntergration)
{
	using namespace ecs;
	entity_type type = { complist<test> }; //定义 entity 类型
	{
		int counter = 1;
		for (auto c : ctx.allocate(type, 100000)) // 生产 10w 个 entity
		{
			//返回创建的 slice，在 slice 中就地初始化生成的 entity 的数据
			auto tests = init_component<test>(ctx, c);
			forloop(i, 0, c.count)
				tests[i] = counter++;
		}
	}

	std::atomic<long long> counter = 0;
	{
		marl::Scheduler scheduler(marl::Scheduler::Config::allCores());
		scheduler.bind();
		defer(scheduler.unbind());  // Automatically unbind before returning.
		std::unordered_map<int, marl::WaitGroup> allPasses;

		//创建一个运算管线，运算管线生命周期内不应该直接操作 world
		ecs::pipeline ppl(ctx);

		ppl.on_sync = [&](pass** dependencies, int dependencyCount)
		{
			forloop(i, 0, dependencyCount)
				allPasses[dependencies[i]->passIndex].wait();
		};

		filters filter;
		filter.archetypeFilter = { 
			{complist<test>}
		}; //筛选所有的 test
		def params = boost::hana::make_tuple(param<const test>); //定义 pass 的参数
		auto passHdl = ecs::schedule(ppl, *ppl.create_pass(filter, params),
			[&](ecs::pipeline& pipeline, ecs::pass& pass, ecs::task& tk)
			{
				//使用 operation 封装 task 的操作，通过先前定义的参数来保证类型安全
				auto o = operation{ params, pass, tk };
				//以 slice 为粒度执行具体的逻辑
				const int* tests = o.get_parameter<test>();
				const core::entity* es = o.get_entities();
				forloop(i, 0, o.get_count())
					counter.fetch_add(tests[i]);
			});
		// 等待单一pass
		passHdl.wait();
		// 等待pipeline
		ppl.wait();
	}
	EXPECT_EQ(counter, 5000050000);
}

TEST_F(CodebaseTest, Dependency)
{
	using namespace core::codebase;
	{
		entity_type type = { complist<test> };
		ctx.allocate(type);
	}
	{
		entity_type type = { complist<test, test2> };
		ctx.allocate(type);
	}

	core::codebase::pipeline ppl(ctx);
	pass* p1, *p2, *p3, *p4, *p5;

	{
		entity_type type = { complist<test> };
		filters filter;
		filter.archetypeFilter = { type };
		auto params = hana::make_tuple(param<test>);
		p1 = ppl.create_pass(filter, params);
		EXPECT_EQ(p1->dependencyCount, 0);
	}
	//基于 comp 的依赖和分享
	{
		//依赖 p1 : const test -> test
		entity_type type = { complist<test> };
		filters filter;
		filter.archetypeFilter = { type };
		auto params = hana::make_tuple(param<const test>);
		p2 = ppl.create_pass(filter, params);
		EXPECT_EQ(p2->dependencyCount, 1);
		EXPECT_EQ(p2->dependencies[0], p1);
	}

	{
		//依赖 p1 : const test -> test
		//与 p2 分享 const test
		entity_type type = { complist<test> };
		filters filter;
		filter.archetypeFilter = { type };
		auto params = hana::make_tuple(param<const test>);
		p3 = ppl.create_pass(filter, params);
		EXPECT_EQ(p3->dependencyCount, 1);
		EXPECT_EQ(p3->dependencies[0], p1);
	}
	//基于 filter 的依赖和分享
	{
		//在 [test] chunk 上依赖 p2, p3 : test -> const test
		entity_type type = { complist<test> };
		entity_type type2 = { complist<test2> };
		filters filter;
		filter.archetypeFilter = { type, {}, type2 };
		auto params = hana::make_tuple(param<test>);
		p4 = ppl.create_pass(filter, params);
		EXPECT_EQ(p4->dependencyCount, 2);
		EXPECT_EQ(p4->dependencies[0], p2);
		EXPECT_EQ(p4->dependencies[1], p3);
	}

	{
		//在 [test, test2] chunk 上依赖 p2, p3 : test -> const test
		//因为和 p4 在不同的 archetype 上，所以不冲突
		entity_type type = { complist<test, test2> };
		filters filter;
		filter.archetypeFilter = { type };
		auto params = hana::make_tuple(param<test>);
		p5 = ppl.create_pass(filter, params);
		EXPECT_EQ(p5->dependencyCount, 2);
		EXPECT_EQ(p5->dependencies[0], p2);
		EXPECT_EQ(p5->dependencies[1], p3);
	}
}

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	core::database::initialize();
	install_test_components();
	return RUN_ALL_TESTS();
}