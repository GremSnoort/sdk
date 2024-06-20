#pragma once

// std
#include <atomic>
#include <concepts>
#include <mutex>
#include <condition_variable>

namespace gremsnoort::sdk {

    namespace internal {

        template<class Q, class ...Args>
        concept __PushInPlaceable = requires(Q & q, Args&&... args) {
            typename Q::value_type;
            std::constructible_from<typename Q::value_type, Args&&...>;
            { q.push(std::forward<Args&&>(args)...) } -> std::same_as<bool>;
        };

        template<class Q, class T>
        concept __Pusheable = requires(Q & q, T&& arg) {
            typename Q::value_type;
            std::same_as<T, typename Q::value_type>;
            { q.push(std::forward<T&&>(arg)) } -> std::same_as<bool>;
        };

        template<class Q, class T>
        concept __Popable = requires(Q & q, T& arg) {
            typename Q::value_type;
            std::same_as<T, typename Q::value_type>;
            { q.pop(arg) } -> std::same_as<bool>;
        };

    }

    template<class Q>
    concept Queue =
        requires(Q & q) {
            typename Q::value_type;
            std::constructible_from<Q, std::size_t>;
        }
        && (internal::__Pusheable<Q, typename Q::value_type> || internal::__PushInPlaceable<Q>)
        && internal::__Popable<Q, typename Q::value_type>;

    template<Queue Q, bool blocking = false>
    class source_t {
        Q queue;
        std::atomic_int64_t size_ = 0;

        std::mutex lock;
        std::condition_variable cv;

    public:
        using value_type = typename Q::value_type;

        explicit source_t(const std::size_t sz = 1024)
            : queue(sz) {}

        auto size() -> std::size_t {
            return size_.load(std::memory_order_relaxed);
        }

        template<class ...Args,
            std::enable_if_t<std::is_constructible_v<value_type, Args&&...>, bool> = true>
        auto push_pure(Args&&... args) {
            if (queue.push(std::forward<Args&&>(args)...)) {
                size_.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
            return false;
        }

        auto pop_pure(value_type& output) {
            if (queue.pop(output)) {
                size_.fetch_sub(1, std::memory_order_relaxed);
                return true;
            }
            return false;
        }

        template<class ...Args,
            std::enable_if_t<std::is_constructible_v<value_type, Args&&...>, bool> = true>
        auto push(Args&&... args) {
            bool pushed = false;
            do {
                if (pushed = queue.push(std::forward<Args&&>(args)...); pushed) {
                    size_.fetch_add(1, std::memory_order_relaxed);
                    cv.notify_one();
                }
            } while (!pushed && blocking);
            return pushed;
        }

        template<class ...Args>
        auto pop(value_type& output, std::chrono::duration<Args...> wait_for = 10) {
            auto status = queue.pop(output);
            if (!status && wait_for.count() > 0) {
                std::unique_lock _(lock);
                cv.wait_for(_, (wait_for));
                if (status = queue.pop(output); status) {
                    size_.fetch_sub(1, std::memory_order_relaxed);
                }
            }
            return status;
        }
    };

}
