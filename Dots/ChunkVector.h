#pragma once


struct chunk_vector_base
{
	constexpr static size_t kChunkSize = kFastBinSize;
	size_t chunkSize = 0;
	size_t size = 0;
	size_t chunkCapacity = kChunkSize;
	void** data = nullptr;

	void grow();
	void shrink(size_t n);
	void reset();
	void flatten(void* dst, size_t eleSize);
	void zero(size_t eleSize);

	chunk_vector_base() = default;
	chunk_vector_base(chunk_vector_base&& r) noexcept;
	chunk_vector_base(const chunk_vector_base& r) noexcept;
	chunk_vector_base& operator=(chunk_vector_base&& r) noexcept;
	~chunk_vector_base();
};

namespace chunk_vector_detail
{
	template<class V>
	struct const_iterator
	{
		using iterator_category = std::random_access_iterator_tag;

		using value_type = typename V::value_type;
		using difference_type = typename V::difference_type;
		using pointer = typename V::pointer;
		using reference = typename V::reference;


		size_t i;
		void** c;

		const_iterator& operator++() noexcept { ++i; return *this; }
		const_iterator operator++(int) noexcept { auto v = *this; ++* this; return v; }
		bool operator==(const const_iterator& right) const noexcept { return i == right.i && c == right.c; }
		bool operator!=(const const_iterator& right) const noexcept { return !(*this == right); };
		const_iterator& operator+=(difference_type d) { i += d; return *this; }
		const_iterator& operator-=(difference_type d) { i -= d; return *this; }
		friend const_iterator operator+(const const_iterator& lhs, difference_type rhs) { auto result = lhs; return result += rhs; }
		friend const_iterator operator+(difference_type lhs, const const_iterator& rhs) { auto result = rhs; return result += lhs; }
		friend const_iterator operator-(const const_iterator& lhs, difference_type rhs) { auto result = lhs; return result -= rhs; }
		friend const_iterator operator-(difference_type lhs, const const_iterator& rhs) { auto result = rhs; return result -= lhs; }
		friend difference_type operator-(const const_iterator& lhs, const const_iterator& rhs) { return lhs.i - rhs.i; }
		const value_type& operator[](difference_type d) { return *(*this + d); }
		bool operator<(const const_iterator& rhs) const { return i < rhs.i; }
		bool operator>(const const_iterator& rhs) const { return i > rhs.i; }
		bool operator<=(const const_iterator& rhs) const { return i <= rhs.i; }
		bool operator>=(const const_iterator& rhs) const { return i >= rhs.i; }


		const value_type& operator*()
		{
			return *V::get(c, i);
		}
		const value_type* operator->()
		{
			return &**this;
		}
	};

	template<class V>
	struct iterator : const_iterator<V>
	{
		iterator() {}
		iterator(size_t size, void** data)
			:const_iterator<V>{ size, data } {}
		using iterator_category = std::random_access_iterator_tag;

		using value_type = typename V::value_type;
		using difference_type = typename V::difference_type;
		using pointer = typename V::pointer;
		using reference = typename V::reference;

		iterator& operator++() noexcept { ++this->i; return *this; }
		iterator operator++(int) noexcept { auto v = *this; ++* this; return v; }
		bool operator==(const iterator& right) const noexcept { return this->i == right.i && this->c == right.c; }
		bool operator!=(const iterator& right) const noexcept { return !(*this == right); };
		iterator& operator+=(difference_type d) { this->i += d; return *this; }
		iterator& operator-=(difference_type d) { this->i -= d; return *this; }
		friend iterator operator+(const iterator& lhs, difference_type rhs) { auto result = lhs; return result += rhs; }
		friend iterator operator+(difference_type lhs, const iterator& rhs) { auto result = rhs; return result += lhs; }
		friend iterator operator-(const iterator& lhs, difference_type rhs) { auto result = lhs; return result -= rhs; }
		friend iterator operator-(difference_type lhs, const iterator& rhs) { auto result = rhs; return result -= lhs; }
		friend difference_type operator-(const iterator& lhs, const iterator& rhs) { return lhs.i - rhs.i; }
		bool operator<(const iterator& rhs) const { return this->i < rhs.i; }
		bool operator>(const iterator& rhs) const { return this->i > rhs.i; }
		bool operator<=(const iterator& rhs) const { return this->i <= rhs.i; }
		bool operator>=(const iterator& rhs) const { return this->i >= rhs.i; }
		value_type& operator[](difference_type d) { return *(*this + d); }
		value_type& operator*()
		{
			return *V::get(this->c, this->i);
		}
		value_type* operator->()
		{
			return &**this;
		}
	};
}
//for transient lightweight data storage
template<class T>
struct chunk_vector : chunk_vector_base
{
	using value_type = T;
	using pointer = T*;
	using const_pointer = const T*;
	using reference = T&;
	using const_reference = const T&;
	using size_type = size_t;
	using difference_type = size_t;

	using chunk_vector_base::chunk_vector_base;
	static T* get(void** data, size_t i)
	{
		return &((T**)data)[i / kChunkCapacity][i % kChunkCapacity];
	}
	static constexpr size_t kChunkCapacity = kChunkSize / sizeof(T);

	using const_iterator = chunk_vector_detail::const_iterator<chunk_vector>;
	using iterator = chunk_vector_detail::iterator<chunk_vector>;
	iterator begin() noexcept { return iterator{ 0, data }; }
	iterator end() noexcept { return iterator{ size, data }; }

	const_iterator begin() const noexcept { return const_iterator{ 0, data }; }
	const_iterator end() const noexcept { return const_iterator{ size, data }; }
	void flatten(T* dst)
	{
		chunk_vector_base::flatten(dst, sizeof(T));
	}
	void resize(int newSize)
	{
		size = newSize;
		if (newSize > chunkSize * kChunkCapacity)
			reserve(newSize);
		else
			shrink();
	}
	template<class... Ts>
	void push(Ts&&... args)
	{
		if (size >= chunkSize * kChunkCapacity)
			grow();
		new(get(data, size)) T{ std::forward<Ts>(args)... };
		++size;
	}
	void pop() { --size; shrink(); }
	T& last() { return *get(data, size - 1); }
	const T& last() const { return *get(data, size - 1); }
	void shrink() noexcept
	{
		chunk_vector_base::shrink(chunkSize - (size + kChunkCapacity - 1) / kChunkCapacity);
	}
	void reserve(size_t n)
	{
		while (n > chunkSize * kChunkCapacity)
			grow();
	}
	void zero() { chunk_vector_base::zero(sizeof(T)); }
	T& operator[](size_t i) noexcept
	{
		assert(i < size);
		return *get(data, i);
	}
	const T& operator[](size_t i) const noexcept { return *get(data, i); }
};

namespace chunk_vector_pool
{
	void free(void* data);
	void* malloc();
}