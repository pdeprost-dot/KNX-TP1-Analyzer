namespace KNXAnalyzer.Core;

public sealed record KnxTelegram(
    string Format, byte Control, bool Repeat, string Priority, string Source,
    string Destination, string DestinationType, int HopCount, int Length,
    string Tpci, int? Apci, string Service, string ApduHex, string PayloadHex,
    bool ChecksumValid);

/// <summary>Conservative decoder for complete standard TP1 L_Data frames only.</summary>
public static class KnxTelegramDecoder
{
    public static KnxTelegram? Decode(Tp1Candidate candidate) => candidate.IsValid ? Decode(candidate.RawBytes) : null;

    public static KnxTelegram? Decode(ReadOnlySpan<byte> bytes)
    {
        if (bytes.Length < 9 || (bytes[0] & 0xD3) != 0x90 || bytes.Length != 8 + (bytes[5] & 15)) return null;
        byte xor = 0;
        foreach (var value in bytes) xor ^= value;
        if (xor != 0xFF) return null;
        var src = (bytes[1] << 8) | bytes[2];
        var dst = (bytes[3] << 8) | bytes[4];
        var group = (bytes[5] & 0x80) != 0;
        var source = $"{src >> 12}.{(src >> 8) & 15}.{src & 255}";
        var destination = group ? $"{dst >> 11}/{(dst >> 8) & 7}/{dst & 255}"
            : $"{dst >> 12}.{(dst >> 8) & 15}.{dst & 255}";
        var priority = ((bytes[0] >> 2) & 3) switch { 0 => "System", 1 => "High", 2 => "Alarm", _ => "Low" };
        var tpciCode = bytes[6] >> 6;
        var tpci = tpciCode switch { 0 => "Unnumbered data", 1 => "Numbered data", 2 => "Unnumbered control", _ => "Numbered control" };
        var apdu = bytes.Slice(6, bytes.Length - 7);
        var apci = tpciCode == 0 ? ((bytes[6] & 3) << 2) | (bytes[7] >> 6) : (int?)null;
        var service = apci switch { 0 => "GroupValueRead", 1 => "GroupValueResponse", 2 => "GroupValueWrite", _ => "Unknown" };
        var payload = apdu.Length > 2 ? Convert.ToHexString(apdu[2..]) : "";
        return new KnxTelegram("Standard", bytes[0], (bytes[0] & 0x20) == 0, priority,
            source, destination, group ? "group" : "individual", (bytes[5] >> 4) & 7,
            bytes[5] & 15, tpci, apci, service, Convert.ToHexString(apdu), payload, true);
    }
}
