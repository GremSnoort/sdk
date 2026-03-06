#pragma once

// std
#include <cassert>
#include <coroutine>
#include <concepts>
#include <exception>
#include <utility>

namespace gremsnoort::sdk::coro {

namespace enqueue {

    template<class T, class A>
    concept enqueueable = requires (T* v, A a) {
        { v->enqueue(std::move(a)) } -> std::same_as<bool>;
    };

    using handle_t = std::coroutine_handle<>;

    template<enqueueable<handle_t> T>
    class awaitable_t {

        T& ref_;
        bool enqueued_;

    public:
        explicit awaitable_t(T& ref)
            : ref_(ref)
            , enqueued_(false)
        {}

        auto await_ready() const noexcept { return false; }

        auto await_suspend(handle_t h) {
            enqueued_ = ref_.enqueue(std::move(h));
            return enqueued_;
        }

        auto await_resume() { return enqueued_; }
    };

}

namespace produce {

    template<class T>
    class generator_t {
    public:

        struct promise_type;
        using handle_t = std::coroutine_handle<promise_type>;

        struct promise_type {

            T value_;
            std::exception_ptr exception_;

            auto get_return_object() -> generator_t {
                return generator_t(handle_t::from_promise(*this));
            }

            auto initial_suspend() const noexcept { return std::suspend_always{}; }
            auto final_suspend() const noexcept { return std::suspend_always{}; }

            void unhandled_exception() {
                exception_ = std::current_exception();
            }

            template<std::convertible_to<T> From>
            auto yield_value(From&& from) {
                value_ = std::forward<From>(from);
                return std::suspend_always{};
            }

            void return_void() {}
        };

        explicit generator_t(handle_t h) noexcept
            : h_(h)
        {}
        generator_t(const generator_t&) = delete;
        auto operator=(const generator_t&) -> generator_t& = delete;

        generator_t(generator_t&& other) noexcept
            : empty_(other.empty_)
            , h_(other.h_) {
            other.empty_ = true;
            other.h_ = nullptr;
        }
        auto operator=(generator_t&& other) noexcept -> generator_t& {
            if (this != &other) {
                if (h_)
                    h_.destroy();
                empty_ = other.empty_;
                h_ = other.h_;
                other.empty_ = true;
                other.h_ = nullptr;
            }
            return *this;
        }

        ~generator_t() {
            if (h_)
                h_.destroy();
        }

        explicit operator bool() {
            return h_ && (!empty_ || !h_.done());
        }

        auto operator()() -> T {

            assert(operator bool());
            process();
            assert(!empty_);
            empty_ = true;

            return std::move(h_.promise().value_);
        }

    private:

        bool empty_ = true;
        handle_t h_;

        auto process() {

            if (empty_ && !h_.done()) {
                h_();
                if (h_.promise().exception_)
                    std::rethrow_exception(h_.promise().exception_);
                empty_ = false;
            }
        }

    };

}

}
