#include "twm/window_classifier.hpp"

#include <dwmapi.h>

#include <array>
#include <string_view>
#include <utility>

namespace twm {
namespace {

[[nodiscard]] std::wstring win32_error(std::wstring_view operation, DWORD error)
{
    return std::wstring(operation) + L": Win32 error " + std::to_wstring(error);
}

[[nodiscard]] bool read_style(HWND hwnd, int index, DWORD& style, DWORD& error) noexcept
{
    // Нулевой стиль допустим; отличаем его от ошибки через GetLastError.
    SetLastError(ERROR_SUCCESS);
    const LONG_PTR value = GetWindowLongPtrW(hwnd, index);
    error = GetLastError();
    if (value == 0 && error != ERROR_SUCCESS) {
        return false;
    }
    style = static_cast<DWORD>(value);
    return true;
}

[[nodiscard]] bool is_shell_class(std::wstring_view class_name) noexcept
{
    // Эвристики совместимости, а не контракт Windows API. Набор классов
    // оболочки может меняться между версиями Windows. Не запрещаем общие
    // классы Windows.UI.Core.CoreWindow / WinUI: ими пользуются приложения.
    constexpr std::array ignored_classes{
        std::wstring_view{L"Progman"},
        std::wstring_view{L"WorkerW"},
        std::wstring_view{L"Shell_TrayWnd"},
        std::wstring_view{L"Shell_SecondaryTrayWnd"},
        std::wstring_view{L"#32768"}, // Системный класс меню.
        std::wstring_view{L"tooltips_class32"},
        std::wstring_view{L"NotifyIconOverflowWindow"},
        std::wstring_view{L"TopLevelWindowForOverflowXamlIsland"},
        std::wstring_view{L"MultitaskingViewFrame"},
        std::wstring_view{L"XamlExplorerHostIslandWindow"},
        std::wstring_view{L"Shell_InputSwitchTopLevelWindow"},
        std::wstring_view{L"TaskListThumbnailWnd"}
    };
    for (const auto ignored : ignored_classes) {
        if (class_name == ignored) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool read_process_name(DWORD process_id, std::wstring& name, DWORD& error)
{
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
    if (process == nullptr) {
        error = GetLastError();
        return false;
    }

    std::array<wchar_t, 32768> buffer{};
    DWORD length = static_cast<DWORD>(buffer.size());
    const BOOL success = QueryFullProcessImageNameW(process, 0, buffer.data(), &length);
    error = success ? ERROR_SUCCESS : GetLastError();
    // Закрываем HANDLE до потенциально бросающей аллокации std::wstring.
    CloseHandle(process);
    if (!success) {
        return false;
    }

    const std::wstring_view path(buffer.data(), length);
    const auto separator = path.find_last_of(L"\\/");
    name.assign(separator == std::wstring_view::npos ? path : path.substr(separator + 1));
    return true;
}

[[nodiscard]] bool is_shell_process(std::wstring_view process_name) noexcept
{
    // Имена также являются эвристикой. explorer.exe здесь намеренно отсутствует:
    // его обычные окна Проводника должны оставаться кандидатами для тайлинга.
    constexpr std::array ignored_processes{
        std::wstring_view{L"StartMenuExperienceHost.exe"},
        std::wstring_view{L"SearchHost.exe"},
        std::wstring_view{L"SearchApp.exe"},
        std::wstring_view{L"SearchUI.exe"},
        std::wstring_view{L"ShellExperienceHost.exe"},
        std::wstring_view{L"TextInputHost.exe"},
        std::wstring_view{L"LockApp.exe"},
        std::wstring_view{L"Widgets.exe"}
    };
    for (const auto ignored : ignored_processes) {
        if (CompareStringOrdinal(
                process_name.data(), static_cast<int>(process_name.size()),
                ignored.data(), static_cast<int>(ignored.size()), TRUE) == CSTR_EQUAL) {
            return true;
        }
    }
    return false;
}

// Проверка в конце чтения уменьшает гонку с уничтожением и повторным
// использованием HWND. Повторное использование тем же потоком она не исключает;
// поколения записей и события жизненного цикла остаются задачей реестра.
[[nodiscard]] WindowObservation finish(
    WindowObservation observation,
    WindowDisposition disposition,
    std::wstring reason)
{
    DWORD process_id = 0;
    const DWORD thread_id = GetWindowThreadProcessId(observation.identity.hwnd, &process_id);
    if (thread_id == 0) {
        observation.disposition = IsWindow(observation.identity.hwnd)
            ? WindowDisposition::Pending
            : WindowDisposition::Missing;
        observation.reason = L"Окно исчезло или его идентичность недоступна";
    } else if (process_id != observation.identity.process_id ||
               thread_id != observation.identity.thread_id) {
        observation.disposition = WindowDisposition::Pending;
        observation.reason = L"Идентичность HWND изменилась во время чтения";
    } else {
        observation.disposition = disposition;
        observation.reason = std::move(reason);
    }
    return observation;
}

} // namespace

WindowClassifier::WindowClassifier(DWORD own_process_id) noexcept
    : own_process_id_(own_process_id)
{
}

WindowObservation WindowClassifier::inspect(HWND hwnd) const
{
    WindowObservation observation;
    observation.identity.hwnd = hwnd;
    if (hwnd == nullptr || !IsWindow(hwnd)) {
        observation.disposition = WindowDisposition::Missing;
        observation.reason = L"HWND больше не существует";
        return observation;
    }

    observation.identity.thread_id =
        GetWindowThreadProcessId(hwnd, &observation.identity.process_id);
    if (observation.identity.thread_id == 0) {
        const DWORD error = GetLastError();
        observation.disposition = IsWindow(hwnd)
            ? WindowDisposition::Pending
            : WindowDisposition::Missing;
        observation.reason = win32_error(L"GetWindowThreadProcessId", error);
        return observation;
    }

    if (observation.identity.process_id == own_process_id_ || hwnd == GetConsoleWindow()) {
        return finish(std::move(observation), WindowDisposition::Ignored,
                      L"Собственное окно или диагностическая консоль");
    }
    if (hwnd == GetDesktopWindow() || hwnd == GetShellWindow()) {
        return finish(std::move(observation), WindowDisposition::Ignored,
                      L"Рабочий стол или окно оболочки");
    }

    DWORD style = 0;
    DWORD extended_style = 0;
    DWORD error = ERROR_SUCCESS;
    if (!read_style(hwnd, GWL_STYLE, style, error)) {
        return finish(std::move(observation), WindowDisposition::Pending,
                      win32_error(L"GWL_STYLE", error));
    }
    if (!read_style(hwnd, GWL_EXSTYLE, extended_style, error)) {
        return finish(std::move(observation), WindowDisposition::Pending,
                      win32_error(L"GWL_EXSTYLE", error));
    }

    const HWND root = GetAncestor(hwnd, GA_ROOT);
    if (root == nullptr) {
        return finish(std::move(observation), WindowDisposition::Pending,
                      L"Не удалось определить корневое окно");
    }
    // У message-only окна реальный parent может быть внутренним HWND,
    // а не псевдодескриптором HWND_MESSAGE, переданным в CreateWindowEx.
    const HWND parent = GetAncestor(hwnd, GA_PARENT);
    if ((style & WS_CHILD) != 0 || root != hwnd ||
        (parent != nullptr && parent != GetDesktopWindow())) {
        return finish(std::move(observation), WindowDisposition::Ignored,
                      L"Дочернее или message-only окно");
    }
    if ((extended_style & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) != 0) {
        return finish(std::move(observation), WindowDisposition::Ignored,
                      L"Служебное или неактивируемое окно");
    }
    if (GetWindow(hwnd, GW_OWNER) != nullptr) {
        // В следующем этапе такие окна станут Floating, связанными с owner.
        return finish(std::move(observation), WindowDisposition::Ignored,
                      L"Окно имеет владельца (диалог или вспомогательное окно)");
    }

    std::array<wchar_t, 256> class_buffer{};
    const int class_length = GetClassNameW(hwnd, class_buffer.data(),
                                         static_cast<int>(class_buffer.size()));
    if (class_length == 0) {
        error = GetLastError();
        return finish(std::move(observation), WindowDisposition::Pending,
                      win32_error(L"GetClassNameW", error));
    }
    const std::wstring_view class_name(class_buffer.data(),
                                       static_cast<std::size_t>(class_length));
    if (is_shell_class(class_name)) {
        return finish(std::move(observation), WindowDisposition::Ignored,
                      L"Служебный класс оболочки: " + std::wstring(class_name));
    }

    // Не обращаемся к WM_GETTEXT через SendMessage. Для чужого процесса
    // GetWindowTextW читает caption без ожидания его UI-потока. Пустой заголовок
    // допустим; верхняя граница защищает от неограниченного выделения памяти.
    std::array<wchar_t, 4096> title_buffer{};
    SetLastError(ERROR_SUCCESS);
    const int title_length = GetWindowTextW(hwnd, title_buffer.data(),
                                          static_cast<int>(title_buffer.size()));
    error = GetLastError();
    if (title_length == 0 && error != ERROR_SUCCESS) {
        return finish(std::move(observation), WindowDisposition::Pending,
                      win32_error(L"GetWindowTextW", error));
    }
    observation.title.assign(title_buffer.data(), static_cast<std::size_t>(title_length));

    if (!IsWindowVisible(hwnd)) {
        return finish(std::move(observation), WindowDisposition::Inactive, L"Окно скрыто");
    }
    if (IsIconic(hwnd)) {
        return finish(std::move(observation), WindowDisposition::Inactive, L"Окно свёрнуто");
    }

    DWORD cloaked = 0;
    const HRESULT result = DwmGetWindowAttribute(
        hwnd, DWMWA_CLOAKED, &cloaked, static_cast<DWORD>(sizeof(cloaked)));
    if (FAILED(result)) {
        return finish(std::move(observation), WindowDisposition::Pending,
                      L"DWMWA_CLOAKED: HRESULT " +
                          std::to_wstring(static_cast<unsigned long>(result)));
    }
    if (cloaked != 0) {
        return finish(std::move(observation), WindowDisposition::Inactive,
                      L"Окно скрыто композитором (cloaked)");
    }

    RECT rectangle{};
    if (!GetWindowRect(hwnd, &rectangle)) {
        error = GetLastError();
        return finish(std::move(observation), WindowDisposition::Pending,
                      win32_error(L"GetWindowRect", error));
    }
    if (rectangle.right <= rectangle.left || rectangle.bottom <= rectangle.top) {
        return finish(std::move(observation), WindowDisposition::Pending,
                      L"Геометрия окна ещё не готова");
    }

    std::wstring process_name;
    if (!read_process_name(observation.identity.process_id, process_name, error)) {
        return finish(std::move(observation), WindowDisposition::Pending,
                      win32_error(L"Получение имени процесса", error));
    }
    if (is_shell_process(process_name)) {
        return finish(std::move(observation), WindowDisposition::Ignored,
                      L"Служебный процесс оболочки: " + process_name);
    }

    // WS_POPUP, WS_EX_LAYERED и отсутствие заголовка сами по себе допустимы.
    // IsWindowVisible не означает, что окно не перекрыто другим окном.
    return finish(std::move(observation), WindowDisposition::Eligible,
                  L"Обычное видимое top-level окно");
}

} // namespace twm
