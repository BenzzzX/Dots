#include "pch.h"
#include "Codebase.h"
#include <execution>
#define forloop(i, z, n) for(auto i = std::decay_t<decltype(n)>(z); i<(n); ++i)

struct test
{
	using value_type = int;
	int v;
};

void install_test_components()
{
	using namespace core::database;
	core::codebase::cid<test> = register_type({ false, false, false, 10, sizeof(test) });
}

class CodebaseTest : public ::testing::Test
{
protected:
	CodebaseTest()
		:ctx(), ppl(ctx) {}
	void SetUp() override
	{}

	core::entity pick(const core::database::chunk_vector<core::database::chunk_slice>& vector)
	{
		auto c = vector[0];
		return ctx.get_entities(c.c)[c.start];
	}
	core::database::world ctx;


	core::codebase::pipeline ppl;
};


TEST_F(CodebaseTest, CreateKernel) {
	using namespace core::codebase;
	index_t t[] = { cid<test> };
	entity_type type = { t };
	ctx.allocate(type);

	filters filter;
	filter.archetypeFilter = { type };
	auto params = hana::make_tuple(param<test>);
	auto k = ppl.create_kernel(filter, params);
	EXPECT_EQ(k->chunks.size, 1);
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
	index_t t[] = { cid<test> };
	entity_type type = { t };
	{
		int counter = 1;
		for (auto c : ctx.allocate(type, 100000))
		{
			auto components = get_component_ro<test>(ctx, c);
			forloop(i, 0, c.count)
				components[i] = counter++;
		}
	}


	filters filter;
	filter.archetypeFilter = { type };
	static constexpr auto params = hana::make_tuple(param<test>);
	auto k = ppl.create_kernel(filter, params);
	auto tasks = ppl.create_tasks(*k);
	long long counter = 0;
	std::for_each(tasks.begin(), tasks.end(), [k, &counter](task& tk)
		{
			auto o = operation{ params, *k, tk };
			int* tests = o.get_parameter<test>();
			forloop(i, 0, o.get_count())
				counter += tests[i];
		});
	EXPECT_EQ(counter, 5000050000);
}

TEST_F(CodebaseTest, TaskMultiThreadStd)
{
	using namespace core::codebase;
	index_t t[] = { cid<test> };
	entity_type type = { t };
	{
		int counter = 1;
		for (auto c : ctx.allocate(type, 100000))
		{
			auto components = get_component_ro<test>(ctx, c);
			forloop(i, 0, c.count)
				components[i] = counter++;
		}
	}
	filters filter;
	filter.archetypeFilter = { type };
	static constexpr auto params = hana::make_tuple(param<test>);
	auto k = ppl.create_kernel(filter, params);
	auto tasks = ppl.create_tasks(*k);
	std::atomic<long long> counter = 0;
	std::for_each(std::execution::parallel_unsequenced_policy{}, 
		tasks.begin(), tasks.end(), [k, &counter](task& tk)
		{
			auto o = operation{ params, *k, tk };
			int* tests = o.get_parameter<test>();
			forloop(i, 0, o.get_count())
				counter.fetch_add(tests[i]);
		});
	EXPECT_EQ(counter, 5000050000);
}

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	core::database::initialize();
	install_test_components();
	return RUN_ALL_TESTS();
}