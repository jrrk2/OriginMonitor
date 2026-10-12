#!/bin/bash
# Get (or renew) a certificate for the OriginWatch phone view from Let's Encrypt with the DNS-01 challenge on Cloudflare, using acme.sh, and install it where watch_web.sh looks.
#   tools/web_cert.sh [NAME]            default NAME: origin.kimmitt.uk
# Needs: acme.sh installed for this user (https://github.com/acmesh-official/acme.sh), and a Cloudflare API token with "Zone - DNS - Edit" on the zone only, in
# ~/.config/origin/cloudflare.token (readable by you only: umask 077). The token is read from that file and never appears on a command line.
set -e
NAME="${1:-origin.kimmitt.uk}"; CFG="$HOME/.config/origin"; ACME="$HOME/.acme.sh/acme.sh"
[ -x "$ACME" ] || { echo "acme.sh is not installed (see the header of this file)"; exit 1; }
[ -r "$CFG/cloudflare.token" ] || { echo "put the Cloudflare API token in $CFG/cloudflare.token (chmod 600)"; exit 1; }
[ "$(stat -c %a "$CFG/cloudflare.token")" = "600" ] || { echo "chmod 600 $CFG/cloudflare.token first"; exit 1; }
export CF_Token="$(tr -d '[:space:]' < "$CFG/cloudflare.token")"
mkdir -p "$CFG/web"; chmod 700 "$CFG/web"
"$ACME" --issue --dns dns_cf -d "$NAME" --server letsencrypt || [ $? -eq 2 ]          # exit 2 = not due for renewal yet
"$ACME" --install-cert -d "$NAME" --key-file "$CFG/web/key.pem" --fullchain-file "$CFG/web/fullchain.pem" \
        --reloadcmd "$(cd "$(dirname "$0")" && pwd)/watch_web.sh restart"
chmod 600 "$CFG/web/key.pem"; echo "installed in $CFG/web"
