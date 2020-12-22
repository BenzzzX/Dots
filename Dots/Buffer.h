#pragma once


constexpr uint32_t GroupBufferSize = sizeof(core::entity) * 5;

inline void* buffer_malloc(size_t size)
{
	return ::malloc(size);
}

inline void buffer_free(void* ptr)
{
	return ::free(ptr);
}

struct buffer
{
	char* d;
	uint16_t size;
	uint16_t capacity;

	buffer(uint16_t cap)
	{
		d = nullptr;
		capacity = cap;
		size = 0;
	}

	char* data()
	{
		if (d == nullptr) return (char*)(this + 1);
		else return d;
	}

	char* data() const
	{
		if (d == nullptr) return (char*)(this + 1);
		else return d;
	}

	void push(const void* dd, uint16_t length)
	{
		if (size + length >= capacity)
			grow();
		memcpy(data() + size, dd, length);
		size += length;
	}

	void* pop(uint16_t length)
	{
		return data() + (size -= length);
	}

	void grow(uint16_t hint = 0)
	{
		uint16_t newCap = (uint16_t)std::max((int)hint, capacity * 2);
		char* newBuffer = (char*)buffer_malloc(newCap);
		memcpy(newBuffer, data(), size);
		if (d != nullptr)
			buffer_free(d);
		d = newBuffer;
		capacity = newCap;
	}

	void shrink(uint16_t inlineSize)
	{
		if (size < inlineSize)
		{
			buffer_free(d);
			d = nullptr;
			return;
		}
		uint16_t newCap = capacity;
		while (newCap > size * 2)
			newCap /= 2;
		if (newCap != capacity)
		{
			char* newBuffer = (char*)buffer_malloc(newCap);
			memcpy(newBuffer, data(), size);
			if (d != nullptr)
				buffer_free(d);
			d = newBuffer;
			capacity = newCap;
			return;
		}
	}

	~buffer()
	{
		if (d != nullptr)
			buffer_free(d);
	}
};

namespace buffer_detail
{
	template<class V>
	struct const_iterator
	{
		using iterator_category = std::random_access_iterator_tag;

		using value_type = typename V::value_type;
		using difference_type = typename V::difference_type;
		using pointer = typename V::pointer;
		using reference = typename V::reference;

		value_type* ptr;

		const_iterator& operator++() noexcept { ++ptr; return *this; }
		void operator++(int) noexcept { ++* this; }
		bool operator==(const const_iterator& right) const noexcept { return ptr == right.ptr; }
		bool operator!=(const const_iterator& right) const noexcept { return !(*this == right); };
		const_iterator& operator+=(difference_type d) { ptr += d; return *this; }
		const_iterator& operator-=(difference_type d) { ptr -= d; return *this; }
		friend const_iterator operator+(const const_iterator& lhs, difference_type rhs) { auto result = lhs; return result += rhs; }
		friend const_iterator operator+(difference_type lhs, const const_iterator& rhs) { auto result = rhs; return result += lhs; }
		friend const_iterator operator-(const const_iterator& lhs, difference_type rhs) { auto result = lhs; return result -= rhs; }
		friend const_iterator operator-(difference_type lhs, const const_iterator& rhs) { auto result = rhs; return result -= lhs; }
		friend difference_type operator-(const const_iterator& lhs, const const_iterator& rhs) { return lhs.ptr - rhs.ptr; }
		const value_type& operator[](difference_type d) { return *(*this + d); }
		bool operator<(const const_iterator& rhs) const { return ptr < rhs.ptr; }
		bool operator>(const const_iterator& rhs) const { return ptr > rhs.ptr; }
		bool operator<=(const const_iterator& rhs) const { return ptr <= rhs.ptr; }
		bool operator>=(const const_iterator& rhs) const { return ptr >= rhs.ptr; }


		const value_type& operator*()
		{
			return *ptr;
		}
		const value_type* operator->()
		{
			return ptr;
		}
	};

	template<class V>
	struct iterator : const_iterator<V>
	{
		using iterator_category = std::random_access_iterator_tag;

		using value_type = typename V::value_type;
		using difference_type = typename V::difference_type;
		using pointer = typename V::pointer;
		using reference = typename V::reference;
		iterator() {}
		iterator(pointer ptr)
			:const_iterator<V>{ ptr } {}

