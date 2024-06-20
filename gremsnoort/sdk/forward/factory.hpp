#pragma once

// std
#include <memory>
#include <type_traits>

namespace gremsnoort::sdk {

	template<class Base>
	class shared_factory_t {
	public:
		using ptr_t = std::shared_ptr<Base>;

		template<class D, class ...Args,
			std::enable_if_t<std::is_constructible_v<D, Args&&...> &&
			(std::is_base_of_v<Base, D> || std::is_same_v<Base, D>), bool> = true>
		static inline auto make(Args&&... args) -> ptr_t {
			return std::make_shared<D>(std::forward<Args&&>(args)... );
		}

		template<class D, class ...Args,
			std::enable_if_t<std::is_constructible_v<D, Args&&...> &&
			(std::is_base_of_v<Base, D> || std::is_same_v<Base, D>), bool> = true>
		static inline auto make_explicit(Args&&... args) -> ptr_t {
			return std::shared_ptr<D>(new D{ std::forward<Args&&>(args)... });
		}

		template<class D,
			std::enable_if_t<std::is_base_of_v<Base, D>, bool> = true>
		auto cast() -> D* {
			return static_cast<D*>(this);
		}
	};

	template<class Base>
	class unique_factory_t {
	public:
		using ptr_t = std::unique_ptr<Base>;

		template<class D, class ...Args,
			std::enable_if_t<std::is_constructible_v<D, Args&&...> &&
			(std::is_base_of_v<Base, D> || std::is_same_v<Base, D>), bool> = true>
		static inline auto make(Args&&... args) -> ptr_t {
			return std::make_unique<D>(std::forward<Args&&>(args)...);
		}

		template<class D, class ...Args,
			std::enable_if_t<std::is_constructible_v<D, Args&&...> &&
			(std::is_base_of_v<Base, D> || std::is_same_v<Base, D>), bool> = true>
		static inline auto make_explicit(Args&&... args) -> ptr_t {
			return std::unique_ptr<D>(new D{ std::forward<Args&&>(args)... });
		}

		template<class D,
			std::enable_if_t<std::is_base_of_v<Base, D>, bool> = true>
		auto cast() -> D* {
			return static_cast<D*>(this);
		}
	};

}
