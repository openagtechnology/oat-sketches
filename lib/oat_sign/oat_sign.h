/* =============================================================================
   oat_sign.h  —  OAT push signing, the shared HMAC module
   -----------------------------------------------------------------------------
   Build once, deploy many (Doctrine #7). EVERY OAT device sketch that pushes to
   a webhook links this and signs identically, so any endpoint verifies the same
   way regardless of which sketch sent the data.

   The security model (see the developer reference, https://openagriculturetechnology.com/standard/reference/):
     - The device holds a shared secret KEY (entered once at setup). It NEVER
       sends the key. It sends a SIGNATURE computed from the key, so the proof
       is safe even over plain HTTP (the older ESP32s have no TLS) — sniffing the
       wire reveals the body and the signature but not the key, so an attacker
       cannot forge a new message.
     - Signed string = "<unix-timestamp>.<raw-body-bytes>".
     - Algorithm = HMAC-SHA256 (mbedTLS, bundled in the ESP32 core; no TLS
       handshake needed — just the hash).
     - Scheme tag "oat1=" prefixes the hex so the scheme can evolve later.
     - Replay is handled by the monotonic "seq" inside the (signed) body, not by
       this module — a device with no reliable clock still authenticates.

   Header-only; one inline function. The sketch owns the HTTP send and adds the
   three headers this enables: X-OAT-Key-Id, X-OAT-Timestamp, X-OAT-Signature.
   ============================================================================= */
#pragma once
#include <Arduino.h>
#include <mbedtls/md.h>

namespace oat {

// HMAC-SHA256(key, msg) -> lowercase hex (64 chars). Empty key -> empty string
// (caller treats "no key" as unsigned / sandbox mode).
inline String hmacSha256Hex(const String& key, const String& msg) {
  if (key.length() == 0) return String("");
  uint8_t mac[32];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!info) return String("");
  if (mbedtls_md_hmac(info,
                      (const unsigned char*)key.c_str(), key.length(),
                      (const unsigned char*)msg.c_str(), msg.length(),
                      mac) != 0) return String("");
  static const char* H = "0123456789abcdef";
  String out; out.reserve(64);
  for (int i = 0; i < 32; i++) { out += H[mac[i] >> 4]; out += H[mac[i] & 0x0F]; }
  return out;
}

// The canonical signed string for a push: timestamp, a literal '.', then the raw
// body bytes exactly as sent. The endpoint MUST verify over the raw received
// bytes (never a re-serialized copy).
inline String signString(const String& unixTs, const String& rawBody) {
  return unixTs + "." + rawBody;
}

} // namespace oat

/* library.json sibling lets PlatformIO resolve this from lib_extra_dirs=../lib */
