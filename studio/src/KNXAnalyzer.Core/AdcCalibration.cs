using System.Text.Json;

namespace KNXAnalyzer.Core;

public readonly record struct AdcCalibrationPoint(ushort Raw, double Millivolts);

/// <summary>Characterized ADC points. Estimates are available only within the measured RAW range.</summary>
public sealed class AdcCalibration
{
    private readonly AdcCalibrationPoint[] _points;
    public string Source { get; }
    public IReadOnlyList<AdcCalibrationPoint> Points => _points;

    private AdcCalibration(string source, AdcCalibrationPoint[] points) { Source = source; _points = points; }

    public static bool TryCreate(string? source, IEnumerable<AdcCalibrationPoint> points, out AdcCalibration? calibration)
    {
        calibration = null;
        var ordered = points.ToArray();
        if (string.IsNullOrWhiteSpace(source) || ordered.Length < 2) return false;
        for (var i = 0; i < ordered.Length; i++) {
            if (!double.IsFinite(ordered[i].Millivolts) || ordered[i].Millivolts < 0) return false;
            if (i > 0 && (ordered[i].Raw <= ordered[i - 1].Raw ||
                          ordered[i].Millivolts <= ordered[i - 1].Millivolts)) return false;
        }
        calibration = new AdcCalibration(source, ordered);
        return true;
    }

    public bool TryEstimateMillivolts(ushort raw, out double millivolts)
    {
        millivolts = 0;
        if (raw < _points[0].Raw || raw > _points[^1].Raw) return false;
        for (var i = 1; i < _points.Length; i++) {
            if (raw > _points[i].Raw) continue;
            var low = _points[i - 1];
            var high = _points[i];
            millivolts = low.Millivolts + (raw - low.Raw) *
                (high.Millivolts - low.Millivolts) / (high.Raw - low.Raw);
            return true;
        }
        return false;
    }
}

public sealed record AnalogDivider(double Gpio5PerBusRatio, string Source)
{
    public bool IsValid => double.IsFinite(Gpio5PerBusRatio) &&
        Gpio5PerBusRatio > 0 && Gpio5PerBusRatio < 1 && !string.IsNullOrWhiteSpace(Source);
}

/// <summary>Optional future metadata chain: RAW -> characterized GPIO5 mV -> divider -> bus V.</summary>
public sealed class AnalogVoltagePipeline
{
    public AdcCalibration? Calibration { get; }
    public AnalogDivider? Divider { get; }
    public AnalogVoltagePipeline(AdcCalibration? calibration, AnalogDivider? divider)
    {
        Calibration = calibration;
        Divider = divider is { IsValid: true } ? divider : null;
    }

    public bool TryEstimateGpio5Millivolts(ushort raw, out double millivolts)
    {
        millivolts = 0;
        return Calibration?.TryEstimateMillivolts(raw, out millivolts) == true;
    }

    public bool TryEstimateKnxBusVolts(ushort raw, out double volts)
    {
        volts = 0;
        if (Divider is null || !TryEstimateGpio5Millivolts(raw, out var millivolts)) return false;
        volts = millivolts / 1000.0 / Divider.Gpio5PerBusRatio;
        return true;
    }

    public static AnalogVoltagePipeline FromSessionMetadata(JsonElement session)
    {
        AdcCalibration? calibration = null;
        AnalogDivider? divider = null;
        if (session.TryGetProperty("adc_calibration", out var adc) && adc.ValueKind == JsonValueKind.Object &&
            adc.TryGetProperty("source", out var source) && source.ValueKind == JsonValueKind.String &&
            adc.TryGetProperty("points", out var points) && points.ValueKind == JsonValueKind.Array) {
            var parsed = new List<AdcCalibrationPoint>();
            var valid = true;
            foreach (var point in points.EnumerateArray()) {
                if (point.ValueKind != JsonValueKind.Object ||
                    !point.TryGetProperty("raw", out var raw) || !raw.TryGetUInt16(out var rawValue) ||
                    !point.TryGetProperty("millivolts", out var mv) || !mv.TryGetDouble(out var mvValue)) {
                    valid = false; break;
                }
                parsed.Add(new AdcCalibrationPoint(rawValue, mvValue));
            }
            if (valid) AdcCalibration.TryCreate(source.GetString(), parsed, out calibration);
        }
        if (session.TryGetProperty("analog_front_end", out var frontEnd) && frontEnd.ValueKind == JsonValueKind.Object &&
            frontEnd.TryGetProperty("gpio5_per_bus_ratio", out var ratio) && ratio.TryGetDouble(out var ratioValue) &&
            frontEnd.TryGetProperty("source", out var dividerSource) && dividerSource.ValueKind == JsonValueKind.String) {
            var candidate = new AnalogDivider(ratioValue, dividerSource.GetString() ?? "");
            if (candidate.IsValid) divider = candidate;
        }
        return new AnalogVoltagePipeline(calibration, divider);
    }
}
