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

#include <gremsnoort/sdk/benchmark/time/thread_timer.h>

namespace gremsnoort::sdk {

	namespace unit {
	
		enum class mode_e : int8_t {
			blocking_push,
			defaultv = blocking_push
		};

	}

	template<Queue Q, unit::mode_e Mode = unit::mode_e::defaultv>
	class unit_t {

		using value_type = typename Q::value_type;
		using storage_t = inplaced_t<source_t<Q, Mode == unit::mode_e::blocking_push>>;

		const std::size_t I;
		const std::size_t wait_for_ms;
		storage_t inputs;

		std::atomic_bool is_running = false;
		std::vector<std::thread> routines;

		static inline auto make_timer() {
			using namespace ::gremsnoort::sdk::benchmark;
			return ThreadTimer::make<ThreadTimer>(ThreadTimer::CreateProcessCpuTime());
		}

		using time_checker_t = ::gremsnoort::sdk::benchmark::time_checker_t;

	protected:
        virtual auto on_start([[maybe_unused]] const std::size_t index) noexcept -> void {}
        virtual auto on_stop([[maybe_unused]] const std::size_t index) noexcept -> void {}
        virtual auto on_exception([[maybe_unused]] const std::size_t index, std::exception&) noexcept -> void {}
        virtual auto on_idle([[maybe_unused]] const std::size_t index) -> void {}
        virtual auto on_data([[maybe_unused]] const std::size_t index, value_type&) -> void {}

		virtual auto on_oversize_source(const std::size_t) -> void {}

	private:
		auto routine(const std::size_t index) {
			on_start(index);
			assert(inputs.check_index(index));
			auto& input = inputs.at(index);
			value_type data;
			bool status = false;
			while (is_running.load(std::memory_order_relaxed)) {
				try {
					auto timer = make_timer();
					{ // check time
						time_checker_t _(*timer);
						status = input.pop(data, std::chrono::milliseconds(wait_for_ms));
					} // check time
					if (status) {
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
		explicit unit_t(const std::size_t scale_factor, const std::size_t wait_for_ms_, const std::size_t source_sz = 1024)
			: I(scale_factor)
			, wait_for_ms(wait_for_ms_)
			, inputs(I)
		{
			std::size_t index = 0;
			for (std::size_t i = 0; i < I; ++i) {
				inputs.add(index, source_sz);
				assert(index == i);
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
		auto produce_to(const std::size_t index, Args&&... args) {
			auto status = false;
			assert(inputs.check_index(index));
			if (inputs.check_index(index)) {
				auto timer = make_timer();
				{ // check time
					time_checker_t _(*timer);
					status = inputs.at(index).push(std::forward<Args&&>(args)...);
				} // check time
			}
			return status;
		}

		template<class ...Args,
			std::enable_if_t<std::is_constructible_v<value_type, Args&&...>, bool> = true>
		auto produce(Args&&... args) {
			const auto index = time_now<std::chrono::microseconds>() % inputs.size();
			return produce_to(index, std::forward<Args&&>(args)...);
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

		auto terminate() {
			if (stop()) {
				join();
			}
		}

		virtual ~unit_t() {
			terminate();
		}

	};

}
