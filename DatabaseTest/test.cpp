#include "pch.h"
#include "Database.h"
#define forloop(i, z, n) for(auto i = decltype(n)(z); i<n; ++i)


template<class T>
core::database::index_t tid = 0;

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

struct test_not_exist
{
	
};

struct alignas(16) test_align
{
	float X, Y, Z, W;
};

struct test_ref
{
	core::entity ref;
};

struct test_tag {};

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

TEST(DSTest, ChunkVector)
{
	using namespace core::database;
	void* ptr = nullptr, *ptr2 = nullptr, * ptr3 = nullptr;
	chunk_vector<int> v = 
		[&]()
	{
		chunk_vector<int> r;
		r.push(1);
		ptr = r.data;
		return r; //RVO
	}();
	EXPECT_EQ(v.data, ptr);
	{
		auto l = [&, v, this]() //caputure
		{
			EXPECT_EQ(v[0], 1);
			ptr2 = v.data;
		};
		{
			l();
			EXPECT_TRUE(ptr != ptr2);
			ptr3 = ptr2;
		}
		{
			auto l2 = l; //copy
			l2();
			EXPECT_TRUE(ptr2 != ptr3);
		}
		{
			auto l3 = std::move(l); //move
			l3();
			EXPECT_TRUE(ptr2 == ptr3);
		}
	}
	EXPECT_EQ(v.data, ptr);
	EXPECT_EQ(v[0], 1);

}

class DatabaseTest : public ::testing::Test
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

TEST_F(DatabaseTest, AllocateMillion2)
{
	using namespace core::database;
	entity_type emptyType;
	ctx.allocate(emptyType, 1000000);
	EXPECT_TRUE(true);
}

TEST_F(DatabaseTest, Instatiate)
{
	using namespace core::database;
	index_t t[] = { tid<test> };
	entity_type type{ t };
	core::entity e[2];
	e[0] = pick(ctx.allocate(type));
	EXPECT_TRUE(ctx.has_component(e[0], { t, 1 }));
	e[1] = pick(ctx.instantiate(e[0]));
	EXPECT_TRUE(ctx.has_component(e[1], { t, 1 }));
}

TEST_F(DatabaseTest, DestroyOne)
{
	using namespace core::database;
	entity_type emptyType;
	core::entity e = pick(ctx.allocate(emptyType));
	EXPECT_TRUE(ctx.exist(e));
	ctx.destroy(ctx.batch(&e, 1)[0]);
	EXPECT_TRUE(!ctx.exist(e));
}

TEST_F(DatabaseTest, Cast)
{
	using namespace core::database;
	entity_type emptyType;
	core::entity e = pick(ctx.allocate(emptyType));
	EXPECT_TRUE(ctx.exist(e));

	index_t t[] = { tid<test> };
	entity_type type{ t };
	for (auto c : ctx.batch(&e, 1))
		ctx.cast(c, type);
	EXPECT_TRUE(ctx.has_component(e, { t, 1 }));
}

TEST_F(DatabaseTest, NotExist)
{
	using namespace core::database;
	index_t t[] = { tid<test> };
	entity_type type{ t };
	core::entity e = pick(ctx.allocate(type));
	EXPECT_TRUE(ctx.exist(e));

	auto component = (test_not_exist*)ctx.get_component_ro(e, tid<test_not_exist>);
	EXPECT_TRUE(!component);

	index_t nt[] = { tid<test_not_exist> };
	entity_type none_type{ nt };
	auto atypes = ctx.query({ none_type });
	EXPECT_TRUE(atypes.size == 0);
}

TEST_F(DatabaseTest, CastDiff)
{
	using namespace core::database;
	entity_type emptyType;
	core::entity e = pick(ctx.allocate(emptyType));
	EXPECT_TRUE(ctx.exist(e));

	index_t t[] = { tid<test> };
	entity_type type{ t };
	type_diff diff = { type };
	for (auto c : ctx.batch(&e, 1))
		ctx.cast(c, diff);
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
		memcpy(e + i, es + c.start, sizeof(core::entity) * c.count);
		i += c.count;
	}
	EXPECT_EQ(i, 10);
	EXPECT_TRUE(ctx.exist(e[9]));
	auto slices = ctx.batch(e, 10);
	EXPECT_EQ(slices[0].count, 10);
}

