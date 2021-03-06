#include "Codebase.h"
#include <set>
#pragma once
#define forloop(i, z, n) for(auto i = std::decay_t<decltype(n)>(z); i<(n); ++i)
namespace core
{
	namespace codebase
	{
		template<class P>
		std::shared_ptr<P> pipeline::create_custom_pass(gsl::span<shared_entry> sharedEntries)
		{
			char* buffer = (char*)::malloc(sizeof(P));
			P* k = new(buffer) P{ *this };
			std::shared_ptr<P> ret{ k };
			k->passIndex = passIndex++;
			setup_custom_pass_dependency(ret, sharedEntries);
			return ret;
		}
		template<class P, class T>
		std::shared_ptr<P> pipeline::create_pass(const filters& v, T paramList, gsl::span<shared_entry> sharedEntries)
		{
			static_assert(hana::is_a<hana::tuple_tag>(paramList), "parameter list should be a hana::list");

			auto paramCount = hana::length(paramList).value;
			auto archs = query(v.archetypeFilter);
			constexpr size_t bits = std::numeric_limits<index_t>::digits;
			const auto bal = paramCount / bits + 1;
			
			size_t bufferSize =
				sizeof(P) //自己
				+ v.get_size()
				+ archs.size * (sizeof(void*) + sizeof(mask)) // mask + archetype
				+ paramCount * sizeof(index_t) * (archs.size + 1) //type + local type list
				+ bal * sizeof(index_t) * 2; //readonly + random access
			char* buffer = (char*)::malloc(bufferSize);
			P* k = new(buffer) P{ *this };
			std::shared_ptr<P> ret{ k };
			buffer += sizeof(P);
			k->passIndex = passIndex++;
			k->archetypeCount = (int)archs.size;
			k->paramCount = (int)paramCount;
			k->archetypes = allocate_inplace<archetype*>(buffer, archs.size);
			k->matched = allocate_inplace<mask>(buffer, archs.size);
			k->types = allocate_inplace<index_t>(buffer, paramCount);
			k->readonly = allocate_inplace<index_t>(buffer, bal);
			k->randomAccess = allocate_inplace<index_t>(buffer, bal);
			memset(k->readonly, 0, sizeof(index_t) * bal);
			memset(k->randomAccess, 0, sizeof(index_t) * bal);
			k->localType = allocate_inplace<index_t>(buffer, paramCount * archs.size);
			k->filter = v.clone(buffer);
			k->hasRandomWrite = false;
			int t = 0;
			hana::for_each(paramList, [&](auto p)
				{
					using type = decltype(p);
					k->types[t] = cid<typename decltype(p.comp_type)::type>;
					if(type::readonly)
						set_bit(k->readonly, t);
					if(type::randomAccess)
						set_bit(k->randomAccess, t);
					k->hasRandomWrite |= (type::randomAccess && !type::readonly);
					t++;
				});
			int counter = 0;
			for (auto i : archs)
			{
				k->archetypes[counter] = i.type;
				v.entityFilter.apply(i);
				k->matched[counter] = i.matched;
				forloop(j, 0, paramCount)
					k->localType[j + counter * paramCount] = i.type->index(k->types[j]);
				counter++;
			}
			setup_pass_dependency(ret, sharedEntries);
			return ret;
		}

		template<class P>
		std::pair<chunk_vector<task>, chunk_vector<task_group>> pipeline::create_tasks(P& k, int batchCount)
		{
			int indexInKernel = 0;
			chunk_vector<task> result;
			chunk_vector<task_group> groups;
			task_group group;
			group.begin = group.end = 0;
			int batch = batchCount;
			forloop(i, 0, k.archetypeCount)
				for (auto c : k.ctx.query(k.archetypes[i], k.filter.chunkFilter))
				{
					uint32_t allocated = 0;
					while (allocated != c->get_count())
					{
						uint32_t sliceCount;
						sliceCount = std::min(c->get_count() - allocated, (uint32_t)batch);
						task newTask{ };
						newTask.gid = i;
						newTask.slice = chunk_slice{ c, allocated, sliceCount };
						newTask.indexInKernel = indexInKernel;
						allocated += sliceCount;
						indexInKernel += sliceCount;
						batch -= sliceCount;
						result.push(newTask);

						if (batch == 0)
						{
							group.end = result.size;
							groups.push(group);
							group.begin = group.end;
							batch = batchCount;
						}
					}
				}
			if (group.end != result.size)
			{
				group.end = result.size;
				groups.push(group);
			}
			return { std::move(result), std::move(groups) };
		}
		template<class base>
		uint32_t pass_t<base>::calc_size() const
		{
			uint32_t entityCount = 0;
			if (filter.chunkFilter.changed.length > 0)
				forloop(i, 0, archetypeCount)
					base::ctx.sync_archetype(archetypes[i]);
			auto& wrd = (world&)base::ctx;
			forloop(i, 0, archetypeCount)
			for (auto j : wrd.query(archetypes[i], filter.chunkFilter))
				entityCount += j->get_count();
			return entityCount;
		}

