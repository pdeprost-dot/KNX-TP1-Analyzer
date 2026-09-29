namespace KNXAnalyzer.Core;

public enum Tp1RecordClassification
{
    VALID_KNOWN,
    VALID_UNKNOWN,
    INVALID_PARITY,
    INVALID_TIMING,
    INVALID_CHECKSUM,
    INCOMPLETE,
    ANALOG_UNDECODED
}

public enum Tp1KnownControl
{
    None,
    ACK,
    NAK,
    BUSY
}

public sealed record Tp1Pulse(
    uint EventId,
    int StartSample,
    int EndSample,
    ulong AbsoluteStartSample,
    ulong AbsoluteEndSample,
    double DurationMicroseconds,
    ushort MinimumAdc,
    Tp1AnalogDecodeProfile Profile);

public sealed record Tp1BitSlot(
    int Index,
    double ExpectedSample,
    double ExpectedMicroseconds,
    bool Value,
    Tp1Pulse? SourcePulse,
    double? TimingErrorSamples,
    double? TimingErrorMicroseconds)
{
    public string Name => Index switch { 0 => "START", 9 => "PARITY", 10 => "STOP", _ => $"D{Index - 1}" };
    public long QuantizedExpectedSample => (long)Math.Floor(ExpectedSample + 0.5);
    public double QuantizationErrorSamples => QuantizedExpectedSample - ExpectedSample;
    public int? AssociatedPulseSample => SourcePulse?.StartSample;
}

public sealed record Tp1Character(
    int Index,
    int StartSample,
    int EndSample,
    ulong AbsoluteStartSample,
    ulong AbsoluteEndSample,
    IReadOnlyList<Tp1BitSlot> Slots,
    byte Value,
    bool StartValid,
    bool ReceivedParity,
    bool ExpectedParity,
    bool ParityValid,
    bool ReceivedStop,
    bool TimingValid,
    IReadOnlyList<string> Errors)
{
    public uint EventId { get; init; }
    public int RecordIndex { get; init; }
    public double RelativeStartMicroseconds { get; init; }
    public IReadOnlyList<Tp1Pulse> UnassignedPulses { get; init; } = [];
    public IReadOnlyList<Tp1Pulse> DuplicateSlotPulses { get; init; } = [];
    public string DataBitsLsb => string.Join(" ", Slots.Skip(1).Take(8).Select(x => x.Value ? '1' : '0'));
    public int DataOneCount => System.Numerics.BitOperations.PopCount(Value);
    public double TimingSignedMeanSamples => AssignedTimingErrors.DefaultIfEmpty(0).Average();
    public double TimingAbsoluteMeanSamples => AssignedTimingErrors.Select(Math.Abs).DefaultIfEmpty(0).Average();
    public double TimingRmsSamples => Math.Sqrt(AssignedTimingErrors.Select(x => x * x).DefaultIfEmpty(0).Average());
    public double TimingMaximumSamples => AssignedTimingErrors.Select(Math.Abs).DefaultIfEmpty(0).Max();
    private IEnumerable<double> AssignedTimingErrors => Slots.Where(x => x.TimingErrorSamples.HasValue).Select(x => x.TimingErrorSamples!.Value);
}

public sealed record Tp1DecodedRecord(
    int StartSample,
    int EndSample,
    ulong AbsoluteStartSample,
    ulong AbsoluteEndSample,
    IReadOnlyList<uint> ProvenanceEventIds,
    IReadOnlyList<Tp1Character> Characters,
    byte[] Bytes,
    Tp1RecordClassification Classification,
    Tp1KnownControl KnownControl,
    int ParityErrors,
    int TimingErrors,
    bool Overflow,
    bool? ChecksumValid,
    bool? StandardLengthValid,
    KnxTelegram? Telegram)
{
    public string RawHex => Convert.ToHexString(Bytes);
    public int RecordIndex { get; init; }
}