TEST_F(DatabaseTest, ComponentReadWrite) 
{
	using namespace core::database;
	core::entity e;

	index_t t[] = { tid<test> };
	entity_type type{ t };
	//就地初始化，避免多次查询
	for (auto c : ctx.allocate(type))
	{
		auto components = (test*)ctx.get_owned_rw(c.c, tid<test>);
		e = ctx.get_entities(c.c)[c.start];
		forloop(i, 0, c.count)
			components[c.start + i].f = -1.f;
	}
	auto component = (test*)ctx.get_component_ro(e, tid<test>);
	EXPECT_EQ(component->f, -1.f);
	((test*)ctx.get_owned_rw(e, tid<test>))->f = 1.f;
	component = (test*)ctx.get_component_ro(e, tid<test>);
	EXPECT_EQ(component->f, 1.f);
}



TEST_F(DatabaseTest, Align)
{
	using namespace core::database;
	core::entity e = {};

	index_t t[] = { tid<test>, tid<test_align>, tid<test_element> };
	entity_type type{ t };
	//就地初始化，避免多次查询
	ctx.allocate(type);
	{
		auto component = (test_align*)ctx.get_component_ro(e, tid<test_align>);
		EXPECT_EQ((size_t)component % 16, 0);
	}
	{
		auto component = (test*)ctx.get_component_ro(e, tid<test>);
		EXPECT_EQ((size_t)component % 8, 0);
	}
}

TEST_F(DatabaseTest, LifeTimeTrack) 
{
	using namespace core::database;

	//初始化
	index_t t[] = { tid<test_track> };
	entity_type type{ t };
	core::entity e = pick(ctx.allocate(type));
	{
		//非就地初始化，更容易书写
		auto component = (test_track*)ctx.get_owned_rw(e, tid<test_track>);
		component->v = 2;
	}

	//克隆并销毁
	core::entity e2 = pick(ctx.instantiate(e));
	for (auto c : ctx.batch(&e, 1))
		ctx.destroy(c);

	//待销毁状态
	EXPECT_TRUE(ctx.exist(e));
	EXPECT_TRUE(ctx.has_component(e, { t,1 }));
	{
		auto component = (test_track*)ctx.get_component_ro(e, tid<test_track>);
		EXPECT_EQ(component->v, 2);
	}
	//清理待销毁组件，完成销毁
	type_diff removetrack{ {}, type };
	for (auto c : ctx.batch(&e, 1))
		ctx.cast(c, removetrack);
	EXPECT_TRUE(!ctx.exist(e));

	//待克隆状态
	EXPECT_TRUE(!ctx.has_component(e2, { t,1 }));
	{
		auto component = (test_track*)ctx.get_component_ro(e2, tid<test_track> + 1);
		EXPECT_EQ(component->v, 2);
	}
	type_diff addtrack{ type };
	//添加待克隆组件，完成拷贝
	for (auto c : ctx.batch(&e2, 1))
		ctx.cast(c, addtrack);
	EXPECT_TRUE(ctx.has_component(e2, { t,1 }));
}

TEST_F(DatabaseTest, BufferReadWrite) 
{
	using namespace core::database;
	core::entity e;

	index_t t[] = { tid<test_element> };
	entity_type type{ t };
	for (auto c : ctx.allocate(type))
	{
		auto buffers = (char*)ctx.get_owned_rw(c.c, tid<test_element>);
		e = ctx.get_entities(c.c)[c.start];
		test_element v{ 3 };
		forloop(i, 0, c.count)
			buffer_t<test_element>(buffers + 128*i).push(v);
	}
	auto b = buffer_t<test_element>(ctx.get_component_ro(e, tid<test_element>));

	EXPECT_EQ(b[0].v, 3);
}

TEST_F(DatabaseTest, BufferInstatiate) 
{
	using namespace core::database;
	core::entity e;

	index_t t[] = { tid<test_element> };
	entity_type type{ t };
	for (auto c : ctx.allocate(type))
	{
		auto buffers = (char*)ctx.get_owned_rw(c.c, tid<test_element>);
		e = ctx.get_entities(c.c)[c.start];
		test_element v{ 3 };
		forloop(i, 0, c.count)
			buffer_t<test_element>(buffers + 128*i).push(v);
	}

	core::entity e2 = pick(ctx.instantiate(e));
	auto b = buffer_t<test_element>(ctx.get_component_ro(e, tid<test_element>));
	EXPECT_EQ(b[0].v, 3);
	ctx.destroy(ctx.batch(&e, 1)[0]);

	auto b2 = buffer_t<test_element>(ctx.get_component_ro(e2, tid<test_element>));
	EXPECT_EQ(b2[0].v, 3);
	ctx.destroy(ctx.batch(&e2, 1)[0]); //ctx.destroy(e2);
}

