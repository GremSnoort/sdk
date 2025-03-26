#pragma once

// std
#include <cstdint>
#include <string>
#include <ranges>
#include <concepts>

namespace gremsnoort::sdk {

	namespace {
	
		template<std::size_t N>
		struct cstring_t {
			const std::size_t size_ = N;
			char data_[N]{};
	
			consteval cstring_t(const char(&input)[N]) noexcept {
				std::ranges::copy_n(input, N, data_);
			}
	
			template<std::size_t L, std::size_t R>
				requires (L + R - 1 == N)
			consteval cstring_t(const cstring_t<L>& l, const cstring_t<R>& r) noexcept {
                auto it = std::ranges::copy(l.data_ | std::views::take(L - 1), data_).out;
                it = std::ranges::copy(r.data_ | std::views::take(R - 1), it).out;
                *it = '\0';
			}

            /// Delimeter ===>
            template<std::size_t L, std::size_t R>
                requires (L + R == N)
            consteval cstring_t(const char D, const cstring_t<L>& l, const cstring_t<R>& r) noexcept {
                auto it = std::ranges::copy(l.data_ | std::views::take(L - 1), data_).out;
                *it = D;
                ++it;
                it = std::ranges::copy(r.data_ | std::views::take(R - 1), it).out;
                *it = '\0';
            }
		};

        template <std::size_t L, std::size_t R>
        cstring_t(const cstring_t<L>&, const cstring_t<R>&) -> cstring_t<L + R - 1>;

        /// Delimeter ===>
        template <std::size_t L, std::size_t R>
        cstring_t(const char D, const cstring_t<L>&, const cstring_t<R>&) -> cstring_t<L + R>;
	
        template <cstring_t S>
        consteval auto get_static_buffer() noexcept -> const char(&)[S.size_] {
            return S.data_;
        }

	}
	
	template<cstring_t L, cstring_t R>
	static consteval auto static_concat() noexcept -> const char(&)[L.size_ + R.size_ - 1] {
        return get_static_buffer < cstring_t{ L, R } > ();
	}

    template<cstring_t L, cstring_t R, cstring_t ...Args>
        requires (sizeof...(Args) != 0)
    static consteval auto static_concat() noexcept {
        return static_concat< cstring_t{ L, R }, Args...>();
    }

    /// Delimeter ===>

    template<char D, cstring_t L, cstring_t R>
    static consteval auto static_concat() noexcept -> const char(&)[L.size_ + R.size_] {
        return get_static_buffer < cstring_t{ D, L, R } > ();
    }

    template<char D, cstring_t L, cstring_t R, cstring_t ...Args>
        requires (sizeof...(Args) != 0)
    static consteval auto static_concat() noexcept {
        return static_concat < D, cstring_t{ D, L, R }, Args... > ();
    }
	
}
