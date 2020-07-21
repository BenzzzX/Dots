#include "MemoryModel.h"
#include "Profiler.h"
#include <iostream>
#include <assert.h>
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

using namespace core::database;

uint16_t disable_id;
uint16_t cleanup_id;
uint16_t test_id;
uint16_t test_track_id;
uint16_t test_element_id;
uint16_t test_tag_id;

struct initialzie
{
	initialzie()
	{
		test_id = register_type({ false, false, false, 10, sizeof(test) });
		test_track_id = register_type({ false, true, true, 11, sizeof(test_track) });
		test_element_id = register_type({ true, false, false, 12, 128, sizeof(test_element) });
		test_tag_id = register_type({ false, false, false, 13, 0 });
	}
};

#define foriter(c, iter) for (auto c = iter.next(); c.has_value(); c = iter.next())
#define forloop(i, z, n) for(auto i = decltype(n)(z); i<n; ++i)
using core::database::index_t;

void TestComponent()
{
	context ctx;
	core::entity e;

	index_t t[] = { test_id };
	entity_type type({ .types = {t,1} });
	auto iter = ctx.allocate(type, &e);
	foriter(c, iter)
	{
		auto components = (test*)ctx.get_owned_rw(c->c, test_id);
		forloop(i, 0, c->count)
			components[c->start + i].f = -1.f;
	}
	auto component = (test*)ctx.get_component_ro(e, test_id);
	assert(component->f == -1.f);
	ctx.destroy(&e, 1);
}

void TestLifeTime()
{
	context ctx;
	core::entity e;
	core::entity e2;

	//初始化
	index_t t[] = { test_track_id };
	entity_type type({ .types = {t,1} });
	auto iter = ctx.allocate(type, &e);
	foriter(c, iter)
	{
		auto components = (test_track*)ctx.get_owned_rw(c->c, test_track_id);
		forloop(i, 0, c->count)
			components[c->start + i].v = 2;
	}
	auto component = (test_track*)ctx.get_component_ro(e, test_track_id);
	assert(component->v == 2);

	//克隆并销毁
	ctx.instantiate(e, &e2);
	ctx.destroy(&e, 1);

	//待销毁状态
	assert(ctx.exist(e));
	assert(ctx.has_component(e, type));
	component = (test_track*)ctx.get_component_ro(e, test_track_id);
	assert(component->v == 2);
	//清理待销毁组件，完成销毁
	ctx.shrink(&e, 1, type);
	assert(!ctx.exist(e));

	//待克隆状态
	assert(!ctx.has_component(e2, type));
	component = (test_track*)ctx.get_component_ro(e2, test_track_id + 1);
	assert(component->v == 2);
	//添加待克隆组件，完成拷贝
	ctx.extend(&e2, 1, type);
	assert(ctx.has_component(e2, type));
}

void TestElement()
{
	using core::database::buffer;
	context ctx;
	core::entity e;

	index_t t[] = { test_element_id };
	entity_type type({ .types = {t,1} });
	auto iter = ctx.allocate(type, &e);
	foriter(c, iter)
	{
		auto buffers = (buffer*)ctx.get_owned_rw(c->c, test_element_id);
		test_element v{ 3 };
		forloop(i, 0, c->count)
			buffers[c->start + i].push(&v, sizeof(test_element));
	}
	auto b = (buffer*)ctx.get_component_ro(e, test_element_id);
	
	assert(((test_element*)b->data())[0].v == 3);
}

void TestMeta()
{
	context ctx;
	core::entity metae;
	core::entity e;

	{
		index_t t[] = { test_id };
		entity_type type({ .types = {t,1} });
		auto iter = ctx.allocate(type, &metae);
		foriter(c, iter)
		{
			auto components = (test*)ctx.get_owned_rw(c->c, test_id);
			forloop(i, 0, c->count)
				components[c->start + i].f = -1.f;
		}
	}
	{
		entity_type type({ {},{} });
		auto iter = ctx.allocate(type, &e);
		foriter(c, iter);
	}
	{
		core::entity me[] = { metae };
		entity_type type({ {},{me, 1} });
		ctx.extend(&e, 1, type);
	}
	{
		auto component = (test*)ctx.get_component_ro(metae, test_id);
		assert(component->f == -1.f); // Shared
	}

	{
		index_t t[] = { test_id };
		entity_type type({ {t,1},{} });
		ctx.extend(&e, 1, type);
		((test*)ctx.get_owned_rw(e, test_id))->f = -2.f;
	}
	{
		auto component = (test*)ctx.get_component_ro(e, test_id);
		assert(component->f == -2.f); //Shared hidden by owned
	}
}