TEST_F(DatabaseTest, Meta) 
{
	using namespace core::database;
	core::entity metae;
	core::entity e;

	{
		index_t t[] = { tid<test> };
		entity_type type{ t };
		for (auto c : ctx.allocate(type))
		{
			auto components = (test*)ctx.get_owned_rw(c.c, tid<test>);
			metae = ctx.get_entities(c.c)[c.start];
			forloop(i, 0, c.count)
				components[c.start + i].f = -1.f;
		}
	}
	{
		entity_type type({ {},{} });
		e = pick(ctx.allocate(type));
	}
	{
		core::entity me[] = { metae };
		entity_type type({ {},{me, 1} });
		type_diff addmeta{ type };
		for (auto c : ctx.batch(&e, 1))
			ctx.cast(c, addmeta);
	}
	{
		auto component = (test*)ctx.get_component_ro(metae, tid<test>);
		EXPECT_EQ(component->f, -1.f); // Shared
	}

	{
		index_t t[] = { tid<test> };
		entity_type type({ t });

		for (auto c : ctx.batch(&e, 1))
			for (auto s : ctx.cast(c, { type }))
			{
				auto tests = (test*)ctx.get_owned_rw(s.c, tid<test>);
				forloop(i, 0, s.count)
					tests[s.start + i].f = -2.f;
			}
	}
	{
		auto component = (test*)ctx.get_component_ro(e, tid<test>);
		EXPECT_EQ(component->f, -2.f); //Shared shadowed by owned
	}
}

TEST_F(DatabaseTest, DistroyMiddle) 
{
	using namespace core::database;
	core::entity es[100];
	index_t t[] = { tid<test> };
	entity_type type{ t };
	{
		int counter = 1;
		for (auto c : ctx.allocate(type, 100))
		{
			auto components = (test*)ctx.get_owned_rw(c.c, tid<test>);
			std::memcpy(es + counter - 1, ctx.get_entities(c.c), c.count * sizeof(core::entity));
			forloop(i, 0, c.count)
				components[c.start + i].v = counter++;
		}
	}
	for (auto c : ctx.batch(es + 33, 1))
		ctx.destroy(c);
	int counter = 0;

	for (auto i : ctx.query({ type })) //遍历 Archetype
	{
		for (auto j : ctx.query(i.type)) //遍历 Chunk
		{
			auto tests = (test*)ctx.get_owned_ro(j, tid<test>);
			auto num = j->get_count();
			forloop(k, 0, num) //遍历组件
				counter += tests[k].v;
		}
	}
	EXPECT_EQ(counter, (5050 - 34));
}


TEST_F(DatabaseTest, SimpleLoop)
{
	using namespace core::database;
	for (int x = 0; x < 5; x++)
	{
		index_t t[] = { tid<test> };
		entity_type type{ t };
		{
			int counter = 1;
			for (auto c : ctx.allocate(type, 100))
			{
				auto components = (test*)ctx.get_owned_rw(c.c, tid<test>);
				forloop(i, 0, c.count)
					components[c.start + i].v = counter++;
			}
		}
		int counter = 0;

		for (auto i : ctx.query({ type }))
			for (auto j : ctx.query(i.type))
			{
				auto tests = (test*)ctx.get_owned_ro(j, tid<test>);
				auto num = j->get_count();
				forloop(k, 0, num) //遍历组件
					counter += tests[k].v;
			}
		EXPECT_EQ(counter, 5050);

		for (auto i : ctx.query({ type }))
			for (auto j : ctx.query(i.type))
				ctx.destroy(j);
	}
}

