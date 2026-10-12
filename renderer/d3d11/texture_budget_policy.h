#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace DarkRecomp::TextureBudgetPolicy {
inline constexpr size_t MiB=1024*1024;
inline constexpr size_t minimum=256*MiB,maximum=2048*MiB,defaultHardware=512*MiB;
inline constexpr uint64_t refreshIntervalMs=5000;

// Keep the existing dedicated-memory quarter and unknown-adapter fallback.
constexpr size_t hardwareCap(uint64_t dedicatedBytes) {
    return dedicatedBytes?size_t(std::clamp(dedicatedBytes/4,uint64_t(minimum),uint64_t(maximum))):defaultHardware;
}
constexpr size_t hostCap(uint64_t availableBytes) {
    return size_t(std::clamp(availableBytes/4,uint64_t(minimum),uint64_t(maximum)));
}
constexpr size_t initialBudget(size_t hardware,bool memorySuccess,uint64_t availableBytes) {
    return memorySuccess?(std::min)(hardware,hostCap(availableBytes)):hardware;
}
constexpr size_t refreshBudget(size_t current,size_t hardware,bool memorySuccess,uint64_t availableBytes) {
    if(!memorySuccess)return current;
    const auto target=(std::min)(hardware,hostCap(availableBytes));
    if(target<=current)return target;
    const auto growthThreshold=(std::max)(size_t(64*MiB),current/8);
    return target-current>=growthThreshold?target:current;
}
// Called once per logical-frame boundary. OS sampling stays behind this gate.
constexpr bool sampleDue(uint64_t now,uint64_t& nextSample) {
    if(now<nextSample)return false;
    nextSample=now+refreshIntervalMs;
    return true;
}
}
