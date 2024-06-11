#pragma once

// std
#include <cstdlib>
#include <cassert>
#include <type_traits>
#include <atomic>

namespace gremsnoort::sdk {

	template<class Item>
	class inplaced_t final {
		char* ptr = nullptr;
		const std::size_t capacity;
		std::atomic_size_t offset = 0;

		static constexpr inline auto size_item = sizeof(Item);

	public:
		explicit inplaced_t(const std::size_t count_in)
			: ptr(nullptr)
			, capacity(count_in * size_item)
		{
			if (capacity > 0) {
				ptr = new char[capacity];
				assert(ptr);
			}
		}
		~inplaced_t() {
			if (ptr) {
				std::size_t pos = 0;
				while (pos < offset) {
					reinterpret_cast<Item*>(ptr + pos)->~Item();
					pos += size_item;
				}
				delete[] ptr;
			}
		}

		inplaced_t(inplaced_t&&) = delete;
		inplaced_t(inplaced_t&) = delete;
		inplaced_t(const inplaced_t&) = delete;

		inplaced_t& operator=(inplaced_t&&) = delete;
		inplaced_t& operator=(inplaced_t&) = delete;
		inplaced_t& operator=(const inplaced_t&) = delete;

		template<class ...Args,
			std::enable_if_t<std::is_constructible_v<Item, Args...>, bool> = true>
		auto add(Args&&... args) -> bool {
			// call from SINGLE thread only
			if (offset < capacity) {
				auto w = new (ptr + offset) Item(std::forward<Args&&>(args)...);
				assert(w);
				offset += size_item;
				return true;
			}
			return false;
		}

		auto size() const  -> std::size_t {
			return offset / size_item;
		}

		auto check_index(const std::size_t idx) const -> bool {
			return idx * size_item < offset;
		}

		auto operator[](std::size_t idx) -> Item& {
			return *reinterpret_cast<Item*>(ptr + idx * size_item);
		}
	};

}
