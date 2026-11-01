# Примеры работы

Клиент:

```
antllcon@antllcon:/mnt/d/networks$ cd /mnt/d/networks/labs/smtp-client && ./smtp-client
SMTP-клиент — лабораторная работа №7
Стрелки — выбор, Пробел — отметить, Enter — подтвердить, Esc — выход

Куда отправляем?
- Локальный тестовый сервер — 127.0.0.1:2525, без шифрования
  От кого (MAIL FROM)
- student@test.local
  Кому (несколько адресов — через запятую)
- teacher@test.local
  Тема
- Лабораторная работа №7
  Текст письма
- Ввести в консоли
  Текст (\n — новая строка)
- Hello! This is test message!
  Что ещё добавить? (Enter — ничего)
- —
  Как показывать SMTP-диалог?
- Полностью — строки S: и C:
  Отправить письмо?
- Да

* Подключение к 127.0.0.1:2525
* Соединение установлено: TCP без шифрования
  S: 220 antllcon.localdomain Python SMTP 1.4.6
  C: EHLO antllcon
  S: 250-antllcon.localdomain
  S: 250-SIZE 33554432
  S: 250-8BITMIME
  S: 250-SMTPUTF8
  S: 250 HELP
  C: MAIL FROM:<student@test.local>
  S: 250 OK
  C: RCPT TO:<teacher@test.local>
  S: 250 OK
  C: DATA
  S: 354 End data with <CR><LF>.<CR><LF>
  C: From: student@test.local
  C: To: teacher@test.local
  C: Subject: =?UTF-8?B?0JvQsNCx0L7RgNCw0YLQvtGA0L3QsNGPINGA0LDQsdC+0YLQsCDihJY3?=
  C: Date: Sun, 01 Nov 2026 12:21:39 +0000
  C: Message-ID: <22735af872b45a6cffcca7d601047501@test.local>
  C: MIME-Version: 1.0
  C: X-Mailer: smtp-client (lab 7)
  C: Content-Type: text/plain; charset=utf-8
  C: Content-Transfer-Encoding: 7bit
  C:
  C: Hello! This is test message!
  C: .
  S: 250 OK: message saved
  C: QUIT
  S: 221 Bye

Письмо принято сервером: 250 OK: message saved
antllcon@antllcon:/mnt/d/networks/labs/smtp-client$ 
```

Сервер:
```
SMTP-сервер слушает 127.0.0.1:2525 (без TLS, без AUTH)
Письма сохраняются в /mnt/d/networks/labs/smtp-client/inbox. Ctrl+C — остановить
============================================================
Письмо принято: inbox/20261101-152139-954305.eml
  MAIL FROM : student@test.local
  RCPT TO   : teacher@test.local
  Subject   : Лабораторная работа №7
  Размер    : 383 байт
  Текст     : Hello! This is test message!
============================================================
```

