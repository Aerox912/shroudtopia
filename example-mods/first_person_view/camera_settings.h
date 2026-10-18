#pragma once
#include <cmath>

namespace FirstPerson {
// Shared legacy/ESM setting semantics, with no platform or SDK dependency.
inline constexpr float FirstPersonDistance = -0.15f;
inline bool ValidCameraDistance(float distance) noexcept {
    return std::isfinite(distance) && distance >= -0.5f && distance <= 0.5f;
}

// The adapter supplies a reader accepting the default for a missing setting.
// Reader failures and invalid values retain the caller's last valid distance.
// This returns a desired value only; it never applies it to native camera state.
template<class Reader>
float ReadCameraDistanceSetting(Reader&& read, float previous) noexcept {
    try {
        const float value = read(FirstPersonDistance);
        return ValidCameraDistance(value) ? value : previous;
    } catch (...) {
        return previous;
    }
}
}
