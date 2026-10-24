#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace skynet::runtime {

// malloc-compatible ownership at the C boundary. Views never own this storage.
class OwnedBuffer {
public:
    using Deleter = void (*)(void*);
    OwnedBuffer() = default;
    OwnedBuffer(OwnedBuffer&& other) noexcept
        : storage_(std::move(other.storage_)), size_(std::exchange(other.size_, 0)) {}
    OwnedBuffer& operator=(OwnedBuffer&& other) noexcept {
        if (this == &other) { return *this; }
        storage_ = std::move(other.storage_);
        size_ = std::exchange(other.size_, 0);
        return *this;
    }
    OwnedBuffer(const OwnedBuffer&) = delete;
    OwnedBuffer& operator=(const OwnedBuffer&) = delete;

    template <std::contiguous_iterator Iterator>
    OwnedBuffer(Iterator first, Iterator last) { assign(first, last); }

    static OwnedBuffer adopt(void* pointer, std::size_t size, Deleter deleter = std::free) {
        OwnedBuffer result;
        result.storage_ = {static_cast<std::byte*>(pointer), deleter};
        result.size_ = size;
        return result;
    }
    static OwnedBuffer copy(const void* pointer, std::size_t size) {
        if (pointer == nullptr) {
            if (size != 0) { throw std::bad_alloc(); }
            return {};
        }
        if (size == std::numeric_limits<std::size_t>::max()) { throw std::bad_alloc(); }
        auto result = adopt(std::malloc(size + 1), size);
        if (!result.data()) { throw std::bad_alloc(); }
        std::memcpy(result.data(), pointer, size);
        result.data()[size] = std::byte{};
        return result;
    }
    template <std::contiguous_iterator Iterator>
    void assign(Iterator first, Iterator last) {
        if (first == last) { *this = OwnedBuffer{}; return; }
        *this = copy(std::to_address(first), static_cast<std::size_t>(last - first));
    }
    std::byte* data() noexcept { return storage_.get(); }
    const std::byte* data() const noexcept { return storage_.get(); }
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    std::byte* begin() noexcept { return data(); }
    const std::byte* begin() const noexcept { return data(); }
    std::byte* end() noexcept { return size_ ? data() + size_ : data(); }
    const std::byte* end() const noexcept { return size_ ? data() + size_ : data(); }
    Deleter deleter() const noexcept { return storage_.get_deleter(); }
    void set_deleter(Deleter deleter) noexcept { storage_.get_deleter() = deleter; }
    void* release() noexcept { size_ = 0; return storage_.release(); }

private:
    std::unique_ptr<std::byte, Deleter> storage_{nullptr, std::free};
    std::size_t size_{0};
};
}  // namespace skynet::runtime
