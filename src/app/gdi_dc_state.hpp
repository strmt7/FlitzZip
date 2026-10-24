#pragma once

#include <utility>
#include <windows.h>

namespace flitzzip::app {

// Purpose: Borrow a device context without leaking selected objects or drawing modes into its caller.
// Inputs: A live, exclusively borrowed DC, retained until this owner finishes restoring its snapshot.
// Outputs: Restores the exact saved level on explicit completion or exception unwinding; never deletes the DC.
class GdiDcState final {
  public:
    // Purpose: Acquire one native state snapshot. Inputs: Borrowed DC. Outputs: Valid owner, or invalid on SaveDC
    // refusal.
    explicit GdiDcState(HDC dc) noexcept : dc_(dc), state_(SaveDC(dc)) {}

    GdiDcState(const GdiDcState&) = delete;
    GdiDcState& operator=(const GdiDcState&) = delete;

    // Purpose: Restore state on every scope exit. Inputs: Active snapshot. Outputs: No snapshot is replayed twice.
    ~GdiDcState() {
        if (state_ != 0) {
            static_cast<void>(restore());
        }
    }

    // Purpose: Admit drawing only after a successful snapshot. Inputs: None. Outputs: Whether state is still owned.
    [[nodiscard]] bool valid() const noexcept {
        return state_ != 0;
    }

    // Purpose: Restore exactly this snapshot, including any later nested levels, without changing ownership twice.
    // Inputs: Exclusively borrowed, live DC. Outputs: True only for successful restoration; consumed or invalid is
    // false.
    [[nodiscard]] bool restore() noexcept {
        const int state = std::exchange(state_, 0);
        return state != 0 && RestoreDC(dc_, state) != FALSE;
    }

  private:
    HDC dc_;
    int state_;
};

}  // namespace flitzzip::app
