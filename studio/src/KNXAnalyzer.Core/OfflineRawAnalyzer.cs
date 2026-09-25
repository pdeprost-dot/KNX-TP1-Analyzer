namespace KNXAnalyzer.Core;

public enum OfflineAnalogClassification
{
    NO_SIGNAL, ACTIVITY_DETECTED, TP1_PULSES_DETECTED, TP1_CANDIDATE,
    TP1_VALID_FRAME, TP1_INVALID_PARITY, TP1_INVALID_CHECKSUM, TP1_INVALID_TIMING, UNDECODED_ACTIVITY
}

public sealed record AnalogSampleStream(uint SampleRateHz, uint TriggerIndex, IReadOnlyList<ushort> Samples);

public sealed record AnalogHistogramBin(ushort Minimum, ushort Maximum, int Count);
public sealed record AnalogExcursion(int StartSample, int EndSample, string Polarity, double Amplitude, double WidthMicroseconds);
public sealed record OfflineTp1Candidate(
    int StartSample, int EndSample, double StartMilliseconds, string Polarity,
    string RawHex, OfflineAnalogClassification Classification, int ParityErrors,
    int TimingErrors, bool ChecksumValid, double TimingRmsMicroseconds, double TimingMaxErrorMicroseconds,
    KnxTelegram? Telegram, IReadOnlyList<string> Reasons);

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
    public int ValidFrameCount => Tp1Candidates.Count(x => x.Classification == OfflineAnalogClassification.TP1_VALID_FRAME);
    public int ParityErrorCount => Tp1Candidates.Count(x => x.Classification == OfflineAnalogClassification.TP1_INVALID_PARITY);
    public int ChecksumErrorCount => Tp1Candidates.Count(x => x.Classification == OfflineAnalogClassification.TP1_INVALID_CHECKSUM);
    public int TimingErrorCount => Tp1Candidates.Count(x => x.Classification == OfflineAnalogClassification.TP1_INVALID_TIMING);
}

