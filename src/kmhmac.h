#ifndef KMHMAC_H
#define KMHMAC_H

/*
 * Keymaster 4 HMAC key agreement, normally done by Android's keystore at
 * boot.
 *
 * keystore is disabled on Sailfish OS (and aborts without SELinux), so the
 * TEE Keymaster never gets its shared HMAC key. Gatekeeper then cannot sign
 * auth tokens (verify fails, QSEE status -24) and the fingerprint TA rejects
 * enrollment (GF_ERROR_INVALID_HAT_VERSION / GF_ERROR_UNTRUSTED_ENROLL on
 * the OnePlus 6T).
 *
 * Must run once per boot, before the fingerprint HAL loads its TA (the first
 * setNotify). Running it again changes the key under an already loaded TA,
 * so a flag in /run makes later calls (fpd restarts) a no-op.
 */
void agreeKeymasterHmac();

#endif // KMHMAC_H
