#include "pch.h"
#include "Database.h"


struct test
{
	int v;
	float f;
};

struct test_track
{
	int v;
};

struct test_element
{
	int v;
};

struct test_tag {};

core::database::index_t test_id;
core::database::index_t test_track_id;
core::database::index_t test_element_id;
core::database::index_t test_tag_id;

TEST(MetaTest, Equal) 
{
  EXPECT_EQ(1, 1);
  EXPECT_TRUE(true);
}

int fuck = 1;

TEST(MetaTest, SetGlobal)
{
	EXPECT_EQ(fuck, 1);
	fuck = 2;
	EXPECT_EQ(fuck, 2);
	EXPECT_TRUE(true);
}

TEST(MetaTest, ReadGlobal)
{
	EXPECT_EQ(fuck, 2);
	EXPECT_TRUE(true);
}

class DatabaseTest : public ::testing::Test
{
protected:
	void SetUp() override
	{}

	core::database::world ctx;
};

TEST_F(DatabaseTest, AllocateOne)
{
	using namespace core::database;
	entity_type emptyType;
	core::entity e;
	for (auto c : ctx.allocate(emptyType))
		e = ctx.get_entities(c.c)[c.start];
	EXPECT_TRUE(ctx.exist(e));
}

TEST_F(DatabaseTest, AllocateMillion)
{
	using namespace core::database;
	entity_type emptyType;
	ctx.allocate(emptyType, 1000000);
	EXPECT_TRUE(true);
}

TEST_F(DatabaseTest, Instatiate)
{
	using namespace core::database;
	index_t t[] = { test_id };
	entity_type type{ {t, 1} };
	core::entity e[2];
	for (auto c : ctx.allocate(type))
		e[0] = ctx.get_entities(c.c)[c.start];
	EXPECT_TRUE(ctx.has_component(e[0], { t, 1 }));
	for(auto c : ctx.instantiate(e[0]))
		e[1] = ctx.get_entities(c.c)[c.start];
	EXPECT_TRUE(ctx.has_component(e[1], { t, 1 }));
}

TEST_F(DatabaseTest, Destroy)
{
	using namespace core::database;
	entity_type emptyType;
	core::entity e;
	for (auto c : ctx.allocate(emptyType))
		e = ctx.get_entities(c.c)[c.start];
	EXPECT_TRUE(ctx.exist(e));
	ctx.destroy(ctx.batch(&e, 1)[0]);
	EXPECT_TRUE(!ctx.exist(e));
}

TEST_F(DatabaseTest, Cast)
{
	using namespace core::database;
	entity_type emptyType;
	core::entity e;
	for (auto c : ctx.allocate(emptyType))
		e = ctx.get_entities(c.c)[c.start];
	EXPECT_TRUE(ctx.exist(e));

	index_t t[] = { test_id };
	entity_type type{ {t, 1} };
	for (auto c : ctx.batch(&e, 1))
		for (auto cc : ctx.cast(c, type))
			e = ctx.get_entities(c.c)[c.start];
	EXPECT_TRUE(ctx.has_component(e, { t, 1 }));
}

TEST_F(DatabaseTest, CastDiff)
{
	using namespace core::database;
	entity_type emptyType;
	core::entity e;
	for (auto c : ctx.allocate(emptyType))
		e = ctx.get_entities(c.c)[c.start];
	EXPECT_TRUE(ctx.exist(e));

	index_t t[] = { test_id };
	entity_type type{ {t, 1} };
	type_diff diff = { type };
	for (auto c : ctx.batch(&e, 1))
		for (auto cc : ctx.cast(c, diff))
			e = ctx.get_entities(c.c)[c.start];
	EXPECT_TRUE(ctx.has_component(e, { t, 1 }));
}

TEST_F(DatabaseTest, Batch)
{
	using namespace core::database;
	entity_type emptyType;
	core::entity e[10];
	int i = 0;
	for (auto c : ctx.allocate(emptyType, 10))
	{
		auto es = ctx.get_entities(c.c);
		for(int j=0;j<c.count;++j)
			e[i++] = es[j + c.start];
	}
	EXPECT_EQ(i, 10);
	EXPECT_TRUE(ctx.exist(e[9]));
	auto slices = ctx.batch(e, 10);
	EXPECT_EQ(slices[0].count, 10);
}


TEST_F(DatabaseTest, Meta) {}
TEST_F(DatabaseTest, MetaRead) {}
TEST_F(DatabaseTest, BufferReadWrite) {}
TEST_F(DatabaseTest, BufferInstatiate) {}
TEST_F(DatabaseTest, LifeTimeTrack) {}
TEST_F(DatabaseTest, Query) {}
TEST_F(DatabaseTest, DisableMask) {}
TEST_F(DatabaseTest, MoveContext) {}
TEST_F(DatabaseTest, Deserialize) {}
TEST_F(DatabaseTest, EntityDeserialize) {}
TEST_F(DatabaseTest, ComponentReadWrite) {}

void install_test_components()
{
	using namespace core::database;
	test_id = register_type({ false, false, false, 10, sizeof(test) });
	test_track_id = register_type({ false, true, true, 11, sizeof(test_track) });
	test_element_id = register_type({ true, false, false, 12, 128, sizeof(test_element) });
	test_tag_id = register_type({ false, false, false, 13, 0 });
}

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	core::database::initialize();
	install_test_components();
	return RUN_ALL_TESTS();
}