public static class OfflineRawAnalyzer
{
    private const double Tp1BitRate = 9600.0;

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
        var reconstructed = Decode(stream, DetectSchmittPulses(values, profile, stream.SampleRateHz));
        var activity = peakToPeak > Math.Max(20, 12 * noiseRms);
        var classification = Classify(activity, pulseCandidates.Length, reconstructed);
        return new AnalogAnalysis {
            Profile = profile, Classification = classification, SampleCount = values.Count, SampleRateHz = stream.SampleRateHz,
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
        if (candidates.Any(x => x.Classification == OfflineAnalogClassification.TP1_INVALID_TIMING)) return OfflineAnalogClassification.TP1_INVALID_TIMING;
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

    private static IReadOnlyList<AnalogExcursion> DetectSchmittPulses(
        IReadOnlyList<ushort> values, Tp1AnalogDecodeProfile profile, uint rate)
    {
        var result = new List<AnalogExcursion>();
        var low = false;
        var start = 0;
        ushort minimum = ushort.MaxValue;
        for (var i = 0; i < values.Count; i++) {
            if (!low && values[i] < profile.LowThreshold) {
                low = true; start = i; minimum = values[i];
            } else if (low) {
                minimum = Math.Min(minimum, values[i]);
                if (values[i] >= profile.HighThreshold) {
                    result.Add(new AnalogExcursion(start, i, "negative",
                        profile.HighThreshold - minimum,
                        rate == 0 ? 0 : 1_000_000.0 * (i - start) / rate));
                    low = false;
                }
            }
        }
        if (low) result.Add(new AnalogExcursion(start, values.Count, "negative",
            profile.HighThreshold - minimum,
            rate == 0 ? 0 : 1_000_000.0 * (values.Count - start) / rate));
        return result;
    }
    private static IReadOnlyList<OfflineTp1Candidate> Decode(AnalogSampleStream stream, IReadOnlyList<AnalogExcursion> rawPulses)
    {
        if (stream.SampleRateHz == 0) return [];
        const int characterEndSamples = 96;
        const int frameGapSamples = 160;
        var bitSamples = stream.SampleRateHz / Tp1BitRate;
        var result = new List<OfflineTp1Candidate>();
        var bytes = new List<byte>();
        var timingErrors = new List<double>();
        var characterActive = false;
        var characterStart = 0;
        var previousCharacterStart = 0;
        var pulseBits = 0;
        var parityErrors = 0;
        var timingErrorCount = 0;
        AnalogExcursion? frameStart = null;

        void FinishCharacter()
        {
            if (!characterActive) return;
            characterActive = false;
            byte value = 0;
            var ones = 0;
            for (var bit = 0; bit < 8; bit++) if ((pulseBits & (1 << (bit + 1))) == 0) {
                value |= (byte)(1 << bit); ones++;
            }
            var parityOne = (pulseBits & (1 << 9)) == 0;
            if (((ones + (parityOne ? 1 : 0)) & 1) != 0) parityErrors++;
            if ((pulseBits & (1 << 10)) != 0) timingErrorCount++;
            bytes.Add(value);
        }

        void QueueRecord()
        {
            if (frameStart is not null && bytes.Count >= 8)
                AddCandidate(result, stream, frameStart, "negative", bytes, parityErrors, timingErrorCount, timingErrors, bitSamples);
            bytes = []; timingErrors = []; parityErrors = 0; timingErrorCount = 0; frameStart = null;
        }

        foreach (var pulse in rawPulses.Where(x => x.Polarity == "negative").OrderBy(x => x.StartSample)) {
            var sampleIndex = pulse.StartSample;
            if (characterActive && sampleIndex - characterStart > characterEndSamples) FinishCharacter();
            if (bytes.Count > 0 && !characterActive && sampleIndex - previousCharacterStart > frameGapSamples) QueueRecord();
            if (!characterActive) {
                if (bytes.Count > 0 && sampleIndex - previousCharacterStart > frameGapSamples) QueueRecord();
                frameStart ??= pulse;
                previousCharacterStart = characterStart = sampleIndex;
                characterActive = true;
                pulseBits = 1;
                continue;
            }
            var delta = sampleIndex - characterStart;
            var bit = (int)Math.Round(delta / bitSamples);
            var errorSamples = Math.Abs(delta - bit * bitSamples);
            if (bit > 10 || errorSamples > 3) timingErrorCount++;
            else {
                pulseBits |= 1 << bit;
                timingErrors.Add(errorSamples);
            }
        }
        FinishCharacter();
        QueueRecord();
        return result.OrderBy(x => x.StartSample).ToArray();
    }
    private static void AddCandidate(List<OfflineTp1Candidate> result, AnalogSampleStream stream,
        AnalogExcursion start, string polarity, List<byte> bytes, int parityErrors, int timingErrorCount,
        List<double> timingErrors, double bitSamples)
    {
        if (bytes.Count < 8) return;
        byte xor = 0; foreach (var value in bytes) xor ^= value;
        var checksum = xor == 0xFF;
        var telegram = parityErrors == 0 && timingErrorCount == 0 && checksum ? KnxTelegramDecoder.Decode(bytes.ToArray()) : null;
        var classification = telegram is not null ? OfflineAnalogClassification.TP1_VALID_FRAME :
            parityErrors > 0 ? OfflineAnalogClassification.TP1_INVALID_PARITY :
            timingErrorCount > 0 ? OfflineAnalogClassification.TP1_INVALID_TIMING :
            !checksum ? OfflineAnalogClassification.TP1_INVALID_CHECKSUM : OfflineAnalogClassification.TP1_CANDIDATE;
        var timingRmsUs = timingErrors.Count == 0 ? 0 : Math.Sqrt(timingErrors.Average(x => x * x)) * 1_000_000 / stream.SampleRateHz;
        var timingMaxUs = timingErrors.Count == 0 ? 0 : timingErrors.Max() * 1_000_000 / stream.SampleRateHz;
        var reasons = new List<string> {
            "9600 bit/s timing hypothesis", "13 bit-times between character starts",
            $"pulse polarity: {polarity}", $"decoded characters: {bytes.Count}",
            $"pulse timing RMS error: {timingRmsUs:F2} us", $"pulse timing max error: {timingMaxUs:F2} us",
            $"parity errors: {parityErrors}", $"timing errors: {timingErrorCount}", $"XOR checksum: {(checksum ? "valid" : "invalid")}"
        };
        var end = (int)Math.Ceiling(start.StartSample + bytes.Count * 13 * bitSamples);
        result.Add(new OfflineTp1Candidate(start.StartSample, end,
            1000.0 * (start.StartSample - stream.TriggerIndex) / stream.SampleRateHz,
            polarity, Convert.ToHexString(bytes.ToArray()), classification, parityErrors, timingErrorCount, checksum,
            timingRmsUs, timingMaxUs, telegram, reasons));
    }


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
