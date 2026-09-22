namespace KNXAnalyzer.Core;

public enum OfflineAnalogClassification
{
    NO_SIGNAL, ACTIVITY_DETECTED, TP1_PULSES_DETECTED, TP1_CANDIDATE,
    TP1_VALID_FRAME, TP1_INVALID_PARITY, TP1_INVALID_CHECKSUM, UNDECODED_ACTIVITY
}

public sealed record AnalogSampleStream(uint SampleRateHz, uint TriggerIndex, IReadOnlyList<ushort> Samples);

public sealed record AnalogHistogramBin(ushort Minimum, ushort Maximum, int Count);
public sealed record AnalogExcursion(int StartSample, int EndSample, string Polarity, double Amplitude, double WidthMicroseconds);
public sealed record OfflineTp1Candidate(
    int StartSample, int EndSample, double StartMilliseconds, string Polarity,
    string RawHex, OfflineAnalogClassification Classification, int ParityErrors,
    bool ChecksumValid, double TimingRmsMicroseconds, double TimingMaxErrorMicroseconds,
    KnxTelegram? Telegram, IReadOnlyList<string> Reasons);

public sealed class AnalogAnalysis
{
    public required OfflineAnalogClassification Classification { get; init; }
    public int SampleCount { get; init; }
    public uint SampleRateHz { get; init; }
    public double DurationMilliseconds { get; init; }
    public ushort Minimum { get; init; }
    public ushort Maximum { get; init; }
    public int PeakToPeak { get; init; }
    public double Mean { get; init; }
    public double Median { get; init; }
    public double StandardDeviation { get; init; }
    public double Baseline { get; init; }
    public double NoiseRms { get; init; }
    public double DetectionThreshold { get; init; }
    public int EdgeCount { get; init; }
    public IReadOnlyList<AnalogHistogramBin> Histogram { get; init; } = [];
    public IReadOnlyList<AnalogExcursion> Excursions { get; init; } = [];
    public IReadOnlyList<AnalogExcursion> PulseCandidates { get; init; } = [];
    public IReadOnlyList<double> PulseIntervalsMicroseconds { get; init; } = [];
    public IReadOnlyList<OfflineTp1Candidate> Tp1Candidates { get; init; } = [];
    public int ValidFrameCount => Tp1Candidates.Count(x => x.Classification == OfflineAnalogClassification.TP1_VALID_FRAME);
}

public static class OfflineRawAnalyzer
{
    private const double Tp1BitRate = 9600.0;

    public static AnalogAnalysis Analyze(RawCapture capture) =>
        Analyze(new AnalogSampleStream(capture.SampleRateHz, capture.TriggerIndex, capture.Samples));

    public static AnalogAnalysis Analyze(AnalogSampleStream stream)
    {
        var values = stream.Samples;
        if (values.Count == 0) return Empty(stream.SampleRateHz);
        var sorted = values.Order().ToArray();
        var median = Median(sorted.Select(x => (double)x).ToArray());
        var mean = values.Average(x => (double)x);
        var variance = values.Sum(x => Math.Pow(x - mean, 2)) / values.Count;
        var baseline = Mode(values);
        var deviations = values.Select(x => Math.Abs(x - baseline)).Order().Take(Math.Max(1, values.Count / 2)).ToArray();
        var noiseRms = Math.Sqrt(deviations.Sum(x => x * x) / deviations.Length);
        var minimum = values.Min(); var maximum = values.Max(); var peakToPeak = maximum - minimum;
        var threshold = Math.Max(12, Math.Max(6 * noiseRms, peakToPeak * 0.08));
        var excursions = DetectExcursions(values, baseline, threshold, stream.SampleRateHz);
        var pulseCandidates = excursions.Where(x => x.WidthMicroseconds is >= 15 and <= 60).ToArray();
        var pulseIntervals = pulseCandidates.OrderBy(x => x.StartSample).Zip(pulseCandidates.OrderBy(x => x.StartSample).Skip(1),
            (a, b) => 1_000_000.0 * (b.StartSample - a.StartSample) / stream.SampleRateHz).ToArray();
        var reconstructed = Decode(stream, pulseCandidates);
        var activity = peakToPeak > Math.Max(20, 12 * noiseRms);
        var classification = Classify(activity, pulseCandidates.Length, reconstructed);
        return new AnalogAnalysis {
            Classification = classification, SampleCount = values.Count, SampleRateHz = stream.SampleRateHz,
            DurationMilliseconds = stream.SampleRateHz == 0 ? 0 : 1000.0 * values.Count / stream.SampleRateHz,
            Minimum = minimum, Maximum = maximum, PeakToPeak = peakToPeak, Mean = mean, Median = median,
            StandardDeviation = Math.Sqrt(variance), Baseline = baseline, NoiseRms = noiseRms,
            DetectionThreshold = threshold, EdgeCount = excursions.Count * 2,
            Histogram = Histogram(values, minimum, maximum), Excursions = excursions,
            PulseCandidates = pulseCandidates, PulseIntervalsMicroseconds = pulseIntervals, Tp1Candidates = reconstructed
        };
    }

