import argparse
import asyncio
import random
import statistics
import sys
import time

SERVER_NUMBER = 50


async def exchange(host, port, number, timeout):
    started = time.perf_counter()
    reader, writer = await asyncio.wait_for(asyncio.open_connection(host, port), timeout)
    try:
        writer.write(f"Client of Load Test\n{number}\n".encode())
        await writer.drain()
        reply = await asyncio.wait_for(reader.read(512), timeout)
    finally:
        writer.close()
        await writer.wait_closed()

    lines = reply.decode().splitlines()
    is_valid = len(lines) >= 2 and lines[0].startswith("Server of ") and int(lines[1]) == SERVER_NUMBER
    return is_valid, time.perf_counter() - started


async def rowdy(host, port, payload, wait_limit):
    started = time.perf_counter()
    reader, writer = await asyncio.open_connection(host, port)
    try:
        if payload:
            writer.write(payload)
            await writer.drain()
        data = await asyncio.wait_for(reader.read(512), wait_limit)
    except (asyncio.TimeoutError, ConnectionResetError):
        data = None
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except ConnectionResetError:
            pass

    return data, time.perf_counter() - started


async def run_load(arguments):
    gate = asyncio.Semaphore(arguments.connections)

    async def guarded(_):
        async with gate:
            try:
                return await exchange(arguments.host, arguments.port, random.randint(1, 100), arguments.timeout)
            except (OSError, asyncio.TimeoutError, ValueError):
                return False, 0.0

    started = time.perf_counter()
    results = await asyncio.gather(*(guarded(index) for index in range(arguments.requests)))
    elapsed = time.perf_counter() - started

    latencies = sorted(latency for is_valid, latency in results if is_valid)
    failures = sum(1 for is_valid, _ in results if not is_valid)
    print(f"Нагрузка: {arguments.requests} запросов, одновременно до {arguments.connections}")
    print(f"  Успешно: {len(latencies)}, ошибок: {failures}, за {elapsed:.2f} с "
          f"({arguments.requests / elapsed:.0f} запросов/с)")
    if latencies:
        p99 = latencies[int(len(latencies) * 0.99) - 1]
        print(f"  Задержка: средняя {statistics.mean(latencies) * 1000:.1f} мс, 99% — {p99 * 1000:.1f} мс")
    return failures == 0


async def run_rowdy(arguments):
    cases = [
        ("молчит после подключения", b""),
        ("прислал половину сообщения", b"Client of Rowdy\n4"),
        ("прислал мусор", b"\x00\x01garbage\n\n"),
    ]
    tasks = [rowdy(arguments.host, arguments.port, payload, 15) for _, payload in cases for _ in range(arguments.rowdy)]
    outcomes = await asyncio.gather(*tasks)

    print(f"Буйные клиенты: по {arguments.rowdy} каждого вида")
    is_ok = True
    for index, (name, _) in enumerate(cases):
        group = outcomes[index * arguments.rowdy:(index + 1) * arguments.rowdy]
        closed = [elapsed for data, elapsed in group if data == b""]
        expected_timeout = name != "прислал мусор"
        in_time = [elapsed for elapsed in closed if (9 <= elapsed <= 12.5) == expected_timeout]
        is_ok &= len(in_time) == len(group)
        timing = f"через {statistics.mean(closed):.1f} с" if closed else "не закрыто"
        print(f"  {name}: сервер закрыл {len(closed)}/{len(group)} {timing}")
    return is_ok


async def main():
    parser = argparse.ArgumentParser(description="Нагрузочный тест сервера лабораторной работы №4")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=5555)
    parser.add_argument("--requests", type=int, default=10000)
    parser.add_argument("--connections", type=int, default=1000)
    parser.add_argument("--rowdy", type=int, default=0, help="сколько буйных клиентов каждого вида")
    parser.add_argument("--timeout", type=float, default=30)
    arguments = parser.parse_args()

    rowdy_task = asyncio.create_task(run_rowdy(arguments)) if arguments.rowdy else None
    is_ok = await run_load(arguments)
    if rowdy_task:
        is_ok &= await rowdy_task

    print("Тест пройден" if is_ok else "Тест провален")
    sys.exit(0 if is_ok else 1)


if __name__ == "__main__":
    asyncio.run(main())
