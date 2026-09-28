namespace KNXAnalyzer.Core.Experimental;

public sealed record Tp1FixedEvidenceCharacter(Tp1Character Historical,
    IReadOnlyList<Tp1EvidenceMeasurement> Slots, byte BestEvidenceValue, bool StrictPhysicalValid,
    bool StartEvidenceMismatch, bool ParityValid, bool StopValid, Tp1PhysicalConfidence PhysicalConfidence)
{
    public int AmbiguousSlotCount => Slots.Count(x => x.Decision == Tp1EvidenceDecision.Ambiguous);
    public bool DataChanged => BestEvidenceValue != Historical.Value;
}

public sealed record Tp1FixedEvidenceRecord(Tp1DecodedRecord Historical,
    IReadOnlyList<Tp1FixedEvidenceCharacter> Characters, byte[] BestEvidenceBytes,
    string StrictClassification, Tp1KnownControl KnownControl, bool? ChecksumValid,
    bool? StandardLengthValid, KnxTelegram? Telegram);

public sealed record Tp1FixedEvidenceDecodeResult(uint EventId,
    IReadOnlyList<Tp1FixedEvidenceRecord> Records)
{
    public IReadOnlyList<Tp1FixedEvidenceCharacter> Characters => Records.SelectMany(x => x.Characters).ToArray();
}

/// <summary>
/// Experimental V6 decoder. FieldCandidate supplies immutable START/grid/record boundaries;
/// only the physical state of each fixed slot is independently re-evaluated.
/// </summary>
public static class Tp1FixedStartEvidenceDecoder
{
    public static Tp1FixedEvidenceDecodeResult Decode(RawCapture capture, Tp1DecodeResult historical,
        Tp1EvidenceModel model, double pulseDepthReference)
    {
        var records = historical.Records.Select(record => BuildRecord(capture, record, model, pulseDepthReference)).ToArray();
        if (records.Sum(x => x.Characters.Count) != historical.CharacterCount || records.Length != historical.Records.Count)
            throw new InvalidOperationException("V6 must preserve every historical character and record boundary.");
        return new(capture.EventId, records);
    }

    private static Tp1FixedEvidenceRecord BuildRecord(RawCapture capture, Tp1DecodedRecord record,
        Tp1EvidenceModel model, double referenceDepth)
    {
        var characters = record.Characters.Select(character => BuildCharacter(capture, character, model, referenceDepth)).ToArray();
        var bytes = characters.Select(x => x.BestEvidenceValue).ToArray();
        var ambiguous = characters.Any(x => !x.StrictPhysicalValid);
        var parity = characters.All(x => x.ParityValid); var stop = characters.All(x => x.StopValid);
        var known = bytes.Length == 1 ? bytes[0] switch {
            0xCC => Tp1KnownControl.ACK, 0x0C => Tp1KnownControl.NAK, 0xC0 => Tp1KnownControl.BUSY, _ => Tp1KnownControl.None
        } : Tp1KnownControl.None;
        bool? checksum = bytes.Length >= 8 ? bytes.Aggregate((byte)0, (a, b) => (byte)(a ^ b)) == 0xFF : null;
        bool? length = bytes.Length >= 8 && (bytes[0] & 0x80) != 0 ? bytes.Length == 8 + (bytes[5] & 0x0F) : null;
        var classification = ambiguous ? "AMBIGUOUS" : !parity ? "INVALID_PARITY" : !stop ? "INVALID_TIMING" :
            bytes.Length == 1 ? known != Tp1KnownControl.None ? "VALID_KNOWN" : "ANALOG_UNDECODED" :
            bytes.Length < 8 ? "INCOMPLETE" : checksum != true ? "INVALID_CHECKSUM" : length == false ? "INCOMPLETE" : "VALID_UNKNOWN";
        var telegram = classification == "VALID_UNKNOWN" ? KnxTelegramDecoder.Decode(bytes) : null;
        return new(record, characters, bytes, classification, known, checksum, length, telegram);
    }

    private static Tp1FixedEvidenceCharacter BuildCharacter(RawCapture capture, Tp1Character historical,
        Tp1EvidenceModel model, double referenceDepth)
    {
        var slots = historical.Slots.Select(slot => Tp1EvidenceDecoder.MeasureFixedSlot(
            capture, model, referenceDepth, slot.Index, slot.ExpectedSample)).ToArray();
        byte value = 0;
        for (var bit = 0; bit < 8; bit++) if (!BestPulse(slots[bit + 1])) value |= (byte)(1 << bit);
        var receivedParity = !BestPulse(slots[9]);
        var expectedParity = (System.Numerics.BitOperations.PopCount(value) & 1) != 0;
        var stop = !BestPulse(slots[10]);
        var strict = slots.All(x => x.Decision != Tp1EvidenceDecision.Ambiguous);
        var minimum = slots.Min(x => x.Confidence);
        var confidence = !strict ? Tp1PhysicalConfidence.Ambiguous : minimum >= .75 ? Tp1PhysicalConfidence.High :
            minimum >= .4 ? Tp1PhysicalConfidence.Medium : Tp1PhysicalConfidence.Low;
        return new(historical, slots, value, strict,
            slots[0].Decision != Tp1EvidenceDecision.Pulse, receivedParity == expectedParity, stop, confidence);
    }

    private static bool BestPulse(Tp1EvidenceMeasurement slot) => slot.Score >= .5;
}
