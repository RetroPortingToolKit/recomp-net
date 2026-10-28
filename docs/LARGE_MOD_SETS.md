# Complete mod settings before frame zero

Super Metroid's 40-feature catalog produces 2,526 bytes of canonical settings.
The previous 512-byte lobby field and 1,024-byte session message could not
carry it. `RNET_MODSET_TEXT_MAX` now gives all consumers the same 4,096-byte
storage bound, including the terminating NUL. Oversized lists are refused.

Sets of at most 1,023 bytes retain the existing single-message encoding.
Larger sets use `MODSET_CHUNK` (type 28): source seat, total length, offset,
part length, aggregate checksum and up to 960 bytes of canonical text. Every
session packet stays within the existing 1,200-byte transport limit. Partial
data is never exposed to comparison or adoption. Reassembly handles duplicates
and reordering, rejects conflicting/invalid parts, checks the complete string,
and resets on a new transfer or connection. The existing pre-match resend/ACK
gate handles loss; it does not negotiate settings during gameplay.

Decoded packet storage stays small; the full list lives in the session's
bounded assembler. Lobby/LAN fields and driver buffers use the public bound.
LAN room-control messages reserve space for that field plus their metadata.
Online caps retain the server's existing 4,000-byte serialization guard; the
complete Super Metroid plan/settings object is 3,690 bytes and fits.

Wire version is **3**. Module ABI is **2**, since public lobby structures grew.
Old peers/modules are rejected by the existing version gates. These identities
must accompany the change; a library version string alone is insufficient.

Validation on 2026-10-28:

* All 34 native Linux CTests pass, plus the loadable-module ABI test.
* Windows GCC builds the library; real UDP transfer and LAN registry tests pass.
* `modset_transfer_test` covers the maximum list, reversed/duplicate chunks,
  incomplete/conflicting/checksum-invalid data, malformed geometry, exact
  legacy boundary and a short consumer buffer that must not consume the list.
* The actual SM catalog/settings traverse real UDP. Two- and three-seat driver
  cases pass with 30 ms per-peer latency, 8 ms jitter and 2% packet loss.
  A tail difference refuses both peers with simulation still at frame zero,
  including one second after refusal.
* Real runtime comparison rejects a valid option difference beyond byte 1,024;
  full-set adoption and restoration round-trip exactly. The actual six-package
  lobby plan/settings round-trip through the native codec without prefixes.

Evidence is retained under `G:/Temp/Codex/sm-qol-20261027`: the catalog probe,
`netplay-all-ctest.log`, `netplay-actual-large-final/`, real caps logs and
Windows build logs. No release, merge or code push was performed.