public sealed record Tp1TimingProfile(
    double BitRate,
    double CharacterEndMicroseconds,
    double RecordGapMicroseconds,
    double TimingToleranceMicroseconds,
    int MaximumRecordBytes)
{
    private const double C6ReferenceRate = 83333.0;
    public static Tp1TimingProfile C6Validated { get; } = new(
        9600.0,
        96.0 * 1_000_000.0 / C6ReferenceRate,
        160.0 * 1_000_000.0 / C6ReferenceRate,
        3.0 * 1_000_000.0 / C6ReferenceRate,
        64);
}

public sealed class Tp1DecodeResult
{
    public required Tp1AnalogDecodeProfile Profile { get; init; }
    public required Tp1TimingProfile Timing { get; init; }
    public uint EventId { get; init; }
    public uint SampleRateHz { get; init; }
    public ulong AbsoluteSampleStart { get; init; }
    public IReadOnlyList<Tp1Pulse> Pulses { get; init; } = [];
    public IReadOnlyList<Tp1DecodedRecord> Records { get; init; } = [];
    public int CharacterCount => Records.Sum(x => x.Characters.Count);
    public int ValidRecordCount => Records.Count(x => x.Classification is Tp1RecordClassification.VALID_KNOWN or Tp1RecordClassification.VALID_UNKNOWN);
    public int AckCount => Records.Count(x => x.KnownControl == Tp1KnownControl.ACK);
}

public static class Tp1AnalogPulseExtractor
{
    public static IReadOnlyList<Tp1Pulse> Extract(RawCapture capture, Tp1AnalogDecodeProfile profile) =>
        Extract(new AnalogSampleStream(capture.SampleRateHz, capture.TriggerIndex, capture.Samples), profile,
            capture.EventId, capture.SampleStart);

    public static IReadOnlyList<Tp1Pulse> Extract(AnalogSampleStream stream, Tp1AnalogDecodeProfile profile,
        uint eventId = 0, ulong absoluteSampleStart = 0)
    {
        var pulses = new List<Tp1Pulse>();
        if (stream.SampleRateHz == 0) return pulses;
        var low = false;
        var start = 0;
        var minimum = ushort.MaxValue;
        for (var i = 0; i < stream.Samples.Count; ++i) {
            var sample = stream.Samples[i];
            if (!low && sample < profile.LowThreshold) {
                low = true;
                start = i;
                minimum = sample;
            } else if (low) {
                minimum = Math.Min(minimum, sample);
                if (sample >= profile.HighThreshold) {
                    pulses.Add(Create(eventId, absoluteSampleStart, start, i, minimum, profile, stream.SampleRateHz));
                    low = false;
                }
            }
        }
        if (low)
            pulses.Add(Create(eventId, absoluteSampleStart, start, stream.Samples.Count, minimum, profile, stream.SampleRateHz));
        return pulses;
    }

    private static Tp1Pulse Create(uint eventId, ulong absoluteStart, int start, int end, ushort minimum,
        Tp1AnalogDecodeProfile profile, uint rate) => new(
            eventId, start, end, absoluteStart + (ulong)start, absoluteStart + (ulong)end,
            1_000_000.0 * (end - start) / rate, minimum, profile);
}

public static class Tp1DeterministicDecoder
{
    public static Tp1DecodeResult Decode(RawCapture capture, Tp1AnalogDecodeProfile profile,
        Tp1TimingProfile? timing = null) => Decode(
            new AnalogSampleStream(capture.SampleRateHz, capture.TriggerIndex, capture.Samples),
            Tp1AnalogPulseExtractor.Extract(capture, profile), profile, timing, capture.EventId, capture.SampleStart);

