#include "twm/win_event_source.hpp"

#include <exception>
#include <stdexcept>

namespace twm {

thread_local WinEventSource* WinEventSource::active_source_ = nullptr;

WinEventSource::WinEventSource()
    : owner_thread_id_(GetCurrentThreadId()) {
}

WinEventSource::~WinEventSource() noexcept {
    stop();
}

void WinEventSource::start() {
    if (GetCurrentThreadId() != owner_thread_id_) {
        throw std::logic_error(
            "WinEventSource::start must run on its owning message-loop thread");
    }

    if (started_) {
        return;
    }

    if (active_source_ != nullptr) {
        throw std::logic_error(
            "Only one active WinEventSource is allowed per thread");
    }

    // PostThreadMessage требует уже существующую очередь сообщений.
    MSG message{};
    PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    struct Subscription {
        DWORD first;
        DWORD last;
    };

    constexpr std::array<Subscription, subscription_count> subscriptions{{
        {EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE},
        {EVENT_OBJECT_STATECHANGE, EVENT_OBJECT_NAMECHANGE},
        {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
        {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
        {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
        {EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND},
    }};

    active_source_ = this;
    started_ = true;

    for (std::size_t index = 0; index < subscriptions.size(); ++index) {
        const auto& subscription = subscriptions[index];
        hooks_[index] = SetWinEventHook(
            subscription.first,
            subscription.last,
            nullptr,
            &WinEventSource::callback,
            0,
            0,
            WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);

        if (hooks_[index] == nullptr) {
            stop();
            // SetWinEventHook не документирует расширенную ошибку через
            // GetLastError; не выдаём случайное старое значение за причину.
            throw std::runtime_error("SetWinEventHook failed to install a subscription");
        }
    }
}

void WinEventSource::stop() noexcept {
    if (!started_) {
        return;
    }

    // UnhookWinEvent необходимо вызывать из потока установки. Продолжать
    // разрушение на другом потоке опасно: callback может обратиться к this.
    if (GetCurrentThreadId() != owner_thread_id_) {
        std::terminate();
    }

    // Сначала отключаем маршрутизацию callback, включая возможный reentry
    // во время снятия hooks. callback никогда не захватывает указатель this.
    active_source_ = nullptr;
    for (auto& hook : hooks_) {
        if (hook != nullptr) {
            UnhookWinEvent(hook);
            hook = nullptr;
        }
    }

    started_ = false;
    read_position_ = 0;
    write_position_ = 0;
    queue_size_ = 0;
    wake_pending_ = false;
    rescan_requested_ = false;
}

bool WinEventSource::pop(WindowEvent& event) noexcept {
    if (queue_size_ == 0) {
        wake_pending_ = false;
        return false;
    }

    event = queue_[read_position_];
    read_position_ = (read_position_ + 1) % queue_capacity;
    --queue_size_;
    if (queue_size_ == 0) {
        wake_pending_ = false;
    }
    return true;
}

bool WinEventSource::take_rescan_request() noexcept {
    const bool requested = rescan_requested_;
    rescan_requested_ = false;
    return requested;
}

void CALLBACK WinEventSource::callback(
    HWINEVENTHOOK,
    const DWORD event,
    const HWND hwnd,
    const LONG object_id,
    const LONG child_id,
    const DWORD event_thread,
    const DWORD timestamp) noexcept {
    WinEventSource* const source = active_source_;
    if (source == nullptr || hwnd == nullptr) {
        return;
    }

    bool object_event = false;
    switch (event) {
    case EVENT_OBJECT_CREATE:
    case EVENT_OBJECT_DESTROY:
    case EVENT_OBJECT_SHOW:
    case EVENT_OBJECT_HIDE:
    case EVENT_OBJECT_STATECHANGE:
    case EVENT_OBJECT_LOCATIONCHANGE:
    case EVENT_OBJECT_NAMECHANGE:
    case EVENT_OBJECT_CLOAKED:
    case EVENT_OBJECT_UNCLOAKED:
        object_event = true;
        break;
    case EVENT_SYSTEM_FOREGROUND:
    case EVENT_SYSTEM_MINIMIZESTART:
    case EVENT_SYSTEM_MINIMIZEEND:
    case EVENT_SYSTEM_MOVESIZESTART:
    case EVENT_SYSTEM_MOVESIZEEND:
        break;
    default:
        return;
    }

    // У системных событий object/child не обязаны описывать OBJID_WINDOW.
    // CHILDID_SELF имеет значение 0; oleacc.h для этого не требуется.
    if (object_event && (object_id != OBJID_WINDOW || child_id != 0)) {
        return;
    }

    // Не спрашиваем здесь свойства HWND: DESTROY может прийти уже после
    // уничтожения окна. Проверка актуальности выполняется потребителем.
    source->enqueue({event, hwnd, object_id, child_id, event_thread, timestamp});
}

void WinEventSource::enqueue(const WindowEvent& event) noexcept {
    // OUTOFCONTEXT доставляет callback на поток установки hooks. Очередь
    // тоже читается этим потоком, поэтому межпоточная синхронизация не нужна.
    if (queue_size_ == queue_capacity) {
        // При потере событий границы жизненного цикла больше ненадёжны.
        // Потребитель должен восстановить фактическое состояние EnumWindows.
        rescan_requested_ = true;
        request_wake();
        return;
    }

    queue_[write_position_] = event;
    write_position_ = (write_position_ + 1) % queue_capacity;
    ++queue_size_;
    request_wake();
}

void WinEventSource::request_wake() noexcept {
    if (wake_pending_) {
        return;
    }

    if (PostThreadMessageW(owner_thread_id_, wake_message, 0, 0)) {
        wake_pending_ = true;
    } else {
        // Очередь остаётся целой. Таймер message loop читает её даже при
        // потере wake-сообщения, затем запрашивает полную сверку реестра.
        rescan_requested_ = true;
    }
}

} // namespace twm
