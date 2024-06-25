// conan
#include <catch2/catch_all.hpp>

// std
#include <memory>
#include <string>
#include <concepts>
#include <thread>
#include <vector>

// sdk
#include <gremsnoort/sdk/forward/inplaced.hpp>
#include <gremsnoort/sdk/tests/data/node.hpp>

template<class T,
	std::enable_if_t<std::is_constructible_v<T, std::size_t> || std::is_same_v<T, std::string>, bool> = true>
struct wrapper_pointers_t {
	auto create(const std::size_t idx) const {
		if constexpr (std::is_same_v<T, std::string>) {
			return new T(std::to_string(idx));
		}
		else {
			return new T(idx);
		}
	}
	auto compare(auto& c, const std::size_t i, auto&& v) const {
		return *c.at(i) == *v;
	}
};

template<class T,
	std::enable_if_t<std::is_constructible_v<T, std::size_t> || std::is_same_v<T, std::string>, bool> = true>
struct wrapper_t {
	auto create(const std::size_t idx) {
		if constexpr (std::is_same_v<T, std::string>) {
			return std::to_string(idx);
		}
		else {
			return idx;
		}
	}
	auto compare(auto& c, const std::size_t i, auto&& v) {
		return c.at(i) == v;
	}
};

TEST_CASE("inplaced") {

	auto run = [](const std::size_t max_size, auto __type, auto wrapper) {
		using type_t = std::decay_t<decltype(__type)>;
		using container_t = gremsnoort::sdk::inplaced_t<type_t>;

		for (std::size_t sz = 0; sz <= max_size; ++sz) {
			auto c = container_t(sz); // create new inplaced container
			REQUIRE(c.capacity() == sz); // check capacity (reserved size) in still empty container
			for (std::size_t i = 0; i < sz; ++i) {
				REQUIRE_FALSE(c.check_index(i)); // check index is NOT available because no element created
				auto v = wrapper.create(i);
				std::size_t index = 0;
				REQUIRE(c.add(index, v)); // add element
				REQUIRE(c.check_index(i)); // check index is available
				REQUIRE(index == i);
				REQUIRE(wrapper.compare(c, i, v)); // check i the same
			}
		}
	};

	auto run_unique_ptr = [&](const std::size_t count, auto __type) {
		using type_t = std::decay_t<decltype(__type)>;
		run(count, std::unique_ptr<type_t>(), wrapper_pointers_t<type_t>{});
	};
	auto run_shared_ptr = [&](const std::size_t count, auto __type) {
		using type_t = std::decay_t<decltype(__type)>;
		run(count, std::shared_ptr<type_t>(), wrapper_pointers_t<type_t>{});
	};

	static const auto count = 256;

	SECTION("smart pointers") {

		run_unique_ptr(count, int64_t());
		run_unique_ptr(count, uint64_t());
		run_unique_ptr(count, int32_t());
		run_unique_ptr(count, uint32_t());
		run_unique_ptr(count, bool());
		run_unique_ptr(count, char());
		run_unique_ptr(count, std::string());

		run_shared_ptr(count, int64_t());
		run_shared_ptr(count, uint64_t());
		run_shared_ptr(count, int32_t());
		run_shared_ptr(count, uint32_t());
		run_shared_ptr(count, bool());
		run_shared_ptr(count, char());
		run_shared_ptr(count, std::string());
		
	}

	SECTION("string") {

		using type_t = std::string;
		run(count, type_t(), wrapper_t<type_t>{});
	
	}

	SECTION("data::node") {
	
		using type_t = gremsnoort::sdk::tests::data::node_t<int>;
		using container_t = gremsnoort::sdk::inplaced_t<type_t>;
		
		for (std::size_t sz = 0; sz <= count; ++sz) {
			auto c = container_t(sz); // create new inplaced container
			REQUIRE(c.capacity() == sz); // check capacity (reserved size) in still empty container
			for (std::size_t i = 0; i < sz; ++i) {
				REQUIRE_FALSE(c.check_index(i)); // check index is NOT available because no element created
				std::size_t index = 0;
				REQUIRE(c.add(index)); // add element
				REQUIRE(c.check_index(i)); // check index is available
				REQUIRE(index == i);

				REQUIRE_FALSE(c.at(i).busy);
				c[i].busy = true;
				REQUIRE(c.at(i).busy);

				REQUIRE_FALSE(c.at(i).next_c);
				c[i].next_c = 46538756;
				REQUIRE(c.at(i).next_c == 46538756);

				REQUIRE_FALSE(c.at(i).next_p);
				c[i].next_p = 98376983;
				REQUIRE(c.at(i).next_p == 98376983);
			}
		}
	
	}

	SECTION("data::node multithreaded") {

		using type_t = gremsnoort::sdk::tests::data::node_t<int>;
		using container_t = gremsnoort::sdk::inplaced_t<type_t>;

		for (std::size_t sz = 0; sz <= count; ++sz) {
			auto c = container_t(sz); // create new inplaced container
			std::vector<std::thread> routines;
			std::atomic_bool start = false;
			std::vector<std::size_t> ids;
			ids.resize(sz);
			for (std::size_t j = 0; j < sz; ++j) {
				routines.emplace_back([&]() {
					while (!start.load(std::memory_order_relaxed));
					std::size_t index = 0;
					REQUIRE(c.add(index));
					REQUIRE(c.check_index(index));
					ids[index]++;
				});
			}
			start = true;
			for (auto& r : routines) {
				r.join();
			}
			for (const auto& id : ids) {
				REQUIRE(id == 1);
			}
		}

	}

}