    public static Tp1DecodeResult Decode(AnalogSampleStream stream, IReadOnlyList<Tp1Pulse> pulses,
        Tp1AnalogDecodeProfile profile, Tp1TimingProfile? timing = null, uint eventId = 0,
        ulong absoluteSampleStart = 0)
    {
        timing ??= Tp1TimingProfile.C6Validated;
        if (stream.SampleRateHz == 0)
            return new Tp1DecodeResult { Profile = profile, Timing = timing, EventId = eventId };

        var records = new List<Tp1DecodedRecord>();
        var characters = new List<Tp1Character>();
        var characterPulses = new Dictionary<int, Tp1Pulse>();
        var characterErrors = new List<string>();
        var unassignedPulses = new List<Tp1Pulse>();
        var duplicatePulses = new List<Tp1Pulse>();
        var characterActive = false;
        var characterStart = 0;
        var previousCharacterStart = 0;
        var recordStart = 0;
        var overflow = false;
        var characterEndSamples = timing.CharacterEndMicroseconds * stream.SampleRateHz / 1_000_000.0;
        var recordGapSamples = timing.RecordGapMicroseconds * stream.SampleRateHz / 1_000_000.0;
        var toleranceSamples = timing.TimingToleranceMicroseconds * stream.SampleRateHz / 1_000_000.0;
        var bitSamples = stream.SampleRateHz / timing.BitRate;

        void FinishCharacter()
        {
            if (!characterActive) return;
            characterActive = false;
            var slots = new List<Tp1BitSlot>(11);
            for (var bit = 0; bit <= 10; ++bit) {
                characterPulses.TryGetValue(bit, out var pulse);
                var expected = characterStart + bit * bitSamples;
                var error = pulse is null ? (double?)null : pulse.StartSample - expected;
                slots.Add(new Tp1BitSlot(bit, expected, 1_000_000.0 * expected / stream.SampleRateHz,
                    pulse is null, pulse, error, error * 1_000_000.0 / stream.SampleRateHz));
            }
            byte value = 0;
            for (var bit = 0; bit < 8; ++bit)
                if (slots[bit + 1].Value) value |= (byte)(1 << bit);
            var receivedParity = slots[9].Value;
            var expectedParity = (System.Numerics.BitOperations.PopCount(value) & 1) != 0;
            var receivedStop = slots[10].Value;
            var errors = characterErrors.ToList();
            if (receivedParity != expectedParity) errors.Add("parity");
            if (!receivedStop) errors.Add("stop");
            var timingValid = characterErrors.Count == 0 && receivedStop;
            var end = Math.Min(stream.Samples.Count,
                Math.Max(characterStart + 1, (int)Math.Ceiling(characterStart + characterEndSamples)));
            characters.Add(new Tp1Character(characters.Count, characterStart, end,
                absoluteSampleStart + (ulong)characterStart, absoluteSampleStart + (ulong)end,
                slots, value, !slots[0].Value, receivedParity, expectedParity,
                receivedParity == expectedParity, receivedStop, timingValid, errors) {
                    EventId = eventId,
                    RelativeStartMicroseconds = 1_000_000.0 * (characterStart - (double)stream.TriggerIndex) / stream.SampleRateHz,
                    UnassignedPulses = unassignedPulses.ToArray(),
                    DuplicateSlotPulses = duplicatePulses.ToArray()
                });
            characterPulses = [];
            characterErrors = [];
            unassignedPulses = [];
            duplicatePulses = [];
        }

        void QueueRecord()
        {
            if (characters.Count == 0) return;
            var recordIndex = records.Count;
            var retained = characters.Take(timing.MaximumRecordBytes)
                .Select((character, index) => character with { Index = index, RecordIndex = recordIndex }).ToArray();
            var bytes = retained.Select(x => x.Value).ToArray();
            overflow |= characters.Count > timing.MaximumRecordBytes;
            var parityErrors = retained.Count(x => !x.ParityValid);
            var timingErrors = retained.Count(x => !x.TimingValid);
            var known = bytes.Length == 1 ? bytes[0] switch {
                0xCC => Tp1KnownControl.ACK,
                0x0C => Tp1KnownControl.NAK,
                0xC0 => Tp1KnownControl.BUSY,
                _ => Tp1KnownControl.None
            } : Tp1KnownControl.None;
            bool? checksum = bytes.Length >= 8 ? bytes.Aggregate((byte)0, (a, b) => (byte)(a ^ b)) == 0xFF : null;
            bool? length = bytes.Length >= 8 && (bytes[0] & 0x80) != 0
                ? bytes.Length == 8 + (bytes[5] & 0x0F) : null;
            var classification = Classify(bytes, parityErrors, timingErrors, overflow, known, checksum, length);
            var telegram = classification == Tp1RecordClassification.VALID_UNKNOWN
                ? KnxTelegramDecoder.Decode(bytes) : null;
            var end = retained[^1].EndSample;
            records.Add(new Tp1DecodedRecord(recordStart, end, absoluteSampleStart + (ulong)recordStart,
                absoluteSampleStart + (ulong)end, [eventId], retained, bytes, classification, known,
                parityErrors, timingErrors, overflow, checksum, length, telegram) { RecordIndex = recordIndex });
            characters = [];
            overflow = false;
        }

        foreach (var pulse in pulses.OrderBy(x => x.StartSample)) {
            var sample = pulse.StartSample;
            if (characterActive && sample - characterStart > characterEndSamples) FinishCharacter();
            if (characters.Count > 0 && !characterActive && sample - previousCharacterStart > recordGapSamples)
                QueueRecord();
            if (!characterActive) {
                if (characters.Count == 0) recordStart = sample;
                previousCharacterStart = characterStart = sample;
                characterActive = true;
                characterPulses = new Dictionary<int, Tp1Pulse> { [0] = pulse };
                characterErrors = [];
                unassignedPulses = [];
                duplicatePulses = [];
                continue;
            }

            var delta = sample - characterStart;
            var bit = (int)Math.Floor(delta * timing.BitRate / stream.SampleRateHz + 0.5);
            var errorSamples = delta - bit * bitSamples;
            if (bit > 10 || Math.Abs(errorSamples) > toleranceSamples) {
                characterErrors.Add($"pulse {sample}: timing {errorSamples:F3} samples");
                unassignedPulses.Add(pulse);
            } else if (!characterPulses.ContainsKey(bit)) {
                characterPulses[bit] = pulse;
            } else duplicatePulses.Add(pulse);
        }
        FinishCharacter();
        QueueRecord();

        return new Tp1DecodeResult {
            Profile = profile,
            Timing = timing,
            EventId = eventId,
            SampleRateHz = stream.SampleRateHz,
            AbsoluteSampleStart = absoluteSampleStart,
            Pulses = pulses,
            Records = records
        };
    }

