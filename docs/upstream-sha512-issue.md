# Upstream issue draft — sdk-sidewalk SHA-512 dependency gap

Draft for filing against **nrfconnect/sdk-sidewalk**. Copy the body below into a
new GitHub issue. Verified against sdk-sidewalk `v1.1.0-add-on` (`557c920`) on
NCS v3.2.4.

---

**Title:** Ed25519 mfg identity-key import fails with PSA -134 on nRF52 — `PSA_WANT_ALG_SHA_512` only implied for nRF54L

**Summary**

On a BLE-only Sidewalk end device targeting **nRF52840**, the manufacturing
**Ed25519** device identity key fails to import into PSA at boot with
`PSA_ERROR_NOT_SUPPORTED (-134)`, so FFN registration can never sign the
challenge and the device loops forever without registering. Root cause is a
missing SHA-512 dependency for non-nRF54L targets.

**Environment**

- NCS v3.2.4, sdk-sidewalk `v1.1.0-add-on` (`557c920`)
- Board: nRF52840 (Seeed XIAO nRF52840), BLE only (`CONFIG_SIDEWALK_SUBGHZ_SUPPORT=n`)
- Pre-provisioned mfg data containing an Ed25519 device identity key (MFG v8)

**The problem**

`Kconfig.dependencies`, `config SIDEWALK_CRYPTO` enables Ed25519 for **all**
SoCs but gates SHA-512 on nRF54L only:

```
imply PSA_WANT_ALG_SHA_512 if SOC_SERIES_NRF54LX   # line 121
...
imply PSA_WANT_ALG_PURE_EDDSA                       # line 128  (all SoCs)
imply PSA_WANT_ECC_TWISTED_EDWARDS_255              # line 131  (all SoCs)
imply PSA_WANT_KEY_TYPE_ECC_KEY_PAIR_IMPORT         # line 133  (all SoCs)
```

Ed25519 (pure EdDSA over Curve25519) requires **SHA-512** — both to derive the
public key from the 32-byte private seed when the key pair is *imported*, and to
sign. On nRF52 the CC310 CryptoCell has no SHA-512, and because SHA-512 isn't
implied, the Oberon software SHA-512 isn't pulled in either. So `psa_import_key`
for the Ed25519 identity key returns `-134 NOT_SUPPORTED`.

**Observed symptom chain (device log)**

```
<inf> sid_mfg: Need to parse mfg data
<err> sid_crypto_key: psa_import_key failed! (err -134 id 1)
<inf> sid_mfg_parser_v8: MFG_ED25519 import failure
<err> sid_mfg: Failed parsing mfg data errno -13
... FFN connects, then:
<err> sid_crypto: PSA Error code: -135 in sid_pal_crypto_ecc_dsa
<inf> sid_ble_conn: BT Disconnected Reason: 0x16 = LOCALHOST_TERM_CONN
```

(The P256/AES mfg keys import fine — only Ed25519 fails, because only it needs
SHA-512.) The device re-advertises with a fresh RPA and repeats indefinitely; it
never registers.

**Fix**

Drop the SoC guard so SHA-512 is implied wherever Ed25519 is — e.g.

```
imply PSA_WANT_ALG_SHA_512
```

or tie it to the algorithm rather than the SoC:

```
imply PSA_WANT_ALG_SHA_512 if PSA_WANT_ALG_PURE_EDDSA
```

**Workaround** (application side)

```
CONFIG_PSA_WANT_ALG_SHA_512=y
```
in the app's `prj.conf` restores the Ed25519 import and lets the device register.