void TestIteration()
{
	context ctx;
	core::entity es[100];
	index_t t[] = { test_id };
	entity_type type({ .types = {t,1} });
	{
		auto iter = ctx.allocate(type, es, 100);
		int counter = 1;
		foriter(c, iter)
		{
			auto components = (test*)ctx.get_owned_rw(c->c, test_id);
			forloop(i, 0, c->count)
				components[c->start + i].v = counter++;
		}
	}
	ctx.destroy(es + 33, 1);
	int counter = 0;

	auto titer = ctx.query({.all = type});
	foriter(i, titer) //遍历 Archetype
	{
		auto citer = ctx.query(*i, {});
		foriter(j, citer) //遍历 Chunk
		{
			auto tests = (test*)ctx.get_owned_ro(*j, test_id);
			auto num = (*j)->get_count();
			forloop(k, 0, num) //遍历组件
				counter += tests[k].v;
		}
	}
	assert(counter == (5050 - 34));
}

void TestDisable()
{
	context ctx;
	core::entity es[100];
	index_t t[] = { mask_id, test_id }; 
	std::sort(t, t + 2);
	entity_type type({ .types = {t,2} });
	{
		auto iter = ctx.allocate(type, es, 100);
		int counter = 1;
		foriter(c, iter) //遍历创建 Entity
		{
			auto components = (test*)ctx.get_owned_rw(c->c, test_id);
			forloop(i, 0, c->count) //初始化 Component
				components[c->start + i].v = counter++;
		}
	}
	ctx.destroy(es + 33, 1);

	index_t dt[] = { test_id };
	entity_type disabledType({ .types = {dt, 1} });
	{
		mask disableMask;
		auto titer = ctx.query({ .all = disabledType });
		foriter(i, titer) //遍历 Archetype
		{
			disableMask = (*i)->get_mask(disabledType);
			auto citer = ctx.query(*i, {});
			foriter(j, citer) //遍历 Chunk
			{
				auto tests = (test*)ctx.get_owned_ro(*j, test_id);
				auto masks = (mask*)ctx.get_owned_ro(*j, mask_id);
				//auto num = (*j)->get_count();
				//forloop(k, 0, num) //原始遍历，不考虑mask
				auto eiter = ctx.query(*j, { titer.get_mask() });
				foriter(k, eiter) //遍历 Entity
					if (tests[*k].v % 2)
						masks[*k].disable(disableMask);
			}
		}
	}
	
	{
		int counter = 0;
		auto titer = ctx.query({ .all = type });
		foriter(i, titer) //遍历 Archetype
		{
			auto citer = ctx.query(*i, {});
			foriter(j, citer) //遍历 Chunk
			{
				auto eiter = ctx.query(*j, {});
				foriter(k, eiter) //遍历 Entity, 带禁用检查
					counter++;
			}
		}
		assert(counter == 49); //只有偶数被匹配到
	}

	{
		index_t qt[] = { mask_id };
		entity_type queryType({ .types = {qt, 1} });
		int counter = 0;
		auto titer = ctx.query({ .all = queryType });
		foriter(i, titer) //遍历 Archetype
		{
			auto citer = ctx.query(*i, {});
			foriter(j, citer) //遍历 Chunk
			{
				auto eiter = ctx.query(*j, { titer.get_mask() });
				foriter(k, eiter) //遍历 Entity
					counter++;
			}
		}
		assert(counter == 99);
	}

	{
		mask enableMask;
		int counter = 0;
		auto titer = ctx.query({ .all = type });
		foriter(i, titer) //遍历 Archetype
		{
			auto disableMask = (*i)->get_mask(disabledType);
			auto citer = ctx.query(*i, {});
			foriter(j, citer) //遍历 Chunk
			{
				auto eiter = ctx.query(*j, 
					{ titer.get_mask().disable(disableMask) }); //关闭 test 的禁用检查
				foriter(k, eiter) //遍历 Entity
					counter++;
			}
		}
		assert(counter == 99); //所有都被匹配到
	}

}

int main()
{ 
	initialzie _;
	TestDisable();
	return 0;
}