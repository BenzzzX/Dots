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

struct disable {};
struct cleanup {};

void install_test_components()
{
	using namespace core::database;
	core::codebase::cid<group> = group_id;
	core::codebase::cid<disable> = disable_id;
	core::codebase::cid<cleanup> = cleanup_id;
	core::codebase::cid<mask> = mask_id;
	core::codebase::cid<test> = register_type({ false, false, false, 10, sizeof(test) });
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
struct complist
{
	operator core::database::typeset()
	{
		static core::database::index_t list[] = { core::codebase::cid<Ts>... };
		return list;
	}
};

TEST_F(CodebaseTest, CreatePass) {
	using namespace core::codebase;
	entity_type type = { complist<test>() };
	ctx.allocate(type);

	core::codebase::pipeline ppl(ctx);
	filters filter;
	filter.archetypeFilter = { type };
	auto params = hana::make_tuple(param<test>);
	auto k = ppl.create_pass(filter, params);
	EXPECT_EQ(k->chunkCount, 1);
}

template<class T>
auto get_component_ro(core::database::world& ctx, core::database::chunk_slice c)
{
	using namespace core::codebase;
	using value_type = component_value_type_t<T>;
	return (value_type*)ctx.get_component_ro(c.c, cid<T>) + c.start;
}

TEST_F(CodebaseTest, TaskSingleThread)
{
	using namespace core::codebase;
	entity_type type = { complist<test>() }; //定义 entity 类型
	{
		int counter = 1;
		for (auto c : ctx.allocate(type, 100000)) // 生产 10w 个 entity
		{
			//返回创建的 slice，在 slice 中就地初始化生成的 entity 的数据
			auto components = get_component_ro<test>(ctx, c);
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
	entity_type type = { complist<test>() }; //定义 entity 类型
	{
		int counter = 1;
		for (auto c : ctx.allocate(type, 100000)) // 生产 10w 个 entity
		{
			//返回创建的 slice，在 slice 中就地初始化生成的 entity 的数据
			auto components = get_component_ro<test>(ctx, c);
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
	entity_type type = { complist<test>() }; //定义 entity 类型
	{
		int counter = 1;
		for (auto c : ctx.allocate(type, 100000)) // 生产 10w 个 entity
		{
			//返回创建的 slice，在 slice 中就地初始化生成的 entity 的数据
			auto tests = get_component_ro<test>(ctx, c);
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

TEST_F(CodebaseTest, MarlIntergration)
{
	using namespace core::codebase;
	entity_type type = { complist<test>() }; //定义 entity 类型
	{
		int counter = 1;
		for (auto c : ctx.allocate(type, 100000)) // 生产 10w 个 entity
		{
			//返回创建的 slice，在 slice 中就地初始化生成的 entity 的数据
			auto tests = get_component_ro<test>(ctx, c);
			forloop(i, 0, c.count)
				tests[i] = counter++;
		}
	}

	std::atomic<long long> counter = 0;
	{
		marl::Scheduler scheduler(marl::Scheduler::Config::allCores());
		scheduler.bind();
		defer(scheduler.unbind());  // Automatically unbind before returning.
		std::vector<marl::Event> allPasses;

		//创建一个运算管线，运算管线生命周期内不应该直接操作 world
		pipeline ppl(ctx);

		ppl.on_sync = [&](pass** dependencies, int dependencyCount)
		{
			forloop(i, 0, dependencyCount)
				allPasses[dependencies[i]->passIndex].wait();
		};

		filters filter;
		filter.archetypeFilter = { type }; //筛选所有的 test
		def params = hana::make_tuple(param<const test>); //定义 pass 的参数
		auto k = ppl.create_pass(filter, params); //创建 pass
		auto tasks = ppl.create_tasks(*k); //从 pass 提取 task
		
		// 将要添加任务的Event.
		marl::Event pass(marl::Event::Mode::Manual);
		allPasses.push_back(pass);
		// 等这个Event激发了才fire.
		marl::Event starterPass(marl::Event::Mode::Manual);
		
		marl::schedule([=, &tasks, &counter] {
			starterPass.wait();
			// 等待依赖的pass信号量
			forloop(j, 0, k->dependencyCount)
				allPasses[k->dependencies[j]->passIndex].wait();
			constexpr int MinParallelTask = 10;
			constexpr bool ForceParallel = false;
			const bool recommandParallel = !k->hasRandomWrite && tasks.size > MinParallelTask;
			if (recommandParallel || ForceParallel) // task交付marl
			{
				marl::WaitGroup tasksGroup(tasks.size);
				forloop(tsk, 0, tasks.size)
				{
					auto& tk = tasks[tsk];
					marl::schedule([=, &tk, &counter] {
						// Decrement the WaitGroup counter when the task has finished.
						defer(tasksGroup.done());
						std::cout << std::this_thread::get_id() << std::endl;
						//使用 operation 封装 task 的操作，通过先前定义的参数来保证类型安全
						auto o = operation{ params, *k, tk };
						//以 slice 为粒度执行具体的逻辑
						const int* tests = o.get_parameter<test>();
						const core::entity* es = o.get_entities();
						forloop(i, 0, o.get_count())
							counter.fetch_add(tests[i]);
						});
				}
				tasksGroup.wait();
			}
			else // 强制串行
			{
				std::for_each(
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
			}

			pass.signal();
		});


		// 开闸泄洪
		starterPass.signal();
		forloop(i, 0, tasks.size)
			allPasses[i].wait();
	}
	EXPECT_EQ(counter, 5000050000);
}

TEST_F(CodebaseTest, Dependency)
{
	using namespace core::codebase;
	entity_type type = { complist<test>() };
	ctx.allocate(type);

	core::codebase::pipeline ppl(ctx);
	filters filter;
	filter.archetypeFilter = { type };
	auto params = hana::make_tuple(param<test>);
	auto k = ppl.create_pass(filter, params);
	EXPECT_EQ(k->chunkCount, 1);
}

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	core::database::initialize();
	install_test_components();
	return RUN_ALL_TESTS();
}