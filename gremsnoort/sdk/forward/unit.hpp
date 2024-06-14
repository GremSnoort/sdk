#pragma once

// std
#include <atomic>
#include <cassert>
#include <functional>
#include <vector>
#include <thread>

// sdk
#include <gremsnoort/sdk/forward/source.hpp>
#include <gremsnoort/sdk/forward/inplaced.hpp>
#include <gremsnoort/sdk/forward/time_utils.hpp>

namespace gremsnoort::sdk {

	using hash_t = std::hash<std::thread::id>;
	static inline auto get_tid() {
		return hash_t()(std::this_thread::get_id());
	}

	namespace unit {
	
		enum class mode_e : int8_t {
			pure,
			blocking_push,
			defaultv = blocking_push
		};

	}

	template<Queue Q, unit::mode_e Mode = unit::mode_e::defaultv>
	class unit_t {

		using value_type = typename Q::value_type;
		using inplaced_t = inplaced_t<source_t<Q, Mode == unit::mode_e::blocking_push>>;

		const std::size_t I;
		inplaced_t inputs;

		std::atomic_bool is_running = false;
		std::vector<std::thread> routines;

	protected:
		virtual auto on_start(const std::size_t index) noexcept -> void {}
		virtual auto on_stop(const std::size_t index) noexcept -> void {}
		virtual auto on_exception(const std::size_t index, std::exception& ex) noexcept -> void {}
		virtual auto on_idle(const std::size_t index) -> void {}
		virtual auto on_data(const std::size_t index, value_type& data) -> void {}

	private:
		auto routine(const std::size_t index) {
			on_start(index);
			assert(inputs.check_index(index));
			auto& input = inputs.at(index);
			value_type data;
			using namespace std::chrono_literals;
			while (is_running.load(std::memory_order_relaxed)) {
				try {
					if (input.pop(data, 10ms)) {
						on_data(index, data);
					}
					else {
						on_idle(index);
					}
				}
				catch (std::exception& ex) { on_exception(index, ex); }
			}
			on_stop(index);
		}

	public:
		explicit unit_t(const std::size_t scale_factor, const std::size_t source_sz = 1024)
			: I(scale_factor)
			, inputs(I)
		{
			for (std::size_t i = 0; i < I; ++i) {
				inputs.add(source_sz);
			}
		}

		auto start() {
			bool expected = false;
			auto r = is_running.compare_exchange_strong(expected, true, std::memory_order_relaxed);
			assert(r);
			if (r) {
				for (std::size_t i = 0; i < I; ++i) {
					routines.emplace_back(std::bind(&unit_t::routine, this, i));
				}
			}
			return r;
		}

		template<class ...Args,
			std::enable_if_t<std::is_constructible_v<value_type, Args&&...>, bool> = true>
		auto produce(Args&&... args) {
			const auto index = time_now<std::chrono::microseconds>() % inputs.size();
			assert(inputs.check_index(index));
			if (inputs.check_index(index)) {
				return inputs.at(index).push(std::forward<Args&&>(args)...);
			}
			return false;
		}

		auto stop() {
			bool expected = true;
			auto r = is_running.compare_exchange_strong(expected, false, std::memory_order_relaxed);
			return r;
		}

		auto join() {
			for (auto& r : routines) {
				if (r.joinable()) {
					r.join();
				}
			}
			routines.clear();
		}

		virtual ~unit_t() {
			stop();
			join();
		}

	};

}
