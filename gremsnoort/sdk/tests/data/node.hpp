#pragma once

// std
#include <atomic>

namespace gremsnoort::sdk::tests::data {

	template<class T, class __type = uint64_t>
	struct node_t {
		T payload;
		std::atomic<__type> next_p = 0;
		std::atomic<__type> next_c = 0;
		std::atomic_bool busy = false;

		node_t() = default;

		node_t(node_t&&) = delete;
		node_t(node_t&) = delete;
		node_t(const node_t&) = delete;

		node_t& operator=(node_t&&) = delete;
		node_t& operator=(node_t&) = delete;
		node_t& operator=(const node_t&) = delete;
	};

}
