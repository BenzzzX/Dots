#pragma once
#include "Database.h"

core::database::index_t disable_id;
core::database::index_t cleanup_id;

namespace TransformSystem
{
	using namespace core;
	using namespace database;

	struct vec3 { float v[3]; };
	struct matrix4x4 { float v[4][4]; };

	index_t rotation_id;
	index_t location_id;
	index_t scale_id;
	index_t local_to_world_id;
	index_t local_to_parent_id;
	index_t parent_id;
	index_t child_id;

	void Install();

	void UpdateHierachy(context& ctx);

	void UpdateLocalToX(context& ctx, index_t X, const archetype_filter& filter);

	void SolveParentToWorld(context& ctx);

	void UpdateLocalToParent(context& ctx)
	{
		archetype_filter fTransformTree;
		UpdateLocalToX(ctx, local_to_parent_id, fTransformTree);
	}

	void UpdateLocalToWorld(context& ctx)
	{
		archetype_filter fTransformRoot;
		UpdateLocalToX(ctx, local_to_world_id, fTransformRoot);
	}

	void Update(context& ctx)
	{
		UpdateHierachy(ctx);
		UpdateLocalToParent(ctx);
		UpdateLocalToWorld(ctx);
		SolveParentToWorld(ctx);
	}
}


namespace TestSystem
{
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

	index_t test_id;
	index_t test_track_id;
	index_t test_element_id;
	index_t test_tag_id;

	void Install();

	void TestComponent();

	void TestLifeTime();

	void TestElement();

	void TestMeta();

	void TestIteration();

	void TestDisable();

	void AllTests()
	{
		TestComponent();
		TestElement();
		TestLifeTime();
		TestMeta();
		TestIteration();
		TestDisable();
	}
}