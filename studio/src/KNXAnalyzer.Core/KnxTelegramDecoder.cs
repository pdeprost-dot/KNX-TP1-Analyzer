namespace KNXAnalyzer.Core;

public enum KnxTelegramParseStatus { Success, Unsupported, Invalid, NotApplicable }
public enum KnxRecordKind { Telegram, BusControl, InvalidTp1Record }
public enum KnxDestinationType { Individual, Group }
public enum KnxBusControlType { ACK, NAK, BUSY }

public readonly record struct KnxIndividualAddress(ushort Raw)
{
    public int Area => Raw >> 12;
    public int Line => (Raw >> 8) & 0x0F;
    public int Device => Raw & 0xFF;
    public override string ToString() => $"{Area}.{Line}.{Device}";
}

public readonly record struct KnxGroupAddress(ushort Raw)
{
    public int Main => Raw >> 11;
    public int Middle => (Raw >> 8) & 0x07;
    public int Sub => Raw & 0xFF;
    public int TwoLevelMain => Raw >> 11;
    public int TwoLevelSub => Raw & 0x07FF;
    public string ThreeLevel => $"{Main}/{Middle}/{Sub}";
    public string TwoLevel => $"{TwoLevelMain}/{TwoLevelSub}";
    public override string ToString() => ThreeLevel;
}

public sealed record KnxControlField(byte Raw, bool StandardFrame, bool Repeat, string Priority,
    bool SystemBroadcast, int PriorityCode);
public sealed record KnxTransportControl(byte Raw, int TypeCode, string Name, int? SequenceNumber);
public sealed record KnxApplicationService(ushort Raw, int MainCode, string Name, bool Known)
{
    public string Display => Known ? Name : $"Unknown (raw=0x{Raw:X3})";
}
public sealed record KnxApplicationData(byte[] Apdu, byte EmbeddedData, byte[] Payload)
{
    public string ApduHex => Convert.ToHexString(Apdu);
    public string PayloadHex => Convert.ToHexString(Payload);
    public string RawDisplay => Payload.Length == 0 ? $"embedded 0x{EmbeddedData:X2}" :
        $"embedded 0x{EmbeddedData:X2}, payload {PayloadHex}";
}
public sealed record KnxBusControl(KnxBusControlType Type, byte Raw);

public sealed record KnxTelegram(
    string Format, byte Control, bool Repeat, string Priority, string Source,
    string Destination, string DestinationType, int HopCount, int Length,
    string Tpci, int? Apci, string Service, string ApduHex, string PayloadHex,
    bool ChecksumValid)
{
    public required byte[] RawBytes { get; init; }
    public required KnxControlField ControlField { get; init; }
    public required KnxIndividualAddress SourceAddress { get; init; }
    public KnxIndividualAddress? IndividualDestination { get; init; }
    public KnxGroupAddress? GroupDestination { get; init; }
    public required KnxDestinationType DestinationKind { get; init; }
    public required KnxTransportControl Transport { get; init; }
    public required KnxApplicationService ApplicationService { get; init; }
    public required KnxApplicationData ApplicationData { get; init; }
    public required byte ReceivedChecksum { get; init; }
    public required byte CalculatedChecksum { get; init; }
}

public sealed record KnxTelegramParseResult(
    KnxTelegramParseStatus Status,
    KnxRecordKind Kind,
    byte[] RawBytes,
    KnxTelegram? Telegram = null,
    KnxBusControl? BusControl = null,
    string? Reason = null);

/// <summary>Parses reconstructed TP1 bytes only; no analog state is observed or modified.</summary>
public static class KnxTelegramDecoder
{
    public static KnxTelegram? Decode(Tp1Candidate candidate) => Parse(candidate).Telegram;
    public static KnxTelegram? Decode(ReadOnlySpan<byte> bytes) => Parse(bytes).Telegram;

    public static KnxTelegramParseResult Parse(Tp1Candidate candidate) => candidate.IsValid
        ? Parse(candidate.RawBytes) : Invalid(candidate.RawBytes, $"TP1 classification {candidate.Classification}");

    public static KnxTelegramParseResult Parse(Tp1DecodedRecord record) =>
        record.Classification is Tp1RecordClassification.VALID_KNOWN or Tp1RecordClassification.VALID_UNKNOWN
            ? Parse(record.Bytes) : Invalid(record.Bytes, $"TP1 classification {record.Classification}");

