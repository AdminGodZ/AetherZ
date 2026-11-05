#include "engine/thread.hpp"

#include <stdexcept>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

namespace aetherz {

struct SearchThread::Handle {
    std::function<void()> work;
#ifdef _WIN32
    HANDLE thread = nullptr;
#else
    pthread_t thread{};
#endif
};

namespace {

#ifdef _WIN32
DWORD WINAPI run_work(LPVOID argument)
{
    static_cast<std::function<void()>*>(argument)->operator()();
    return 0;
}
#else
void* run_work(void* argument)
{
    static_cast<std::function<void()>*>(argument)->operator()();
    return nullptr;
}
#endif

} // namespace

SearchThread::SearchThread() = default;

SearchThread::SearchThread(std::function<void()> work)
    : handle_(std::make_unique<Handle>())
{
    handle_->work = std::move(work);
#ifdef _WIN32
    handle_->thread = CreateThread(
        nullptr, stack_size, run_work, &handle_->work, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (handle_->thread == nullptr) {
        throw std::runtime_error{"could not start search thread"};
    }
#else
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setstacksize(&attributes, stack_size);
    const int status = pthread_create(&handle_->thread, &attributes, run_work, &handle_->work);
    pthread_attr_destroy(&attributes);
    if (status != 0) {
        throw std::runtime_error{"could not start search thread"};
    }
#endif
}

SearchThread::SearchThread(SearchThread&& other) noexcept = default;

SearchThread& SearchThread::operator=(SearchThread&& other) noexcept
{
    if (this != &other) {
        if (joinable()) {
            join();
        }
        handle_ = std::move(other.handle_);
    }
    return *this;
}

SearchThread::~SearchThread()
{
    if (joinable()) {
        join();
    }
}

bool SearchThread::joinable() const
{
    return handle_ != nullptr;
}

void SearchThread::join()
{
    if (!handle_) {
        return;
    }
#ifdef _WIN32
    WaitForSingleObject(handle_->thread, INFINITE);
    CloseHandle(handle_->thread);
#else
    pthread_join(handle_->thread, nullptr);
#endif
    handle_.reset();
}

} // namespace aetherz
