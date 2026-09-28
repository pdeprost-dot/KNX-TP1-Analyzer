namespace KNXAnalyzer.Core.Experimental;

public enum Tp1EvidenceDecision { NoPulse, Ambiguous, Pulse }
public enum Tp1PhysicalConfidence { Ambiguous, Low, Medium, High }

public sealed record Tp1EvidenceModel(double[] Template, double NegativeDepth90 = .196,
    double PositiveDepth10 = .823, double NegativeTemplate90 = .637, double PositiveTemplate10 = .742);

public sealed record Tp1EvidencePulse(int Sample, double Score, Tp1EvidenceDecision Decision,
    double RelativeDepth, double TemplateScore, ushort MinimumAdc);

public sealed record Tp1EvidenceSlot(int Index, double ExpectedSample, Tp1EvidenceDecision Decision,
    double Score, double RelativeDepth, double TemplateScore, int MinimumSample);

public sealed record Tp1EvidenceMeasurement(int Index, double ExpectedSample, Tp1EvidenceDecision Decision,
    double Score, double Baseline, double Minimum, int MinimumSample, double RelativeDepth,
    double TemplateScore, int Width, double Fall, double Rise, double Area, double Noise)
{
    public double Confidence => Math.Abs(Score - .5) * 2;
}

public sealed record Tp1EvidenceCharacter(int StartSample, IReadOnlyList<Tp1EvidenceSlot> Slots,
    byte BestEvidenceValue, bool StrictPhysicalValid, bool ParityValid, bool StopValid,
    Tp1PhysicalConfidence PhysicalConfidence);

public sealed record Tp1EvidenceRecord(IReadOnlyList<Tp1EvidenceCharacter> Characters, byte[] Bytes,
    string Classification, Tp1KnownControl KnownControl, bool? ChecksumValid, bool? StandardLengthValid,
    KnxTelegram? Telegram, Tp1PhysicalConfidence PhysicalConfidence);

public sealed record Tp1EvidenceDecodeResult(uint EventId, IReadOnlyList<Tp1EvidencePulse> Pulses,
    IReadOnlyList<Tp1EvidenceCharacter> Characters, IReadOnlyList<Tp1EvidenceRecord> Records)
{
    public int AmbiguousPulseCount => Pulses.Count(x => x.Decision == Tp1EvidenceDecision.Ambiguous);
    public int DefinitePulseCount => Pulses.Count(x => x.Decision == Tp1EvidenceDecision.Pulse);
}

/// <summary>
/// Experimental physical decoder. It is deliberately not used by Studio's production path.
/// Physical decisions never consume parity, STOP, checksum, record validity or KNX semantics.
/// </summary>
public static class Tp1EvidenceDecoder
{
    private const double BitRate = 9600.0;
    private const double CharacterEndAt83333 = 96.0;
    private const double RecordGapAt83333 = 160.0;

    public static Tp1EvidenceModel TrainTemplate(IEnumerable<(RawCapture Capture, Tp1Pulse Pulse)> references)
    {
        var vectors = references.Select(x => Vector(x.Capture, x.Pulse.StartSample)).ToArray();
        if (vectors.Length == 0) throw new ArgumentException("At least one physical pulse reference is required.", nameof(references));
        return new Tp1EvidenceModel(Enumerable.Range(0, 16).Select(i => Quantile(vectors.Select(x => x[i]), .5)).ToArray());
    }

    public static Tp1EvidenceDecodeResult Decode(RawCapture capture, Tp1EvidenceModel model)
    {
        if (capture.SampleRateHz == 0 || capture.Samples.Length < 32)
            return new(capture.EventId, [], [], []);
        var referenceDepth = EstimatePulseDepth(capture);
        var candidates = ExtractCandidates(capture, model, referenceDepth);
        var starts = candidates.Where(x => x.Decision == Tp1EvidenceDecision.Pulse).OrderBy(x => x.Sample).ToArray();
        var bitPeriod = capture.SampleRateHz / BitRate;
        var characterEnd = CharacterEndAt83333 * capture.SampleRateHz / 83333.0;
        var recordGap = RecordGapAt83333 * capture.SampleRateHz / 83333.0;
        var characters = new List<Tp1EvidenceCharacter>();
        var startIndex = 0;
        while (startIndex < starts.Length) {
            var start = starts[startIndex].Sample;
            var slots = Enumerable.Range(0, 11).Select(slot =>
                MeasureSlot(capture, model, referenceDepth, slot, start + slot * bitPeriod)).ToArray();
            byte value = 0;
            for (var bit = 0; bit < 8; bit++) if (!BestPulse(slots[bit + 1])) value |= (byte)(1 << bit);
            var parity = !BestPulse(slots[9]);
            var expectedParity = (System.Numerics.BitOperations.PopCount(value) & 1) != 0;
            var stop = !BestPulse(slots[10]);
            var ambiguous = slots.Count(x => x.Decision == Tp1EvidenceDecision.Ambiguous);
            var minimumMargin = slots.Min(x => Math.Abs(x.Score - .5) * 2);
            var confidence = ambiguous > 0 ? Tp1PhysicalConfidence.Ambiguous : minimumMargin >= .75
                ? Tp1PhysicalConfidence.High : minimumMargin >= .4 ? Tp1PhysicalConfidence.Medium : Tp1PhysicalConfidence.Low;
            characters.Add(new(start, slots, value, ambiguous == 0, parity == expectedParity, stop, confidence));
            startIndex++;
            while (startIndex < starts.Length && starts[startIndex].Sample - start <= characterEnd) startIndex++;
        }

        var records = new List<Tp1EvidenceRecord>();
        foreach (var group in SplitRecords(characters, recordGap)) records.Add(BuildRecord(group));
        return new(capture.EventId, candidates, characters, records);
    }

