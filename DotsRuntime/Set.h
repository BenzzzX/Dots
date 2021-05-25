#pragma once
#include <cstdint>
#include <memory>
#include <algorithm>
#undef max
namespace core
{
	using tsize_t = uint16_t;
	namespace database
	{
		template<class T>
		struct set
		{
			const T* data = nullptr;
			tsize_t length = 0;

			constexpr set() {}
			constexpr set(const T* data, tsize_t length)
				:data(data), length(length) {}
			template<int n>
			constexpr set(T (&data)[n])
				:data(data), length(n) 
			{
				std::sort(data, data + n);
			}

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

			int get_size() const
			{
				return sizeof(T) * length;
			}

			set clone(char*& buffer) const
			{
				set s{ (T*)buffer, length };
				std::memcpy(buffer, data, sizeof(T) * length);
				buffer += sizeof(T) * length;
				return s;
			}

			static set merge(const set& lhs, const set& rhs, void* d)
			{
				tsize_t i = 0, j = 0, k = 0;
				T* dst = (T*)d;
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

			static set substract(const set& lhs, const set& rhs, void* d)
			{
				tsize_t i = 0, j = 0, k = 0;
				T* dst = (T*)d;
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

			static set intersect(const set& lhs, const set& rhs, void* d)
			{
				tsize_t i = 0, j = 0, k = 0;
				T* dst = (T*)d;
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
	}
}