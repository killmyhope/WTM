#include "twm/window_registry.hpp"

#include <utility>
#include <stdexcept>

namespace twm {

void WindowRegistry::on_interactive_move(HWND hwnd, bool starting, std::uint64_t event_at) {
    const auto found = records_.find(hwnd);
    if (found == records_.end() || event_at <= found->second.first_observed_at) { return; }
    if (starting) { (void)managed_.begin_interactive_move(found->second.generation); }
    else { (void)managed_.end_interactive_move(found->second.generation); }
}

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
            (void)managed_.remove(it->second.generation);
            emit(NotificationKind::Closed, it->second);
            records_.erase(it);
        }
        return;
    }

    // Ошибка чтения свойств не доказывает ни закрытие, ни скрытие.
    if (observation.disposition == WindowDisposition::Pending) {
        if (it != records_.end()) {
            (void)managed_.set_availability(it->second.generation, CandidateAvailability::Pending);
        }
        return;
    }

    if (it != records_.end() &&
        it->second.identity != observation.identity) {
        (void)managed_.remove(it->second.generation);
        emit(NotificationKind::Closed, it->second);
        records_.erase(it);
        it = records_.end();
    }

    if (observation.disposition == WindowDisposition::Eligible) {
        if (it == records_.end()) {
            if (next_generation_ == 0) {
                throw std::overflow_error("Window lifetime ID space exhausted");
            }
            Record record{observation.identity, next_generation_++,
                          observation.title, true, observed_at};
            auto [inserted, unused] = records_.emplace(
                observation.identity.hwnd, std::move(record));
            (void)unused;
            if (!managed_.admit(inserted->second.generation)) {
                throw std::logic_error("Non-monotonic window lifetime ID");
            }
            emit(NotificationKind::Found, inserted->second);
        } else {
            auto& record = it->second;
            record.title = observation.title;
            (void)managed_.set_availability(record.generation, CandidateAvailability::Ready);
            if (!record.eligible) {
                record.eligible = true;
                emit(NotificationKind::Found, record);
            }
        }
        return;
    }

    // Не заполняем реестр системными HWND. Сохраняем лишь окна,
    // которые хотя бы однажды прошли классификатор.
    if (it != records_.end()) {
        (void)managed_.set_availability(it->second.generation,
            observation.disposition == WindowDisposition::Inactive
                ? CandidateAvailability::Inactive : CandidateAvailability::Ineligible);
    }
    if (it != records_.end() &&
        observation.disposition == WindowDisposition::Inactive) {
        // Inactive устанавливается после успешного чтения caption. Имя
        // может измениться и у скрытого окна; Pending/Ignored такого
        // контракта не имеют и не должны затирать последнее известное имя.
        it->second.title = observation.title;
    }
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
        (void)managed_.remove(it->second.generation);
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

std::optional<WindowIdentity> WindowRegistry::find_identity(WindowId id) const noexcept {
    for (const auto& [hwnd, record] : records_) {
        (void)hwnd;
        if (record.generation == id) {
            return record.identity;
        }
    }
    return std::nullopt;
}

} // namespace twm
