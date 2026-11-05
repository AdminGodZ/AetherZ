#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace aetherz {

// A joinable thread with a large stack. The search recurses up to
// max_search_ply deep with several kilobytes of move lists per ply, which
// does not fit the default stack of secondary threads (512 KB on macOS,
// 1 MB on Windows). The stack is reserved address space, not committed
// memory.
class SearchThread {
public:
    static constexpr std::size_t stack_size = 64 * 1024 * 1024;

    SearchThread();
    explicit SearchThread(std::function<void()> work);
    SearchThread(SearchThread&& other) noexcept;
    SearchThread& operator=(SearchThread&& other) noexcept;
    SearchThread(const SearchThread&) = delete;
    SearchThread& operator=(const SearchThread&) = delete;
    // Joins a thread that is still running.
    ~SearchThread();

    [[nodiscard]] bool joinable() const;
    void join();

private:
    struct Handle;
    std::unique_ptr<Handle> handle_;
};

} // namespace aetherz
