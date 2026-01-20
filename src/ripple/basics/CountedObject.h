#ifndef RIPPLE_BASICS_COUNTEDOBJECT_H_INCLUDED
#define RIPPLE_BASICS_COUNTEDOBJECT_H_INCLUDED

#include <ripple/beast/type_name.h>
#include <atomic>
#include <cstddef>
#include <iterator>
#include <string>

namespace ripple {

class CountedObjects
{
public:
    class Counter
    {
    public:
        Counter(std::string name) noexcept;

        int
        increment() noexcept
        {
            auto const newCount = ++count_;

            if (auto maxCount = maxCount_.load(); newCount > maxCount)
                maxCount_.compare_exchange_strong(maxCount, newCount);

            return newCount;
        }

        int
        decrement() noexcept
        {
            return --count_;
        }

        int
        count() const noexcept
        {
            return count_.load();
        }

        int
        max() const noexcept
        {
            return std::max(count_.load(), maxCount_.load());
        }

        std::string const&
        name() const noexcept
        {
            return name_;
        }

    private:
        friend class CountedObjects;

        Counter* next_;
        std::atomic<std::uint32_t> count_ = 0;
        std::atomic<std::uint32_t> maxCount_ = 0;
        std::string const name_;
    };

    class Iterator
    {
    public:
        using value_type = Counter const;
        using reference = value_type&;
        using pointer = value_type*;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::forward_iterator_tag;

        explicit Iterator(Counter* c = nullptr) noexcept : current_(c)
        {
        }

        reference
        operator*() const noexcept
        {
            return *current_;
        }
        pointer
        operator->() const noexcept
        {
            return current_;
        }

        Iterator&
        operator++() noexcept
        {
            current_ = current_->next_;
            return *this;
        }

        Iterator
        operator++(int) noexcept
        {
            auto tmp = *this;
            ++*this;
            return tmp;
        }

        bool
        operator==(Iterator const&) const noexcept = default;

    private:
        Counter* current_;
    };

    constexpr CountedObjects() noexcept = default;

    auto
    begin() const noexcept
    {
        return Iterator{head_.load()};
    }
    auto
    end() const noexcept
    {
        return Iterator{};
    }

private:
    friend class Counter;

    std::atomic<Counter*> head_ = nullptr;
};

inline constinit CountedObjects countedObjects;

inline CountedObjects::Counter::Counter(std::string name) noexcept
    : name_(std::move(name))
{
    do
        next_ = countedObjects.head_.load();
    while (!countedObjects.head_.compare_exchange_weak(next_, this));
}

//------------------------------------------------------------------------------

template <class Object>
class CountedObject
{
    static CountedObjects::Counter counter_;

public:
    CountedObject() noexcept
    {
        counter_.increment();
    }
    CountedObject(CountedObject const&) noexcept
    {
        counter_.increment();
    }
    CountedObject&
    operator=(CountedObject const&) noexcept = default;
    ~CountedObject() noexcept
    {
        counter_.decrement();
    }
};

// Instantiation of the static CountedObject<T>::counter_
template <class Object>
CountedObjects::Counter CountedObject<Object>::counter_{
    beast::type_name<Object>()};

}  // namespace ripple

#endif
