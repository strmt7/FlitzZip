#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace flitzzip::app {

// Purpose: Coalesce asynchronous native repaint notifications until their UI-thread acknowledgement.
// Inputs: A bounded nonthrowing platform-post operation; acknowledgement also permits minimization resets.
// Outputs: Owns queue-admission state only; never retains a window, callback or caller-owned storage.
class RepaintRequest {
  public:
    // Purpose: Admit one asynchronous post while suppressing duplicate requests.
    // Inputs: Nonthrowing post reports actual queue admission. Outputs: True for an admitted or pending notification.
    template <typename Post> bool request(Post&& post) noexcept {
        static_assert(std::is_nothrow_invocable_r_v<bool, Post&>);
        auto observed = state_.load();
        while ((observed & kPending) == 0U) {
            const auto claimed = (observed + 2U) | kPending;
            if (state_.compare_exchange_strong(observed, claimed)) {
                if (std::forward<Post>(post)())
                    return true;
                // Acknowledgement may have admitted a newer post; release only our failed generation.
                auto failed = claimed;
                static_cast<void>(state_.compare_exchange_strong(failed, claimed & ~kPending));
                return false;
            }
        }
        return true;
    }

    // Purpose: Release a consumed or explicitly invalidated repaint request.
    // Inputs: UI-thread message consumption or window-state reset. Outputs: Permits a subsequent native post.
    void acknowledge() noexcept {
        static_cast<void>(state_.fetch_and(~kPending));
    }

  private:
    static constexpr std::uint64_t kPending = 1U;
    // The low bit owns pending admission; the remaining bits distinguish successive owners, with unsigned wrap.
    std::atomic_uint64_t state_ = 0U;
};

}  // namespace flitzzip::app
