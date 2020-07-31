#pragma once
#include "Database.h"
#include <span>

using namespace core::database;

struct parameter
{
	index_t type;
	bool readonly = false;
};

struct kernel_view
{
	archetype_filter af;
	chunk_filter cf;
	entity_filter ef;
	std::span<parameter> params;
	//random access parameter
	std::span<parameter> rap;
	
	bool randomRW;
};

struct transient_allocator
{
	static char data[10000];
	static size_t ptr;
	static char* alloc(size_t size);
};

struct kernel_context
{
	world* ctx;
	kernel_view view;
	uint16_t* localType;
	mask localMask;
	archetype* currArchetype;
	chunk* currChunk;
	uint32_t indexInChunk;
	uint32_t indexInKernel;
	core::entity currEntity;
	void PhaseArchetype() {}
	void PhaseChunk() {}
	void PhaseEntity() {}
};

namespace codebase
{
	class pipeline
	{
		struct kernel_pool 
		{

		};

		template<class T>
		struct kernel
		{
			T* kernel_data;

		};
	};
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

	void Update(world& ctx);

	void SetParent(world& ctx, entity e, void* data, entity parent);
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