    private static OfflineAnalogClassification Classify(bool activity, int pulses, IReadOnlyList<OfflineTp1Candidate> candidates)
    {
        if (candidates.Any(x => x.Classification == OfflineAnalogClassification.TP1_VALID_FRAME)) return OfflineAnalogClassification.TP1_VALID_FRAME;
        if (candidates.Any(x => x.Classification == OfflineAnalogClassification.TP1_INVALID_CHECKSUM)) return OfflineAnalogClassification.TP1_INVALID_CHECKSUM;
        if (candidates.Any(x => x.Classification == OfflineAnalogClassification.TP1_INVALID_PARITY)) return OfflineAnalogClassification.TP1_INVALID_PARITY;
        if (candidates.Count > 0) return OfflineAnalogClassification.TP1_CANDIDATE;
        if (pulses >= 3) return OfflineAnalogClassification.TP1_PULSES_DETECTED;
        if (activity) return OfflineAnalogClassification.UNDECODED_ACTIVITY;
        return OfflineAnalogClassification.NO_SIGNAL;
    }

    private static AnalogAnalysis Empty(uint sampleRate) => new() { Classification = OfflineAnalogClassification.NO_SIGNAL, SampleRateHz = sampleRate };

    private static double Mode(IReadOnlyList<ushort> values) => values.GroupBy(x => x).OrderByDescending(g => g.Count()).ThenBy(g => g.Key).First().Key;

    private static IReadOnlyList<AnalogHistogramBin> Histogram(IReadOnlyList<ushort> values, ushort minimum, ushort maximum)
    {
        const int bins = 32;
        if (minimum == maximum) return [new AnalogHistogramBin(minimum, maximum, values.Count)];
        var counts = new int[bins];
        foreach (var value in values) counts[Math.Min(bins - 1, (int)((long)(value - minimum) * bins / (maximum - minimum + 1)))]++;
        return Enumerable.Range(0, bins).Select(i => new AnalogHistogramBin(
            (ushort)(minimum + (long)(maximum - minimum + 1) * i / bins),
            (ushort)Math.Min(maximum, minimum + (long)(maximum - minimum + 1) * (i + 1) / bins - 1), counts[i])).ToArray();
    }

    private static IReadOnlyList<AnalogExcursion> DetectExcursions(IReadOnlyList<ushort> values, double baseline, double threshold, uint rate)
    {
        var result = new List<AnalogExcursion>();
        for (var i = 0; i < values.Count;) {
            var delta = values[i] - baseline;
            var polarity = delta > threshold ? "positive" : delta < -threshold ? "negative" : null;
            if (polarity is null) { i++; continue; }
            var start = i; var amplitude = Math.Abs(delta); i++;
            while (i < values.Count) {
                delta = values[i] - baseline;
                if (polarity == "positive" ? delta <= threshold : delta >= -threshold) break;
                amplitude = Math.Max(amplitude, Math.Abs(delta)); i++;
            }
            result.Add(new AnalogExcursion(start, i, polarity, amplitude,
                rate == 0 ? 0 : 1_000_000.0 * (i - start) / rate));
        }
        return result;
    }

    private static IReadOnlyList<OfflineTp1Candidate> Decode(AnalogSampleStream stream, IReadOnlyList<AnalogExcursion> rawPulses)
    {
        if (stream.SampleRateHz == 0) return [];
        var bitSamples = stream.SampleRateHz / Tp1BitRate;
        var candidates = new List<OfflineTp1Candidate>();
        foreach (var polarity in new[] { "negative", "positive" }) {
            var pulses = Collapse(rawPulses.Where(x => x.Polarity == polarity).OrderBy(x => x.StartSample).ToArray(), bitSamples);
            foreach (var startPulse in pulses) {
                var bytes = new List<byte>(); var parityErrors = 0; var framingError = false;
                var timingErrors = new List<double>();
                for (var character = 0; character < 23; character++) {
                    if (!TryCharacter(startPulse.StartSample + character * 13 * bitSamples, pulses, bitSamples,
                        timingErrors, out var value, out var parityValid, out var framingValid)) break;
                    bytes.Add(value);
                    if (!parityValid) parityErrors++;
                    if (!framingValid) framingError = true;
                    if (character == 0 && (value & 0xD3) != 0x90) break;
                    if (bytes.Count >= 6) {
                        var expected = 8 + (bytes[5] & 15);
                        if (bytes.Count == expected) {
                            AddCandidate(candidates, stream, startPulse, polarity, bytes, parityErrors, framingError, timingErrors, bitSamples);
                            break;
                        }
                    }
                }
            }
        }
        return candidates.GroupBy(x => (x.StartSample, x.RawHex)).Select(g => g.First())
            .OrderBy(x => x.StartSample).ToArray();
    }

