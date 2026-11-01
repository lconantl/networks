# Вывод сервера в консоли

Асинхронный режим, запросы как от браузера, затем Ctrl+C:

```text
Сервер запущен: http://localhost:8080/
  Режим:  асинхронный, epoll в одном потоке
  Сайт:   /home/claude/webserver/www
  Адрес:  127.0.0.1 (только этот компьютер)
Ctrl+C — корректная остановка

15:47:36.381  127.0.0.1:56174        GET / HTTP/1.1  →  200 OK  1.8 КБ  0.2 мс  [epoll]
15:47:36.386  127.0.0.1:56178        GET /style.css HTTP/1.1  →  200 OK  1.2 КБ  0.1 мс  [epoll]
15:47:36.391  127.0.0.1:56192        GET /images/logo.png HTTP/1.1  →  200 OK  1.4 КБ  0.1 мс  [epoll]
15:47:36.395  127.0.0.1:56206        GET /images/photo.jpg HTTP/1.1  →  200 OK  26.4 КБ  0.1 мс  [epoll]
15:47:36.400  127.0.0.1:56216        GET /about.html HTTP/1.1  →  200 OK  1.6 КБ  0.1 мс  [epoll]
15:47:36.404  127.0.0.1:56220        GET /images HTTP/1.1  →  301 Moved Permanently  216 Б  0.1 мс  [epoll]
15:47:36.409  127.0.0.1:56230        GET /missing.html HTTP/1.1  →  404 Not Found  181 Б  0.1 мс  [epoll]
15:47:36.413  127.0.0.1:56234        POST / HTTP/1.1  →  405 Method Not Allowed  216 Б  0.0 мс  [epoll]
15:47:36.418  127.0.0.1:56244        GET /../webserver.cpp HTTP/1.1  →  403 Forbidden  180 Б  0.1 мс  [epoll]
^C* Получен SIGINT: новые подключения не принимаются, активных соединений: 0

Итог работы
  Время работы:  1.1 с
  Соединений:    9 (200: 5, 301: 1, 403: 1, 404: 1, 405: 1)
  Отправлено:    33.2 КБ
Сервер остановлен, все сокеты закрыты

```

В терминале статусы подсвечены цветом: 2xx — зелёный, 3xx — голубой, 4xx — жёлтый, 5xx — красный.

Сырой ответ на запрос несуществующего файла (`curl -i http://localhost:8080/missing.html`):

```text
HTTP/1.1 404 Not Found
Server: lab5-webserver
Date: Sun, 01 Nov 2026 12:47:36 GMT
Content-Type: text/plain; charset=utf-8
Content-Length: 14
Connection: close

File Not Found
```
