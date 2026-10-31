/* crypto.h - SHA-256, HMAC-SHA256 and a CSPRNG over `bytes`.
 *
 * These are the only natives that use the OpenSSL crypto library beyond TLS
 * (decision Q11): the algorithms are not reimplemented here, because a
 * hand-rolled hash or random generator would be a security liability. The
 * module is registered with the VM through crypto_init() from vm_init, the way
 * codec_init() registers the codecs. */
#ifndef SPARK_CRYPTO_H
#define SPARK_CRYPTO_H

/* Registers the sha256, sha256_hex, hmac_sha256 and rand_bytes natives with the
 * VM. Called once from vm_init. */
void crypto_init(void);

#endif