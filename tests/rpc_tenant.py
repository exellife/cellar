#!/usr/bin/env python3
"""cellar M-3 regression: pooled-mode RPC fails closed on an empty tenant.

Booted with CEL_TENANT_COLUMN set (pooled mode) and a policy that whitelists
rpc_add for 'anon'. The unauthenticated caller is permitted by the whitelist but
has no tenant, so the RPC must be DENIED (403) rather than run with
app.tenant_id='' in an undefined RLS context. Pre-fix the call returned 200 (the
function ran in the empty-tenant context). In single-tenant mode the same call
would succeed — the seatbelt is pooled-mode only.
Run as: rpc_tenant.py ws://127.0.0.1:<port>/
"""
import http.client, json, sys
from urllib.parse import urlparse


def main(host, port):
    c = http.client.HTTPConnection(host, port, timeout=10)
    c.request("POST", "/rpc/rpc_add", body=json.dumps({"a": 2, "b": 40}),
              headers={"Content-Type": "application/json"})
    r = c.getresponse(); _ = r.read(); c.close()
    print(f"== M-3 pooled-mode anon RPC -> {host}:{port} ==")
    ok = (r.status == 403)
    print(f"  {'ok' if ok else 'FAIL'}  anon RPC denied in pooled mode "
          f"(empty tenant, got {r.status} want 403)")
    print("\nPASS" if ok else "\nFAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    u = urlparse(sys.argv[1] if len(sys.argv) > 1 else "ws://127.0.0.1:8080/")
    sys.exit(main(u.hostname or "127.0.0.1", u.port or 8080))