    public static KnxTelegramParseResult Parse(ReadOnlySpan<byte> bytes)
    {
        var raw = bytes.ToArray();
        if (bytes.Length == 1) {
            var type = bytes[0] switch { 0xCC => KnxBusControlType.ACK, 0x0C => KnxBusControlType.NAK,
                0xC0 => KnxBusControlType.BUSY, _ => (KnxBusControlType?)null };
            return type is { } known
                ? new(KnxTelegramParseStatus.Success, KnxRecordKind.BusControl, raw, BusControl: new(known, bytes[0]))
                : new(KnxTelegramParseStatus.NotApplicable, KnxRecordKind.BusControl, raw,
                    Reason: $"Unknown one-byte bus control 0x{bytes[0]:X2}");
        }
        if (bytes.Length < 8) return Invalid(raw, "Record is too short for a standard TP1 telegram");

        byte calculated = 0xFF;
        for (var i = 0; i < bytes.Length - 1; ++i) calculated ^= bytes[i];
        var received = bytes[^1];
        if (received != calculated || Xor(bytes) != 0xFF)
            return Invalid(raw, $"Checksum mismatch: received 0x{received:X2}, calculated 0x{calculated:X2}");
        if ((bytes[0] & 0xD3) != 0x90)
            return new(KnxTelegramParseStatus.Unsupported, KnxRecordKind.Telegram, raw,
                Reason: $"Unsupported control field 0x{bytes[0]:X2}");
        var length = bytes[5] & 0x0F;
        if (bytes.Length != 8 + length)
            return Invalid(raw, $"Length mismatch: NPCI={length}, record bytes={bytes.Length}");

        var source = new KnxIndividualAddress((ushort)((bytes[1] << 8) | bytes[2]));
        var destinationRaw = (ushort)((bytes[3] << 8) | bytes[4]);
        var group = (bytes[5] & 0x80) != 0;
        var priorityCode = (bytes[0] >> 2) & 3;
        var priority = priorityCode switch { 0 => "System", 1 => "High", 2 => "Alarm", _ => "Low" };
        var control = new KnxControlField(bytes[0], true, (bytes[0] & 0x20) == 0,
            priority, (bytes[0] & 0x10) == 0, priorityCode);
        var tpciCode = bytes[6] >> 6;
        int? sequence = tpciCode is 1 or 3 ? (bytes[6] >> 2) & 0x0F : null;
        var tpci = new KnxTransportControl(bytes[6], tpciCode, tpciCode switch {
            0 => "Unnumbered data", 1 => "Numbered data", 2 => "Unnumbered control", _ => "Numbered control"
        }, sequence);
        var apdu = bytes.Slice(6, bytes.Length - 7).ToArray();
        var apciRaw = (ushort)(((bytes[6] & 0x03) << 8) | bytes[7]);
        var apciMain = apciRaw >> 6;
        var serviceName = ServiceName(apciRaw, apciMain);
        var service = new KnxApplicationService(apciRaw, apciMain, serviceName ?? "Unknown", serviceName is not null);
        var applicationData = new KnxApplicationData(apdu, (byte)(bytes[7] & 0x3F), apdu.Length > 2 ? apdu[2..] : []);
        var destination = group ? new KnxGroupAddress(destinationRaw).ThreeLevel : new KnxIndividualAddress(destinationRaw).ToString();

        var telegram = new KnxTelegram("Standard", bytes[0], control.Repeat, priority, source.ToString(),
            destination, group ? "group" : "individual", (bytes[5] >> 4) & 7, length,
            tpci.Name, apciMain, service.Display, applicationData.ApduHex, applicationData.PayloadHex, true) {
            RawBytes = raw, ControlField = control, SourceAddress = source,
            IndividualDestination = group ? null : new KnxIndividualAddress(destinationRaw),
            GroupDestination = group ? new KnxGroupAddress(destinationRaw) : null,
            DestinationKind = group ? KnxDestinationType.Group : KnxDestinationType.Individual,
            Transport = tpci, ApplicationService = service, ApplicationData = applicationData,
            ReceivedChecksum = received, CalculatedChecksum = calculated
        };
        return new(KnxTelegramParseStatus.Success, KnxRecordKind.Telegram, raw, telegram);
    }

    private static byte Xor(ReadOnlySpan<byte> bytes) { byte xor = 0; foreach (var value in bytes) xor ^= value; return xor; }
    private static string? ServiceName(ushort raw, int main) => main switch {
        0 => "GroupValueRead", 1 => "GroupValueResponse", 2 => "GroupValueWrite",
        _ => raw switch {
            0x0C0 => "IndividualAddressWrite", 0x100 => "IndividualAddressRequest",
            0x140 => "IndividualAddressResponse", 0x180 => "AdcRead", 0x1C0 => "AdcResponse",
            0x200 => "MemoryRead", 0x240 => "MemoryResponse", 0x280 => "MemoryWrite",
            0x2C0 => "UserMessage", 0x300 => "MaskVersionRead", 0x340 => "MaskVersionResponse",
            0x380 => "Restart", 0x3C0 => "Escape", _ => null
        }
    };
    private static KnxTelegramParseResult Invalid(byte[] bytes, string reason) =>
        new(KnxTelegramParseStatus.Invalid, KnxRecordKind.InvalidTp1Record, bytes, Reason: reason);
}
