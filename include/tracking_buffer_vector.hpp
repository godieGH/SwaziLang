#pragma once

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "memory_tracking.hpp"

template <typename T>
class TrackedVector {
    static_assert(std::is_trivially_destructible<T>::value || std::is_move_constructible<T>::value,
        "TrackedVector assumes elements are cheaply moveable");

   public:
    using value_type = T;
    using iterator = typename std::vector<T>::iterator;
    using const_iterator = typename std::vector<T>::const_iterator;
    using size_type = typename std::vector<T>::size_type;
    using reverse_iterator = typename std::vector<T>::reverse_iterator;
    using const_reverse_iterator = typename std::vector<T>::const_reverse_iterator;
    using allocator_type = typename std::vector<T>::allocator_type;

    TrackedVector() noexcept = default;

    explicit TrackedVector(size_t n) : v(n) {
        adjust_tracking_on_capacity_change(0, v.capacity());
    }

    // construct from std::vector (copy/move)
    TrackedVector(const std::vector<T>& other) : v(other) {
        adjust_tracking_on_capacity_change(0, v.capacity());
    }
    TrackedVector(std::vector<T>&& other) noexcept : v(std::move(other)) {
        adjust_tracking_on_capacity_change(0, v.capacity());
    }

    // copy
    TrackedVector(const TrackedVector& other)
        : v(other.v) {
        adjust_tracking_on_capacity_change(0, v.capacity());
    }

    TrackedVector& operator=(const TrackedVector& other) {
        if (this == &other) return *this;
        size_t old_cap = v.capacity();
        v = other.v;
        adjust_tracking_on_capacity_change(old_cap, v.capacity());
        return *this;
    }

    // assign from std::vector (copy)
    TrackedVector& operator=(const std::vector<T>& other) {
        size_t old_cap = v.capacity();
        v = other;
        adjust_tracking_on_capacity_change(old_cap, v.capacity());
        return *this;
    }

    // move: transfer ownership without changing global byte counts
    TrackedVector(TrackedVector&& other) noexcept
        : v(std::move(other.v)) {
        other.v = std::vector<T>();
        // allocation moved; global total unchanged
    }

    TrackedVector& operator=(TrackedVector&& other) noexcept {
        if (this == &other) return *this;
        // subtract our old allocation
        adjust_tracking_on_capacity_change(v.capacity(), 0);
        // steal
        v = std::move(other.v);
        other.v = std::vector<T>();
        // new allocation is now present under *this (we subtracted old above)
        adjust_tracking_on_capacity_change(0, v.capacity());
        return *this;
    }

    // assign from std::vector (move)
    TrackedVector& operator=(std::vector<T>&& other) noexcept {
        // other (std::vector) is not tracked; remove our old and add new
        adjust_tracking_on_capacity_change(v.capacity(), 0);
        v = std::move(other);
        adjust_tracking_on_capacity_change(0, v.capacity());
        return *this;
    }

    // implicit conversion to std::vector<T> (returns a copy)
    operator std::vector<T>() const {
        return v;
    }

    ~TrackedVector() {
        adjust_tracking_on_capacity_change(v.capacity(), 0);
    }

    // ---------- basic iterators & access ----------
    iterator begin() noexcept { return v.begin(); }
    iterator end() noexcept { return v.end(); }
    const_iterator begin() const noexcept { return v.begin(); }
    const_iterator end() const noexcept { return v.end(); }

    const_iterator cbegin() const noexcept { return v.cbegin(); }
    const_iterator cend() const noexcept { return v.cend(); }
    const_reverse_iterator crbegin() const noexcept { return v.crbegin(); }
    const_reverse_iterator crend() const noexcept { return v.crend(); }

    reverse_iterator rbegin() noexcept { return v.rbegin(); }
    reverse_iterator rend() noexcept { return v.rend(); }
    const_reverse_iterator rbegin() const noexcept { return v.rbegin(); }
    const_reverse_iterator rend() const noexcept { return v.rend(); }

    T* data() noexcept { return v.data(); }
    const T* data() const noexcept { return v.data(); }

    size_t size() const noexcept { return v.size(); }
    size_t capacity() const noexcept { return v.capacity(); }
    bool empty() const noexcept { return v.empty(); }

    // subscript operator (important — many callsites use this)
    T& operator[](size_t i) { return v[i]; }
    const T& operator[](size_t i) const { return v[i]; }

    void clear() noexcept { v.clear(); }
    void reserve(size_t new_cap) {
        size_t old_cap = v.capacity();
        if (new_cap <= old_cap) return;
        v.reserve(new_cap);
        adjust_tracking_on_capacity_change(old_cap, v.capacity());
    }

    void resize(size_t new_size, const T& val = T()) {
        size_t old_cap = v.capacity();
        v.resize(new_size, val);
        if (v.capacity() != old_cap) adjust_tracking_on_capacity_change(old_cap, v.capacity());
    }

    void push_back(const T& x) {
        size_t old_cap = v.capacity();
        v.push_back(x);
        if (v.capacity() != old_cap) adjust_tracking_on_capacity_change(old_cap, v.capacity());
    }
    void push_back(T&& x) {
        size_t old_cap = v.capacity();
        v.push_back(std::move(x));
        if (v.capacity() != old_cap) adjust_tracking_on_capacity_change(old_cap, v.capacity());
    }

    template <class... Args>
    void emplace_back(Args&&... args) {
        size_t old_cap = v.capacity();
        v.emplace_back(std::forward<Args>(args)...);
        if (v.capacity() != old_cap) adjust_tracking_on_capacity_change(old_cap, v.capacity());
    }

