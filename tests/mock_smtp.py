#!/usr/bin/env python3
"""Minimal SMTP sink for testing cellar's mailer (no TLS, no auth).

Speaks just enough SMTP for libcurl: 220 greeting, EHLO/MAIL/RCPT -> 250, DATA ->
354 then collect until the lone "." -> 250. Writes the captured DATA payload to
the given file. Accepts in a loop so a liveness probe that connects-and-closes
doesn't consume the real delivery.

    mock_smtp.py [--forever] <port> <capture-file>

Default: one-shot — overwrites the file and exits after the first message (test
harnesses). With --forever: stays up and APPENDS every message (dev use, e.g.
run.sh, so each signup's code is captured).
"""
import socket, sys


def handle(conn, capture_path, append=False):
    def reply(s):
        try: conn.sendall(s.encode())
        except OSError: pass

    reply("220 mock ESMTP cellar-test\r\n")
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
        with open(capture_path, "ab" if append else "wb") as f:
            if append: f.write(b"\r\n----\r\n")
            f.write(b"\r\n".join(captured))
    return got_message


def main():
    args = sys.argv[1:]
    forever = False
    if args and args[0] == "--forever":
        forever = True; args = args[1:]
    port = int(args[0])
    capture_path = args[1]
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(5)
    try:
        while True:
            conn, _ = srv.accept()
            done = handle(conn, capture_path, append=forever)
            conn.close()
            if done and not forever:
                break
    finally:
        srv.close()


if __name__ == "__main__":
    main()
