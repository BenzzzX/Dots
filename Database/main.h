#pragma once
#include "Database.h"


using namespace core::database;

struct View
{
	archetype_filter af;
	chunk_filter cf;
	entity_filter ef;
	typeset write;
	typeset read;
	bool randomRW;
};

namespace TransformSystem
{
	using namespace core;
	using namespace database;

	using transform = float;
	using rotation = float;
	using location = float;

	transform mul(rotation, location);
	transform mul(location, rotation);
	transform mul(transform, transform);

	index_t rotation_id;
	index_t location_id;
	//index_t scale_id;
	index_t local_to_world_id;
	index_t local_to_parent_id;
	index_t parent_id;
	index_t child_id;

	constexpr uint16_t child_size = sizeof(entity) * 4 + sizeof(buffer);

	void Install();

	void Update(context& ctx);

	void SetParent(context& ctx, entity e, void* data, entity parent);
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

	void Update()
	{
		TestComponent();
		TestElement();
		TestLifeTime();
		TestMeta();
		TestIteration();
		TestDisable();
	}
}