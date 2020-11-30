#pragma once
#include <cstdint>
#include <memory>
#include <algorithm>
#undef max
namespace core
{
	template<size_t A, size_t B, class T = uint32_t>
	struct handle
	{
		using underlying_type = T;
#if __cplusplus >= 201703L
		static_assert(A + B == sizeof(T) * 8);
#endif
		union
		{
			T value;
			struct
			{
				T version : B;
				T id : A;
			};
		};
		constexpr static T TransientMagicNumber = ((1 << B) - 1);
		constexpr operator T() const { return value; }
		constexpr handle() = default;
		constexpr handle(T t) : value(t) { }
		constexpr handle(T i, T v) : id(i), version(v) { }
		constexpr bool is_transient() { return version == ((1 << B) - 1); }
		constexpr static T make_transient(T i) { return handle{ i, TransientMagicNumber }.value; }
		constexpr static T recycle(T version)
		{
			return (version + 1) == TransientMagicNumber ? (version + 2) : (version + 1);
		}
	};

	struct entity : handle<24, 8>
	{
		using ut = handle<24, 8>;
		using ut::handle;
	};
	constexpr entity NullEntity = std::numeric_limits<uint32_t>::max();


	namespace database
	{
		using index_t = uint32_t;
		using tsize_t = uint16_t;
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
#if __cplusplus >= 201703L
			static_assert(std::is_trivial_v<T>, "Only trivial types can be directly hashed.");
#endif
			return hash_append(
				basis, reinterpret_cast<const unsigned char*>(data), length * sizeof(T));
		}

		template<class T>
		struct set
		{
			const T* data = nullptr;
			tsize_t length = 0;

			const T& operator[](uint32_t i) const noexcept { return data[i]; }

			bool operator==(const set& other) const
			{
				return length == other.length ? std::equal(data, data + length, other.data) : false;
			}

			struct hash
			{
				size_t operator()(const set& set) const
				{
					return hash_array(set.data, set.length);
				}
			};

			static set merge(const set& lhs, const set& rhs, T* dst)
			{
				tsize_t i = 0, j = 0, k = 0;
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

			static set substract(const set& lhs, const set& rhs, T* dst)
			{
				tsize_t i = 0, j = 0, k = 0;
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

			static set intersect(const set& lhs, const set& rhs, T* dst)
			{
				tsize_t i = 0, j = 0, k = 0;
				while (i < lhs.length && j < rhs.length)
				{
					if (lhs[i] > rhs[j])
						j++;
					else if (lhs[i] < rhs[j])
						i++;
					else
						dst[k++] = lhs[(j++, i++)];
				}
				return  { dst, k };
			}

			bool any(const set& s) const
			{
				tsize_t i = 0, j = 0;
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

			bool all(const set& s) const
			{
				tsize_t i = 0, j = 0;
				while (i < length && j < s.length)
				{
					if (data[i] > s[j])
						return false;
					else if (data[i] < s[j])
						i++;
					else
						(j++, i++);
				}
				return j == s.length;
			}
		};

		using typeset = set<index_t>;
		using metaset = set<entity>;
	}
}