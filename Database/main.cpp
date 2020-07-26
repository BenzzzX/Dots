
#include "Profiler.h"
#include <iostream>
#include <assert.h>
#include "main.h"


#define foriter(c, iter) for (auto c = iter.next(); c.has_value(); c = iter.next())
#define forloop(i, z, n) for(auto i = decltype(n)(z); i<n; ++i)


int main()
{ 
	TestSystem::Install();
	TestSystem::Update();
	return 0;
}

namespace TransformSystem
{
	void UpdateHierachy(context& ctx);

	void UpdateLocalToX(context& ctx, index_t X, const archetype_filter& filter);

	void SolveParentToWorld(context& ctx);

	void RemoveChild(context& ctx, entity e, entity child);

	void AddChild(context& ctx, entity e, entity child);

	void SolvePTWRecursive(context& ctx, entity e, const transform& pltw);

	float mul(float a, float b) { return a * b; }
}

void TransformSystem::Install()
{
	rotation_id = register_type({ false, false, false, 14, sizeof(rotation) });
	location_id = register_type({ false, false, false, 15, sizeof(location) });

	local_to_world_id = register_type({ false, false, false, 16, sizeof(transform) });
	local_to_parent_id = register_type({ false, false, false, 17, sizeof(transform) });

	parent_id = register_type({ false, false, true, 18, sizeof(entity) });
	child_id = register_type({ true, false, true, 19, child_size, sizeof(entity) });
}

void TransformSystem::SetParent(context& ctx, entity e, void* data, entity inParent)
{
	if (data == nullptr)
		data = ctx.get_owned_rw(e, parent_id);
	auto& parent = *(entity*)data;
	
	RemoveChild(ctx, parent, e);
	parent = inParent;
	AddChild(ctx, parent, e);
}

void TransformSystem::RemoveChild(context& ctx, entity e, entity child)
{
	auto cs = (buffer*)ctx.get_owned_ro(e, child_id);
	if (cs == nullptr)
		return;
	auto ents = (entity*)cs->data();
	auto end = ents + cs->size / sizeof(entity);
	auto ref = std::find(ents, end, child);
	if (ref == end)
		return;
	std::swap(*ents, *(end - 1));
	cs->pop(sizeof(entity));
}

void TransformSystem::AddChild(context& ctx, entity e, entity child)
{
	auto cs = (buffer*)ctx.get_owned_ro(e, child_id);
	if (cs == nullptr)
		return;
	cs->push(&child, sizeof(entity));
}

void TransformSystem::UpdateHierachy(context& ctx)
{
	index_t _cleanT[] = { cleanup_id };
	index_t _toCleanT[] = { parent_id, child_id };
	index_t _ltpT[] = { local_to_parent_id };
	index_t _parentT[] = { parent_id };
	index_t _treeT[] = { local_to_parent_id , parent_id };
	{
		//清理删除 Entity 后的层级关系

		auto titer = ctx.query({
		.all = {.types = {_cleanT,1} },
		.any = {.types = {_toCleanT,2} } });
		foriter(i, titer)
		{
			auto citer = ctx.query(*i, {});
			foriter(j, citer)
			{
				auto parents = (entity*)ctx.get_owned_ro(*j, parent_id);
				auto childs = (char*)ctx.get_owned_ro(*j, child_id);
				auto entities = ctx.get_entities(*j);
				auto num = (*j)->get_count();
				if (parents != nullptr)
				{ 
					//清理被删除的 Child
					forloop(k, 0, num)
						RemoveChild(ctx, parents[k], entities[k]);
				}
				if (childs != nullptr)
				{  
					//清理被删除的 Parent
					forloop(k, 0, num)
					{
						auto cs = (buffer*)(childs + child_size * k);
						auto ents = (entity*)cs->data();
						auto count = cs->size / (uint16_t)sizeof(entity);
						ctx.shrink(ents, count, { .types = {_treeT, 2} });
					}
				}
			}
		}
	}
	{
		//修复丢失的 local to parent
		auto titer = ctx.query({
		.all = {.types = {_parentT,1} },
		.none = {.types = {_ltpT,1} } });
		foriter(i, titer) 
		{
			auto citer = ctx.query(*i, {});
			ctx.extend(*i, { .types = {_ltpT, 1} });
		}
	}
	//需不需要检查 Parent 和 Child 的合法性？
}

void TransformSystem::UpdateLocalToX(context& ctx, index_t X, const archetype_filter& filter)
{
	auto titer = ctx.query(filter);
	foriter(i, titer) //遍历 Archetype
	{
		auto citer = ctx.query(*i, {});
		foriter(j, citer) //遍历 Chunk
		{
			auto trans = (location*)ctx.get_owned_ro(*j, location_id);
			auto rots = (rotation*)ctx.get_owned_ro(*j, rotation_id);
			auto ltxs = (transform*)ctx.get_owned_rw(*j, X);
			auto eiter = ctx.query(*j);
			foriter(k, eiter) //遍历 Entity
				ltxs[*k] = mul(trans[*k], rots[*k]);
		}
	}
}