    // assign overloads
    template <class InputIt>
    void assign(InputIt first, InputIt last) {
        size_t old_cap = v.capacity();
        v.assign(first, last);
        if (v.capacity() != old_cap) adjust_tracking_on_capacity_change(old_cap, v.capacity());
    }

    void assign(size_t n, const T& val) {
        size_t old_cap = v.capacity();
        v.assign(n, val);
        if (v.capacity() != old_cap) adjust_tracking_on_capacity_change(old_cap, v.capacity());
    }

    void assign(std::initializer_list<T> il) {
        size_t old_cap = v.capacity();
        v.assign(il);
        if (v.capacity() != old_cap) adjust_tracking_on_capacity_change(old_cap, v.capacity());
    }

    // insert overloads (common cases)
    iterator insert(const_iterator pos, const T& val) {
        size_t old_cap = v.capacity();
        auto it = v.insert(pos, val);
        if (v.capacity() != old_cap) adjust_tracking_on_capacity_change(old_cap, v.capacity());
        return it;
    }

    iterator insert(const_iterator pos, T&& val) {
        size_t old_cap = v.capacity();
        auto it = v.insert(pos, std::move(val));
        if (v.capacity() != old_cap) adjust_tracking_on_capacity_change(old_cap, v.capacity());
        return it;
    }

    iterator insert(const_iterator pos, size_type count, const T& val) {
        size_t old_cap = v.capacity();
        auto it = v.insert(pos, count, val);
        if (v.capacity() != old_cap) adjust_tracking_on_capacity_change(old_cap, v.capacity());
        return it;
    }

    template <class InputIt2>
    iterator insert(const_iterator pos, InputIt2 first, InputIt2 last) {
        size_t old_cap = v.capacity();
        auto it = v.insert(pos, first, last);
        if (v.capacity() != old_cap) adjust_tracking_on_capacity_change(old_cap, v.capacity());
        return it;
    }

    iterator insert(const_iterator pos, std::initializer_list<T> il) {
        size_t old_cap = v.capacity();
        auto it = v.insert(pos, il);
        if (v.capacity() != old_cap) adjust_tracking_on_capacity_change(old_cap, v.capacity());
        return it;
    }

    iterator erase(const_iterator first, const_iterator last) {
        return v.erase(first, last);
    }
    iterator erase(const_iterator pos) {
        return v.erase(pos);
    }

    // element access (bounds-checked etc.)
    T& at(size_t i) { return v.at(i); }
    const T& at(size_t i) const { return v.at(i); }

    T& front() { return v.front(); }
    const T& front() const { return v.front(); }

    T& back() { return v.back(); }
    const T& back() const { return v.back(); }

    // modifiers
    void pop_back() noexcept { v.pop_back(); }

    void shrink_to_fit() {
        size_t old_cap = v.capacity();
        v.shrink_to_fit();
        if (v.capacity() != old_cap) adjust_tracking_on_capacity_change(old_cap, v.capacity());
    }

    template <class... Args>
    iterator emplace_hint(const_iterator pos, Args&&... args) {
        size_t old_cap = v.capacity();
        auto it = v.emplace_hint(pos, std::forward<Args>(args)...);
        if (v.capacity() != old_cap) adjust_tracking_on_capacity_change(old_cap, v.capacity());
        return it;
    }

    // swap: swapping two TrackedVectors keeps global total unchanged
    void swap(TrackedVector& other) noexcept {
        using std::swap;
        swap(v, other.v);
    }
    friend void swap(TrackedVector& a, TrackedVector& b) noexcept { a.swap(b); }

    // comparisons
    bool operator==(const TrackedVector& other) const noexcept { return v == other.v; }
    bool operator!=(const TrackedVector& other) const noexcept { return v != other.v; }

    // comparisons with std::vector
    bool operator==(const std::vector<T>& other) const noexcept { return v == other; }
    bool operator!=(const std::vector<T>& other) const noexcept { return v != other; }

    // allocator & helpers
    allocator_type get_allocator() const noexcept { return v.get_allocator(); }

    // expose underlying vector for rare use (dangerous!)
    // Note: if caller changes capacity through this reference, they MUST call:
    //   adjust_tracking_on_capacity_change(old_cap, v.capacity());
    // DANGEROUS: Do NOT call reserve/shrink_to_fit/swap/assign on this
    // without manually fixing tracking via adjust_tracking_on_capacity_change.
    std::vector<T>& underlying_vector() noexcept { return v; }
    const std::vector<T>& underlying_vector() const noexcept { return v; }

    // safe helper to swap with a raw std::vector while fixing tracking
    void swap_with_std_vector(std::vector<T>& other) {
        size_t old_cap = v.capacity();
        v.swap(other);
        adjust_tracking_on_capacity_change(old_cap, v.capacity());
    }

   private:
    std::vector<T> v;

    static inline void adjust_tracking_on_capacity_change(size_t old_cap_elems, size_t new_cap_elems) {
        size_t old_bytes = old_cap_elems * sizeof(T);
        size_t new_bytes = new_cap_elems * sizeof(T);
        if (new_bytes > old_bytes) {
            MemoryTracking::g_buffer_bytes.fetch_add(new_bytes - old_bytes);
        } else if (new_bytes < old_bytes) {
            MemoryTracking::g_buffer_bytes.fetch_sub(old_bytes - new_bytes);
        }
    }
};
