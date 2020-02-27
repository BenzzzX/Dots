#include "MemoryModel.h"
#include "Profiler.h"
#include <iostream>
struct test
{
	int v;
	float f;
};

struct test_internal
{
	int v;
};

struct test_element
{
	int v;
};

struct test_tag {};

struct test_meta
{
	int v;
};

using namespace ecs::memory_model;

uint16_t disable_id;
uint16_t cleanup_id;
uint16_t test_id;
uint16_t test_internal_id;
uint16_t test_element_id;
uint16_t test_tag_id;
uint16_t test_meta_id;

void release_meta(metakey key)
{
	std::cout << "release meta:" << key.metatype << "\n";
}

struct initialzie
{
	initialzie()
	{
		test_id = register_type({ false, false, false, 2, sizeof(test) });
		test_internal_id = register_type({ true, false, false, 3, sizeof(test_internal) });
		test_element_id = register_type({ false, true, false, 4, 128, sizeof(test_element) });
		test_tag_id = register_type({ false, false, false, 5, 0 });
		test_meta_id = register_type({ false, false, true, 6, 0 });
		
		register_metatype_release_callback(release_meta);
	}
};

#define foriter(c, iter) for (auto c = iter.next(); c.has_value(); c = iter.next())
constexpr uint32_t size = 10000000;
constexpr uint32_t size2 = 100000;
static ecs::entity es[size];
static ecs::entity ess[size2];
int main()
{ 
	initialzie _;
	context cont;
	win32::Stopwatch sw;
	
	uint16_t t[] = { test_element_id };
	entity_type type{ {t,1}, {} };

	//uint16_t ext[] = { test_tag_id };
	//entity_type extype{ {ext,1}, {} };

	uint16_t mt[] = { 1 };
	uint16_t ext[] = { test_tag_id, test_meta_id };
	entity_type extype{ {ext,2}, {{mt, 1}, ext+1} };

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

	sw.Start();
	cont.allocate(type, es, size);
	sw.Stop();
	std::cout << "create entity(size 128byte) x10000000 Elapsed time: " << sw.ElapsedMilliseconds() << " ms\n";
	
	
	
	sw.Start(); 
	for (int i = 0; i < size2; ++i)
		ess[i] = es[i * 10];
	iter = cont.batch(ess, size2);
	foriter(c, iter)
		cont.extend(*c, extype);
	sw.Stop();
	std::cout << "add tag and metatype to sparse entity x100000 Elapsed time: " << sw.ElapsedMilliseconds() << " ms\n";
	
	sw.Start(); 
	iter = cont.batch(es, size);
	foriter(c, iter)
		cont.destroy(*c);
	sw.Stop();
	std::cout << "destory 10000000 Elapsed time: " << sw.ElapsedMilliseconds() << " ms\n";
	
	return 0;
}