    private static IReadOnlyList<Tp1EvidencePulse> ExtractCandidates(RawCapture capture, Tp1EvidenceModel model, double referenceDepth)
    {
        var raw = new List<Tp1EvidencePulse>(); var samples = capture.Samples;
        for (var i = 20; i < samples.Length - 21; i++) {
            if (samples[i] > samples[i - 1] || samples[i] >= samples[i + 1]) continue;
            var evidence = Measure(capture, model, referenceDepth, i);
            if (evidence.Decision == Tp1EvidenceDecision.NoPulse) continue;
            raw.Add(new(evidence.Anchor, evidence.Score, evidence.Decision, evidence.Depth, evidence.Template, samples[i]));
        }
        var kept = new List<Tp1EvidencePulse>();
        foreach (var candidate in raw.OrderByDescending(x => x.Score)) {
            if (kept.Any(x => Math.Abs(x.Sample - candidate.Sample) <= 5)) continue;
            kept.Add(candidate);
        }
        return kept.OrderBy(x => x.Sample).ToArray();
    }

    private static Tp1EvidenceSlot MeasureSlot(RawCapture capture, Tp1EvidenceModel model, double referenceDepth, int slot, double expected)
    {
        var evidence = MeasureFixedSlot(capture, model, referenceDepth, slot, expected);
        return new(slot, expected, evidence.Decision, evidence.Score, evidence.RelativeDepth, evidence.TemplateScore, evidence.MinimumSample);
    }

    public static Tp1EvidenceMeasurement MeasureFixedSlot(RawCapture capture, Tp1EvidenceModel model,
        double referenceDepth, int slot, double expected)
    {
        var center = (int)Math.Round(expected);
        // V6 freezes the historical grid: measure once at its quantized position.
        // The ±3 minimum search and template alignment remain inside the physical
        // measurement itself; no second slot-position search is allowed here.
        var evidence = Measure(capture, model, referenceDepth, center, fixedGrid: true);
        var samples = capture.Samples;
        var localFrom = Math.Max(0, evidence.Anchor - 20); var localTo = Math.Min(samples.Length, evidence.Anchor + 21);
        var local = samples.Skip(localFrom).Take(localTo - localFrom).Select(x => (double)x).ToArray();
        var baseline = Quantile(local, .75); var depth = Math.Max(0, baseline - samples[evidence.Minimum]);
        double fall = 0, rise = 0, area = 0;
        for (var i = Math.Max(1, evidence.Minimum - 6); i <= Math.Min(samples.Length - 2, evidence.Minimum + 8); i++) {
            fall = Math.Max(fall, samples[i - 1] - (double)samples[i]);
            rise = Math.Max(rise, samples[i + 1] - (double)samples[i]);
            area += Math.Max(0, baseline - samples[i]);
        }
        var widthFrom = Math.Max(0, evidence.Minimum - 5); var widthTo = Math.Min(samples.Length, evidence.Minimum + 7);
        var width = Enumerable.Range(widthFrom, widthTo - widthFrom).Count(i => samples[i] <= baseline - depth * .5);
        var upper = local.Where(x => x >= Quantile(local, .5)).ToArray(); var median = Quantile(upper, .5);
        var noise = Quantile(upper.Select(x => Math.Abs(x - median)), .5);
        return new(slot, expected, evidence.Decision, evidence.Score, baseline, samples[evidence.Minimum],
            evidence.Minimum, evidence.Depth, evidence.Template, width, fall, rise, area, noise);
    }

