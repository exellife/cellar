#!/usr/bin/env python3
"""cellar media upload/serve e2e (E1.1/E1.2). Booted by run_with_server, which
passes ws://127.0.0.1:<port>/ and seeds admin@cellar.dev.

Generates a real PNG in-process (zlib — no PIL), uploads it to POST /media,
then exercises GET /media/<id>/<variant>: the server re-encodes to canonical
JPEG variants, so every served byte-stream must start with the JPEG SOI marker
and the thumbnail must be smaller than the full image. Also checks auth, the
size/format rejections, and the 404s.
"""
import sys, json, zlib, struct, http.client
from urllib.parse import urlparse

ADMIN = ("admin@cellar.dev", "s3cret-admin")
ok = 0; fail = 0
def chk(name, cond, detail=""):
    global ok, fail
    print(f"  {'ok' if cond else 'FAIL':<5} {name:<44} {detail}")
    ok += bool(cond); fail += (not cond)

def png(w, h):
    """A minimal valid RGB PNG (filter 0 per row), built with zlib."""
    raw = bytearray()
    for y in range(h):
        raw.append(0)                                  # filter type: none
        for x in range(w):
            raw += bytes([(x * 255) // max(w-1,1), (y * 255) // max(h-1,1), (x + y) & 0xFF])
    def chunk(tag, data):
        c = tag + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xffffffff)
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)  # 8-bit RGB
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))

def main():
    u = urlparse(sys.argv[1].replace("ws://", "http://"))
    HOST, PORT = u.hostname, u.port

    def req(method, path, body=None, token=None, ctype=None):
        c = http.client.HTTPConnection(HOST, PORT, timeout=10)
        h = {}
        if ctype: h["Content-Type"] = ctype
        if token: h["Authorization"] = "Bearer " + token
        c.request(method, path, body=body, headers=h)
        r = c.getresponse(); data = r.read()
        hdrs = {k.lower(): v for k, v in r.getheaders()}
        c.close()
        return r.status, data, hdrs

    def login(creds):
        s, b, _ = req("POST", "/auth/login",
                      json.dumps({"email": creds[0], "password": creds[1]}), ctype="application/json")
        try: return json.loads(b).get("token")
        except Exception: return None

    print(f"== cellar media e2e -> {HOST}:{PORT} ==")
    tok = login(ADMIN); chk("login admin", bool(tok))

    img = png(400, 300)

    # ---- unauthenticated upload -> 401 ----
    s, b, _ = req("POST", "/media", img, ctype="image/png")
    chk("upload without auth -> 401", s == 401, f"status={s}")

    # ---- valid upload -> 201 + id + variants ----
    s, b, _ = req("POST", "/media", img, token=tok, ctype="image/png")
    chk("upload -> 201", s == 201, f"status={s} {b[:120]}")
    meta = json.loads(b) if s == 201 else {}
    mid = meta.get("id", "")
    chk("id is 32 hex", len(mid) == 32 and all(c in "0123456789abcdef" for c in mid), mid)
    chk("reports source dims", meta.get("width") == 400 and meta.get("height") == 300, str(meta))
    chk("variants full+thumb", meta.get("variants") == ["full", "thumb"], str(meta.get("variants")))

    # ---- serve each variant: canonical JPEG, cached ----
    s, full, h = req("GET", f"/media/{mid}/full", token=tok)
    chk("serve full -> 200", s == 200, f"status={s}")
    chk("full is JPEG (SOI)", full[:2] == b"\xff\xd8", full[:4].hex())
    chk("full content-type image/jpeg", h.get("content-type") == "image/jpeg", h.get("content-type"))
    chk("full has immutable cache", "immutable" in h.get("cache-control", ""), h.get("cache-control"))

    s, thumb, _ = req("GET", f"/media/{mid}/thumb", token=tok)
    chk("serve thumb -> 200", s == 200, f"status={s}")
    chk("thumb is JPEG", thumb[:2] == b"\xff\xd8", thumb[:4].hex())
    chk("thumb smaller than full", 0 < len(thumb) < len(full), f"thumb={len(thumb)} full={len(full)}")

    # browsing media is public (anon) — no token needed to view
    s, _, _ = req("GET", f"/media/{mid}/full")
    chk("serve full anon -> 200", s == 200, f"status={s}")

    # ---- 404s ----
    s, _, _ = req("GET", f"/media/{mid}/bogus", token=tok)
    chk("unknown variant -> 404", s == 404, f"status={s}")
    s, _, _ = req("GET", "/media/deadbeef/full", token=tok)
    chk("malformed id -> 404", s == 404, f"status={s}")
    s, _, _ = req("GET", f"/media/{'0'*32}/full", token=tok)
    chk("missing object -> 404", s == 404, f"status={s}")

    # ---- non-image body -> 400 ----
    s, _, _ = req("POST", "/media", b"this is not an image", token=tok, ctype="application/octet-stream")
    chk("non-image -> 400", s == 400, f"status={s}")
    # empty body -> 400
    s, _, _ = req("POST", "/media", b"", token=tok, ctype="image/png")
    chk("empty body -> 400", s == 400, f"status={s}")

    print(f"\n{'ALL PASS' if fail == 0 else f'FAILED ({fail})'}  ({ok} ok)")
    return 1 if fail else 0

if __name__ == "__main__":
    sys.exit(main())
