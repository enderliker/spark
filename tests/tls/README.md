# TLS test fixtures

Throwaway certificates and keys for the local TLS tests. The private keys are
public fixtures: they are not secret, they are only valid for localhost and they
must never be used outside this test suite.

| File | Purpose |
| --- | --- |
| `ca.pem` / `ca.key` | Trusted test CA. |
| `other-ca.pem` / `other-ca.key` | A second CA the client does not trust. |
| `server.pem` / `server.key` | Leaf signed by `ca.pem`, valid for `localhost`, `127.0.0.1` and `::1`. |
| `wrong-host.pem` / `wrong-host.key` | Leaf signed by `ca.pem` but only valid for `wrong.example`. |
| `untrusted.pem` / `untrusted.key` | Leaf signed by `other-ca.pem`. |
| `self-signed.pem` / `self-signed.key` | Self-signed leaf for `localhost`. |

The certificates expire in 100 years so the suite stays deterministic. Regenerate
them with `./generate.sh` (requires the `openssl` command-line tool); the suite
itself only needs the committed files.
