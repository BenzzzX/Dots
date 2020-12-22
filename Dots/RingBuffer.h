#pragma once
#include <cassert>
#include <atomic>
namespace core
{
	template<class T, class basic_ringbuffer_storage>
	class ringbuffer_pattern
	{
		using memory_order = std::memory_order;
	private:
		alignas(CACHE_LINE_SIZE) std::atomic<size_t> readIndex;
		alignas(CACHE_LINE_SIZE) std::atomic<size_t> writeIndex;
	public:
		using value_type = T;
		ringbuffer_pattern()
		{
			readIndex.store(0, memory_order::memory_order_release);
			writeIndex.store(0, memory_order::memory_order_release);
		}
		size_t capacity() const
		{
			return static_cast<const basic_ringbuffer_storage*>(this)->capacity();
		}
		value_type* data()
		{
			return static_cast<basic_ringbuffer_storage*>(this)->data();
		}
		size_t size() const
		{
			const auto wI = writeIndex.load(memory_order::memory_order_acquire);
			const auto rI = readIndex.load(memory_order::memory_order_acquire);
			return wI - rI;
		}
		value_type* write_ptr(size_t& count)
		{
			const auto wI = writeIndex.load(memory_order::memory_order_relaxed);
			const auto rI = readIndex.load(memory_order::memory_order_acquire);
			return ptr(wI, capacity() - (wI - rI), count);
		}
		void write_ptr_update(void* writePtr, size_t count)
		{
			ptr_update(writePtr, &writeIndex, count);
		}
		const value_type* read_ptr(size_t& count)
		{
			const auto rI = readIndex.load(memory_order::memory_order_relaxed);
			const auto wI = writeIndex.load(memory_order::memory_order_acquire);
			return ptr(rI, (wI - rI), count);
		}
		void read_ptr_update(const void* readPtr, size_t count)
		{
			ptr_update(readPtr, &readIndex, count);
		}
	private:
		value_type* ptr(const std::atomic<size_t> position, const size_t elementsAvailable, size_t& count)
		{
			const size_t index = position % capacity();
			const size_t spaceUntilEnd = capacity() - index;
			const size_t continousElementsAvailable = std::min(elementsAvailable, spaceUntilEnd);
			count = std::min(count, continousElementsAvailable);
			return &(data()[index]);
		}
		void ptr_update(const void* ptr, std::atomic<size_t>* position, std::size_t bytes)
		{
			if (ECS_ENABLE_ASSERTIONS)
			{
				const auto result =
					std::atomic_fetch_add_explicit(position, bytes, memory_order::memory_order_acq_rel);
				assert(ptr == &(data()[result % capacity()]));
				return;
			}
			std::atomic_fetch_add_explicit(position, bytes, memory_order::memory_order_release);
		}
	};

	template<class T, size_t N>
	class static_ringbuffer_base : public ringbuffer_pattern<T, static_ringbuffer_base<T, N> >
	{
	private:
		alignas(CACHE_LINE_SIZE) std::array<T, N> data_;

	public:
		using typename ringbuffer_pattern<T, static_ringbuffer_base<T, N> >::value_type;

		static size_t capacity() { return N; }
		value_type* data() { return data_.data(); }
	};

	template<class base_ringbuffer>
	class ringbuffer_queue_pattern : public base_ringbuffer
	{
	public:
		using typename base_ringbuffer::value_type;

		ringbuffer_queue_pattern() : base_ringbuffer() {}
		template<typename... Args>
		ringbuffer_queue_pattern(Args&&... args) : base_ringbuffer(std::forward<Args>(args)...) {}

		// Write access.

		bool push_back(const value_type& value)
		{
			size_t numElements = 1;
			value_type* writePtr = this->write_ptr(numElements);
			if (numElements == 0)
				return false;

			memcpy(writePtr, &value, sizeof(value_type));
			this->write_ptr_update(writePtr, 1);
			return true;
		}