TEST_F(DatabaseTest, DisableMask) 
{
	using namespace core::database;
	core::entity es[100];
	index_t t[] = { mask_id, tid<test> };
	std::sort(t, t + 2);
	index_t dt[] = { tid<test> };
	entity_type type{ t };
	{
		int counter = 1;
		for (auto c : ctx.allocate(type, 100)) //遍历创建 Entity
		{
			mask disableMask = c.c->get_mask({ dt, 1 });
			auto tests = (test*)ctx.get_owned_rw(c.c, tid<test>);
			auto masks = (mask*)ctx.get_owned_ro(c.c, mask_id);
			std::memcpy(es + counter - 1, ctx.get_entities(c.c), c.count * sizeof(core::entity));
			forloop(i, 0, c.count) //初始化 Component
			{
				auto k = c.start + i;
				tests[k].v = counter++;
				masks[k] = (mask)-1;
				if (tests[k].v % 2)
					masks[k] &= ~disableMask;
			}
		}
	}
	for (auto c : ctx.batch(es + 33, 1))
		ctx.destroy(c);

	{
		int counter = 0;
		for (auto i : ctx.query({ type }))
			for (auto j : ctx.query(i.type))
			{
				auto masks = (mask*)ctx.get_owned_ro(j, mask_id);
				auto length = j->get_count();
				forloop(k, 0, length)
				{
					if ((masks[k] & i.matched) == i.matched)  //遍历 Entity, 带禁用检查
						counter++;
				}
			}


		EXPECT_EQ(counter, 49); //只有偶数被匹配到
	}

	{
		index_t qt[] = { mask_id };
		entity_type queryType{ typeset{qt,1} };
		int counter = 0;
		for (auto i : ctx.query({ queryType })) // 空 Query
			for (auto j : ctx.query(i.type)) 
			{
				auto masks = (mask*)ctx.get_owned_ro(j, mask_id);
				auto length = j->get_count();
				forloop(k, 0, length)
				{
					if ((masks[k] & i.matched) == i.matched)  //遍历 Entity, 带禁用检查
						counter++;
				}
			}
		EXPECT_EQ(counter, 99); //全部匹配
	}

	{
		mask enableMask;
		int counter = 0;
		for (auto i : ctx.query({ type }))
		{
			auto mymask = i.matched & ~i.type->get_mask({ dt, 1 }); //关闭检查
			for (auto j : ctx.query(i.type))
			{
				auto masks = (mask*)ctx.get_owned_ro(j, mask_id);
				auto length = j->get_count();
				forloop(k, 0, length)
				{
					if ((masks[k] & mymask) == mymask)  //遍历 Entity, 带禁用检查
						counter++;
				}
			}
		}
		EXPECT_EQ(counter, 99); //全部匹配
	}
}

TEST_F(DatabaseTest, MoveContext) 
{
	using namespace core::database;
	index_t t[] = { tid<test> };
	entity_type type{ t };
	{
		int counter = 1;
		for (auto c : ctx.allocate(type, 100))
		{
			auto components = (test*)ctx.get_owned_rw(c.c, tid<test>);
			forloop(i, 0, c.count)
				components[c.start + i].v = counter++;
		}
	}
	int counter = 0;

	for (auto i : ctx.query({ type }))
		for (auto j : ctx.query(i.type))
		{
			auto tests = (test*)ctx.get_owned_ro(j, tid<test>);
			auto num = j->get_count();
			forloop(k, 0, num) 
				counter += tests[k].v;
		}
	EXPECT_EQ(counter, 5050);

	world ctx2;
	ctx2.move_context(ctx); //把所有 ctx 的内容移动到 ctx2
	int counter2 = 0;
	for (auto i : ctx2.query({ type }))
		for (auto j : ctx2.query(i.type))
		{
			auto tests = (test*)ctx.get_owned_ro(j, tid<test>);
			auto num = j->get_count();
			forloop(k, 0, num) 
				counter2 += tests[k].v;
		}
	EXPECT_EQ(counter2, 5050);

	//ctx 应该是空的
	int counter3 = 0;
	for (auto i : ctx.query({ type }))
		for (auto j : ctx.query(i.type))
		{
			auto tests = (test*)ctx.get_owned_ro(j, tid<test>);
			auto num = j->get_count();
			forloop(k, 0, num) 
				counter3 += tests[k].v;
		}
	EXPECT_EQ(counter3, 0);
}

