Почтовый клиент, который передаёт письмо SMTP-серверу. Пользователь задаёт параметры сервера (`host`, `port`), при
необходимости логин/пароль и создаёт письмо: отправитель, получатели, тема, текст, вложения. Программа устанавливает
TCP-соединение с SMTP-сервером, сервер отвечает `220`, клиент представляется через `EHLO`, при необходимости выполняет
`AUTH`, затем передаёт SMTP-конверт командами `MAIL FROM` и `RCPT TO`, после `DATA` отправляет сформированное письмо и
завершает диалог через `QUIT`.

Если соединение защищённое, поверх TCP создаётся TLS-соединение через OpenSSL; затем весь тот же SMTP-диалог идёт уже
внутри защищённого канала.

```
flowchart TD
    A["main.cpp"] --> B["ConsoleView"]
    A --> C["MailService"]
    B --> D["SendMailRequest"]
    C --> D
    C --> E["IMailSender"]
    E -.-> F["SmtpMailSender"]
    F --> G["SmtpSession / TcpTransport"]
    style B fill:#dbeafe,color:#172554,stroke:#93c5fd
    style C fill:#dcfce7,color:#14532d,stroke:#86efac
    style F fill:#f3f4f6,color:#374151,stroke:#d1d5db
    style G fill:#f3f4f6,color:#374151,stroke:#d1d5db
```