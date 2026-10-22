#include "twm/console_logger.hpp"
#include "twm/layout_controller.hpp"
#include "twm/monitor_context.hpp"
#include "twm/window_geometry.hpp"
#include "twm/win_event_source.hpp"
#include "twm/window_classifier.hpp"
#include "twm/window_registry.hpp"

#include <atomic>
#include <charconv>
#include <exception>
#include <iostream>
#include <sstream>
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
        if (event.event == EVENT_OBJECT_DESTROY || event.event == EVENT_SYSTEM_MOVESIZESTART ||
            event.event == EVENT_SYSTEM_MOVESIZEEND) {
            // Разворачиваем 32-битную метку WinEvent относительно текущего
            // uptime. Событие должно быть доставлено менее чем за 49.7 дней.
            const ULONGLONG now = GetTickCount64();
            const DWORD age = static_cast<DWORD>(now) - event.timestamp;
            const ULONGLONG event_at = age <= now ? now - age : 0;
            if (event.event == EVENT_OBJECT_DESTROY) { registry.on_destroy(event.hwnd, event_at); }
            else { registry.on_interactive_move(event.hwnd, event.event == EVENT_SYSTEM_MOVESIZESTART, event_at); }
        }
        // Событие не является снимком. Missing удалит запись даже при
        // IsWindow == FALSE; новое окно с reused HWND получит новую запись.
        const auto observation = classifier.inspect(event.hwnd);
        registry.observe(observation, GetTickCount64());
    }
}

struct Options {
    ULONGLONG duration = 0;
    bool preview = false;
    bool manage = false;
};

Options arguments(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--preview-layout") {
            options.preview = true;
        } else if (argument == "--manage-windows") {
            options.manage = true;
        } else if (argument == "--duration-ms" && index + 1 < argc) {
            const std::string_view input(argv[++index]);
            const auto [end, error] = std::from_chars(
                input.data(), input.data() + input.size(), options.duration);
            if (error != std::errc{} || end != input.data() + input.size() ||
                options.duration == 0 || options.duration > 3600000) {
                throw std::invalid_argument("--duration-ms must be between 1 and 3600000");
            }
        } else {
            throw std::invalid_argument(
                "Usage: window_diagnostics [--duration-ms 1..3600000] [--preview-layout | --manage-windows]");
        }
    }
    if (options.preview && options.manage) {
        throw std::invalid_argument("--preview-layout and --manage-windows are mutually exclusive");
    }
    return options;
}

void update_management(const twm::WindowRegistry& registry, twm::MonitorContext& monitors,
                       twm::LayoutController& controller) {
    if (!monitors.state().snapshot) { controller.pause(); return; }
    const auto monitor = *monitors.state().snapshot;
    const auto revision = monitors.state().revision;
    const auto gap = twm::dip_to_pixels(8, monitor.dpi);
    if (!gap.ok()) { controller.pause(); return; }
    const twm::WindowGeometryReader reader;
    std::vector<twm::LayoutInput> inputs;
    for (const auto& candidate : registry.managed_windows().layout_windows()) {
        const auto identity = registry.find_identity(candidate.id);
        if (!identity) { controller.pause(); return; }
        const auto host = MonitorFromWindow(identity->hwnd, MONITOR_DEFAULTTONULL);
        if (!host) { controller.pause(); return; }
        if (host != monitor.monitor) { continue; }
        const auto read = reader.read(*identity);
        if (read.status == twm::GeometryReadStatus::Inactive) { continue; }
        if (!read.ok() || MonitorFromWindow(identity->hwnd, MONITOR_DEFAULTTONULL) != host) {
            controller.pause(); return;
        }
        // Borderless fullscreen applications are not resized by the MVP.
        if (twm::contains_rectangle(read.snapshot->visible, monitor.monitor_area)) { continue; }
        inputs.push_back({candidate.id, *read.snapshot, candidate.constraints});
    }
    (void)monitors.process_notifications();
    if (!monitors.state().snapshot || monitors.state().revision != revision) { controller.pause(); return; }
    for (const auto& input : inputs) {
        if (registry.find_identity(input.id) != input.geometry.identity) { controller.pause(); return; }
    }
    controller.update({monitor.monitor, monitor.work_area, monitor.dpi, revision,
                       {gap.value, gap.value, 600}}, inputs);
}

