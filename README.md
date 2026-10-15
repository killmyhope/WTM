# Диагностический реестр окон Windows

Первый этап TWM: C++20, CMake, Win32 и DWM. Приложение наблюдает окна текущего
desktop, классифицирует их и выводит изменения в консоль. Оно не перемещает
окна, не меняет оформление, фокус или горячие клавиши.

## Структура

```text
CMakeLists.txt
include/twm/
  win_event_source.hpp     # WinEvent + ограниченная очередь событий
  window_classifier.hpp   # Снимок окна, идентичность, результат фильтра
  window_registry.hpp     # Поколения HWND и переходы состояния
  console_logger.hpp      # Unicode-консоль / UTF-8 stdout
src/
  main.cpp                # Message loop, начальный снимок, сверка, Ctrl+C
  win_event_source.cpp
  window_classifier.cpp
  window_registry.cpp
  console_logger.cpp
resources/
  app.manifest            # asInvoker, PerMonitorV2
  app.rc
tests/
  registry_tests.cpp      # Детерминированные переходы и HWND reuse
  native_tests.cpp        # Настоящие HWND, DWM, межпроцессные WinEvents
```

## Сборка и запуск

Нужны Windows 10/11, CMake 3.24+ и компилятор C++20. Внешних библиотек нет.
Для MSVC используйте Developer PowerShell с установленным Windows SDK;
для MinGW-w64 добавьте каталог toolchain в PATH. Пример для Ninja:

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
.\build\window_diagnostics.exe
```

С генератором Visual Studio:

```powershell
cmake -S . -B build-msvc -A x64
cmake --build build-msvc --config Debug
ctest --test-dir build-msvc -C Debug --output-on-failure
.\build-msvc\Debug\window_diagnostics.exe
```

Остановка — Ctrl+C/Ctrl+Break. Для ограниченного диагностического запуска:

```powershell
.\build\window_diagnostics.exe --duration-ms 5000
```

MinGW-сборке нужны runtime DLL её toolchain в PATH. Нативные тесты требуют
интерактивный Windows desktop с DWM. Они кратко показывают собственные окна
без активации и запускают собственный дочерний процесс; существующие окна
пользователя не изменяются.

## Что означает лог

```text
Найдено окно: [Блокнот] [0x123456]
Окно скрыто: [Блокнот] [0x123456] (Окно свёрнуто)
Найдено окно: [Блокнот] [0x123456]
Окно закрыто: [Блокнот] [0x123456]
```

`Найдено окно` означает переход в состояние, подходящее для тайлинга, включая
первоначальное обнаружение и возвращение после скрытия. Повторные SHOW,
LOCATIONCHANGE, сверка и изменение заголовка не дублируют сообщение.
`Окно скрыто` означает hide/minimize/cloak, а `Окно закрыто` — завершение жизни
окна. Если известное окно перестало проходить структурный фильтр, выводится
`Окно исключено` с причиной. Закрытие ранее скрытого окна тоже фиксируется.
Служебные окна, которые никогда не проходили фильтр, в лог не попадают.

Заголовки читаются в Unicode, ограничены 4095 UTF-16 code units; управляющие
символы заменяются пробелами. В консоль используется WriteConsoleW, при
перенаправлении stdout — UTF-8. Пустой заголовок отображается как `без заголовка`.

## Событийная модель

1. Основной поток устанавливает hooks **до** начального EnumWindows.
2. OUTOFCONTEXT callback сохраняет POD-событие в очередь на 4096 элементов и
   отправляет объединённое wake-сообщение. В callback нет запросов свойств,
   логирования, аллокаций или изменений реестра.
3. Message loop дренирует очередь и обновляет реестр. Состояние имеет одного
   владельца; повторный вход в WinEvent callback не запускает обработку рекурсивно.
4. Таймер каждые 250 мс обеспечивает резервное дренирование. Полная сверка
   выполняется раз в секунду и после переполнения очереди/ошибки wake.
5. Все hooks снимаются на том же потоке через RAII. GetMessage различает
   нормальное сообщение, WM_QUIT и ошибку -1. Исключения не выходят из callbacks.

Подписки: CREATE/DESTROY/SHOW/HIDE, STATECHANGE/LOCATIONCHANGE/NAMECHANGE,
CLOAKED/UNCLOAKED, FOREGROUND и MINIMIZESTART/END. Для object events допускаются
только OBJID_WINDOW/CHILDID_SELF; для system events это ограничение не применяется.

WinEvents служат поводом проверить актуальное состояние. DESTROY дополнительно
завершает поколение записи, даже если HWND уже используется новым окном того же
потока. Метка события сравнивается с моментом первого наблюдения, чтобы старый
DESTROY не закрыл новое окно, уже обнаруженное снимком. 32-битная метка
разворачивается относительно GetTickCount64; предполагается задержка доставки
менее одного периода DWORD (около 49.7 дней).

## Классификатор

`WindowClassifier::inspect` возвращает одно из пяти состояний:

| Состояние | Значение |
|---|---|
| Eligible | Обычное видимое top-level окно, кандидат для тайлинга |
| Inactive | Скрыто, свёрнуто или cloaked; известная запись сохраняется |
| Ignored | Собственное, дочернее, owned, служебное или системное окно |
| Pending | Свойства недоступны, идентичность меняется или геометрия не готова |
| Missing | HWND больше не существует |

Проверяются PID/TID, собственная консоль, desktop/shell, WS_CHILD, GA_ROOT,
WS_EX_TOOLWINDOW, WS_EX_NOACTIVATE, owner, IsWindowVisible, IsIconic,
DWMWA_CLOAKED и непустая геометрия. PID/TID повторно проверяются после чтения.
Ошибки Win32/DWM сохраняются в причине Pending и не трактуются как закрытие.

Классы оболочки и точные имена её процессов перечислены в
`src/window_classifier.cpp`. Это изменяемые эвристики совместимости. explorer.exe
целиком не исключается: обычный Проводник подходит для тайлинга. Owned dialogs
пока исключаются; в будущем они смогут стать floating-окнами.

Пустой заголовок, WS_POPUP, WS_EX_LAYERED и сам факт использования WinUI 3
не являются причиной исключения. Отдельного определения всех UWP/frameworks
в этом MVP нет; глобального запрета пользовательских UWP/WinUI 3 тоже нет.

## Границы точности

- Это реестр текущего наблюдаемого состояния, а не полный исторический журнал.
  Окно, созданное и уничтоженное до обработки события, может не иметь уже
  читаемого заголовка и не попасть в лог. Быстрые промежуточные hide/show также
  могут схлопнуться в последнее наблюдаемое состояние.
- IsWindowVisible не проверяет перекрытие другими окнами или наличие пикселей
  на мониторе. Такое окно остаётся обычным кандидатом для тайлинга.
- HWND и PID/TID не дают атомарной идентичности чужого окна. При same-thread
  reuse в пределах одинаковой временной метки остаётся неоднозначность;
  сверка не превращает Win32 в атомарный снимок.
- EnumWindows не обещает обнаружение всех вариантов UWP. Новые поверхности
  оболочки могут потребовать уточнения эвристик. Окна других desktop/session
  вне области этого наблюдателя.
- Если свойства или имя процесса недоступны, кандидат остаётся Pending;
  периодическая сверка повторяет проверку. Уже известная запись при временной
  ошибке сохраняет последнее подтверждённое состояние.

## Проверки

`registry_transitions`: дедупликация, rename, hide/restore, ошибки свойств,
исключение по стилям, удаление скрытого окна, смена PID, same-thread HWND reuse,
устаревший и повторный DESTROY.

`native_windows_and_events`: настоящие обычные/пустые/hidden/tool/noactivate/
owned/child/cloaked/destroyed окна; межпроцессная последовательность
CREATE → SHOW → HIDE → SHOW → DESTROY; WINEVENT_SKIPOWNPROCESS и отсутствие
дублирующихся подписок.

## Контракты Windows API

- [SetWinEventHook](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-setwineventhook)
- [GetMessageW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getmessagew)
- [EnumWindows](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-enumwindows)
- [IsWindow](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-iswindow)
- [DWM attributes](https://learn.microsoft.com/en-us/windows/win32/api/dwmapi/ne-dwmapi-dwmwindowattribute)
- [GetWindowTextW](https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getwindowtextw)
