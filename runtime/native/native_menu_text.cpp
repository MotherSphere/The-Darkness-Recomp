#include "native_menu_text.h"
#include "native_menu_text.generated.h"
#include <atomic>

namespace DarkRecomp::Native {
namespace {
// Private tag for the Russian revision's retail English content slot.
constexpr uint32_t russianMenuLanguage = 0xffffffffu;
std::atomic<uint32_t> menuLanguage{1};
}
void initializeNativeMenuText(bool russianRevision, uint32_t consoleLanguage) noexcept {
    const auto language = russianRevision && consoleLanguage == 1 ? russianMenuLanguage :
        consoleLanguage == 3 || consoleLanguage == 4 || consoleLanguage == 5 || consoleLanguage == 6 ? consoleLanguage : 1;
    menuLanguage.store(language, std::memory_order_relaxed);
}
bool russianNativeMenus() noexcept { return menuLanguage.load(std::memory_order_relaxed) == russianMenuLanguage; }
std::string_view nativeMenuText(std::string_view english) noexcept {
    const auto language = menuLanguage.load(std::memory_order_relaxed);
    return language == russianMenuLanguage ? russianNativeMenuText(english) : subtitleNativeMenuText(language, english);
}
}
