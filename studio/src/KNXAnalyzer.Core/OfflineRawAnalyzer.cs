namespace KNXAnalyzer.Core;

public enum OfflineAnalogClassification
{
    NO_SIGNAL, ACTIVITY_DETECTED, TP1_PULSES_DETECTED, TP1_CANDIDATE,
    TP1_VALID_FRAME, TP1_INVALID_PARITY, TP1_INVALID_CHECKSUM, TP1_INVALID_TIMING,
    TP1_INCOMPLETE, ANALOG_UNDECODED, UNDECODED_ACTIVITY
}

public sealed record AnalogSampleStream(uint SampleRateHz, uint TriggerIndex, IReadOnlyList<ushort> Samples);

public sealed record AnalogHistogramBin(ushort Minimum, ushort Maximum, int Count);
public sealed record AnalogExcursion(int StartSample, int EndSample, string Polarity, double Amplitude, double WidthMicroseconds);
public sealed record OfflineTp1Candidate(
    int StartSample, int EndSample, double StartMilliseconds, string Polarity,
    string RawHex, OfflineAnalogClassification Classification, int ParityErrors,
    int TimingErrors, bool ChecksumValid, double TimingRmsMicroseconds, double TimingMaxErrorMicroseconds,
    KnxTelegram? Telegram, IReadOnlyList<string> Reasons,
    Tp1RecordClassification RecordClassification = Tp1RecordClassification.ANALOG_UNDECODED,
    Tp1KnownControl KnownControl = Tp1KnownControl.None);

public sealed class AnalogAnalysis
{
    public required OfflineAnalogClassification Classification { get; init; }
    public Tp1AnalogDecodeProfile Profile { get; init; } = Tp1AnalogDecodeProfile.Historical;
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
    public Tp1DecodeResult? DecodeResult { get; init; }
    public int ValidFrameCount => DecodeResult?.ValidRecordCount ?? Tp1Candidates.Count(x => x.Classification == OfflineAnalogClassification.TP1_VALID_FRAME);
    public int ParityErrorCount => Tp1Candidates.Count(x => x.Classification == OfflineAnalogClassification.TP1_INVALID_PARITY);
    public int ChecksumErrorCount => Tp1Candidates.Count(x => x.Classification == OfflineAnalogClassification.TP1_INVALID_CHECKSUM);
    public int TimingErrorCount => Tp1Candidates.Count(x => x.Classification == OfflineAnalogClassification.TP1_INVALID_TIMING);
}

public static class OfflineRawAnalyzer
{
    public static AnalogAnalysis Analyze(RawCapture capture, Tp1AnalogDecodeProfile? profile = null) =>
        Analyze(new AnalogSampleStream(capture.SampleRateHz, capture.TriggerIndex, capture.Samples), profile);

    public static AnalogAnalysis Analyze(AnalogSampleStream stream, Tp1AnalogDecodeProfile? profile = null)
    {
        profile ??= Tp1AnalogDecodeProfile.Historical;
        var values = stream.Samples;
        if (values.Count == 0) return Empty(stream.SampleRateHz, profile);
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
        var decoded = Tp1DeterministicDecoder.Decode(stream,
            Tp1AnalogPulseExtractor.Extract(stream, profile), profile);
        var reconstructed = ToOfflineCandidates(stream, decoded);
        var activity = peakToPeak > Math.Max(20, 12 * noiseRms);
        var classification = Classify(activity, pulseCandidates.Length, reconstructed);
        return new AnalogAnalysis {
            Profile = profile, Classification = classification, SampleCount = values.Count, SampleRateHz = stream.SampleRateHz,
            DurationMilliseconds = stream.SampleRateHz == 0 ? 0 : 1000.0 * values.Count / stream.SampleRateHz,
            Minimum = minimum, Maximum = maximum, PeakToPeak = peakToPeak, Mean = mean, Median = median,
            StandardDeviation = Math.Sqrt(variance), Baseline = baseline, NoiseRms = noiseRms,
            DetectionThreshold = threshold, EdgeCount = excursions.Count * 2,
            Histogram = Histogram(values, minimum, maximum), Excursions = excursions,
            PulseCandidates = pulseCandidates, PulseIntervalsMicroseconds = pulseIntervals,
            Tp1Candidates = reconstructed, DecodeResult = decoded
        };
    }

