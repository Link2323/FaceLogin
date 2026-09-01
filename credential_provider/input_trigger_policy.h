#pragma once

#include <array>
#include <cstdint>

namespace facelogin::credential_provider {

// The first lock-screen round has one asymmetric input channel: while the
// wallpaper is being dismissed, Raw Input may expose only the keyboard BREAK.
// Retry rounds run on the fully visible credential view and therefore require
// a fresh MAKE; this prevents a key held across a failed round from retrying.
enum class InputDetectionRound {
    InitialLock,
    FailureRetry
};

// Pure, COM-free input edge policy. The Win32 watcher owns event collection;
// this class only decides whether an observed edge belongs to the current
// round. It deliberately has no timers or keyboard-repeat assumptions.
class InputTriggerPolicy {
public:
    explicit InputTriggerPolicy(InputDetectionRound round) noexcept
        : m_round(round),
          m_drainInitialL(round == InputDetectionRound::InitialLock) {}

    void SeedKeyDown(std::uint16_t vk) noexcept {
        if (!IsValidVk(vk)) return;
        m_keyDown[vk] = true;
        m_inheritedKey[vk] = true;
    }

    void SeedMouseButtonDown(std::uint16_t vk) noexcept {
        if (!IsValidVk(vk)) return;
        m_mouseDown[vk] = true;
        m_inheritedMouseButton[vk] = true;
    }

    // Returns true exactly once, on the first qualifying keyboard press.
    bool ObserveKey(std::uint16_t vk, bool isDown) noexcept {
        if (m_triggered || !IsValidVk(vk)) return false;

        if (isDown) {
            // The initial L MAKE can be an auto-repeat from the Win+L gesture
            // that created this lock screen. Drain the entire first L lineage
            // through its BREAK. This intentionally means that an immediate
            // first L press after a non-Win+L lock can require a second press;
            // without a pre-lock agent those two cases are indistinguishable.
            if (m_drainInitialL && vk == kVkL) {
                m_keyDown[vk] = true;
                return false;
            }

            if (m_inheritedKey[vk] || m_keyDown[vk]) {
                return false;  // held-at-arm key or auto-repeat
            }

            m_keyDown[vk] = true;
            m_triggered = true;
            return true;
        }

        const bool inherited = m_inheritedKey[vk];
        const bool hadMake = m_keyDown[vk] && !inherited;
        m_keyDown[vk] = false;
        m_inheritedKey[vk] = false;

        if (m_drainInitialL && vk == kVkL) {
            m_drainInitialL = false;
            return false;
        }
        if (inherited || hadMake) {
            return false;
        }

        // A Win BREAK without a MAKE is the invisible modifier half left by
        // Win+L. It never proves a new press. Ordinary orphan BREAKs do prove
        // a wallpaper keypress because Windows can swallow their MAKE.
        if (IsWindowsKey(vk)) {
            return false;
        }
        if (m_round == InputDetectionRound::InitialLock) {
            m_triggered = true;
            return true;
        }
        return false;
    }

    // Called with sampled physical button state. Movement never enters this
    // API, so it cannot qualify. Returns true once on a new button-down edge.
    bool ObserveMouseButton(std::uint16_t vk, bool isDown) noexcept {
        if (m_triggered || !IsValidVk(vk)) return false;

        if (isDown) {
            if (m_inheritedMouseButton[vk] || m_mouseDown[vk]) {
                return false;
            }
            m_mouseDown[vk] = true;
            m_triggered = true;
            return true;
        }

        m_mouseDown[vk] = false;
        m_inheritedMouseButton[vk] = false;
        return false;
    }

    bool triggered() const noexcept { return m_triggered; }

private:
    static constexpr std::uint16_t kVkL = 0x4C;
    static constexpr std::uint16_t kVkLWin = 0x5B;
    static constexpr std::uint16_t kVkRWin = 0x5C;

    static constexpr bool IsValidVk(std::uint16_t vk) noexcept {
        return vk > 0 && vk < 0xFF;
    }

    static constexpr bool IsWindowsKey(std::uint16_t vk) noexcept {
        return vk == kVkLWin || vk == kVkRWin;
    }

    InputDetectionRound m_round;
    bool m_drainInitialL = false;
    bool m_triggered = false;
    std::array<bool, 256> m_keyDown{};
    std::array<bool, 256> m_inheritedKey{};
    std::array<bool, 256> m_mouseDown{};
    std::array<bool, 256> m_inheritedMouseButton{};
};

} // namespace facelogin::credential_provider
