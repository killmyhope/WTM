#include "twm/console_logger.hpp"

#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace twm {
namespace {

[[noreturn]] void fail(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()),
                            std::system_category(), operation);
}

std::wstring single_line(std::wstring text) {
    for (auto& ch : text) {
        if (ch < L' ' || ch == L'\x7f') {
            ch = L' ';
        }
    }
    return text;
}

} // namespace

ConsoleLogger::ConsoleLogger() : output_(GetStdHandle(STD_OUTPUT_HANDLE)) {
    if (output_ == nullptr || output_ == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("stdout is unavailable");
    }
    DWORD mode = 0;
    is_console_ = GetConsoleMode(output_, &mode) != FALSE;
}

void ConsoleLogger::info(std::wstring_view message) const {
    const std::wstring line = std::wstring(message) + L"\r\n";
    if (is_console_) {
        std::size_t offset = 0;
        while (offset < line.size()) {
            DWORD written = 0;
            if (!WriteConsoleW(output_, line.data() + offset,
                               static_cast<DWORD>(line.size() - offset),
                               &written, nullptr)) {
                fail("WriteConsoleW");
            }
            if (written == 0) {
                throw std::runtime_error("WriteConsoleW made no progress");
            }
            offset += written;
        }
        return;
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, line.data(),
        static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (size == 0) {
        fail("WideCharToMultiByte");
    }
    std::string utf8(static_cast<std::size_t>(size), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, line.data(),
            static_cast<int>(line.size()), utf8.data(), size, nullptr, nullptr)) {
        fail("WideCharToMultiByte");
    }
    std::size_t offset = 0;
    while (offset < utf8.size()) {
        DWORD written = 0;
        if (!WriteFile(output_, utf8.data() + offset,
                       static_cast<DWORD>(utf8.size() - offset),
                       &written, nullptr)) {
            fail("WriteFile(stdout)");
        }
        if (written == 0) {
            throw std::runtime_error("WriteFile(stdout) made no progress");
        }
        offset += written;
    }
}

void ConsoleLogger::operator()(const RegistryNotification& notification) const {
    const wchar_t* prefix = L"";
    switch (notification.kind) {
    case NotificationKind::Found: prefix = L"Найдено окно"; break;
    case NotificationKind::Hidden: prefix = L"Окно скрыто"; break;
    case NotificationKind::Excluded: prefix = L"Окно исключено"; break;
    case NotificationKind::Closed: prefix = L"Окно закрыто"; break;
    }
    std::wostringstream line;
    line << prefix << L": ["
         << single_line(notification.title.empty()
                ? L"без заголовка" : notification.title)
         << L"] [0x" << std::hex << std::uppercase
         << reinterpret_cast<std::uintptr_t>(notification.identity.hwnd) << L"]";
    if (!notification.reason.empty()) {
        line << L" (" << single_line(notification.reason) << L")";
    }
    info(line.str());
}

} // namespace twm
