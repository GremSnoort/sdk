#pragma once

// std
#include <thread>

namespace gremsnoort::sdk {

    using hash_t = std::hash<std::thread::id>;
    inline auto get_tid() {
        return hash_t()(std::this_thread::get_id());
    }

}