		namespace detail
		{
			struct weak_ptr_compare
			{
				template<class T>
				bool operator() (const std::weak_ptr<T>& lhs, const std::weak_ptr<T>& rhs)const
				{
					return lhs.owner_before(rhs);
				}
			};
			using weak_ptr_set = std::set<std::weak_ptr<custom_pass>, weak_ptr_compare>;
		}
		template<class P>
		void setup_shared_dependency(std::shared_ptr<P>& k, gsl::span<shared_entry> sharedEntries, detail::weak_ptr_set& dependencies)
		{
			for (auto& i : sharedEntries)
			{
				auto& entry = i.entry;
				if (i.readonly)
				{
					if (!entry.owned.expired())
						dependencies.insert(entry.owned);
					entry.shared.erase(remove_if(entry.shared.begin(), entry.shared.end(), [](auto& n) {return n.expired(); }), entry.shared.end());
					entry.shared.push_back(k);
				}
				else
				{
					for (auto dp : entry.shared)
						dependencies.insert(dp);
					if (entry.shared.empty() && !entry.owned.expired())
						dependencies.insert(entry.owned);
					entry.shared.clear();
					entry.owned = k;
				}
			}
		}

		template<class P>
		void pipeline::setup_custom_pass_dependency(std::shared_ptr<P>& k, gsl::span<shared_entry> sharedEntries)
		{
			detail::weak_ptr_set dependencies;
			setup_shared_dependency(k, sharedEntries, dependencies);
			k->dependencies = new std::weak_ptr<custom_pass>[dependencies.size()];
			k->dependencyCount = static_cast<int>(dependencies.size());
			int i = 0;
			for (auto dp : dependencies)
				k->dependencies[i++] = dp;
		}