    private static AnalogExcursion[] Collapse(AnalogExcursion[] pulses, double bitSamples)
    {
        var result = new List<AnalogExcursion>();
        foreach (var pulse in pulses) {
            if (result.Count == 0 || pulse.StartSample - result[^1].StartSample > bitSamples * 0.65) result.Add(pulse);
            else if (pulse.Amplitude > result[^1].Amplitude) result[^1] = pulse;
        }
        return result.ToArray();
    }

    private static bool TryCharacter(double start, AnalogExcursion[] pulses, double bitSamples,
        List<double> timingErrors, out byte value, out bool parityValid, out bool framingValid)
    {
        value = 0; parityValid = false; framingValid = false;
        bool Active(int bit) {
            var expected = start + bit * bitSamples;
            var error = pulses.Select(p => Math.Abs(p.StartSample - expected)).DefaultIfEmpty(double.MaxValue).Min();
            if (error > bitSamples * 0.32) return false;
            timingErrors.Add(error); return true;
        }
        if (!Active(0)) return false;
        var ones = 0;
        for (var bit = 0; bit < 8; bit++) if (!Active(bit + 1)) { value |= (byte)(1 << bit); ones++; }
        var parityOne = !Active(9);
        parityValid = ((ones + (parityOne ? 1 : 0)) & 1) == 0;
        framingValid = !Active(10);
        return true;
    }

    private static void AddCandidate(List<OfflineTp1Candidate> result, AnalogSampleStream stream,
        AnalogExcursion start, string polarity, List<byte> bytes, int parityErrors, bool framingError,
        List<double> timingErrors, double bitSamples)
    {
        if (bytes.Count < 8) return;
        byte xor = 0; foreach (var value in bytes) xor ^= value;
        var checksum = xor == 0xFF;
        var telegram = parityErrors == 0 && !framingError && checksum ? KnxTelegramDecoder.Decode(bytes.ToArray()) : null;
        var classification = telegram is not null ? OfflineAnalogClassification.TP1_VALID_FRAME :
            parityErrors > 0 || framingError ? OfflineAnalogClassification.TP1_INVALID_PARITY :
            !checksum ? OfflineAnalogClassification.TP1_INVALID_CHECKSUM : OfflineAnalogClassification.TP1_CANDIDATE;
        var timingRmsUs = timingErrors.Count == 0 ? 0 : Math.Sqrt(timingErrors.Average(x => x * x)) * 1_000_000 / stream.SampleRateHz;
        var timingMaxUs = timingErrors.Count == 0 ? 0 : timingErrors.Max() * 1_000_000 / stream.SampleRateHz;
        var reasons = new List<string> {
            "9600 bit/s timing hypothesis", "13 bit-times between character starts",
            $"pulse polarity: {polarity}", $"decoded characters: {bytes.Count}",
            $"pulse timing RMS error: {timingRmsUs:F2} us", $"pulse timing max error: {timingMaxUs:F2} us",
            $"parity errors: {parityErrors}", $"framing error: {framingError}", $"XOR checksum: {(checksum ? "valid" : "invalid")}"
        };
        var end = (int)Math.Ceiling(start.StartSample + bytes.Count * 13 * bitSamples);
        result.Add(new OfflineTp1Candidate(start.StartSample, end,
            1000.0 * (start.StartSample - stream.TriggerIndex) / stream.SampleRateHz,
            polarity, Convert.ToHexString(bytes.ToArray()), classification, parityErrors, checksum,
            timingRmsUs, timingMaxUs, telegram, reasons));
    }

    private static double Median(double[] sorted) => sorted.Length % 2 == 0
        ? (sorted[sorted.Length / 2 - 1] + sorted[sorted.Length / 2]) / 2 : sorted[sorted.Length / 2];
}