TEST_F(DatabaseTest, Group) 
{
	using namespace core::database;
	core::entity es[10];
	{
		index_t t[] = { tid<test> };
		entity_type type{ t };
		int counter = 1;
		for (auto c : ctx.allocate(type, 10))
		{
			auto components = (test*)ctx.get_owned_rw(c.c, tid<test>);
			auto entities = ctx.get_entities(c.c);
			memcpy(es + counter - 1, entities, c.count * sizeof(core::entity));
			forloop(i, 0, c.count)
				components[c.start + i].v = counter++;
		}
	}
	{
		index_t t[] = { group_id };
		entity_type type{ t };
		type_diff addGroup{ type };
		for (auto s : ctx.batch(es, 1))
			ctx.cast(s, addGroup);
		auto group = buffer_t<core::entity>(ctx.get_owned_rw(es[0], group_id));
		group.resize(10);
		memcpy(group.data(), es, sizeof(core::entity) * group.size());
	}
	{
		index_t t[] = { tid<test_ref> };
		entity_type type{ t };
		type_diff addRef{ type };
		for (auto s : ctx.batch(es+1, 1))
			ctx.cast(s, addRef);
		auto ref = (test_ref*)ctx.get_owned_rw(es[1], tid<test_ref>);
		ref->ref = es[0];
	}
	{
		core::entity newE;
		{
			auto c = ctx.instantiate(es[0])[0];
			newE = ctx.get_entities(c.c)[c.start];
		}
		{
			index_t t[] = { group_id };
			EXPECT_TRUE(ctx.has_component(newE, { t,1 }));
		}

		index_t t[] = { tid<test> };
		entity_type type{ t };
		int counter = 0;
		for (auto i : ctx.query({ type }))
			for (auto j : ctx.query(i.type))
			{
				auto tests = (test*)ctx.get_owned_ro(j, tid<test>);
				auto num = j->get_count();
				forloop(k, 0, num) 
					counter += tests[k].v;
			}
		EXPECT_EQ(counter, 110);
		{
			auto group = buffer_t<core::entity>(ctx.get_component_ro(newE, group_id));
			EXPECT_EQ(group[0], newE);
			auto ref = (test_ref*) ctx.get_component_ro(group[1], tid<test_ref>);
			EXPECT_EQ(ref->ref, newE);
		}
	}
}

struct buffer_serializer : core::database::serializer_i
{
	std::vector<char>& buffer;
	buffer_serializer(std::vector<char>& buffer)
		: buffer(buffer) {}
	void stream(const void* data, uint32_t bytes) override
	{
		if (bytes == 0)
			return;
		size_t size = buffer.size();
		buffer.resize(size + bytes);
		memcpy(&buffer[size], data, bytes);
	}
	bool is_serialize() override { return true; }
};
struct buffer_deserializer : core::database::serializer_i
{
	std::vector<char>& buffer;
	size_t offset = 0;
	int i = 0;
	buffer_deserializer(std::vector<char>& buffer)
		: buffer(buffer) {}
	void stream(const void* data, uint32_t bytes) override
	{
		void* mdata = const_cast<void*>(data);
		if (bytes == 0)
			return;
		memcpy(mdata, &buffer[offset], bytes);
		offset += bytes;
	}
	bool is_serialize() override { return false; }
};

TEST_F(DatabaseTest, Deserialize) 
{
	using namespace core::database;
	index_t t[] = { tid<test> };
	entity_type type{ t };
	{
		int counter = 1;
		for (auto c : ctx.allocate(type, 100))
		{
			auto components = (test*)ctx.get_owned_rw(c.c, tid<test>);
			forloop(i, 0, c.count)
				components[c.start + i].v = counter++;
		}
	}
	int counter = 0;

	for (auto i : ctx.query({ type }))
		for (auto j : ctx.query(i.type))
		{
			auto tests = (test*)ctx.get_owned_ro(j, tid<test>);
			auto num = j->get_count();
			forloop(k, 0, num)
				counter += tests[k].v;
		}
	EXPECT_EQ(counter, 5050);
	std::vector<char> buffer;
	buffer_serializer bs{ buffer };
	buffer_deserializer ds{ buffer };
	ctx.serialize(&bs);
	world ctx2;
	ctx2.deserialize(&ds);
	int counter2 = 0;
	for (auto i : ctx2.query({ type }))
		for (auto j : ctx2.query(i.type))
		{
			auto tests = (test*)ctx2.get_owned_ro(j, tid<test>);
			auto num = j->get_count();
			forloop(k, 0, num)
				counter2 += tests[k].v;
		}
	EXPECT_EQ(counter2, 5050);
}

TEST_F(DatabaseTest, EntityDeserialize) 
{
	using namespace core::database;
	index_t t[] = { tid<test> };
	entity_type type{ t };
	auto e = pick(ctx.allocate(type));
	((test*)ctx.get_owned_rw(e, tid<test>))->v = -1.f;
	std::vector<char> buffer;
	buffer_serializer bs{ buffer };
	buffer_deserializer ds{ buffer };
	ctx.serialize(&bs, e);
	auto e2 = ctx.deserialize(&ds, nullptr);
	EXPECT_EQ(((test*)ctx.get_component_ro(e2, tid<test>))->v, -1.f);
}

