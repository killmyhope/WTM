#include "twm/console_logger.hpp"
#include "twm/win_event_source.hpp"
#include "twm/window_classifier.hpp"
#include "twm/window_registry.hpp"

#include <atomic>
#include <charconv>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

namespace {

constexpr UINT stop_message = WM_APP + 2;
constexpr UINT timer_period_ms = 250;
constexpr ULONGLONG rescan_period_ms = 1000;
std::atomic<DWORD> message_thread_id{0};
std::atomic<bool> stop_requested{false};

[[noreturn]] void fail(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()),
                            std::system_category(), operation);
}

BOOL WINAPI console_control(DWORD event) noexcept {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) {
        return FALSE;
    }
    stop_requested.store(true, std::memory_order_relaxed);
    // Handler работает на другом потоке: реестр здесь не трогаем.
    // Даже при отказе PostThreadMessage основной поток проснётся по таймеру.
    const DWORD thread = message_thread_id.load(std::memory_order_relaxed);
    if (thread != 0) {
        PostThreadMessageW(thread, stop_message, 0, 0);
    }
    return TRUE;
}

class ConsoleControl final {
public:
    ConsoleControl() {
        message_thread_id.store(GetCurrentThreadId(), std::memory_order_relaxed);
        if (!SetConsoleCtrlHandler(console_control, TRUE)) {
            message_thread_id.store(0, std::memory_order_relaxed);
            fail("SetConsoleCtrlHandler");
        }
    }
    ~ConsoleControl() {
        message_thread_id.store(0, std::memory_order_relaxed);
        SetConsoleCtrlHandler(console_control, FALSE);
    }
    ConsoleControl(const ConsoleControl&) = delete;
    ConsoleControl& operator=(const ConsoleControl&) = delete;
};

class ThreadTimer final {
public:
    ThreadTimer() : id_(SetTimer(nullptr, 0, timer_period_ms, nullptr)) {
        if (id_ == 0) {
            fail("SetTimer");
        }
    }
    ~ThreadTimer() { KillTimer(nullptr, id_); }
    ThreadTimer(const ThreadTimer&) = delete;
    ThreadTimer& operator=(const ThreadTimer&) = delete;
private:
    UINT_PTR id_;
};

struct Enumeration {
    std::vector<HWND> handles;
    std::exception_ptr exception;
};

BOOL CALLBACK collect_window(HWND hwnd, LPARAM parameter) noexcept {
    auto& enumeration = *reinterpret_cast<Enumeration*>(parameter);
    try {
        enumeration.handles.push_back(hwnd);
        return TRUE;
    } catch (...) {
        // Исключения C++ не должны пересекать границу Win32 callback.
        enumeration.exception = std::current_exception();
        return FALSE;
    }
}

void reconcile(const twm::WindowClassifier& classifier,
               twm::WindowRegistry& registry) {
    Enumeration enumeration;
    const BOOL success = EnumWindows(collect_window,
        reinterpret_cast<LPARAM>(&enumeration));
    if (enumeration.exception) {
        std::rethrow_exception(enumeration.exception);
    }
    if (!success) {
        fail("EnumWindows");
    }

    const std::unordered_set<HWND> enumerated(
        enumeration.handles.begin(), enumeration.handles.end());
    for (const HWND hwnd : enumeration.handles) {
        const auto observation = classifier.inspect(hwnd);
        registry.observe(observation, GetTickCount64());
    }
    for (const HWND hwnd : registry.tracked_handles()) {
        if (!enumerated.contains(hwnd)) {
            // Отсутствие в EnumWindows само по себе не доказывает DESTROY.
            const auto observation = classifier.inspect(hwnd);
            registry.observe(observation, GetTickCount64());
        }
    }
}

void drain_events(twm::WinEventSource& source,
                  const twm::WindowClassifier& classifier,
                  twm::WindowRegistry& registry) {
    twm::WindowEvent event;
    // Ограничиваем работу за один проход, чтобы обработать Ctrl+C и таймер.
    for (std::size_t count = 0; count < 4096 && source.pop(event); ++count) {
        if (event.event == EVENT_OBJECT_DESTROY) {
            // Разворачиваем 32-битную метку WinEvent относительно текущего
            // uptime. Событие должно быть доставлено менее чем за 49.7 дней.
            const ULONGLONG now = GetTickCount64();
            const DWORD age = static_cast<DWORD>(now) - event.timestamp;
            const ULONGLONG event_at = age <= now ? now - age : 0;
            registry.on_destroy(event.hwnd, event_at);
        }
        // Событие не является снимком. Missing удалит запись даже при
        // IsWindow == FALSE; новое окно с reused HWND получит новую запись.
        const auto observation = classifier.inspect(event.hwnd);
        registry.observe(observation, GetTickCount64());
    }
}

ULONGLONG duration_argument(int argc, char** argv) {
    if (argc == 1) {
        return 0;
    }
    if (argc != 3 || std::string_view(argv[1]) != "--duration-ms") {
        throw std::invalid_argument("Usage: window_diagnostics [--duration-ms 1..3600000]");
    }
    const std::string_view input(argv[2]);
    ULONGLONG duration = 0;
    const auto [end, error] = std::from_chars(
        input.data(), input.data() + input.size(), duration);
    if (error != std::errc{} || end != input.data() + input.size() ||
        duration == 0 || duration > 3600000) {
        throw std::invalid_argument("--duration-ms must be between 1 and 3600000");
    }
    return duration;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "Usage: window_diagnostics [--duration-ms 1..3600000]\n"
                         "Read-only window diagnostics. Stop with Ctrl+C.\n";
            return 0;
        }
        const ULONGLONG duration = duration_argument(argc, argv);
        const twm::ConsoleLogger logger;
        const twm::WindowClassifier classifier(GetCurrentProcessId());
        twm::WindowRegistry registry([&logger](const auto& event) { logger(event); });
        twm::WinEventSource source;

        // Подписка предшествует начальному снимку. Все объекты состояния
        // принадлежат этому потоку, callback лишь заполняет свою очередь.
        source.start();
        const ConsoleControl control;
        const ThreadTimer timer;
        const ULONGLONG started = GetTickCount64();
        logger.info(L"Диагностика окон запущена. Выход: Ctrl+C.");
        reconcile(classifier, registry);
        ULONGLONG last_rescan = GetTickCount64();

        MSG message{};
        while (!stop_requested.load(std::memory_order_relaxed)) {
            const BOOL result = GetMessageW(&message, nullptr, 0, 0);
            if (result == -1) {
                fail("GetMessageW");
            }
            if (result == 0 || message.message == stop_message) {
                break;
            }

            if (message.message != twm::WinEventSource::wake_message &&
                message.message != WM_TIMER) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            drain_events(source, classifier, registry);

            const ULONGLONG now = GetTickCount64();
            if (source.take_rescan_request() || now - last_rescan >= rescan_period_ms) {
                reconcile(classifier, registry);
                last_rescan = GetTickCount64();
            }
            if (duration != 0 && now - started >= duration) {
                break;
            }
        }
        source.stop();
        logger.info(L"Диагностика остановлена.");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Fatal: " << error.what() << '\n';
        return 1;
    }
}
