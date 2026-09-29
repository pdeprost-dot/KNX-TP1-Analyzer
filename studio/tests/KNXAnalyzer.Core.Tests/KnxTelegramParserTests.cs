using KNXAnalyzer.Core;

namespace KNXAnalyzer.Core.Tests;

public class KnxTelegramParserTests
{
    [Theory]
    [InlineData("BCFF160001E10080CA", false, "CA")]
    [InlineData("9CFF160001E10080EA", true, "EA")]
    public void ParsesRealFieldTelegrams(string hex, bool repeated, string checksum)
    {
        var result = KnxTelegramDecoder.Parse(Convert.FromHexString(hex));
        var telegram = Assert.IsType<KnxTelegram>(result.Telegram);
        Assert.Equal(KnxTelegramParseStatus.Success, result.Status);
        Assert.Equal(KnxRecordKind.Telegram, result.Kind);
        Assert.Equal("Standard", telegram.Format);
        Assert.Equal(repeated, telegram.Repeat);
        Assert.Equal("Low", telegram.Priority);
        Assert.False(telegram.ControlField.SystemBroadcast);
        Assert.Equal((ushort)0xFF16, telegram.SourceAddress.Raw);
        Assert.Equal("15.15.22", telegram.Source);
        var group = Assert.IsType<KnxGroupAddress>(telegram.GroupDestination);
        Assert.Equal((ushort)0x0001, group.Raw);
        Assert.Equal("0/0/1", group.ThreeLevel);
        Assert.Equal("0/1", group.TwoLevel);
        Assert.Equal(KnxDestinationType.Group, telegram.DestinationKind);
        Assert.Equal(6, telegram.HopCount);
        Assert.Equal(1, telegram.Length);
        Assert.Equal("Unnumbered data", telegram.Transport.Name);
        Assert.Equal((ushort)0x080, telegram.ApplicationService.Raw);
        Assert.Equal("GroupValueWrite", telegram.Service);
        Assert.Equal("0080", telegram.ApduHex);
        Assert.Equal((byte)0, telegram.ApplicationData.EmbeddedData);
        Assert.Empty(telegram.ApplicationData.Payload);
        Assert.Equal(checksum, telegram.ReceivedChecksum.ToString("X2"));
        Assert.Equal(telegram.ReceivedChecksum, telegram.CalculatedChecksum);
        Assert.True(telegram.ChecksumValid);
    }

    [Fact]
    public void DifferenceBetweenRealFramesIsOnlyRepeatFlagAndMatchingChecksum()
    {
        var first = Convert.FromHexString("BCFF160001E10080CA");
        var second = Convert.FromHexString("9CFF160001E10080EA");
        Assert.Equal(0x20, first[0] ^ second[0]);
        Assert.Equal(0x20, first[^1] ^ second[^1]);
        Assert.True(first.AsSpan(1, 7).SequenceEqual(second.AsSpan(1, 7)));
    }

    [Theory]
    [InlineData("CC", KnxBusControlType.ACK)]
    [InlineData("C0", KnxBusControlType.BUSY)]
    public void SeparatesBusControlsFromTelegrams(string hex, KnxBusControlType expected)
    {
        var result = KnxTelegramDecoder.Parse(Convert.FromHexString(hex));
        Assert.Equal(KnxTelegramParseStatus.Success, result.Status);
        Assert.Equal(KnxRecordKind.BusControl, result.Kind);
        Assert.Equal(expected, Assert.IsType<KnxBusControl>(result.BusControl).Type);
        Assert.Null(result.Telegram);
    }

    [Fact]
    public void RejectsChecksumAndShortOrInvalidTp1Records()
    {
        var checksum = Convert.FromHexString("BCFF160001E10080CB");
        Assert.Equal(KnxTelegramParseStatus.Invalid, KnxTelegramDecoder.Parse(checksum).Status);
        Assert.Contains("Checksum mismatch", KnxTelegramDecoder.Parse(checksum).Reason);
        Assert.Equal(KnxTelegramParseStatus.Invalid, KnxTelegramDecoder.Parse(new byte[] { 0xBC, 0x01 }).Status);

        var candidate = new Tp1Candidate { Line = 1, Classification = "INVALID_PARITY", RawHex = "BCFF160001E10080CA",
            RawBytes = Convert.FromHexString("BCFF160001E10080CA") };
        var invalid = KnxTelegramDecoder.Parse(candidate);
        Assert.Equal(KnxTelegramParseStatus.Invalid, invalid.Status);
        Assert.Null(invalid.Telegram);
    }

    [Fact]
    public void PreservesUnknownApciAsRawInsteadOfGuessing()
    {
        var bytes = Convert.FromHexString("BCFF160001E100C18B");
        var result = KnxTelegramDecoder.Parse(bytes);
        var telegram = Assert.IsType<KnxTelegram>(result.Telegram);
        Assert.False(telegram.ApplicationService.Known);
        Assert.Equal((ushort)0x0C1, telegram.ApplicationService.Raw);
        Assert.Equal("Unknown (raw=0x0C1)", telegram.Service);
    }

    [Fact]
    public void SupportsIndividualDestinationAndBothGroupRepresentations()
    {
        var result = KnxTelegramDecoder.Parse(Convert.FromHexString("BC12342110610080B5"));
        var telegram = Assert.IsType<KnxTelegram>(result.Telegram);
        var individual = Assert.IsType<KnxIndividualAddress>(telegram.IndividualDestination);
        Assert.Equal((ushort)0x2110, individual.Raw);
        Assert.Equal("2.1.16", individual.ToString());
        Assert.Equal(KnxDestinationType.Individual, telegram.DestinationKind);

        var group = new KnxGroupAddress(0x1234);
        Assert.Equal("2/2/52", group.ThreeLevel);
        Assert.Equal("2/564", group.TwoLevel);
    }
}
