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
constexpr uint32_t size = 10000000;
constexpr uint32_t size2 = 100000;
static core::entity es[size];
static core::entity ess[size2];
using core::database::index_t;

void TestComponent()
{
	context ctx;
	core::entity e;

	index_t t[] = { test_id };
	entity_type type({ {t,1},{} });
	auto iter = ctx.allocate(type, &e);
	foriter(c, iter)
	{
		auto components = (test*)ctx.get_owned_rw(c->c, test_id);
		forloop(i, 0, c->count)
			components[c->start + i].f = -1.f;
	}
	auto component = (test*)ctx.get_component_ro(e, test_id);
	assert(component->f == -1.f);
}

void TestLifeTime()
{
	context ctx;
	core::entity e;

	index_t t[] = { test_track_id };
	entity_type type({ {t,1},{} });
	auto iter = ctx.allocate(type, &e);
	foriter(c, iter)
	{
		auto components = (test_track*)ctx.get_owned_rw(c->c, test_track_id);
		forloop(i, 0, c->count)
			components[c->start + i].v = 2;
	}
	auto component = (test_track*)ctx.get_component_ro(e, test_track_id);
	assert(component->v == 2);
	ctx.destroy(&e, 1);
	assert(ctx.exist(e));
	assert(ctx.has_component(e, type));
	component = (test_track*)ctx.get_component_ro(e, test_track_id + 1);
	assert(component->v == 2);
	ctx.shrink(&e, 1, type);
	assert(!ctx.exist(e));
}

void TestElement()
{
	using core::database::buffer;
	context ctx;
	core::entity e;

	index_t t[] = { test_element_id };
	entity_type type({ {t,1},{} });
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
		entity_type type({ {t,1},{} });
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
		auto component = (test*)ctx.get_component_ro(metae, test_id);
		assert(component->f == -2.f); //Shared hidden by owned
	}
}

void TestIteration()
{
	context ctx;
	core::entity es[100];
	{
		index_t t[] = { test_id };
		entity_type type({ {t,1},{} });
		auto iter = ctx.allocate(type, es, 100);
		foriter(c, iter)
		{
			auto components = (test*)ctx.get_owned_rw(c->c, test_id);
			forloop(i, 0, c->count)
				components[c->start + i].f = -1.f;
		}
	}
	ctx.destroy(es + 50, 1);
	{
		index_t t[] = { test_id };
		entity_type type({ {t,1},{} });
		core::database::entity_filter filter;
		filter.all = type;
		int i = 0;
		auto iter = ctx.query(filter);
		foriter(c, iter)
		{
			i += (*c)->get_count();
		}
		assert(i == 99);
	}
}

void TestDisable()
{
	context ctx;
	core::entity e;

	index_t t[] = { test_id, mask_id };
	entity_type type({ {t,2},{} });
	auto iter = ctx.allocate(type, &e);
	foriter(c, iter)
	{
		auto components = (test*)ctx.get_owned_rw(c->c, test_id);
		forloop(i, 0, c->count)
			components[c->start + i].f = -1.f;
	}
	auto component = (test*)ctx.get_component_ro(e, test_id);
	assert(component->f == -1.f);
}

int main()
{ 
	initialzie _;
	context cont;
	win32::Stopwatch sw;
	index_t t[] = { test_element_id };
	entity_type type{ {t,1}, {} };

	//uint16_t ext[] = { test_tag_id };
	//entity_type extype{ {ext,1}, {} };
	index_t ext[] = { test_tag_id };
	entity_type extype{ {ext,2}, {} };

	sw.Start();
	cont.allocate(type, es, size); 
	sw.Stop();
	std::cout << "create entity(size 128byte) x10000000 Elapsed time: " << sw.ElapsedMilliseconds() << " ms\n";

	sw.Start();
	auto iter = cont.batch(es, size2);
	foriter(c, iter)
		cont.extend(*c, extype);
	sw.Stop();
	std::cout << "add tag and metatype to entity x100000 Elapsed time: " << sw.ElapsedMilliseconds() << " ms\n";

	sw.Start();
	iter = cont.batch(es, size);
	foriter(c, iter)
		cont.destroy(*c);
	sw.Stop();
	std::cout << "destory 10000000 Elapsed time: " << sw.ElapsedMilliseconds() << " ms\n";
	
	return 0;
}