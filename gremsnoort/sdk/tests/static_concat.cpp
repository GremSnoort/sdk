// conan
#include <catch2/catch_all.hpp>

// sdk
#include <gremsnoort/sdk/forward/static_concat.hpp>

// std
#include <print>

TEST_CASE("static_concat") {

	static constexpr auto v = gremsnoort::sdk::static_concat<' ', "{}", "{}", "{}">();

	const auto value = std::format(v, "test1", "test2", "test3");

	REQUIRE(value == "test1 test2 test3");

}