    private static Tp1RecordClassification Classify(byte[] bytes, int parityErrors, int timingErrors,
        bool overflow, Tp1KnownControl known, bool? checksum, bool? standardLength)
    {
        if (parityErrors > 0) return Tp1RecordClassification.INVALID_PARITY;
        if (timingErrors > 0 || overflow) return Tp1RecordClassification.INVALID_TIMING;
        if (bytes.Length == 1) return known != Tp1KnownControl.None
            ? Tp1RecordClassification.VALID_KNOWN : Tp1RecordClassification.ANALOG_UNDECODED;
        if (bytes.Length < 8) return Tp1RecordClassification.INCOMPLETE;
        if (checksum != true) return Tp1RecordClassification.INVALID_CHECKSUM;
        if (standardLength == false) return Tp1RecordClassification.INCOMPLETE;
        return Tp1RecordClassification.VALID_UNKNOWN;
    }

    public static IReadOnlyList<Tp1DecodedRecord> MergeExactOverlaps(IEnumerable<Tp1DecodeResult> results) =>
        results.SelectMany(x => x.Records)
            .GroupBy(x => (x.AbsoluteStartSample, x.AbsoluteEndSample, x.RawHex, x.Classification))
            .Select(group => group.First() with {
                ProvenanceEventIds = group.SelectMany(x => x.ProvenanceEventIds).Distinct().Order().ToArray()
            })
            .OrderBy(x => x.AbsoluteStartSample).ToArray();
}
