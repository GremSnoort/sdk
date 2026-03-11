// std
#include <stdio.h>

// gsdk
#include <gremsnoort/sdk/logger/logger.h>

namespace gremsnoort::sdk {

    namespace logger {

        class trivial_t final : public logger_t {

            virtual auto log_impl([[maybe_unused]] const level_e level, std::string_view message) -> void final {
                std::printf("%.*s\n", static_cast<int>(message.size()), message.data());
            }
            virtual auto clone_impl() const -> ptr_t final {
                return std::make_shared<trivial_t>();
            }

        public:
            virtual ~trivial_t() = default;
        };

    }

    auto make_trivial_logger() ->logger_t::ptr_t {
        return std::make_shared<logger::trivial_t>();
    }

}