    private static (int Anchor, int Minimum, double Score, Tp1EvidenceDecision Decision, double Depth, double Template) Measure(
        RawCapture capture, Tp1EvidenceModel model, double referenceDepth, int anchor, bool fixedGrid = false)
    {
        anchor = Math.Clamp(anchor, 20, capture.Samples.Length - 21);
        var baseline = LocalBaseline(capture.Samples, anchor);
        var minimum = Enumerable.Range(anchor - 3, 7).MinBy(i => capture.Samples[i]);
        var depth = Math.Max(0, baseline - capture.Samples[minimum]) / Math.Max(1, referenceDepth);
        var template = (fixedGrid ? Enumerable.Range(-3, 7) : Enumerable.Range(-5, 8))
            .Max(shift => Cosine(Vector(capture, anchor + shift), model.Template));
        var d = Math.Clamp((depth - model.NegativeDepth90) / (model.PositiveDepth10 - model.NegativeDepth90), 0, 1);
        var t = Math.Clamp((template - model.NegativeTemplate90) / (model.PositiveTemplate10 - model.NegativeTemplate90), 0, 1);
        var score = .55 * d + .45 * t;
        var decision = score >= .75 ? Tp1EvidenceDecision.Pulse : score <= .25 ? Tp1EvidenceDecision.NoPulse : Tp1EvidenceDecision.Ambiguous;
        return (anchor, minimum, score, decision, depth, template);
    }

    private static bool BestPulse(Tp1EvidenceSlot slot) => slot.Score >= .5;

    private static IEnumerable<IReadOnlyList<Tp1EvidenceCharacter>> SplitRecords(List<Tp1EvidenceCharacter> characters, double gap)
    {
        var current = new List<Tp1EvidenceCharacter>(); var previous = 0;
        foreach (var character in characters) {
            if (current.Count > 0 && character.StartSample - previous > gap) { yield return current.ToArray(); current.Clear(); }
            current.Add(character); previous = character.StartSample;
        }
        if (current.Count > 0) yield return current.ToArray();
    }

    private static Tp1EvidenceRecord BuildRecord(IReadOnlyList<Tp1EvidenceCharacter> characters)
    {
        var retained = characters.Take(64).ToArray(); var bytes = retained.Select(x => x.BestEvidenceValue).ToArray();
        var known = bytes.Length == 1 ? bytes[0] switch { 0xCC => Tp1KnownControl.ACK, 0x0C => Tp1KnownControl.NAK, 0xC0 => Tp1KnownControl.BUSY, _ => Tp1KnownControl.None } : Tp1KnownControl.None;
        bool? checksum = bytes.Length >= 8 ? bytes.Aggregate((byte)0, (a, b) => (byte)(a ^ b)) == 0xFF : null;
        bool? length = bytes.Length >= 8 && (bytes[0] & 0x80) != 0 ? bytes.Length == 8 + (bytes[5] & 0x0F) : null;
        var ambiguous = retained.Any(x => !x.StrictPhysicalValid);
        var parity = retained.All(x => x.ParityValid); var stop = retained.All(x => x.StopValid);
        var classification = ambiguous ? "AMBIGUOUS" : !parity ? "INVALID_PARITY" : !stop ? "INVALID_TIMING" :
            bytes.Length == 1 ? known != Tp1KnownControl.None ? "VALID_KNOWN" : "ANALOG_UNDECODED" :
            bytes.Length < 8 ? "INCOMPLETE" : checksum != true ? "INVALID_CHECKSUM" : length == false ? "INCOMPLETE" : "VALID_UNKNOWN";
        var telegram = classification == "VALID_UNKNOWN" ? KnxTelegramDecoder.Decode(bytes) : null;
        var confidence = retained.Min(x => x.PhysicalConfidence);
        return new(retained, bytes, classification, known, checksum, length, telegram, confidence);
    }

    private static double EstimatePulseDepth(RawCapture capture)
    {
        var depths = new List<double>(); var samples = capture.Samples;
        for (var i = 20; i < samples.Length - 21; i++)
            if (samples[i] <= samples[i - 1] && samples[i] < samples[i + 1]) depths.Add(Math.Max(0, LocalBaseline(samples, i) - samples[i]));
        return Math.Max(1, Quantile(depths, .95));
    }

    private static double LocalBaseline(ushort[] samples, int anchor) => Quantile(samples.Skip(anchor - 20).Take(41).Select(x => (double)x), .75);
    private static double[] Vector(RawCapture capture, int anchor)
    {
        anchor = Math.Clamp(anchor, 20, capture.Samples.Length - 21); var baseline = LocalBaseline(capture.Samples, anchor);
        var minimum = Enumerable.Range(anchor - 2, 12).Min(i => (double)capture.Samples[i]); var depth = Math.Max(1, baseline - minimum);
        return Enumerable.Range(-3, 16).Select(offset => (baseline - capture.Samples[anchor + offset]) / depth).ToArray();
    }
    private static double Cosine(double[] a, double[] b)
    {
        double dot = 0, aa = 0, bb = 0; for (var i = 0; i < a.Length; i++) { dot += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i]; }
        return aa == 0 || bb == 0 ? 0 : dot / Math.Sqrt(aa * bb);
    }
    private static double Quantile(IEnumerable<double> source, double p)
    {
        var x = source.Order().ToArray(); if (x.Length == 0) return 1; var at = p * (x.Length - 1); var lo = (int)Math.Floor(at); var hi = (int)Math.Ceiling(at);
        return x[lo] + (x[hi] - x[lo]) * (at - lo);
    }
}
