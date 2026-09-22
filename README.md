# KNX TP1 Analyzer Studio

Cross-platform desktop application for viewing and analyzing KNX TP1 Analyzer sessions, telegrams and analog captures.

This first version reads SD data from the [KNX TP1 Analyzer](https://github.com/pdeprost-dot/KNX-TP1-Analyzer) firmware at tag `tp1-v1`, commit `66e51a16274bc4cebe6ff055752467c873bbabad`. It never writes to the selected SD folder. No ETS project or DPT mapping is required.

## Build and run

Install the .NET 10 SDK on Windows or Linux. Then:

```sh
dotnet restore KNXAnalyzerStudio.slnx
dotnet build KNXAnalyzerStudio.slnx
dotnet test KNXAnalyzerStudio.slnx
dotnet run --project src/KNXAnalyzer.Desktop
```

Open the SD root, `knx-analyzer/sessions`, or an individual session folder. The TP1 tab shows candidates and classification filters. Select a row to inspect every recorded field and the original hex bytes. The Analog captures tab reads a finalized RAW file on demand, validates its CRC, and plots ADC sample min/max per pixel with a trigger marker. ADC RAW counts remain the source of truth. The graph can display an explicitly experimental GPIO5 mV estimate. Normal TP1 telegrams are not linked to analog captures by this firmware.

`VALID_UNKNOWN` is a valid frame with uninterpreted higher semantics. The UI does not guess DPTs or group names. An unset `date_time` is shown as device uptime, not a fabricated wall clock time. Warnings report corrupt JSONL lines, and original JSON fields remain available in the detail panel.

## Architecture and formats

See [docs/architecture.md](docs/architecture.md). The engine in `KNXAnalyzer.Core` has no UI dependency. `KNXAnalyzer.Desktop` uses Avalonia and MVVM. Synthetic parser and RAW tests are in `KNXAnalyzer.Core.Tests`.

Real bus validation requires actual `tp1-v1` SD sessions; synthetic tests verify file handling only. Do not commit real SD data or private network information.

The Analog captures list shows computed min, max, peak-to-peak and CRC and can be sorted by peak-to-peak. Pass a folder path followed by --top-capture to select the capture with the greatest validated peak-to-peak value. Near-constant captures are called out explicitly.

In the analog graph, use the wheel to zoom time, left-drag to pan, Ctrl+wheel to zoom vertically, and double-click or Reset/Fit to restore the full capture. The visible time range and trigger position are shown. The graph also offers Zoom and Position sliders for touchpads. The unit selector switches between Estimated mV and RAW. Historical captures have no recorded calibration: the experimental display uses one approximate prototype observation (RAW 1888 at GPIO5 ≈ 1930 mV) and an assumed proportional model. This is not a characterized ADC response; accuracy across the ADC range is unknown. It is not KNX bus voltage. If a future session records a valid multi-point ADC calibration, the graph uses it automatically. RAW is always preserved.

The TP1 details include a conservative decoder for complete, checksum-valid standard frames: control, repeat, priority, addresses, hop count, TPCI, a small documented APCI service set, and APDU bytes. Unsupported or uncertain semantics remain Unknown. ACK-to-telegram assignment, DPTs, device roles, and group meanings are not inferred. Use Générer rapport for a French UTF-8 technician report or Export JSON for a versioned structured analysis. Choose an output path outside the SD session folder; exports contain session-derived private data and must be handled accordingly.

The generic frame layout and control bit meanings follow the KNX Association's TP1 documentation: [KNX TP Telegram Visualiser](https://www.knx.org/projects/knx-tp-telegram-visualiser) and [KNX System Specifications](https://support.knx.org/hc/en-us/articles/360000040999-KNX-Specifications).


## Dataset analysis

Reports and JSON exports can target either the selected session or the full opened folder. The dataset JSON is a versioned analytical index: aggregates keep session provenance and error or analog references retain session IDs plus line, timestamp, or event ID where available. It does not flatten incompatible sessions into one capture.

**Générer lot complet** creates a dated KNX-Analysis folder outside the selected SD data with the global report and index plus one report and JSON pair for every session containing candidates or analog events. Source sessions remain read only. The waveform has a graduated Y axis; switching between authoritative ADC RAW and estimated GPIO5 mV preserves samples and the complete time viewport.