		// Tries to copy a range of elements element to the ringbuffer and moves the write pointer.
		// Returns number of successfully added elements.
		size_t push_range(const value_type* const sourceBufferStart, const value_type* const sourceBufferEnd)
		{
			assert(sourceBufferStart);
			assert(sourceBufferEnd);
			assert(sourceBufferEnd >= sourceBufferStart);

			const size_t totalNumElementsToWrite = static_cast<size_t>(sourceBufferEnd - sourceBufferStart);
			size_t numElementsWritten = 0;
			do
			{
				size_t writeBatchSize = totalNumElementsToWrite - numElementsWritten;
				value_type* const writePtr = this->write_ptr(writeBatchSize);
				// Happens only if the buffer is full or somebody passed zero elements in.
				if (writeBatchSize == 0)
					return numElementsWritten;
				memcpy(writePtr, sourceBufferStart + numElementsWritten, writeBatchSize * sizeof(value_type));
				this->write_ptr_update(writePtr, writeBatchSize);
				numElementsWritten += writeBatchSize;
			} while (numElementsWritten != totalNumElementsToWrite);
			// Happens only if we have wrap-around or buffer need to grow (dynamic_ringbuffer)
			return totalNumElementsToWrite;
		}

		// Read access.

		// Tries to remove and read a single element from the ringbuffer.
		// Undefined behavior/assertion for empty ringbuffer.
		value_type pop_front()
		{
			size_t numElements = 1;
			const value_type* readPtr = this->read_ptr(numElements);
			assert(numElements == 1);
			value_type outputElement = *readPtr;
			this->read_ptr_update(readPtr, numElements);
			return outputElement;
		}

		// Tries to remove and copy several element from the ringbuffer to the given range.
		//   Returns number of elements actually removed + copied.
		size_t pop_range(value_type* targetBufferStart, value_type* targetBufferEnd)
		{
			assert(targetBufferStart);
			assert(targetBufferEnd);
			assert(targetBufferEnd >= targetBufferStart);

			const size_t totalNumElementsToRead = static_cast<size_t>(targetBufferEnd - targetBufferStart);
			size_t numElementsRead = 0;
			do
			{
				size_t readBatchSize = totalNumElementsToRead - numElementsRead;
				const value_type* const readPtr = this->read_ptr(readBatchSize);
				// Happens if the buffer has no more data or somebody passed zero elements in.
				if (readBatchSize == 0)
					return numElementsRead;

				memcpy(targetBufferStart + numElementsRead, readPtr, readBatchSize * sizeof(value_type));
				this->read_ptr_update(readPtr, readBatchSize);
				numElementsRead += readBatchSize;
			} while (numElementsRead != totalNumElementsToRead);
			// Happens only if we have wrap-around.
			return totalNumElementsToRead;
		}

		// Tries to remove a given number of elements from the ringbuffer.
		//   Returns number of elements actually removed.
		size_t pop_range(const size_t numElements)
		{
			const size_t totalNumElementsToRead = numElements;
			size_t numElementsRead = 0;
			do
			{
				size_t readBatchSize = totalNumElementsToRead - numElementsRead;
				const value_type* const readPtr = this->read_ptr(readBatchSize);
				// Happens if the buffer has no more data or somebody passed zero elements in.
				if (readBatchSize == 0)
					return numElementsRead;

				this->read_ptr_update(readPtr, readBatchSize);
				numElementsRead += readBatchSize;
			} while (numElementsRead != totalNumElementsToRead);
			// Happens only if we have wrap-around.

			return totalNumElementsToRead;
		}

		// Returns reference to front most element without removing it from the ringbuffer.
		//   Undefined behavior/assertion for empty ringbuffer.
		const value_type& front()
		{
			size_t numElements = 1;
			const value_type* value = this->read_ptr(numElements);
			assert(numElements == 1);
			return *value;
		}

		// Returns pointer to front most element without removing it from the ringbuffer.
		//   Returns null for empty ringbuffer.
		const value_type* front_ptr()
		{
			size_t numElements = 1;
			const value_type* value = this->read_ptr(numElements);
			return numElements == 0 ? nullptr : value;
		}

		bool empty()
		{
			size_t numElements = 1;
			this->read_ptr(numElements);
			return numElements == 0;
		}
	};
	template<class T, size_t N>
	class static_ringbuffer : public ringbuffer_queue_pattern<static_ringbuffer_base<T, N> >
	{
	public:
		using typename ringbuffer_queue_pattern<static_ringbuffer_base<T, N> >::value_type;
	};
}