# Field Acquisition V1 / Acquisition terrain V1

## Français

`field-analyzer-v1` fige l'acquisition terrain ESP32-C6 issue de la baseline historique `f040038` (blob `5dbada3cd2cf13fccee7b7dbe2073013ef30f200`). La chaîne validée est : KNX TP1 → front-end analogique → ADC/D44 → Event RAW Logger → microSD → Wi-Fi/LAN → Network Session API.

États visibles : `IDLE`, `CAPTURING`, `FINALIZING`, `CLOSED`. Démarrage et arrêt sont disponibles par écran tactile ou Web; l'arrêt tactile demande confirmation. Ne jamais couper l'alimentation pendant `CAPTURING` ou `FINALIZING`. Utiliser une alimentation fiable. OTA est autorisé uniquement en `IDLE` ou `CLOSED`.

Le mode réseau normal combine STA et AP de secours. L'Analyzer est accessible par son hostname mDNS ou son adresse IP. Deux profils Wi-Fi sont configurables et conservés en NVS; leurs mots de passe ne sont jamais retournés par l'API. Le fallback SSID2 est implémenté mais **NOT TESTED — USER ENVIRONMENT LIMITATION**.

Limites : la récupération après coupure brutale n'est pas garantie; LS5/LS5B et la préallocation FAT32 restent expérimentaux et ne font pas partie de cette version. Une coupure peut endommager la session ou FAT32. BUS CHECK DC, auto-calibration TP1, multi-Analyzer et synchronisation sont futurs.

## English

`field-analyzer-v1` freezes the ESP32-C6 field acquisition derived from historical baseline `f040038` (blob `5dbada3cd2cf13fccee7b7dbe2073013ef30f200`). The validated chain is: KNX TP1 → analog front-end → ADC/D44 → Event RAW Logger → microSD → Wi-Fi/LAN → Network Session API.

Visible states are `IDLE`, `CAPTURING`, `FINALIZING`, and `CLOSED`. Capture can be started and stopped from the touchscreen or Web UI; touchscreen stop requires confirmation. Never remove power during `CAPTURING` or `FINALIZING`. Use a reliable power supply. OTA is allowed only while `IDLE` or `CLOSED`.

Normal networking combines STA with a fallback AP. The Analyzer is reachable by mDNS hostname or IP address. Two Wi-Fi profiles are configurable and stored in NVS; passwords are never returned by the API. SSID2 fallback is implemented but **NOT TESTED — USER ENVIRONMENT LIMITATION**.

Limitations: abrupt power-loss recovery is not guaranteed; LS5/LS5B and FAT32 preallocation remain experimental and are not part of this release. Power loss may damage the session or FAT32 filesystem. DC bus checks, TP1 auto-calibration, multi-Analyzer operation and synchronization are future work.

## Data format / Format des données

Format: `knx-long-session-1.0`. Main files: `session-start.json`, `events.jsonl`, `segments.jsonl`, `chunks.jsonl`, `chunk-index.jsonl`, `raw-NNNN.bin`, `test-result.json`.

Sample indexes and event timestamps are 64-bit. RAW is segmented; event metadata is streamed with RAM usage O(1) versus event count. Chunk, segment and session CRCs are computed while writing. Physical media verification is performed offline. The sparse index maps sample → chunk → segment → offset and enables partial HTTP Range access.

## Development / Développement

Normal development uses LAN + authenticated OTA (PBKDF2-HMAC-SHA256). USB is reserved for exceptional recovery. Never publish credentials. OTA is unavailable during `CAPTURING` and `FINALIZING` and returns after `CLOSED`.
