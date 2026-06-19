#!/usr/bin/env python3
"""Minimal one-shot SMTP sink for testing pgforge's mailer (no TLS, no auth).

Speaks just enough SMTP for libcurl: 220 greeting, EHLO/MAIL/RCPT -> 250, DATA ->
354 then collect until the lone "." -> 250. Writes the captured DATA payload to
the given file and exits after the first complete message. Accepts in a loop so a
liveness probe that connects-and-closes doesn't consume the real delivery.

    mock_smtp.py <port> <capture-file>
"""
import socket, sys


def handle(conn, capture_path):
    def reply(s):
        try: conn.sendall(s.encode())
        except OSError: pass

    reply("220 mock ESMTP pgforge-test\r\n")
    buf = b""
    data_mode = False
    captured = []
    got_message = False
    while True:
        try: chunk = conn.recv(4096)
        except OSError: break
        if not chunk: break
        buf += chunk
        while b"\r\n" in buf:
            line, buf = buf.split(b"\r\n", 1)
            if data_mode:
                if line == b".":
                    data_mode = False
                    got_message = True
                    reply("250 OK queued\r\n")
                else:
                    captured.append(line)
                continue
            u = line.decode("utf-8", "replace").upper()
            if u.startswith(("EHLO", "HELO")):
                reply("250 mock\r\n")
            elif u.startswith("DATA"):
                reply("354 End data with <CR><LF>.<CR><LF>\r\n")
                data_mode = True
            elif u.startswith("QUIT"):
                reply("221 Bye\r\n")
                break
            else:                       # MAIL, RCPT, RSET, NOOP, ...
                reply("250 OK\r\n")
    if got_message:
        with open(capture_path, "wb") as f:
            f.write(b"\r\n".join(captured))
    return got_message


def main():
    port = int(sys.argv[1])
    capture_path = sys.argv[2]
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(5)
    try:
        while True:
            conn, _ = srv.accept()
            done = handle(conn, capture_path)
            conn.close()
            if done:
                break
    finally:
        srv.close()


if __name__ == "__main__":
    main()
