#pragma once

#include <Windows.h>

#include <string>

namespace twm {

struct WindowIdentity {
    HWND hwnd{};
    DWORD process_id{};
    DWORD thread_id{};

    bool operator==(const WindowIdentity&) const = default;
};

enum class WindowDisposition {
    Eligible,
    Inactive,
    Ignored,
    Pending,
    Missing
};

struct WindowObservation {
    WindowIdentity identity;
    WindowDisposition disposition{WindowDisposition::Pending};
    std::wstring title;
    std::wstring reason;
};

// Только наблюдение: классификатор не меняет стили, видимость или геометрию.
class WindowClassifier {
public:
    explicit WindowClassifier(DWORD own_process_id) noexcept;

    [[nodiscard]] WindowObservation inspect(HWND hwnd) const;

private:
    DWORD own_process_id_;
};

} // namespace twm