std::wstring preview_layout(const twm::WindowRegistry& registry, twm::MonitorContext& monitors) {
    (void)monitors.process_notifications();
    if (!monitors.state().snapshot) {
        return L"Предпросмотр отложен: контекст монитора недоступен.";
    }
    const auto monitor = *monitors.state().snapshot;
    const auto revision = monitors.state().revision;
    const auto gap = twm::dip_to_pixels(8, monitor.dpi);
    if (!gap.ok()) {
        return L"Предпросмотр отложен: некорректный масштаб DIP.";
    }
    const twm::WindowGeometryReader reader;
    std::vector<twm::LayoutWindow> candidates;
    std::vector<twm::WindowGeometrySnapshot> snapshots;
    for (const auto& candidate : registry.managed_windows().layout_windows()) {
        const auto identity = registry.find_identity(candidate.id);
        if (!identity) {
            return L"Предпросмотр отложен: изменилась жизнь окна.";
        }
        const auto host = MonitorFromWindow(identity->hwnd, MONITOR_DEFAULTTONULL);
        if (host == nullptr) {
            return L"Предпросмотр отложен: монитор окна недоступен.";
        }
        if (host != monitor.monitor) {
            continue;
        }
        const auto read = reader.read(*identity);
        if (read.status == twm::GeometryReadStatus::Inactive) {
            continue; // Например, максимизированное окно не включается в тайлинг.
        }
        if (!read.ok()) {
            return L"Предпросмотр отложен: нет согласованной геометрии окна (status=" +
                std::to_wstring(static_cast<int>(read.status)) + L").";
        }
        if (MonitorFromWindow(identity->hwnd, MONITOR_DEFAULTTONULL) != host) {
            return L"Предпросмотр отложен: окно сменило монитор во время чтения.";
        }
        candidates.push_back(candidate);
        snapshots.push_back(*read.snapshot);
    }
    const auto layout = twm::solve_master_stack(monitor.work_area, candidates,
                                                {gap.value, gap.value, 600});
    if (!layout.ok()) {
        return L"Предпросмотр: раскладка не построена (error=" +
            std::to_wstring(static_cast<int>(layout.error)) + L").";
    }
    std::vector<twm::Rect> outer_targets;
    for (std::size_t index = 0; index < layout.placements.size(); ++index) {
        const auto outer = twm::outer_from_visible(layout.placements[index].target,
                                                  snapshots[index].insets, monitor.work_area);
        if (!outer.ok()) {
            return L"Предпросмотр отложен: targetOuter не прошёл проверку.";
        }
        outer_targets.push_back(outer.value);
    }
    // Уведомление могло прийти реентрантно во время чтения чужих HWND.
    (void)monitors.process_notifications();
    (void)monitors.refresh();
    if (!monitors.state().snapshot || monitors.state().revision != revision) {
        return L"Предпросмотр отложен: изменилась конфигурация монитора.";
    }
    for (std::size_t index = 0; index < snapshots.size(); ++index) {
        if (registry.find_identity(candidates[index].id) != snapshots[index].identity ||
            MonitorFromWindow(snapshots[index].identity.hwnd, MONITOR_DEFAULTTONULL) != monitor.monitor) {
            return L"Предпросмотр отложен: изменилась принадлежность окна.";
        }
    }
    std::wostringstream text;
    text << L"Предпросмотр Master-Stack: DPI=" << monitor.dpi << L", окон=" << candidates.size();
    const auto describe = [&text](const twm::Rect& rectangle) {
        text << L'(' << rectangle.left << L',' << rectangle.top << L','
             << rectangle.right << L',' << rectangle.bottom << L')';
    };
    for (std::size_t index = 0; index < layout.placements.size(); ++index) {
        text << L"\r\n  WindowId=" << layout.placements[index].id << L" visible=";
        describe(layout.placements[index].target);
        text << L" outer=";
        describe(outer_targets[index]);
    }
    return text.str();
}

