#include "twm/window_registry.hpp"

#include <utility>

namespace twm {

WindowRegistry::WindowRegistry(Sink sink) : sink_(std::move(sink)) {}

void WindowRegistry::emit(NotificationKind kind, const Record& record,
                          const std::wstring& reason) {
    sink_({kind, record.identity, record.generation, record.title, reason});
}

void WindowRegistry::observe(const WindowObservation& observation,
                             std::uint64_t observed_at) {
    auto it = records_.find(observation.identity.hwnd);

    if (observation.disposition == WindowDisposition::Missing) {
        if (it != records_.end()) {
            emit(NotificationKind::Closed, it->second);
            records_.erase(it);
        }
        return;
    }

    // Ошибка чтения свойств не доказывает ни закрытие, ни скрытие.
    if (observation.disposition == WindowDisposition::Pending) {
        return;
    }

    if (it != records_.end() &&
        it->second.identity != observation.identity) {
        emit(NotificationKind::Closed, it->second);
        records_.erase(it);
        it = records_.end();
    }

    if (observation.disposition == WindowDisposition::Eligible) {
        if (it == records_.end()) {
            Record record{observation.identity, next_generation_++,
                          observation.title, true, observed_at};
            auto [inserted, unused] = records_.emplace(
                observation.identity.hwnd, std::move(record));
            (void)unused;
            emit(NotificationKind::Found, inserted->second);
        } else {
            auto& record = it->second;
            record.title = observation.title;
            if (!record.eligible) {
                record.eligible = true;
                emit(NotificationKind::Found, record);
            }
        }
        return;
    }

    // Не заполняем реестр системными HWND. Сохраняем лишь окна,
    // которые хотя бы однажды прошли классификатор.
    if (it != records_.end() && it->second.eligible) {
        auto& record = it->second;
        record.eligible = false;
        emit(observation.disposition == WindowDisposition::Inactive
                 ? NotificationKind::Hidden : NotificationKind::Excluded,
             record, observation.reason);
    }
}

void WindowRegistry::on_destroy(HWND hwnd, std::uint64_t event_at) {
    const auto it = records_.find(hwnd);
    // DESTROY завершает старую жизнь даже при reuse тем же PID/TID.
    // Старое событие, предшествующее обнаружению текущей записи, не должно
    // закрыть новое окно из начального/периодического снимка. При одинаковой
    // метке и same-PID/TID reuse жизни неразличимы; выбираем не закрывать
    // живую запись. Наблюдение Missing всё равно удалит исчезнувшее окно.
    if (it != records_.end() && event_at > it->second.first_observed_at) {
        emit(NotificationKind::Closed, it->second);
        records_.erase(it);
    }
}

std::vector<HWND> WindowRegistry::tracked_handles() const {
    std::vector<HWND> handles;
    handles.reserve(records_.size());
    for (const auto& [hwnd, record] : records_) {
        (void)record;
        handles.push_back(hwnd);
    }
    return handles;
}

} // namespace twm