		iterator& operator++() noexcept { ++this->ptr; return *this; }
		void operator++(int) noexcept { ++* this; }
		bool operator==(const iterator& right) const noexcept { return this->ptr == right.ptr; }
		bool operator!=(const iterator& right) const noexcept { return !(*this == right); };
		iterator& operator+=(difference_type d) { this->ptr += d; return *this; }
		iterator& operator-=(difference_type d) { this->ptr -= d; return *this; }
		friend iterator operator+(const iterator& lhs, difference_type rhs) { auto result = lhs; return result += rhs; }
		friend iterator operator+(difference_type lhs, const iterator& rhs) { auto result = rhs; return result += lhs; }
		friend iterator operator-(const iterator& lhs, difference_type rhs) { auto result = lhs; return result -= rhs; }
		friend iterator operator-(difference_type lhs, const iterator& rhs) { auto result = rhs; return result -= lhs; }
		friend difference_type operator-(const iterator& lhs, const iterator& rhs) { return lhs.ptr - rhs.ptr; }
		bool operator<(const iterator& rhs) const { return this->ptr < rhs.ptr; }
		bool operator>(const iterator& rhs) const { return this->ptr > rhs.ptr; }
		bool operator<=(const iterator& rhs) const { return this->ptr <= rhs.ptr; }
		bool operator>=(const iterator& rhs) const { return this->ptr >= rhs.ptr; }
		value_type& operator[](difference_type d) { return *(*this + d); }
		value_type& operator*()
		{
			return *this->ptr;
		}
		value_type* operator->()
		{
			return this->ptr;
		}
	};
}

template<class T>
class buffer_t
{
	buffer* _data;

public:
	using value_type = T;
	using pointer = T*;
	using const_pointer = const T*;
	using reference = T&;
	using const_reference = const T&;
	using size_type = size_t;
	using difference_type = size_t;

	buffer_t(const void* inData)
		:_data((buffer*)inData) {}
	T& operator[](int i)
	{
		return ((T*)_data->data())[i];
	}
	const T& operator[](int i) const
	{
		return ((const T*)_data->data())[i];
	}
	uint16_t size() const
	{
		return _data->size / sizeof(T);
	}
	using const_iterator = buffer_detail::const_iterator<buffer_t>;
	using iterator = buffer_detail::iterator<buffer_t>;
	iterator begin() noexcept
	{
		return iterator{ (T*)_data->data() };
	}
	iterator end() noexcept
	{
		return iterator{ (T*)_data->data() + (_data->size / sizeof(T)) };
	}

	const_iterator begin() const noexcept
	{
		return const_iterator{ (T*)_data->data() };
	}
	const_iterator end() const noexcept
	{
		return const_iterator{ (T*)_data->data() + (_data->size / sizeof(T)) };
	}

	template<class P>
	void push(P&& p)
	{
		_data->push(&p, sizeof(T));
	}
	void pop()
	{
		_data->pop(sizeof(T));
	}
	T& last()
	{
		((T*)_data->data())[size()];
	}
	const T& last() const
	{
		((const T*)_data->data())[size()];
	}
	void shrink(uint16_t inlineSize) noexcept
	{
		_data->shrink(inlineSize);
	}
	void reserve(uint16_t newSize) noexcept
	{
		if (newSize * sizeof(T) > _data->capacity)
			_data->grow(newSize * sizeof(T));
	}
	T* data()
	{
		return (T*)_data->data();
	}
	void resize(uint16_t newSize) noexcept
	{
		reserve(newSize);
		_data->size = newSize * sizeof(T);
	}
};

template<class T, size_t strip>
class buffer_pointer_t
{
	char* buffer;

public:
	buffer_pointer_t(void* buffer)
		:buffer((char*)buffer) {}
	buffer_t<T> operator[](size_t i)
	{
		return buffer_t<T>(buffer + strip * i);
	}

	const buffer_t<T> operator[](size_t i) const
	{
		return buffer_t<T>(buffer + strip * i);
	}

	buffer_pointer_t operator+(size_t i) const
	{
		buffer_pointer_t r{ *this };
		r.buffer = r.buffer + strip * i;
		return r;
	}

	operator bool()
	{
		return buffer != nullptr;
	}
};
