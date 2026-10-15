#pragma once

#include "twm/window_registry.hpp"

#include <string_view>

namespace twm {

// WriteConsoleW для терминала; UTF-8 для перенаправленного stdout.
class ConsoleLogger {
public:
    ConsoleLogger();
    void operator()(const RegistryNotification& notification) const;
    void info(std::wstring_view message) const;

private:
    HANDLE output_;
    bool is_console_;
};

} // namespace twm
