#include <iostream>
#include <assert.h>
#include "main.h"

#define forloop(i, z, n) for(auto i = decltype(n)(z); i<n; ++i)

namespace Util
{
	using namespace core::database;
	using core::entity;
	void Cast(world& ctx, std::span<entity> es, type_diff diff)
	{
		for (auto c : ctx.batch(es.data(), es.size()))
			ctx.cast(c, diff);
	}
	void Cast(world& ctx, chunk_slice c, type_diff diff)
	{
		ctx.cast(c, diff);
	}
}

namespace TransformSystem
{
	void UpdateHierachy(world& ctx);

	void UpdateLocalToX(world& ctx, index_t X, const archetype_filter& filter);

	void SolveParentToWorld(world& ctx);

	void RemoveChild(world& ctx, entity e, entity child);

	void AddChild(world& ctx, entity e, entity child);

	void SolvePTWRecursive(world& ctx, entity e, const transform& pltw);

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

void TransformSystem::SetParent(world& ctx, entity e, void* data, entity inParent)
{
	if (data == nullptr)
		data = ctx.get_owned_rw(e, parent_id);
	auto& parent = *(entity*)data;
	
	RemoveChild(ctx, parent, e);
	parent = inParent;
	AddChild(ctx, parent, e);
}

void TransformSystem::RemoveChild(world& ctx, entity e, entity child)
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

void TransformSystem::AddChild(world& ctx, entity e, entity child)
{
	auto cs = (buffer*)ctx.get_owned_ro(e, child_id);
	if (cs == nullptr)
		return;
	cs->push(&child, sizeof(entity));
}

void TransformSystem::UpdateHierachy(world& ctx)
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
		for(auto i : titer)
		{
			for(auto j : ctx.query(i.type, {}))
			{
				auto parents = (entity*)ctx.get_owned_ro(j, parent_id);
				auto childs = (char*)ctx.get_owned_ro(j, child_id);
				auto entities = ctx.get_entities(j);
				auto num = j->get_count();
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
						size_t count = cs->size / (uint16_t)sizeof(entity);
						Util::Cast(ctx, { ents, count }, { .shrink = {.types = {_treeT, 2} } });
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
		for (auto i : titer)
			for(auto j : ctx.query(i.type, {}))
				Util::Cast(ctx, j, { .extend = {.types = {_ltpT, 1} } });
	}
	//需不需要检查 Parent 和 Child 的合法性？
}

void TransformSystem::UpdateLocalToX(world& ctx, index_t X, const archetype_filter& filter)
{
	for(auto i : ctx.query(filter)) //遍历 Archetype
	{
		for(auto j : ctx.query(i.type, {})) //遍历 Chunk
		{
			auto trans = (location*)ctx.get_owned_ro(j, location_id);
			auto rots = (rotation*)ctx.get_owned_ro(j, rotation_id);
			auto ltxs = (transform*)ctx.get_owned_rw(j, X);
			forloop(k, 0, j->get_count()) //遍历 Entity
				ltxs[k] = mul(trans[k], rots[k]);
		}
	}
}


void TransformSystem::SolvePTWRecursive(world& ctx, entity e, const transform& pltw)
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


void TransformSystem::SolveParentToWorld(world& ctx)
{
	index_t _rootT[] = { local_to_world_id, child_id };
	index_t _treeT[] = { parent_id };
	archetype_filter fTransformRoot{
		.all = {.types = {_rootT,2}},
		.none = {.types = {_treeT,1}}
	};
	for(auto i : ctx.query(fTransformRoot)) //遍历 Archetype
	{
		for (auto j : ctx.query(i.type, {})) //遍历 Chunk
		{
			auto childs = (char*)ctx.get_owned_ro(j, child_id);
			auto pltw = (transform*)ctx.get_owned_rw(j, local_to_world_id);
			auto num = j->get_count();
			forloop(k, 0, num) //遍历组件
				for(auto child : buffer_t<entity>(childs + child_size * k))
					SolvePTWRecursive(ctx, child, pltw[k]);
		}
	}
}

void TransformSystem::Update(world& ctx)
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