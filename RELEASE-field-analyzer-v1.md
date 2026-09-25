# KNX TP1 Analyzer — Field Acquisition V1

## CE QUI EST VALIDÉ / WHAT IS VALIDATED

FR : acquisition ADC ~83,3 kS/s, sélection D44, PRE/POST 8 333 samples, Event RAW sans perte observée, R1, CRC streaming, index 64 bits, métadonnées O(1), segmentation LS3, finalisation rapide LS4, tactile sécurisé, Web UI, STA+AP, mDNS, OTA et Network Session API V1 avec HTTP Range/reprise byte-perfect.

EN: ~83.3 kS/s ADC acquisition, D44 selection, 8,333-sample PRE/POST, Event RAW with no observed loss, R1, streaming CRC, 64-bit indexes, O(1) metadata, LS3 segmentation, LS4 fast finalizing, protected touchscreen, Web UI, STA+AP, mDNS, OTA, and Network Session API V1 with byte-perfect HTTP Range/resume.

## LIMITES CONNUES / KNOWN LIMITATIONS

FR : fallback SSID2 non testé (limitation environnement utilisateur). Power-loss recovery non garanti; LS5/LS5B en pause. Alimentation fiable requise. FieldCandidate 1715/1815 est confirmé uniquement sur l'installation testée, pas universel.

EN: SSID2 fallback is not tested (user environment limitation). Power-loss recovery is not guaranteed; LS5/LS5B are paused. Reliable power is required. FieldCandidate 1715/1815 is confirmed only on the tested installation and is not universal.

## FORMAT DES DONNÉES / DATA FORMAT

`knx-long-session-1.0`: progressive JSONL metadata, segmented RAW, streaming CRC, sparse index, offline media verification.

## RÉSEAU & OTA / NETWORK & OTA

LAN STA plus fallback AP. OTA is authenticated and available only in `IDLE`/`CLOSED`. USB is exceptional recovery only. No credentials are included.

## PROCHAINE ÉTAPE / NEXT STEP

Import réseau Studio et analyse des données / Studio Network Import and data analysis.

Future: BUS CHECK DC, TP1 auto-calibration, multi-Analyzer synchronization/comparison, optional SD/Wi-Fi throughput improvements, and power-loss hardening if required.
