# Self-hosting a send-only MTA for cellar (runbook)

A reproducible guide to running your **own** outbound mail server for cellar's
transactional email (verify-email, password-reset) and getting it to actually land in
Gmail/Outlook/Yahoo. Send-only — no mailboxes, no IMAP, no receiving.

> **Why not the home box?** cellar's data stays at home, but mail must NOT leave from a
> residential IP behind the tunnel (port 25 blocked by the ISP, IP blocklisted by
> default). So the MTA runs on a **small VPS with a clean IP you control**, and cellar
> relays into it. You still self-host and own the reputation — you just don't send from
> the worst-possible IP. (For the rent-the-reputation alternative, see
> [`dist/cellar.env.example`](../dist/cellar.env.example) — point `CEL_SMTP_URL` at a
> provider. This runbook is the sovereign path.)

Throughout, **replace `example.com`** with your domain and `203.0.113.10` with your VPS IP.

---

## 0. Decisions & prerequisites

- A **domain you control** (DNS access).
- A **small VPS** that (a) allows **outbound port 25** and (b) lets you set **reverse DNS
  (PTR)**. Good: Hetzner, OVH, Vultr, Linode, DigitalOcean (25 unblocked on request after
  account age). **Bad: AWS/GCP** (block 25 by default). 1 vCPU / 1 GB is plenty.
- **Check the IP before you commit.** It's pre-tainted on a shared range more often than
  you'd think:
  ```sh
  # from anywhere
  dig +short txt 10.113.0.203.zen.spamhaus.org    # (reversed octets) — a TXT answer = listed
  ```
  Or paste the IP into mxtoolbox.com/blacklists or multirbl.valli.org. **Listed → ask the
  provider for a different IP, or walk away.**

This guide uses **Postfix** (the standard MTA) + **OpenDKIM** (signing) on Debian/Ubuntu.

---

## 1. Hostname + reverse DNS (the make-or-break step)

On the VPS:
```sh
sudo hostnamectl set-hostname mail.example.com
```
Forward DNS — `mail.example.com` → your IP (add the A record in step 3).

**Reverse DNS (PTR)** is set at your **VPS provider's** control panel (not your DNS host):
point `203.0.113.10` → `mail.example.com`. It must *forward-confirm* (the A record points
back to the same IP). Verify once both exist:
```sh
dig +short mail.example.com           # → 203.0.113.10
dig +short -x 203.0.113.10            # → mail.example.com.
```
Receivers reject/junk mail whose IP has no matching PTR. Do not skip this.

---

## 2. TLS certificate

Get a real cert for the mail host (so outbound STARTTLS presents a valid cert, and for
the submission port if you expose it):
```sh
sudo apt update && sudo apt install -y certbot
sudo certbot certonly --standalone -d mail.example.com
# → /etc/letsencrypt/live/mail.example.com/{fullchain,privkey}.pem
```
Add a renewal hook later to reload Postfix (`certbot renew --deploy-hook "systemctl reload postfix"`).

---

## 3. DNS records (one-time, at your DNS host)

| Type | Name | Value |
|---|---|---|
| A | `mail.example.com` | `203.0.113.10` |
| TXT | `example.com` (SPF) | `v=spf1 ip4:203.0.113.10 -all` |
| TXT | `default._domainkey.example.com` (DKIM) | *filled in step 5* |
| TXT | `_dmarc.example.com` (DMARC) | `v=DMARC1; p=none; rua=mailto:dmarc@example.com; fo=1` |
| MX | `example.com` | `10 mail.example.com` *(optional but recommended — see note)* |

Notes:
- **SPF**: `-all` (hard fail) is correct once only this IP sends for the domain.
- **DMARC**: start at `p=none` and *monitor* the `rua` reports for a couple of weeks, then
  tighten to `p=quarantine` and eventually `p=reject`.
- **MX**: not required to *send*, but having one (and a way to read bounces) makes you look
  like a real domain and lets you catch delivery failures. Pure send-only without an MX
  works; you just won't see bounces. (Receiving bounces = running an inbound mailbox, which
  is out of scope here.)

---

## 4. Postfix — send-only, deliver direct, no open relay

