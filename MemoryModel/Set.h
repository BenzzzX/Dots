#pragma once
#include <cstdint>
#include <memory>
#include <variant>

namespace ecs
{
	namespace memory_model
	{
		using index_t = uint32_t;
#if _WIN64
		constexpr size_t _FNV_offset_basis = 14695981039346656037ULL;
		constexpr size_t _FNV_prime = 1099511628211ULL;
#elif _WIN32
		constexpr size_t _FNV_offset_basis = 2166136261U;
		constexpr size_t _FNV_prime = 16777619U;
#else
#error Unknown platform
#endif

		inline size_t hash_append(size_t val, const unsigned char* const data, const size_t length) noexcept
		{ // accumulate range [data, data + length) into partial FNV-1a hash val
			for (size_t i = 0; i < length; ++i) {
				val ^= static_cast<size_t>(data[i]);
				val *= _FNV_prime;
			}

			return val;
		}

		template <class T>
		inline size_t hash_array(const T* const data, const size_t length, const size_t basis = _FNV_offset_basis) noexcept
		{ // bitwise hashes the representation of an array
			static_assert(std::is_trivial_v<T>, "Only trivial types can be directly hashed.");
			return hash_append(
				basis, reinterpret_cast<const unsigned char*>(data), length * sizeof(T));
		}


		struct typeset
		{
			const index_t* data = nullptr;
			uint16_t length = 0;

			const index_t& operator[](uint32_t i) const noexcept { return data[i]; }

			bool operator==(const typeset& other) const
			{
				return length == other.length ? std::equal(data, data + length, other.data) : false;
			}

			struct hash
			{
				size_t operator()(const typeset& set) const
				{
					return hash_array(set.data, set.length);
				}
			};

			static typeset merge(const typeset& lhs, const typeset& rhs, index_t* dst)
			{
				uint16_t i = 0, j = 0, k = 0;
				while (i < lhs.length && j < rhs.length)
				{
					if (lhs[i] > rhs[j])
						dst[k++] = rhs[j++];
					else if (lhs[i] < rhs[j])
						dst[k++] = lhs[i++];
					else
						dst[k++] = lhs[(j++, i++)];
				}
				while (i < lhs.length)
					dst[k++] = lhs[i++];
				while (j < rhs.length)
					dst[k++] = rhs[j++];
				return  { dst, k };
			}

			static typeset substract(const typeset& lhs, const typeset& rhs, index_t* dst)
			{
				uint16_t i = 0, j = 0, k = 0;
				while (i < lhs.length && j < rhs.length)
				{
					if (lhs[i] > rhs[j])
						j++;
					else if (lhs[i] < rhs[j])
						dst[k++] = lhs[i++];
					else
						(j++, i++);
				}
				while (i < lhs.length)
					dst[k++] = lhs[i++];
				return  { dst, k };
			}

			bool any(const typeset& s) const
			{
				uint16_t i = 0, j = 0;
				while (i < length && j < s.length)
				{
					if (data[i] > s[j])
						j++;
					else if (data[i] < s[j])
						i++;
					else
						return true;
				}
				return false;
			}

			bool all(const typeset& s) const
			{
				uint16_t i = 0, j = 0;
				while (i < length && j < s.length)
				{
					if (data[i] > s[j])
						j++;
					else if (data[i] < s[j])
						i++;
					else
						(j++, i++);
				}
				return j == s.length;
			}
		};


		struct metaset : typeset
		{
			const index_t* metaData = nullptr;

			const uint16_t& operator[](uint32_t i) const noexcept { return data[i]; }

			bool operator==(const metaset& other) const
			{
				return length == other.length ? std::equal(data, data + length, other.data) : false;
			}

			struct hash
			{
				size_t operator()(const metaset& set) const
				{
					return hash_array(set.data, set.length);
				}
			};

			static metaset merge(const metaset& lhs, const metaset& rhs, const index_t* dst, index_t* metaDst)
			{
				uint16_t i = 0, j = 0, k = 0;
				while (i < lhs.length && j < rhs.length)
				{
					if (lhs.metaData[i] > rhs.metaData[j])
						metaDst[k++] = rhs[j++];
					else if (lhs.metaData[i] < rhs.metaData[j])
						metaDst[k++] = lhs[i++];
					else
						metaDst[k++] = lhs[(i++, j++)];
				}
				while (i < lhs.length)
					metaDst[k++] = lhs[i++];
				while (j < rhs.length)
					metaDst[k++] = rhs[j++];

				return { {metaDst, k}, dst };
			}

			static metaset substract(const metaset& lhs, const typeset& rhs, const index_t* dst, index_t* metaDst)
			{
				uint16_t i = 0, j = 0, k = 0;
				while (i < lhs.length && j < rhs.length)
				{
					if (lhs.metaData[i] > rhs[j])
						j++;
					else if (lhs.metaData[i] < rhs[j])
						metaDst[k++] = lhs[i++];
					else
						(j++, i++);
				}
				while (i < lhs.length)
					metaDst[k++] = lhs[i++];
				return   { {metaDst, k}, dst };
			}

			bool any(const metaset& s) const
			{
				uint16_t i = 0, j = 0;
				while (i < length && j < s.length)
				{
					if (metaData[i] > s.metaData[j])
						j++;
					else if (metaData[i] < s.metaData[j])
						i++;
					else if(data[i] == s[i])
						return true;
				}
				return false;
			}

			bool all(const metaset& s) const
			{
				uint16_t i = 0, j = 0;
				while (i < length && j < s.length)
				{
					if (metaData[i] > s.metaData[j])
						j++;
					else if (metaData[i] < s.metaData[j])
						i++;
					else if(data[i] == s[i])
						(j++, i++);
				}
				return j == s.length;
			}
		};
	}
}