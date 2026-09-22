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

Open the SD root, `knx-analyzer/sessions`, or an individual session folder. The TP1 tab shows candidates and classification filters. Select a row to inspect every recorded field and the original hex bytes. The Analog captures tab reads a finalized RAW file on demand, validates its CRC, and plots ADC sample min/max per pixel with a trigger marker. ADC units are uncalibrated RAW counts. Normal TP1 telegrams are not linked to analog captures by this firmware.

`VALID_UNKNOWN` is a valid frame with uninterpreted higher semantics. The UI does not guess DPTs or group names. An unset `date_time` is shown as device uptime, not a fabricated wall clock time. Warnings report corrupt JSONL lines, and original JSON fields remain available in the detail panel.

## Architecture and formats

See [docs/architecture.md](docs/architecture.md). The engine in `KNXAnalyzer.Core` has no UI dependency. `KNXAnalyzer.Desktop` uses Avalonia and MVVM. Synthetic parser and RAW tests are in `KNXAnalyzer.Core.Tests`.

Real bus validation requires actual `tp1-v1` SD sessions; synthetic tests verify file handling only. Do not commit real SD data or private network information.

The Analog captures list shows computed min, max, peak-to-peak and CRC and can be sorted by peak-to-peak. Pass a folder path followed by --top-capture to select the capture with the greatest validated peak-to-peak value. Near-constant captures are called out explicitly.
