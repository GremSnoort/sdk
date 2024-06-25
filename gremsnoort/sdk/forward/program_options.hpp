#pragma once

// std
#include <iostream>
#include <string>

// conan
#include <cxxopts.hpp>

namespace gremsnoort::sdk {

    class program_options_t final {
        cxxopts::Options desc;

    public:
        explicit program_options_t(const auto& name, const auto& d)
            : desc(name, d)
        {}

        auto add_options() {
            return desc.add_options();
        }
        auto parse(int argc, char* argv[]) {
            return desc.parse(argc, argv);
        }
        auto help() {
            return desc.help({ "" });
        }

        template<class T>
        auto retrieve_opt(T& output, const char* name, const char* info, auto&& result, bool is_necessary = true) {
            if (result.count(name)) {
                output = result[name].template as<T>();
                return true;
            }
            else {
                std::cout << std::string(info) << " is not set.\n";
                if (is_necessary) {
                    std::exit(-1);
                }
            }
            return false;
        }
    };

}
