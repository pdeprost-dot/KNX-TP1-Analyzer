# Network Session API V1

The machine API is language-independent and keeps English endpoint names and JSON keys.

- `GET /api/analyzer`: identity, firmware, logger format, state, network, SD and OTA status.
- `GET /api/sessions`: lightweight `EVENT-*` session inventory from metadata only.
- `GET /api/sessions/{session}/manifest`: session start/result plus links to streamed metadata.
- `GET /api/sessions/{session}/files/{file}`: controlled file access.

Files support `Range: bytes=start-end` and `Range: bytes=start-`. Valid ranges return `206` with `Accept-Ranges`, `Content-Length`, and `Content-Range`; invalid ranges return `416`. Full requests return `200`. RAW download is refused during `CAPTURING` and `FINALIZING`.

Session and filename components reject absolute paths, separators, `..`, and characters outside the controlled name set. Transfers read 8 KiB from SD and use approximately 1360-byte TCP writes with write-all, disconnect detection and a no-progress timeout. Clients resume from the last verified offset and validate available CRCs.

FR : cette API machine reste volontairement en anglais. Elle permet l'identité Analyzer, l'inventaire et le manifeste des sessions, l'accès contrôlé aux fichiers, HTTP Range et la reprise exacte. Les interfaces humaines et la documentation d'utilisation restent bilingues.