void TransformSystem::SolvePTWRecursive(context& ctx, entity e, const transform& pltw)
{
	auto ltw = (transform*)ctx.get_owned_rw(e, local_to_world_id);
	auto ltp = (transform*)ctx.get_owned_ro(e, local_to_parent_id);
	*ltw = mul(pltw, (*ltp));

	auto cs = (buffer*)ctx.get_owned_ro(e, child_id);
	if (cs == nullptr)
		return;
	auto ents = (entity*)cs->data();
	auto count = cs->size / sizeof(entity);
	forloop(l, 0, count)
	{
		auto child = ents[l];
		SolvePTWRecursive(ctx, child, *ltw);
	}
}


void TransformSystem::SolveParentToWorld(context& ctx)
{
	index_t _rootT[] = { local_to_world_id, child_id };
	index_t _treeT[] = { parent_id };
	archetype_filter fTransformRoot{
		.all = {.types = {_rootT,2}},
		.none = {.types = {_treeT,1}}
	};
	auto titer = ctx.query(fTransformRoot);
	foriter(i, titer) //遍历 Archetype
	{
		auto citer = ctx.query(*i, {});
		foriter(j, citer) //遍历 Chunk
		{
			auto childs = (char*)ctx.get_owned_ro(*j, child_id);
			auto pltw = (transform*)ctx.get_owned_rw(*j, local_to_world_id);
			auto entities = ctx.get_entities(*j);
			auto num = (*j)->get_count();
			forloop(k, 0, num) //遍历组件
			{
				auto cs = (buffer*)(childs + child_size * k);
				auto ents = (entity*)cs->data();
				auto count = cs->size / sizeof(entity);
				forloop(l, 0, count)
					SolvePTWRecursive(ctx, ents[l], *pltw);
			}
		}
	}
}

void TransformSystem::Update(context& ctx)
{
	//维护层级结构
	UpdateHierachy(ctx); 
	//求解出根节点的世界坐标
	{
		index_t _treeT[] = { local_to_world_id, parent_id, rotation_id, location_id };
		UpdateLocalToX(ctx, local_to_parent_id, {
			.all = {.types = {_treeT, 4}} });
	}
	//求解出非根节点的局部坐标
	{
		index_t _rootT[] = { local_to_world_id , rotation_id, location_id };
		index_t _treeT[] = { parent_id };
		UpdateLocalToX(ctx, local_to_world_id, {
			.all = {.types = {_rootT,4}},
			.none = {.types = {_treeT,1}} });
	}
	//递归计算非根节点的世界坐标
	SolveParentToWorld(ctx);
}

void TestSystem::Install()
{
	test_id = register_type({ false, false, false, 10, sizeof(test) });
	test_track_id = register_type({ false, true, true, 11, sizeof(test_track) });
	test_element_id = register_type({ true, false, false, 12, 128, sizeof(test_element) });
	test_tag_id = register_type({ false, false, false, 13, 0 });
}

void TestSystem::TestComponent()
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

void TestSystem::TestLifeTime()
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
	assert(ctx.has_component(e, { t,1 }));
	component = (test_track*)ctx.get_component_ro(e, test_track_id);
	assert(component->v == 2);
	//清理待销毁组件，完成销毁
	ctx.shrink(&e, 1, type);
	assert(!ctx.exist(e));

	//待克隆状态
	assert(!ctx.has_component(e2, { t,1 }));
	component = (test_track*)ctx.get_component_ro(e2, test_track_id + 1);
	assert(component->v == 2);
	//添加待克隆组件，完成拷贝
	ctx.extend(&e2, 1, type);
	assert(ctx.has_component(e2, { t,1 }));
}

void TestSystem::TestElement()
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

void TestSystem::TestMeta()
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

void TestSystem::TestIteration()
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

	auto titer = ctx.query({ .all = type });
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

void TestSystem::TestDisable()
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
			disableMask = (*i)->get_mask({ dt, 1 });
			auto citer = ctx.query(*i, {});
			foriter(j, citer) //遍历 Chunk
			{
				auto tests = (test*)ctx.get_owned_ro(*j, test_id);
				auto masks = (mask*)ctx.get_owned_ro(*j, mask_id);
				//auto num = (*j)->get_count();
				//forloop(k, 0, num) //原始遍历，不考虑mask
				auto eiter = ctx.query(*j);
				foriter(k, eiter) //遍历 Entity
					if (tests[*k].v % 2)
						masks[*k] &= ~disableMask;
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
				auto eiter = ctx.query(*j);
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
			auto mask = titer.get_mask({ dt, 1 });
			auto citer = ctx.query(*i, {});
			foriter(j, citer) //遍历 Chunk
			{
				auto eiter = ctx.query(*j, mask); //关闭 test 的禁用检查
				foriter(k, eiter) //遍历 Entity
					counter++;
			}
		}
		assert(counter == 99); //所有都被匹配到
	}

}