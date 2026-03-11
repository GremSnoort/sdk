#pragma once

// std
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

// conan
//#include <magic_enum.hpp>

namespace gremsnoort::sdk {

    // https://stackoverflow.com/questions/68675303/how-to-create-a-function-that-forwards-its-arguments-to-fmtformat-keeping-the
    template <std::size_t N>
    struct static_string {
        char str[N]{};
        constexpr static_string(const char(&s)[N]) {
            std::copy(s, s + N, str);
        }
    };

    class logger_t {
    public:

        enum class level_e : int8_t {
            trace,
            debug,
            info,
            warn,
            err,
            critical,
            none,
        };
        using ptr_t = std::shared_ptr<logger_t>;

    protected:

        virtual auto log_impl(const level_e level, std::string_view message) -> void = 0;
        virtual auto clone_impl() const -> ptr_t = 0;

    public:
        virtual ~logger_t() = default;

        auto clone() const -> ptr_t {
            return clone_impl();
        }

        template<level_e L, static_string F, class ...Args>
        auto log(Args&&... args) {
            const auto message = std::format(F.str, std::forward<Args>(args)...);
            log_impl(L, message);
        }
    };

    auto make_trivial_logger() ->logger_t::ptr_t;

}