		template<class P>
		void pipeline::setup_pass_dependency(std::shared_ptr<P>& k, gsl::span<shared_entry> sharedEntries)
		{
			constexpr uint16_t InvalidIndex = (uint16_t)-1;
			detail::weak_ptr_set dependencies;
			std::set<std::pair<archetype*, index_t>> syncedEntry;
			setup_shared_dependency(k, sharedEntries, dependencies);

			struct HELPER
			{
				static archetype* get_owning_archetype(world* ctx, archetype* sharing, index_t type)
				{
					if (sharing->index(type) != InvalidIndex)
						return sharing;
					entity* metas = sharing->metatypes;
					forloop(i, 0, sharing->metaCount)
						if (archetype* owning = get_owning_archetype(ctx, ctx->get_archetype(metas[i]), type))
							return owning;
					return nullptr;
				}
			};

			auto sync_entry = [&](archetype* at, index_t localType, bool readonly)
			{
				auto pair = std::make_pair(at, localType);
				if (syncedEntry.find(pair) != syncedEntry.end())
					return;
				syncedEntry.insert(pair);
				auto iter = dependencyEntries.find(at);
				if (iter == dependencyEntries.end())
					return;

				auto entries = (*iter).second.get();
				if (localType >= at->firstTag || localType == InvalidIndex)
					return;
				auto& entry = entries[localType];
				if (readonly)
				{
					if (!entry.owned.expired())
						dependencies.insert(entry.owned);
					entry.shared.erase(remove_if(entry.shared.begin(), entry.shared.end(), [](auto& n) {return n.expired(); }), entry.shared.end());
					entry.shared.push_back(k);
				}
				else
				{
					for (auto& dp : entry.shared)
						if (!dp.expired())
							dependencies.insert(dp);
					if (entry.shared.empty() && !entry.owned.expired())
						dependencies.insert(entry.owned);
					entry.shared.clear();
					entry.owned = k;
				}
			};

			auto sync_type = [&](index_t type, bool readonly)
			{
				for (auto& pair : dependencyEntries)
				{
					index_t localType = pair.first->index(type);
					auto entries = pair.second.get();
					if (localType >= pair.first->firstTag || localType == InvalidIndex)
						return;
					auto& entry = entries[localType];
					if (readonly)
					{
						if (!entry.owned.expired())
							dependencies.insert(entry.owned);
						entry.shared.erase(remove_if(entry.shared.begin(), entry.shared.end(), [](auto& n) {return n.expired(); }), entry.shared.end());
						entry.shared.push_back(k);
					}
					else
					{
						for (auto& dp : entry.shared)
							if (!dp.expired())
								dependencies.insert(dp);
						if (entry.shared.empty() && !entry.owned.expired())
							dependencies.insert(entry.owned);
						entry.shared.clear();
						entry.owned = k;
					}
				}
			};

			auto sync_entities = [&](archetype* at)
			{
				auto iter = dependencyEntries.find(at);
				if (iter == dependencyEntries.end())
					return;
				auto entries = (*iter).second.get();
				auto& entry = entries[at->firstTag];
				entry.shared.erase(remove_if(entry.shared.begin(), entry.shared.end(), [](auto& n) {return n.expired(); }), entry.shared.end());
				entry.shared.push_back(k);
			};

			forloop(i, 0, k->archetypeCount)
			{
				archetype* at = k->archetypes[i];
				forloop(j, 0, k->paramCount)
				{
					if (check_bit(k->randomAccess, j))
					{
						sync_type(k->types[j], check_bit(k->readonly, j));
					}
					else
					{
						auto localType = k->localType[i * k->paramCount + j];
						if (localType == InvalidIndex)
						{
							//assert(check_bit(k->readonly, j))
							auto type = k->types[j];
							auto oat = HELPER::get_owning_archetype((world*)this, at, type);
							if (!oat) // 存在 any 时可能出现
								continue;
							sync_entry(oat, oat->index(type), true);
						}
						else
							sync_entry(at, localType, check_bit(k->readonly, j));
					}
				}
				sync_entities(at);
				auto& changed = k->filter.chunkFilter.changed;
				forloop(j, 0, changed.length)
				{
					auto localType = at->index(changed[j]);
					sync_entry(at, localType, true);
				}
			}
			if (dependencies.size() > 0)
				k->dependencies = new std::weak_ptr<custom_pass>[dependencies.size()];
			else
				k->dependencies = nullptr;
			k->dependencyCount = static_cast<int>(dependencies.size());
			int i = 0;
			for (auto dp : dependencies)
				k->dependencies[i++] = dp;
		}

		template<class P, class ...params>
		template<class T>
		inline constexpr auto operation<P, params...>::param_id()
		{
			return *hana::index_if(compList, hana::_ == hana::type_c<T>);
		}

		template<class P, class ...params>
		template<class T>
		detail::array_ret_t<T> operation<P, params...>::get_parameter()
		{
			constexpr uint16_t InvalidIndex = (uint16_t)-1;
			using DT = std::remove_const_t<T>;
			//using value_type = component_value_type_t<DT>;
			auto paramId_c = param_id<DT>();
			auto param = hana::at(paramList, paramId_c);
			using return_type = detail::array_ret_t<T>;
			//if (localType >= ctx.archetypes[gid]->firstTag)
			//	return (return_type)nullptr;
			void* ptr = nullptr;
			int paramId = paramId_c.value;
			auto localType = ctx.localType[gid * ctx.paramCount + paramId];
			auto& wrd = (world&)ctx.ctx;
			if constexpr (param.readonly)
			{
				static_assert(std::is_const_v<T>, "Can only perform const-get for readonly params.");
				if (localType == InvalidIndex)
					ptr = const_cast<void*>(wrd.get_shared_ro(ctx.archetypes[gid], ctx.types[paramId]));
				else
					ptr = const_cast<void*>(wrd.get_owned_ro_local(slice.c, localType));
			}
			else
			{
				if (localType == InvalidIndex)
					ptr = nullptr; // 不允许非 owner 修改 share 的值
				else
					ptr = const_cast<void*>(wrd.get_owned_rw_local(slice.c, localType));
			}
			return (ptr && localType != InvalidIndex) ? (return_type)ptr + slice.start : (return_type)ptr;
		}