std::wstring monitor_description(const twm::MonitorState& state) {
    if (!state.snapshot) {
        return L"Контекст монитора недоступен: status=" +
            std::to_wstring(static_cast<int>(state.status)) + L", error=" + std::to_wstring(state.error);
    }
    const auto& monitor = *state.snapshot;
    std::wostringstream text;
    text << L"Монитор: DPI=" << monitor.dpi << L", rcWork=("
         << monitor.work_area.left << L',' << monitor.work_area.top << L','
         << monitor.work_area.right << L',' << monitor.work_area.bottom
         << L"), revision=" << state.revision;
    return text.str();
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string_view(argv[1]) == "--help") {
            std::cout << "Usage: window_diagnostics [--duration-ms 1..3600000] [--preview-layout | --manage-windows]\n"
                         "Read-only by default. --manage-windows enables real tiling on the selected monitor.\n"
                         "Stop with Ctrl+C; window positions are retained (restore is a later stage).\n";
            return 0;
        }
        const Options options = arguments(argc, argv);
        const twm::ConsoleLogger logger;
        const twm::WindowClassifier classifier(GetCurrentProcessId());
        twm::WindowRegistry registry([&logger](const auto& event) { logger(event); });
        twm::WinEventSource source;

        // Подписка предшествует начальному снимку. Все объекты состояния
        // принадлежат этому потоку, callback лишь заполняет свою очередь.
        source.start();
        const ConsoleControl control;
        const ThreadTimer timer;
        twm::MonitorContext monitors;
        (void)monitors.select_for_window(GetForegroundWindow());
        (void)monitors.process_notifications();
        const ULONGLONG started = GetTickCount64();
        logger.info(L"Диагностика окон запущена. Выход: Ctrl+C.");
        logger.info(monitor_description(monitors.state()));
        auto reported_monitor_revision = monitors.state().revision;
        reconcile(classifier, registry);
        // Declared after MonitorContext: worker is stopped before control HWND dies.
        std::unique_ptr<twm::WindowActuator> actuator;
        std::unique_ptr<twm::LayoutController> controller;
        if (options.manage) {
            actuator = std::make_unique<twm::WindowActuator>(monitors.control_hwnd());
            controller = std::make_unique<twm::LayoutController>(*actuator,
                [&logger](std::wstring_view text) { logger.info(std::wstring(text)); });
            logger.info(L"Управление окнами включено (--manage-windows). При выходе позиции сохраняются.");
            update_management(registry, monitors, *controller);
            controller->tick(GetTickCount64());
        }
        std::wstring previous_preview;
        const auto print_preview = [&] {
            const auto text = preview_layout(registry, monitors);
            if (text != previous_preview) {
                logger.info(text);
                previous_preview = text;
            }
        };
        if (options.preview) { print_preview(); }
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
            const auto previous_monitor_revision = monitors.state().revision;
            (void)monitors.process_notifications();

            const ULONGLONG now = GetTickCount64();
            if (source.take_rescan_request() || now - last_rescan >= rescan_period_ms) {
                reconcile(classifier, registry);
                (void)monitors.refresh();
                if (options.preview) { print_preview(); }
                last_rescan = GetTickCount64();
            } else if (options.preview && monitors.state().revision != previous_monitor_revision) {
                print_preview();
            }
            if (monitors.state().revision != reported_monitor_revision) {
                logger.info(monitor_description(monitors.state()));
                reported_monitor_revision = monitors.state().revision;
            }
            if (controller) {
                // Read first, then drain worker replies: an early LOCATIONCHANGE
                // can acknowledge a command without requiring a second event.
                update_management(registry, monitors, *controller);
                controller->tick(GetTickCount64());
            }
            if (options.duration != 0 && now - started >= options.duration) {
                break;
            }
        }
        if (controller) { controller->pause(); }
        if (actuator) { actuator->stop(); }
        source.stop();
        logger.info(L"Диагностика остановлена.");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Fatal: " << error.what() << '\n';
        return 1;
    }
}
