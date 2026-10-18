#include "../camera_settings.h"
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>

int main() {
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) {
        ++checks;
        if (!ok) { ++failures; std::printf("FAIL: %s\n", name); }
    };
    using namespace FirstPerson;
    check(FirstPersonDistance == -0.15f, "legacy default preserved");
    for (float value : {-0.5f, -0.15f, 0.0f, 0.5f}) {
        check(ValidCameraDistance(value), "inclusive valid range");
        check(ReadCameraDistanceSetting([=](float) { return value; }, 0.1f) == value,
              "valid value replaces previous distance");
    }
    const float infinity = std::numeric_limits<float>::infinity();
    for (float value : {std::nextafter(-0.5f, -infinity), std::nextafter(0.5f, infinity),
                        -infinity, infinity, std::numeric_limits<float>::quiet_NaN()}) {
        check(!ValidCameraDistance(value), "reject out-of-range and non-finite values");
        check(ReadCameraDistanceSetting([=](float) { return value; }, 0.1f) == 0.1f,
              "invalid value preserves previous distance");
    }
    check(ReadCameraDistanceSetting([](float fallback) { return fallback; }, 0.1f) == -0.15f,
          "missing setting resets to legacy default");
    check(ReadCameraDistanceSetting([](float) -> float { throw 42; }, 0.1f) == 0.1f,
          "reader failure preserves previous distance");
    std::function<float(float)> unavailable;
    check(ReadCameraDistanceSetting(unavailable, 0.1f) == 0.1f,
          "unavailable callable preserves previous distance");
    int calls = 0;
    check(ReadCameraDistanceSetting([&](float fallback) { ++calls; return fallback; }, 0.1f) == -0.15f && calls == 1,
          "reader called once with the legacy fallback");
    check(ReadCameraDistanceSetting([](float) { return -0.0f; }, 0.1f) == 0.0f,
          "negative zero remains valid");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
