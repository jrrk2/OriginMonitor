#!/usr/bin/env python3
"""Dynamic DNS for the OriginWatch phone view: keep origin.kimmitt.uk's A and AAAA records on Cloudflare pointing at this host.

  A    = the home public IPv4 address (as Cloudflare's 1.1.1.1/cdn-cgi/trace sees us; the hub forwards 443 to this host)
  AAAA = this host's own stable global IPv6 address (not a temporary/privacy one, not ULA); the hub has a pinhole for it

Only existing records are updated (never created or deleted), and only when the address differs. Each change, and each new kind of failure,
is e-mailed once through the "smtp" settings in ~/.config/origin/mail.conf (the same route as the briefings). Quiet when nothing changes.

  origin_ddns.py              check and update (cron:  */5 * * * * python3 ~/OriginMonitor/tools/origin_ddns.py >> ~/.cache/origin-ddns.log 2>&1)
  origin_ddns.py --dry-run    show the addresses and what would change; writes nothing, sends nothing
  origin_ddns.py --mail-test  send a test of the change e-mail

Token: ~/.config/origin/cloudflare.token (Zone.DNS edit on kimmitt.uk, mode 600); never printed.
"""
import argparse, ipaddress, json, os, smtplib, ssl, subprocess, sys, time, urllib.parse, urllib.request
from email.message import EmailMessage
from email.utils import formatdate, make_msgid

CFG = os.path.expanduser("~/.config/origin")
STATE = os.path.expanduser("~/.cache/origin-ddns.state")
API = "https://api.cloudflare.com/client/v4"


def log(msg):
    print(time.strftime("%Y-%m-%dT%H:%M:%S"), msg, flush=True)


def public_ipv4():
    for host in ("1.1.1.1", "1.0.0.1"):   # literal IPv4 addresses, so the answer is our IPv4 address even on a dual-stack host
        try:
            text = urllib.request.urlopen(f"https://{host}/cdn-cgi/trace", timeout=15).read().decode()
            ip = dict(l.split("=", 1) for l in text.splitlines() if "=" in l).get("ip", "")
            if ipaddress.ip_address(ip).version == 4:
                return ip
        except Exception as e:
            err = e
    raise RuntimeError(f"cannot learn the public IPv4 address: {err}")


def local_ipv6():
    out = json.loads(subprocess.run(["ip", "-j", "-6", "addr", "show", "scope", "global"], capture_output=True, text=True, check=True).stdout or "[]")
    cands = []
    for ifc in out:
        for a in ifc.get("addr_info", []):
            if "local" not in a:   # ip -j ends the list with an empty {}
                continue
            ip = ipaddress.ip_address(a["local"])
            if a.get("temporary") or a.get("deprecated") or a.get("tentative") or not ip.is_global:
                continue
            # prefer the stable SLAAC address (mngtmpaddr = the one privacy addresses are made from), then any other
            cands.append((0 if a.get("mngtmpaddr") else 1, a.get("preferred_life_time", 0) == 0, str(ip)))
    if not cands:
        raise RuntimeError("no stable global IPv6 address on this host")
    return sorted(cands)[0][2]


class CF:
    def __init__(self):
        path = os.path.join(CFG, "cloudflare.token")
        self.token = open(path).read().strip()

    def call(self, method, path, body=None):
        rq = urllib.request.Request(API + path, method=method, data=None if body is None else json.dumps(body).encode(),
                                    headers={"Authorization": "Bearer " + self.token, "Content-Type": "application/json"})
        try:
            r = json.load(urllib.request.urlopen(rq, timeout=20))
        except urllib.error.HTTPError as e:
            r = json.loads(e.read() or b"{}") or {"errors": [{"message": f"HTTP {e.code}"}]}
        if not r.get("success"):
            raise RuntimeError(f"Cloudflare {method} {path.split('?')[0]}: " + "; ".join(x.get("message", "?") for x in r.get("errors", [])))
        return r["result"]


def send_mail(subject, body):
    conf = json.load(open(os.path.join(CFG, "mail.conf")))
    smtp, to = conf["smtp"], conf["to"]
    u = urllib.parse.urlparse(smtp["url"])
    m = EmailMessage()
    m["From"] = f'{smtp.get("from_name", "Origin observatory")} <{smtp["from"]}>'
    m["To"] = to
    m["Subject"] = subject
    m["Date"] = formatdate(localtime=True)
    m["Message-ID"] = make_msgid(domain=smtp["from"].split("@")[1])
    m.set_content(body)
    with smtplib.SMTP(u.hostname, u.port or 25, local_hostname=u.path.strip("/") or None, timeout=60) as s:
        s.starttls(context=ssl.create_default_context())
        s.ehlo(u.path.strip("/") or None)
        if smtp.get("user"):
            s.login(smtp["user"], open(os.path.join(CFG, smtp.get("password_file", "smtp.password"))).read().strip())
        s.send_message(m)


