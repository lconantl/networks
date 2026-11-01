import argparse
import datetime
import pathlib
import ssl
import sys
import time
from email import policy
from email.parser import BytesParser

try:
    from aiosmtpd.controller import Controller
    from aiosmtpd.smtp import AuthResult, LoginPassword
except ImportError:
    print("Не найден aiosmtpd. Установите: pip install -r tools/requirements.txt")
    sys.exit(1)


class InboxHandler:
    def __init__(self, inbox: pathlib.Path):
        self.inbox = inbox
        self.inbox.mkdir(parents=True, exist_ok=True)

    async def handle_DATA(self, server, session, envelope):
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S-%f")
        path = self.inbox / f"{stamp}.eml"
        path.write_bytes(envelope.original_content)
        self.print_summary(envelope, path)
        return "250 OK: message saved"

    def print_summary(self, envelope, path):
        message = BytesParser(policy=policy.default).parsebytes(envelope.original_content)
        print("=" * 60)
        print(f"Письмо принято: {path}")
        print(f"  MAIL FROM : {envelope.mail_from}")
        print(f"  RCPT TO   : {', '.join(envelope.rcpt_tos)}")
        print(f"  Subject   : {message.get('Subject', '')}")
        print(f"  Размер    : {len(envelope.original_content)} байт")
        body = message.get_body(preferencelist=("plain",))
        if body is not None:
            print("  Текст     : " + body.get_content().strip().replace("\n", "\n              "))
        for part in message.iter_attachments():
            payload = part.get_payload(decode=True) or b""
            print(f"  Вложение  : {part.get_filename()} ({part.get_content_type()}, {len(payload)} байт)")
        print("=" * 60, flush=True)


class Authenticator:
    def __init__(self, login: str, password: str):
        self.login = login.encode()
        self.password = password.encode()

    def __call__(self, server, session, envelope, mechanism, auth_data):
        if not isinstance(auth_data, LoginPassword):
            return AuthResult(success=False, handled=False)
        is_valid = auth_data.login == self.login and auth_data.password == self.password
        return AuthResult(success=is_valid, handled=False)


def build_tls_context(cert: str, key: str) -> ssl.SSLContext:
    context = ssl.create_default_context(ssl.Purpose.CLIENT_AUTH)
    context.load_cert_chain(cert, key)
    return context


def build_options(arguments) -> dict:
    options = {"hostname": arguments.host, "port": arguments.port}
    if arguments.cert:
        options["tls_context"] = build_tls_context(arguments.cert, arguments.key)
    if arguments.auth:
        login, _, password = arguments.auth.partition(":")
        options["authenticator"] = Authenticator(login, password)
        options["auth_required"] = True
        options["auth_require_tls"] = bool(arguments.cert)
    return options


def parse_arguments():
    parser = argparse.ArgumentParser(description="Локальный SMTP-сервер для лабораторной работы №7")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=2525)
    parser.add_argument("--inbox", default="inbox", help="папка для принятых писем (.eml)")
    parser.add_argument("--cert", help="сертификат PEM — включает STARTTLS")
    parser.add_argument("--key", help="закрытый ключ PEM к сертификату")
    parser.add_argument("--auth", help="логин:пароль — требовать AUTH PLAIN/LOGIN")
    arguments = parser.parse_args()
    if bool(arguments.cert) != bool(arguments.key):
        parser.error("--cert и --key указываются вместе")
    return arguments


def main():
    arguments = parse_arguments()
    controller = Controller(InboxHandler(pathlib.Path(arguments.inbox)), **build_options(arguments))
    controller.start()

    features = ["STARTTLS" if arguments.cert else "без TLS", "AUTH" if arguments.auth else "без AUTH"]
    print(f"SMTP-сервер слушает {arguments.host}:{arguments.port} ({', '.join(features)})")
    print(f"Письма сохраняются в {pathlib.Path(arguments.inbox).resolve()}. Ctrl+C — остановить", flush=True)

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        controller.stop()


if __name__ == "__main__":
    main()
