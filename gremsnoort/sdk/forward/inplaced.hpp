#pragma once

// std
#include <cstdlib>
#include <cassert>
#include <type_traits>
#include <atomic>
#include <memory>

namespace gremsnoort::sdk {

	template<class Item, class __Alloc = std::allocator<Item>>
	class inplaced_t final {
	public:
		using value_type = Item;
		using allocator_type = __Alloc;

	private:
		value_type* ptr = nullptr;
		const std::size_t capacity_;
		std::atomic_size_t offset = 0;

		static constexpr inline auto size_item = sizeof(value_type);

	public:
		explicit inplaced_t(const std::size_t count_in)
			: ptr(nullptr)
			, capacity_(count_in)
		{
			if (count_in > 0) {
				ptr = allocator_type().allocate(count_in);
				assert(ptr);
			}
		}
		~inplaced_t() {
			if (ptr) {
				std::size_t pos = 0;
				while (pos < offset) {
					(ptr + pos)->~value_type();
					pos++;
				}
				allocator_type().deallocate(ptr, capacity_);
			}
		}

		inplaced_t() = delete;

		inplaced_t(inplaced_t&&) = delete;
		inplaced_t(inplaced_t&) = delete;
		inplaced_t(const inplaced_t&) = delete;

		inplaced_t& operator=(inplaced_t&&) = delete;
		inplaced_t& operator=(inplaced_t&) = delete;
		inplaced_t& operator=(const inplaced_t&) = delete;

		template<class ...Args,
			std::enable_if_t<std::is_constructible_v<value_type, Args...>, bool> = true>
		auto add(std::size_t& index_out, Args&&... args) -> bool {
			std::size_t expected = 0;
			do {
				if (expected >= capacity_) {
					return false;
				}
				if (offset.compare_exchange_weak(expected, expected + 1, std::memory_order_relaxed)) {
					index_out = expected;
					[[maybe_unused]] auto w = new (ptr + expected) value_type(std::forward<Args&&>(args)...);
					assert(w);
					return true;
				}
			} while (expected < capacity_);
			return false;
		}

		auto size() const {
			return offset.load(std::memory_order_relaxed);
		}

		auto capacity() const {
			return capacity_;
		}

		auto check_index(const std::size_t idx) const -> bool {
			return idx < offset;
		}

		auto at(const std::size_t idx) -> value_type& {
			assert(check_index(idx));
			return *(ptr + idx);
		}

		auto operator[](const std::size_t idx) -> value_type& {
			return at(idx);
		}
	};

}
