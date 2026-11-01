import argparse
import concurrent.futures
import hashlib
import http.client
import os
import pathlib
import socket
import sys
import time


class Checker:
    def __init__(self, host, port, root):
        self.host = host
        self.port = port
        self.root = pathlib.Path(root)
        self.failures = 0

    def check(self, name, condition, details=""):
        mark = "OK  " if condition else "FAIL"
        if not condition:
            self.failures += 1
        print(f"  [{mark}] {name}" + (f" — {details}" if details and not condition else ""))

    def request(self, method, path):
        connection = http.client.HTTPConnection(self.host, self.port, timeout=15)
        connection.request(method, path)
        response = connection.getresponse()
        body = response.read()
        connection.close()
        return response, body

    def raw(self, payload, timeout=15):
        with socket.create_connection((self.host, self.port), timeout=timeout) as client:
            if payload:
                client.sendall(payload)
            chunks = []
            while True:
                data = client.recv(65536)
                if not data:
                    break
                chunks.append(data)
        return b"".join(chunks)

    def expect_file(self, path, relative, content_type):
        response, body = self.request("GET", path)
        expected = (self.root / relative).read_bytes()
        self.check(f"GET {path} → 200 {content_type}",
                   response.status == 200 and response.getheader("Content-Type", "").startswith(content_type)
                   and body == expected and int(response.getheader("Content-Length")) == len(expected),
                   f"{response.status} {response.getheader('Content-Type')} {len(body)}/{len(expected)} байт")

    def expect_status(self, name, payload, status_line):
        reply = self.raw(payload)
        first = reply.split(b"\r\n", 1)[0].decode(errors="replace")
        self.check(f"{name} → {status_line}", first == f"HTTP/1.1 {status_line}", first)

    def run_basic(self):
        print("Базовые ответы")
        self.expect_file("/", "index.html", "text/html")
        self.expect_file("/about.html", "about.html", "text/html")
        self.expect_file("/style.css", "style.css", "text/css")
        self.expect_file("/images/logo.png", "images/logo.png", "image/png")
        self.expect_file("/images/photo.jpg", "images/photo.jpg", "image/jpeg")
        self.expect_file("/index.html?v=1#top", "index.html", "text/html")

        response, body = self.request("GET", "/missing.html")
        self.check("GET /missing.html → 404 text/plain «File Not Found»",
                   response.status == 404 and response.getheader("Content-Type", "").startswith("text/plain")
                   and body == b"File Not Found", f"{response.status} {body!r}")

    def run_protocol(self):
        print("Протокол и безопасность")
        response, body = self.request("HEAD", "/index.html")
        size = (self.root / "index.html").stat().st_size
        self.check("HEAD /index.html → 200 без тела",
                   response.status == 200 and body == b"" and int(response.getheader("Content-Length")) == size)

        response, _ = self.request("POST", "/")
        self.check("POST / → 405 с заголовком Allow",
                   response.status == 405 and response.getheader("Allow") == "GET, HEAD")

        response, _ = self.request("GET", "/images")
        self.check("GET /images → 301 на /images/",
                   response.status == 301 and response.getheader("Location") == "/images/")

        self.expect_status("GET /../webserver.cpp", b"GET /../webserver.cpp HTTP/1.1\r\n\r\n", "403 Forbidden")
        self.expect_status("GET /%2e%2e/%2e%2e/etc/passwd",
                           b"GET /%2e%2e/%2e%2e/etc/passwd HTTP/1.1\r\n\r\n", "403 Forbidden")
        self.expect_status("GET //etc/passwd (остаётся внутри www)", b"GET //etc/passwd HTTP/1.1\r\n\r\n", "404 Not Found")
        self.expect_status("GET /%zz", b"GET /%zz HTTP/1.1\r\n\r\n", "400 Bad Request")
        self.expect_status("мусор вместо запроса", b"hello world\r\n\r\n", "400 Bad Request")
        self.expect_status("HTTP/2.0", b"GET / HTTP/2.0\r\n\r\n", "505 HTTP Version Not Supported")
        self.expect_status("заголовки больше 16 КБ", b"GET / HTTP/1.1\r\nX: " + b"a" * 20000,
                           "431 Request Header Fields Too Large")
        self.expect_status("запрос по кускам",
                           b"GET /about.html HTTP/1.1\r\nHost: x\r\n\r\n", "200 OK")

    def run_binary(self):
        print("Большой бинарный файл")
        big = self.root / "big-test.bin"
        big.write_bytes(os.urandom(64 * 1024 * 1024))
        try:
            started = time.time()
            response, body = self.request("GET", "/big-test.bin")
            elapsed = time.time() - started
            self.check(f"GET 64 МБ application/octet-stream, SHA-256 совпадает ({elapsed:.2f} с)",
                       response.status == 200
                       and response.getheader("Content-Type") == "application/octet-stream"
                       and hashlib.sha256(body).digest() == hashlib.sha256(big.read_bytes()).digest())
        finally:
            big.unlink()

    def run_concurrency(self):
        print("Параллельные клиенты")
        expected = (self.root / "images/photo.jpg").read_bytes()

        def fetch(_):
            _, body = self.request("GET", "/images/photo.jpg")
            return body == expected

        with concurrent.futures.ThreadPoolExecutor(max_workers=64) as pool:
            results = list(pool.map(fetch, range(400)))
        self.check("400 запросов в 64 потока — все ответы совпадают", all(results),
                   f"ошибок: {results.count(False)}")

    def run_slow_client(self, timeout):
        print("Медленный клиент")
        started = time.time()
        reply = self.raw(b"GET / HTTP/1.1\r\n", timeout=timeout + 10)
        elapsed = time.time() - started
        self.check(f"незаконченный запрос → 408 через ~{timeout} с ({elapsed:.1f} с)",
                   reply.startswith(b"HTTP/1.1 408"), reply[:40])


def main():
    parser = argparse.ArgumentParser(description="Проверка веб-сервера лабораторной работы №5")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--root", default="www")
    parser.add_argument("--slow", type=int, default=0, help="проверить таймаут медленного клиента, сек")
    arguments = parser.parse_args()

    checker = Checker(arguments.host, arguments.port, arguments.root)
    checker.run_basic()
    checker.run_protocol()
    checker.run_binary()
    checker.run_concurrency()
    if arguments.slow:
        checker.run_slow_client(arguments.slow)

    print("Все проверки пройдены" if checker.failures == 0 else f"Провалено проверок: {checker.failures}")
    sys.exit(1 if checker.failures else 0)


if __name__ == "__main__":
    main()
