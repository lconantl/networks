# Как воспроизвести результаты

Соберите проект из корневой папки репозитория:

```bash
cmake -S rdtp -B 7-lab/build -DCMAKE_BUILD_TYPE=Release
cmake --build 7-lab/build -j
```

## Небольшой файл

В терминале 1:

```bash
./7-lab/build/rdt_receiver 9000 small_received.txt
```

В терминале 2:

```bash
./7-lab/build/rdt_sender 127.0.0.1 9000 7-lab/examples/small_input.txt
cmp 7-lab/examples/small_input.txt small_received.txt
```

## Большой файл (1 МиБ)

В терминале 1:

```bash
./7-lab/build/rdt_receiver 9001 large_received.bin
```

В терминале 2:

```bash
./7-lab/build/rdt_sender 127.0.0.1 9001 7-lab/examples/large_input.bin
sha256sum 7-lab/examples/large_input.bin large_received.bin
cmp 7-lab/examples/large_input.bin large_received.bin
```

## Передача с потерями DATA и ACK

В терминале 1:

```bash
./7-lab/build/rdt_receiver 9002 loss_received.bin --loss 0.20 --delay-ms 20 --seed 99 -d
```

В терминале 2:

```bash
./7-lab/build/rdt_sender 127.0.0.1 9002 7-lab/examples/loss_input.bin --loss 0.20 --delay-ms 20 --window 8 --timeout-ms 120 --seed 42 -d
cmp 7-lab/examples/loss_input.bin loss_received.bin
```

На разных компьютерах и под разной нагрузкой количество тайм-аутов и время могут отличаться даже при одинаковом `--seed` из-за планирования процессов. При больших потерях увеличьте тайм-аут или лимит повторов. Ожидание получателя после FIN задаётся `--linger-ms` (по умолчанию 2500 мс).

В `logs/` находятся реальные журналы четырёх прогонов (small/large/loss/empty), а в `logs/verification.txt` — SHA-256 входных и фактически полученных файлов.