    private static OfflineAnalogClassification Classify(bool activity, int pulses, IReadOnlyList<OfflineTp1Candidate> candidates)
    {
        if (candidates.Any(x => x.Classification == OfflineAnalogClassification.TP1_VALID_FRAME)) return OfflineAnalogClassification.TP1_VALID_FRAME;
        if (candidates.Any(x => x.Classification == OfflineAnalogClassification.TP1_INVALID_CHECKSUM)) return OfflineAnalogClassification.TP1_INVALID_CHECKSUM;
        if (candidates.Any(x => x.Classification == OfflineAnalogClassification.TP1_INVALID_PARITY)) return OfflineAnalogClassification.TP1_INVALID_PARITY;
        if (candidates.Any(x => x.Classification == OfflineAnalogClassification.TP1_INVALID_TIMING)) return OfflineAnalogClassification.TP1_INVALID_TIMING;
        if (candidates.Any(x => x.Classification == OfflineAnalogClassification.TP1_INCOMPLETE)) return OfflineAnalogClassification.TP1_INCOMPLETE;
        if (candidates.Any(x => x.Classification == OfflineAnalogClassification.ANALOG_UNDECODED)) return OfflineAnalogClassification.ANALOG_UNDECODED;
        if (candidates.Count > 0) return OfflineAnalogClassification.TP1_CANDIDATE;
        if (pulses >= 3) return OfflineAnalogClassification.TP1_PULSES_DETECTED;
        if (activity) return OfflineAnalogClassification.UNDECODED_ACTIVITY;
        return OfflineAnalogClassification.NO_SIGNAL;
    }

    private static AnalogAnalysis Empty(uint sampleRate, Tp1AnalogDecodeProfile profile) => new() { Profile = profile, Classification = OfflineAnalogClassification.NO_SIGNAL, SampleRateHz = sampleRate };

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

    private static IReadOnlyList<OfflineTp1Candidate> ToOfflineCandidates(AnalogSampleStream stream, Tp1DecodeResult decoded) =>
        decoded.Records.Select(record => {
            var errors = record.Characters.SelectMany(character => character.Slots)
                .Where(slot => slot.SourcePulse is not null && slot.TimingErrorMicroseconds is not null)
                .Select(slot => Math.Abs(slot.TimingErrorMicroseconds!.Value)).ToArray();
            var timingRms = errors.Length == 0 ? 0 : Math.Sqrt(errors.Average(value => value * value));
            var timingMax = errors.Length == 0 ? 0 : errors.Max();
            var classification = record.Classification switch {
                Tp1RecordClassification.VALID_KNOWN or Tp1RecordClassification.VALID_UNKNOWN => OfflineAnalogClassification.TP1_VALID_FRAME,
                Tp1RecordClassification.INVALID_PARITY => OfflineAnalogClassification.TP1_INVALID_PARITY,
                Tp1RecordClassification.INVALID_TIMING => OfflineAnalogClassification.TP1_INVALID_TIMING,
                Tp1RecordClassification.INVALID_CHECKSUM => OfflineAnalogClassification.TP1_INVALID_CHECKSUM,
                Tp1RecordClassification.INCOMPLETE => OfflineAnalogClassification.TP1_INCOMPLETE,
                _ => OfflineAnalogClassification.ANALOG_UNDECODED
            };
            var reasons = new[] {
                "deterministic C6-derived state machine", $"record classification: {record.Classification}",
                $"decoded characters: {record.Characters.Count}", $"parity errors: {record.ParityErrors}",
                $"timing errors: {record.TimingErrors}",
                $"XOR checksum: {(record.ChecksumValid is null ? "not applicable" : record.ChecksumValid.Value ? "valid" : "invalid")}",
                $"known control: {record.KnownControl}"
            };
            return new OfflineTp1Candidate(record.StartSample, record.EndSample,
                1000.0 * (record.StartSample - stream.TriggerIndex) / stream.SampleRateHz,
                "negative", record.RawHex, classification, record.ParityErrors, record.TimingErrors,
                record.ChecksumValid == true, timingRms, timingMax, record.Telegram, reasons,
                record.Classification, record.KnownControl);
        }).ToArray();


    public static Tp1ProfileComparison CompareProfiles(RawCapture capture)
    {
        var historical = Analyze(capture, Tp1AnalogDecodeProfile.Historical);
        var candidate = Analyze(capture, Tp1AnalogDecodeProfile.FieldCandidate);
        return new Tp1ProfileComparison(historical, candidate,
            historical.Tp1Candidates.Select(x => x.RawHex).SequenceEqual(candidate.Tp1Candidates.Select(x => x.RawHex)));
    }
    private static double Median(double[] sorted) => sorted.Length % 2 == 0
        ? (sorted[sorted.Length / 2 - 1] + sorted[sorted.Length / 2]) / 2 : sorted[sorted.Length / 2];
}
