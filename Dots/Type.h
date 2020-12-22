#pragma once
namespace core
{
	namespace database
	{

		struct entity_type
		{
			typeset types;
			metaset metatypes;

			bool operator==(const entity_type& other) const
			{
				return types == other.types && metatypes == other.metatypes;
			}

			bool operator!=(const entity_type& other) const
			{
				return !(*this == other);
			}

			struct hash
			{
				size_t operator()(const entity_type& key) const
				{
					size_t hash = hash_array(key.types.data, key.types.length);
					hash = hash_array(key.metatypes.data, key.metatypes.length, hash);
					return hash;
				}
			};

			static entity_type merge(const entity_type& lhs, const entity_type& rhs, index_t* dst, entity* metaDst)
			{
				typeset ts = typeset::merge(lhs.types, rhs.types, dst);
				metaset ms = metaset::merge(lhs.metatypes, rhs.metatypes, metaDst);
				return { ts, ms };
			}

			static entity_type substract(const entity_type& lhs, const entity_type& rhs, index_t* dst, entity* metaDst)
			{
				typeset ts = typeset::substract(lhs.types, rhs.types, dst);
				metaset ms = metaset::substract(lhs.metatypes, rhs.metatypes, metaDst);
				return { ts, ms };
			}

			int get_size() const
			{
				return types.get_size() + 
					metatypes.get_size();
			}

			entity_type clone(char*& buffer) const
			{
				return { 
					types.clone(buffer), 
					metatypes.clone(buffer) 
				};
			}
		};

		inline constexpr entity_type EmptyType;

		struct archetype_filter
		{
			entity_type all;
			entity_type any;
			entity_type none;
			typeset shared;
			typeset owned;


			struct hash
			{
				size_t operator()(const archetype_filter& key) const
				{
					size_t hash = hash_array(key.all.types.data, key.all.types.length);
					hash = hash_array(key.all.metatypes.data, key.all.metatypes.length, hash);
					hash = hash_array(key.any.types.data, key.any.types.length, hash);
					hash = hash_array(key.any.metatypes.data, key.any.metatypes.length, hash);
					hash = hash_array(key.none.types.data, key.none.types.length, hash);
					hash = hash_array(key.none.metatypes.data, key.none.metatypes.length, hash);
					hash = hash_array(key.shared.data, key.shared.length, hash);
					hash = hash_array(key.owned.data, key.owned.length, hash);
					return hash;
				}
			};

			bool operator==(const archetype_filter& other) const
			{
				return all == other.all && any == other.any &&
					none == other.none && all.metatypes == other.all.metatypes &&
					any.metatypes == other.any.metatypes && none.metatypes == other.none.metatypes&&
					shared == other.shared && owned == other.owned;
			}

			ECS_API int get_size() const;
			ECS_API archetype_filter clone(char*& buffer) const;

			bool match(const entity_type& t, const typeset& sharedT) const;
		};

		struct chunk_filter
		{
			typeset changed;
			size_t prevTimestamp;

			struct hash
			{
				size_t operator()(const chunk_filter& key) const
				{
					size_t hash = hash_array(key.changed.data, key.changed.length);
					hash = hash_array(&key.prevTimestamp, 1, hash);
					return hash;
				}
			};

			bool operator==(const chunk_filter& other) const
			{
				return changed == other.changed && prevTimestamp == other.prevTimestamp;
			}

			ECS_API int get_size() const;
			ECS_API chunk_filter clone(char*& buffer) const;

			bool match(const entity_type& t, uint32_t* timestamps) const;
		};


		//todo: should mask support none filter?
		struct entity_filter
		{
			typeset inverseMask;

			struct hash
			{
				size_t operator()(const entity_filter& key) const
				{
					size_t hash = hash_array(key.inverseMask.data, key.inverseMask.length);
					return hash;
				}
			};

			void ECS_API apply(struct matched_archetype& ma) const;

			bool operator==(const entity_filter& other) const
			{
				return inverseMask == other.inverseMask;
			}

			ECS_API int get_size() const;
			ECS_API entity_filter clone(char*& buffer) const;
		};
	}
}