TEST_F(DatabaseTest, GroupDeserialize)
{
	using namespace core::database;
	core::entity es[10];
	{
		index_t t[] = { tid<test> };
		entity_type type{ t };
		int counter = 1;
		for (auto c : ctx.allocate(type, 10))
		{
			auto components = (test*)ctx.get_owned_rw(c.c, tid<test>);
			auto entities = ctx.get_entities(c.c);
			memcpy(es + counter - 1, entities, c.count * sizeof(core::entity));
			forloop(i, 0, c.count)
				components[c.start + i].v = counter++;
		}
	}
	{
		index_t t[] = { group_id };
		entity_type type{ t };
		type_diff addGroup{ type };
		for (auto s : ctx.batch(es, 1))
			ctx.cast(s, addGroup);
		auto group = buffer_t<core::entity>(ctx.get_owned_rw(es[0], group_id));
		group.resize(10);
		memcpy(group.data(), es, sizeof(core::entity) * group.size());
	}
	{
		index_t t[] = { tid<test_ref> };
		entity_type type{ t };
		type_diff addRef{ type };
		for (auto s : ctx.batch(es + 1, 1))
			ctx.cast(s, addRef);
		auto ref = (test_ref*)ctx.get_owned_rw(es[1], tid<test_ref>);
		ref->ref = es[0];
	}
	{
		core::entity newE;
		{

			std::vector<char> buffer;
			buffer_serializer bs{ buffer };
			buffer_deserializer ds{ buffer };
			ctx.serialize(&bs, es[0]);
			newE = ctx.deserialize(&ds, nullptr);
		}
		{
			index_t t[] = { group_id };
			EXPECT_TRUE(ctx.has_component(newE, { t,1 }));
		}

		index_t t[] = { tid<test> };
		entity_type type{ t };
		int counter = 0;
		for (auto i : ctx.query({ type }))
			for (auto j : ctx.query(i.type))
			{
				auto tests = (test*)ctx.get_owned_ro(j, tid<test>);
				auto num = j->get_count();
				forloop(k, 0, num)
					counter += tests[k].v;
			}
		EXPECT_EQ(counter, 110);
		{
			auto group = buffer_t<core::entity>(ctx.get_component_ro(newE, group_id));
			EXPECT_EQ(group[0], newE);
			auto ref = (test_ref*)ctx.get_component_ro(group[1], tid<test_ref>);
			EXPECT_EQ(ref->ref, newE);
		}
	}
}

TEST_F(DatabaseTest, FormatTest)
{

}

TEST_F(DatabaseTest, Tag)
{
	using namespace core::database;
	index_t t[] = { tid<test_tag> };
	entity_type type{ t };
	core::entity e;
	e = pick(ctx.allocate(type));
	EXPECT_EQ(ctx.get_component_ro(e, tid<test_tag>), nullptr);
	EXPECT_EQ(ctx.get_archetype(e)->entitySize, sizeof(core::entity));
}

void install_test_components()
{
	using namespace core::database;
	
	tid<test> = register_type({ false, false, false,
		"3CFC8364-417F-45C0-B8D3-9E48F04C3DAF",
		sizeof(test) });
	tid<test_track> = register_type({ false, true, true,
		"23DFDC29-F1FC-439A-B57E-C6E86F577CA1",
		sizeof(test_track) });
	tid<test_element> = register_type({ true, false, false,
		"48D74F82-D070-4494-907A-0BC72F873FF5",
		128, sizeof(test_element) });
	tid<test_not_exist> = register_type({ false, false, false,
		"742DE13F-14B0-444C-B546-F21B99ACC335",
		0 });
	tid<test_align> = register_type({ false, false, false,
		"9C9C1EE9-1312-45E4-A746-3B7915D2B093",
		sizeof(test_align), 0, alignof(test_align) });
	{
		component_desc desc;
		desc.isElement = false; desc.manualCopy = false;
		desc.manualClean = false; desc.uuid = "45FE8BDE-209E-4736-AF5D-7136DD061D8E";
		desc.size = sizeof(test_ref); desc.elementSize = 0;
		intptr_t refs[] = { offsetof(test_ref, ref) };
		desc.entityRefs = refs; desc.entityRefCount = 1;
		tid<test_ref> = register_type(desc);
	}
	tid<test_tag> = register_type({ false, false, false,
		"6AB0D784-CD4A-4EB1-8CF9-EE8C6BADEB81",
		0 });
}

int main(int argc, char** argv)
{
	::testing::InitGoogleTest(&argc, argv);
	core::database::initialize();
	install_test_components();
	return RUN_ALL_TESTS();
}