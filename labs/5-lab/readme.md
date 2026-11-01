# TCP клиент-сервер — лабораторная работа №4

Клиент отправляет серверу своё имя и число, сервер отвечает своим именем и числом 50,
обе стороны печатают сумму. Число вне 1–100 останавливает сервер.
Три режима сервера: последовательный, `fork()` (+1), epoll + пул потоков с Ctrl+C,
таймаутами и статистикой (+2). Архитектура — в [docs/DESIGN.md](docs/design.md).

## Сборка в WSL

```bash
sudo apt install -y build-essential cmake
cd /mnt/d/networks/labs/sockets
cmake -S . -B build -DSTUDENT_NAME="Ivan Petrov"
cmake --build build
```

Подставьте своё имя в `STUDENT_NAME` — получится «Client of …» и «Server of …».

## Запуск

Терминал 1:
```bash
./server
```
Меню: режим (1, 2 или 3, Enter — 3), порт (Enter — 5555), подробный вывод (Enter — да).

Терминал 2:
```bash
./client          # ввести 42 → сумма 92
./client          # ввести 150 → сервер ответит и завершится
```

## Проверки

```bash
python3 tools/load_test.py --connections 1000 --requests 20000 --rowdy 10   # режим 3, вывод без подробностей
valgrind --leak-check=full --track-fds=yes ./server
```

Для 1000 одновременных подключений может понадобиться `ulimit -n 4096` в обоих терминалах.
«Буйных» клиентов (`--rowdy`) показывайте в режиме 3: последовательный сервер обслуживает
их по очереди по 10 с.

## Что сдавать

- `client.c`, `server.c`
- [docs/DESIGN.md](docs/design.md) — архитектура
- [docs/examples/](docs/examples) — реальный вывод клиента и сервера во всех режимах, нагрузка и Ctrl+C
