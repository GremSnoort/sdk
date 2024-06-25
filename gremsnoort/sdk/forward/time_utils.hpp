#pragma once

// std
#include <chrono>

namespace gremsnoort::sdk {

	using clock_t = std::chrono::system_clock;

    template<class Duration>
    static inline auto time_now() -> uint64_t {
        return std::chrono::duration_cast<Duration>(clock_t::now().time_since_epoch()).count();
    }

}