def load_state():
    try:
        return json.load(open(STATE))
    except Exception:
        return {}


def save_state(st):
    os.makedirs(os.path.dirname(STATE), exist_ok=True)
    with open(STATE, "w") as f:
        json.dump(st, f)


AFTER = """What may need a look after a change:
  - IPv4: the hub's port forward (TCP 443 -> 192.168.1.176) normally survives a new public address; nothing to do.
  - IPv6: a new prefix means the hub's pinhole may still name the old address (edit it on the hub to the new one), and the nginx
    home range (geo $origin_lan in /etc/nginx/sites-available/origin-watch) still names the old prefix: until it is edited and
    nginx reloaded, phones at home on IPv6 need their client certificate like anywhere else.
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", default="origin.kimmitt.uk")
    ap.add_argument("--zone", default="kimmitt.uk")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--mail-test", action="store_true")
    a = ap.parse_args()

    if a.mail_test:
        send_mail(f"[Origin] {a.name} address changed (test)",
                  f"This is a test of the dynamic DNS e-mail from {os.uname().nodename}.\n\nA change would read:\n  AAAA {a.name}: 2001:db8::1 -> 2001:db8::2\n\n" + AFTER)
        log("test e-mail sent")
        return 0

    st = load_state()
    changes, errors = [], []
    try:
        cf = CF()
        zid = cf.call("GET", f"/zones?name={a.zone}")[0]["id"]
    except Exception as e:
        errors.append(str(e)); zid = None

    for typ, finder in (("A", public_ipv4), ("AAAA", local_ipv6)):
        if zid is None:
            break
        try:
            want = finder()
        except Exception as e:
            # no address (network down, interface not up yet): leave the record alone; not worth an e-mail unless it persists
            st.setdefault("noaddr", {}).setdefault(typ, time.time())
            if time.time() - st["noaddr"][typ] > 3600:
                errors.append(f"{typ}: {e} (for over an hour)")
            elif a.dry_run:
                log(f"{typ}: {e}")
            continue
        st.get("noaddr", {}).pop(typ, None)
        try:
            recs = cf.call("GET", f"/zones/{zid}/dns_records?type={typ}&name={a.name}")
            if len(recs) != 1:
                errors.append(f"{typ} {a.name}: expected one record, found {len(recs)}; not touched")
                continue
            have = recs[0]["content"]
            if ipaddress.ip_address(have) == ipaddress.ip_address(want):
                if a.dry_run:
                    log(f"{typ} {a.name} = {have}, matches this host: no change")
                continue
            if a.dry_run:
                log(f"{typ} {a.name} = {have}, would become {want}")
                continue
            cf.call("PATCH", f"/zones/{zid}/dns_records/{recs[0]['id']}", {"content": want})
            log(f"{typ} {a.name}: {have} -> {want}")
            changes.append(f"{typ} {a.name}: {have} -> {want}")
        except Exception as e:
            errors.append(f"{typ}: {e}")

    if a.dry_run:
        for e in errors:
            log("error: " + e)
        return 1 if errors else 0

    mail_err = None
    if changes:
        try:
            send_mail(f"[Origin] {a.name} address changed",
                      "Dynamic DNS on " + os.uname().nodename + " updated Cloudflare:\n\n  " + "\n  ".join(changes) + "\n\n" + AFTER)
        except Exception as e:
            mail_err = f"change e-mail failed: {e}"; log(mail_err)

    # failures: log every time, e-mail only when the set of failures changes (not every 5 minutes)
    sig = "\n".join(sorted(errors))
    if errors:
        for e in errors:
            log("error: " + e)
        if sig != st.get("errsig"):
            try:
                send_mail(f"[Origin] dynamic DNS for {a.name} is failing",
                          "Dynamic DNS on " + os.uname().nodename + " could not keep the records up to date:\n\n  " + "\n  ".join(errors) +
                          "\n\nThe records were left as they were. Log: ~/.cache/origin-ddns.log. You will get one more e-mail if the failure changes.\n")
            except Exception as e:
                log(f"failure e-mail failed: {e}")
    elif st.get("errsig"):
        log("recovered")
    st["errsig"] = sig
    save_state(st)
    return 1 if errors or mail_err else 0


if __name__ == "__main__":
    sys.exit(main())
