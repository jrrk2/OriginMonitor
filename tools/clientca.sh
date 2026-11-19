#!/bin/bash
# Private CA for OriginWatch phone-view client certificates (nginx mutual TLS).
#   tools/clientca.sh init            create the CA and an empty revocation list (once)
#   tools/clientca.sh issue NAME      issue a certificate for one device -> NAME.p12 (+ NAME.p12.password, mode 600)
#   tools/clientca.sh revoke NAME     revoke it and regenerate the CRL (then: sudo systemctl reload nginx)
#   tools/clientca.sh list
# Lives in ~/.config/origin/web/clientca (mode 700). nginx reads ca.crt and crl.pem.
set -euo pipefail
D="$HOME/.config/origin/web/clientca"
CONF="$D/ca.cnf"
mkconf() {
  cat > "$CONF" <<EOF
[ ca ]
default_ca = oca
[ oca ]
dir = $D
database = \$dir/index.txt
new_certs_dir = \$dir/issued
serial = \$dir/serial
crlnumber = \$dir/crlnumber
certificate = \$dir/ca.crt
private_key = \$dir/ca.key
default_md = sha256
default_days = 825
default_crl_days = 3650
policy = anything
unique_subject = no
copy_extensions = none
[ anything ]
commonName = supplied
[ client ]
basicConstraints = critical,CA:FALSE
keyUsage = critical,digitalSignature
extendedKeyUsage = clientAuth
subjectKeyIdentifier = hash
authorityKeyIdentifier = keyid
EOF
}
case "${1:-}" in
  init)
    [ -e "$D/ca.key" ] && { echo "CA already exists in $D"; exit 1; }
    mkdir -p "$D/issued" "$D/devices"; chmod 700 "$D"
    mkconf; : > "$D/index.txt"; echo 1000 > "$D/serial"; echo 1000 > "$D/crlnumber"
    openssl ecparam -name prime256v1 -genkey -noout -out "$D/ca.key"; chmod 600 "$D/ca.key"
    openssl req -new -x509 -key "$D/ca.key" -sha256 -days 3650 -subj "/O=Origin observatory/CN=Origin observatory client CA" \
      -addext "basicConstraints=critical,CA:TRUE,pathlen:0" -addext "keyUsage=critical,keyCertSign,cRLSign" -out "$D/ca.crt"
    openssl ca -config "$CONF" -gencrl -out "$D/crl.pem" 2>/dev/null
    chmod 644 "$D/ca.crt" "$D/crl.pem"
    echo "CA created: $D/ca.crt" ;;
  issue)
    n="${2:?name}"; o="$D/devices/$n"; [ -e "$o.crt" ] && { echo "$n already issued"; exit 1; }
    openssl ecparam -name prime256v1 -genkey -noout -out "$o.key"; chmod 600 "$o.key"
    openssl req -new -key "$o.key" -subj "/O=Origin observatory/CN=$n" -out "$o.csr"
    openssl ca -batch -config "$CONF" -extensions client -in "$o.csr" -out "$o.crt" 2>/dev/null
    rm -f "$o.csr"
    # 3DES/SHA1 PKCS#12: iOS rejects OpenSSL 3's default AES-encrypted .p12 with "wrong password"
    pw=$(openssl rand -base64 12 | tr -d '/+=' | cut -c1-12); umask 077; printf '%s\n' "$pw" > "$o.p12.password"
    openssl pkcs12 -export -name "Origin observatory - $n" -inkey "$o.key" -in "$o.crt" -certfile "$D/ca.crt" \
      -certpbe PBE-SHA1-3DES -keypbe PBE-SHA1-3DES -macalg sha1 -passout "pass:$pw" -out "$o.p12"
    echo "issued $o.p12 (password in $o.p12.password), valid until $(openssl x509 -noout -enddate -in "$o.crt" | cut -d= -f2)" ;;
  revoke)
    n="${2:?name}"; openssl ca -config "$CONF" -revoke "$D/devices/$n.crt" 2>/dev/null
    openssl ca -config "$CONF" -gencrl -out "$D/crl.pem" 2>/dev/null; chmod 644 "$D/crl.pem"
    echo "revoked $n; now: sudo systemctl reload nginx" ;;
  list) cat "$D/index.txt" ;;
  *) sed -n '2,7p' "$0"; exit 2 ;;
esac