```sh
sudo apt install -y postfix      # choose "Internet Site"; system mail name: example.com
```
Set the core config (`/etc/postfix/main.cf` — adjust, don't blindly append duplicates):
```ini
myhostname = mail.example.com
mydomain   = example.com
myorigin   = $mydomain
mydestination = localhost                 # we accept local delivery for nothing else
relayhost  =                              # EMPTY: deliver to the internet directly
inet_protocols = ipv4

# Who may relay THROUGH us to external domains. This prevents an open relay —
# only loopback and your private relay link (step 6) may send outbound.
mynetworks = 127.0.0.0/8 [::1]/128
smtpd_relay_restrictions = permit_mynetworks reject_unauth_destination

# Outbound TLS to receivers (opportunistic — encrypt when they support it).
smtp_tls_security_level = may
smtp_tls_loglevel = 1

# Present a valid cert on inbound connections (used by the submission port, step 6b).
smtpd_tls_cert_file = /etc/letsencrypt/live/mail.example.com/fullchain.pem
smtpd_tls_key_file  = /etc/letsencrypt/live/mail.example.com/privkey.pem
smtpd_tls_security_level = may
```
> ⚠️ **Open-relay check.** `smtpd_relay_restrictions` with `reject_unauth_destination` is
> mandatory — without it, spammers relay through you and your IP is dead within hours.
> Verify after setup (step 7).

---

## 5. OpenDKIM — sign every outbound message

```sh
sudo apt install -y opendkim opendkim-tools
sudo mkdir -p /etc/opendkim/keys/example.com
sudo opendkim-genkey -b 2048 -s default -d example.com \
     -D /etc/opendkim/keys/example.com
sudo chown -R opendkim:opendkim /etc/opendkim
```
`/etc/opendkim.conf`:
```ini
Domain                  example.com
Selector                default
KeyFile                 /etc/opendkim/keys/example.com/default.private
Socket                  inet:8891@localhost
Canonicalization        relaxed/simple
Mode                    s
SubDomains              no
```
Publish the **public key** — print it and copy the quoted blob into the
`default._domainkey.example.com` TXT record from step 3:
```sh
sudo cat /etc/opendkim/keys/example.com/default.txt
# → default._domainkey  IN TXT ( "v=DKIM1; k=rsa; p=MIIBIjANBg...=" )
```
Wire the milter into Postfix (`/etc/postfix/main.cf`):
```ini
milter_default_action = accept
milter_protocol       = 6
smtpd_milters         = inet:localhost:8891
non_smtpd_milters     = inet:localhost:8891     # signs mail injected locally (cellar's path)
```
Start everything:
```sh
sudo systemctl enable --now opendkim
sudo systemctl restart postfix
```

---

## 6. Let cellar relay into the MTA

cellar runs at home; the MTA is on the VPS. Two safe ways to connect them — **don't expose
an unauthenticated SMTP port to the internet.**

### Option A — private link (recommended: simplest + nothing public)
Put home and VPS on a **WireGuard** network (e.g. VPS = `10.10.0.1`, home = `10.10.0.2`).
Then let Postfix relay from the WG subnet, and cellar sends over the encrypted tunnel:
```ini
# /etc/postfix/main.cf  — add the WG subnet to mynetworks
mynetworks = 127.0.0.0/8 [::1]/128 10.10.0.0/24
inet_interfaces = 127.0.0.1, 10.10.0.1        # listen on loopback + WG only, NOT public
```
cellar env (the mail crosses the encrypted WG link, so plaintext SMTP is fine):
```sh
CEL_SMTP_URL=smtp://10.10.0.1:25
CEL_SMTP_TLS=none
CEL_MAIL_FROM=noreply@example.com
CEL_MAIL_FROM_NAME=Acme
CEL_APP_URL=https://app.example.com           # builds the reset/verify links
```
Nothing SMTP is reachable from the public internet — least attack surface.

### Option B — authenticated submission (587) over TLS
If you can't run WireGuard, expose the **submission** port with SASL auth + STARTTLS and
have cellar authenticate. Enable `submission` in `/etc/postfix/master.cf`:
```
submission inet n - y - - smtpd
  -o syslog_name=postfix/submission
  -o smtpd_tls_security_level=encrypt
  -o smtpd_sasl_auth_enable=yes
  -o smtpd_relay_restrictions=permit_sasl_authenticated,reject
  -o smtpd_client_restrictions=permit_sasl_authenticated,reject
```
Set up a SASL backend (Dovecot SASL is easiest) and a single mail user; then:
```sh
CEL_SMTP_URL=smtp://mail.example.com:587
CEL_SMTP_USER=cellar@example.com
CEL_SMTP_PASS=<strong-secret>
CEL_SMTP_TLS=require
CEL_MAIL_FROM=noreply@example.com
```
Add **fail2ban** for the submission port; expect brute-force probes.

---

## 7. Test before you trust it

```sh
# from the cellar host, with the env above loaded
cellar send-test-mail you@gmail.com
```
Then:
1. **It arrives + headers pass.** Open the message in Gmail → "Show original": you want
   **SPF=pass, DKIM=pass, DMARC=pass**.
2. **Score it.** Send a test to the address shown on **mail-tester.com** → aim for **10/10**
   (it flags exactly what's missing: rDNS, SPF, DKIM, blocklist hits).
3. **Confirm you're not an open relay** (critical):
   ```sh
   # from an OUTSIDE host, try to relay to a foreign domain — must be REJECTED
   swaks --server mail.example.com --to test@gmail.com --from x@evil.com
   # expect: 554 ... Relay access denied
   ```

---

## 8. Reputation: enroll, warm up, monitor

Deliverability is earned over weeks, not configured once.

- **Google Postmaster Tools** (postmaster.google.com) — add + verify `example.com` to see
  your Gmail domain reputation and spam rate.
- **Microsoft SNDS + JMRP** (sendersupport.olc.protection.outlook.com) — register your IP.
  Outlook is the hardest receiver; even a perfect setup may junk new small senders — submit
  a sender-mitigation request and be patient.
- **Warm up.** Send a few messages/day for the first week, ramp gradually. Sudden volume
  from a cold IP looks like a compromise.
- **Stay clean.** Keep complaint rate ~0, don't keep mailing addresses that bounce, keep
  content plain and transactional with a real From. Re-check blocklists periodically.

---

## 9. Maintenance & troubleshooting

- **Cert renewal** reloads Postfix (the certbot deploy-hook from step 2).
- **DKIM key rotation** every 6–12 months: genkey a new selector, publish, switch
  `Selector`, keep the old DNS record live until no mail uses it.
- **Watch the queue/logs**: `mailq`, `journalctl -u postfix`, `journalctl -u opendkim`.
- **Got listed?** Find which list (mxtoolbox), fix the cause (often a config slip or a
  forwarded bounce loop), then use the list's delisting form.
- **Outlook still junking?** Common and frustrating; SNDS data + a mitigation request is the
  only lever. Many run direct-to-Gmail and **smarthost Outlook-bound mail through a
  provider** as a hybrid.

### Stepping stone / fallback
Want mail landing **today** while your IP warms? Keep this exact Postfix setup but add a
`relayhost` pointing at a transactional provider (smarthost) — your infra, queue, and
config, their IP reputation:
```ini
# /etc/postfix/main.cf
relayhost = [smtp.your-provider.com]:587
smtp_sasl_auth_enable = yes
smtp_sasl_password_maps = static:provider-user:provider-key
smtp_sasl_security_options = noanonymous
smtp_tls_security_level = encrypt
```
Cut over to direct send (empty `relayhost`) once your own IP has reputation. Fully reversible.

---

## Quick checklist

- [ ] VPS with port 25 + rDNS control; IP not blocklisted
- [ ] `mail.example.com` A record + **PTR** (forward-confirmed)
- [ ] Let's Encrypt cert for `mail.example.com`
- [ ] DNS: **SPF**, **DKIM** (key published), **DMARC** (`p=none`)
- [ ] Postfix send-only, `relayhost` empty, **relay restricted** (no open relay)
- [ ] OpenDKIM signing (milter wired into Postfix)
- [ ] cellar → MTA over WireGuard (Option A) or authenticated 587 (Option B)
- [ ] `send-test-mail` → SPF/DKIM/DMARC **pass**, mail-tester **10/10**, open-relay test **rejected**
- [ ] Google Postmaster + Microsoft SNDS enrolled; warm up slowly
