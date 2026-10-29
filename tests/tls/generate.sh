#!/usr/bin/env bash
# Regenerates the throwaway certificates used by the local TLS tests.
#
# These keys are public test fixtures: they are not secret and must never be
# used outside the test suite. Run this script only when the fixtures need to
# change; the generated files are committed so the suite does not need openssl.
set -euo pipefail

cd "$(dirname "$0")"

days=36500

quiet() {
    "$@" >/dev/null 2>&1
}

# Trusted test CA.
quiet openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days "$days" \
    -keyout ca.key -out ca.pem -subj "/CN=Spark Test CA" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign"

# A second CA that the client does not trust.
quiet openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days "$days" \
    -keyout other-ca.key -out other-ca.pem -subj "/CN=Spark Other Test CA" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign"

leaf_extensions() {
    local san=$1
    printf '%s\n' \
        "basicConstraints=critical,CA:FALSE" \
        "keyUsage=critical,digitalSignature,keyEncipherment" \
        "extendedKeyUsage=serverAuth" \
        "subjectAltName=$san"
}

make_leaf() {
    local name=$1 common_name=$2 san=$3 ca=$4 ca_key=$5

    quiet openssl req -new -newkey rsa:2048 -nodes -sha256 \
        -keyout "$name.key" -out "$name.csr" -subj "/CN=$common_name"
    leaf_extensions "$san" > "$name.ext"
    quiet openssl x509 -req -sha256 -days "$days" -in "$name.csr" \
        -CA "$ca" -CAkey "$ca_key" -CAcreateserial -extfile "$name.ext" \
        -out "$name.pem"
    rm -f "$name.csr" "$name.ext"
}

# Trusted by ca.pem, valid for localhost and both loopback literals.
make_leaf server localhost "DNS:localhost,IP:127.0.0.1,IP:::1" ca.pem ca.key

# Trusted by ca.pem, but only for a different host name.
make_leaf wrong-host wrong.example "DNS:wrong.example" ca.pem ca.key

# Signed by other-ca.pem, which the client does not trust.
make_leaf untrusted localhost "DNS:localhost,IP:127.0.0.1,IP:::1" other-ca.pem other-ca.key

# Self-signed leaf: its own issuer, not a CA.
quiet openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days "$days" \
    -keyout self-signed.key -out self-signed.pem -subj "/CN=localhost" \
    -addext "basicConstraints=critical,CA:FALSE" \
    -addext "keyUsage=critical,digitalSignature,keyEncipherment" \
    -addext "extendedKeyUsage=serverAuth" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1"

rm -f ca.srl other-ca.srl

quiet openssl verify -CAfile ca.pem server.pem
quiet openssl verify -CAfile ca.pem wrong-host.pem
quiet openssl verify -CAfile other-ca.pem untrusted.pem

echo "regenerated the TLS test certificates"
