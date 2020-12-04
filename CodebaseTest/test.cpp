#include "pch.h"
#include "Codebase.h"
#include <execution>
#define forloop(i, z, n) for(auto i = std::decay_t<decltype(n)>(z); i<(n); ++i)

struct test
{
	int v;
	float f;
};

core::database::index_t test_id;

void install_test_components()
{
	using namespace core::database;
	test_id = register_type({ false, false, false, 10, sizeof(test) });
}

class CodebaseTest : public ::testing::Test
{
protected:
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
	index_t t[] = { test_id };
	entity_type type = { t };
	ctx.allocate(type);

	view v;
	v.archetypeFilter = { type };
	param params[] = { {test_id, false, false} };
	v.params = params;
	v.paramCount = 1;
	auto k = ppl.create_kernel(ctx, v);
	EXPECT_EQ(k->chunks.size, 1);
}

TEST_F(CodebaseTest, TaskSingleThread)
{
	using namespace core::codebase;
	index_t t[] = { test_id };
	entity_type type = { t };
	{
		int counter = 1;
		for (auto c : ctx.allocate(type, 100000))
		{
			auto components = (test*)ctx.get_owned_rw(c.c, test_id);
			forloop(i, 0, c.count)
				components[c.start + i].v = counter++;
		}
	}

	view v;
	v.archetypeFilter = { type };
	param params[] = { {test_id, true, false} };
	v.params = params;
	v.paramCount = 1;
	auto k = ppl.create_kernel(ctx, v);
	auto tasks = ppl.create_tasks(*k);
	long long counter = 0;
	std::for_each(tasks.begin(), tasks.end(), [k, &counter](task& tk)
		{
			operation o{ *k, tk };
			auto tests = o.get_parameter<test>(0);
			forloop(i, 0, o.get_count())
				counter += tests[i].v;
		});
	EXPECT_EQ(counter, 5000050000);
}


TEST_F(CodebaseTest, TaskMultiThreadStd)
{
	using namespace core::codebase;
	index_t t[] = { test_id };
	entity_type type = { t };
	{
		int counter = 1;
		for (auto c : ctx.allocate(type, 100000))
		{
			auto components = (test*)ctx.get_owned_rw(c.c, test_id);
			forloop(i, 0, c.count)
				components[c.start + i].v = counter++;
		}
	}
	view v;
	v.archetypeFilter = { type };
	param params[] = { {test_id, true, false} };
	v.params = params; v.paramCount = 1;
	auto k = ppl.create_kernel(ctx, v); 
	auto tasks = ppl.create_tasks(*k);
	std::atomic<long long> counter = 0;
	std::for_each(std::execution::parallel_unsequenced_policy{}, 
		tasks.begin(), tasks.end(), [k, &counter](task& tk)
		{
			operation o{ *k, tk };
			auto tests = o.get_parameter<test>(0);
			forloop(i, 0, o.get_count())
				counter.fetch_add(tests[i].v);
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