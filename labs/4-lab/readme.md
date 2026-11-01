# Веб-сервер — лабораторная работа №5

Статический HTTP-сервер на C++20: GET и HEAD, ответы 200/404 и другие, отдача любых файлов,
три режима — последовательный, многопоточный (+1) и асинхронный на epoll (+2).
Архитектура — в [docs/DESIGN.md](docs/design.md).

## Запуск в WSL

```bash
sudo apt install -y build-essential cmake git
cd /mnt/d/networks/labs/webserver
cmake -S . -B build && cmake --build build -j
./webserver
```

Меню (стрелки или цифры, Enter — по умолчанию):

1. Режим: асинхронный (epoll) / многопоточный / последовательный.
2. Порт — 8080. Папка с сайтом — `www`.
3. Доступ из локальной сети — по умолчанию нет, сервер слушает только 127.0.0.1.
4. Печатать каждый запрос — да (для нагрузочных тестов лучше «нет»).

Откройте `http://localhost:8080/` в браузере Windows. Ctrl+C — корректная остановка с итогами.

Нужен GCC 13+ (есть в Ubuntu 24.04 и новее). Библиотека меню
[survey](https://github.com/lconantl/survey) скачивается CMake автоматически.

## Проверки

```bash
python3 tools/smoke_test.py --port 8080            # 21 проверка, ~2 с
python3 tools/smoke_test.py --port 8080 --slow 10  # + таймаут медленного клиента (5 для блокирующих режимов)
sudo apt install -y apache2-utils valgrind
ab -n 20000 -c 200 http://127.0.0.1:8080/images/photo.jpg
valgrind --leak-check=full --track-fds=yes ./webserver
```

## Что сдавать

- `webserver.cpp`, [docs/DESIGN.md](docs/design.md) — архитектура, сборка и запуск.
- Скриншот браузера с `http://localhost:8080/` и с `http://localhost:8080/missing.html` (404).
- Вывод сервера в консоли — пример в [docs/console.md](docs/console.md).