		template<class P, class ...params>
		template<class... Ts>
		std::tuple<detail::array_ret_t<Ts>...> operation<P, params...>::get_parameters()
		{
			return std::make_tuple(get_parameter<Ts>()...);
		}

		template<class P, class ...params>
		template<class T>
		detail::array_ret_t<T> operation<P, params...>::get_parameter_owned()
		{
			constexpr uint16_t InvalidIndex = (uint16_t)-1;
			//using value_type = component_value_type_t<std::decay_t<T>>;
			auto paramId_c = param_id<std::decay_t<T>>();
			auto param = hana::at(paramList, paramId_c);
			using return_type = detail::array_ret_t<T>;
			//if (localType >= ctx.archetypes[gid]->firstTag)
			//	return (return_type)nullptr;
			void* ptr = nullptr;
			int paramId = paramId_c.value;
			auto localType = ctx.localType[gid * ctx.paramCount + paramId];
			auto& wrd = (world&)ctx.ctx;
			if constexpr (param.readonly)
			{ 
				static_assert(std::is_const_v<T>, "Can only perform const-get for readonly params.");
				ptr = const_cast<void*>(wrd.get_owned_ro_local(slice.c, localType));
			}
			else
				ptr = const_cast<void*>(wrd.get_owned_rw_local(slice.c, localType));
			return (ptr && localType != InvalidIndex) ? (return_type)ptr + slice.start : (return_type)ptr;
		}

		template<class P, class ...params>
		template<class... Ts>
		std::tuple<detail::array_ret_t<Ts>...> operation<P, params...>::get_parameters_owned()
		{
			return std::make_tuple(get_parameter_owned<Ts>()...);
		}

		template<class P, class ...params>
		template<class T>
		detail::value_ret_t<T> operation<P, params...>::get_parameter(entity e)
		{
			//using value_type = component_value_type_t<std::decay_t<T>>;
			auto paramId_c = param_id<std::decay_t<T>>();
			int paramId = paramId_c.value;
			auto param = hana::at(paramList, paramId_c);
			static_assert(param.randomAccess, "only random access parameter can be accessed by entity");
			using return_type = detail::value_ret_t<T>;
			auto& wrd = (world&)ctx.ctx;
			if constexpr (param.readonly)
			{
				static_assert(std::is_const_v<T>, "Can only perform const-get for readonly params.");
				return (return_type)const_cast<void*>(wrd.get_component_ro(e, ctx.types[paramId]));
			}
			else
				return (return_type)const_cast<void*>(wrd.get_owned_rw(e, ctx.types[paramId]));
		}
		
		template<class P, class ...params>
		template<class... Ts>
		std::tuple<detail::array_ret_t<Ts>...> operation<P, params...>::get_parameters(entity e)
		{
			return std::make_tuple(get_parameter<Ts>(e)...);
		}

		template<class P, class ...params>
		template<class T>
		detail::value_ret_t<T> operation<P, params...>::get_parameter_owned(entity e)
		{
			//using value_type = component_value_type_t<std::decay_t<T>>;
			auto paramId_c = param_id<std::decay_t<T>>();
			int paramId = paramId_c.value;
			auto param = hana::at(paramList, paramId_c);
			static_assert(param.randomAccess, "only random access parameter can be accessed by entity");
			using return_type = detail::value_ret_t<T>;
			auto& wrd = (world&)ctx.ctx;
			if constexpr (param.readonly)
			{
				static_assert(std::is_const_v<T>, "Can only perform const-get for readonly params.");
				return (return_type)const_cast<void*>(wrd.get_owned_ro(e, ctx.types[paramId]));
			}
			else
				return (return_type)const_cast<void*>(wrd.get_owned_rw(e, ctx.types[paramId]));
		}

		template<class P, class ...params>
		template<class... Ts>
		std::tuple<detail::array_ret_t<Ts>...> operation<P, params...>::get_parameters_owned(entity e)
		{
			return std::make_tuple(get_parameter_owned<Ts>(e)...);
		}
	}
}
#undef forloop