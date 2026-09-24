using KNXAnalyzer.Core;

namespace KNXAnalyzer.Core.Tests;

public class OfflineRawAnalyzerTests
{
    private const uint Rate = 96_000;

    [Fact]
    public void ComputesRobustStatisticsAndRejectsFlatOrNoisyCapture()
    {
        var flat = Enumerable.Repeat((ushort)1888, 2000).ToArray();
        var result = OfflineRawAnalyzer.Analyze(new AnalogSampleStream(Rate, 0, flat));
        Assert.Equal(OfflineAnalogClassification.NO_SIGNAL, result.Classification);
        Assert.Equal(1888, result.Baseline);
        Assert.Equal(0, result.NoiseRms);
        Assert.Empty(result.Tp1Candidates);

        var noise = Enumerable.Range(0, 4000).Select(i => (ushort)(1888 + (i * 17 % 7) - 3)).ToArray();
        result = OfflineRawAnalyzer.Analyze(new AnalogSampleStream(Rate, 0, noise));
        Assert.Equal(OfflineAnalogClassification.NO_SIGNAL, result.Classification);
        Assert.Empty(result.PulseCandidates);
    }

    [Fact]
    public void DetectsPulsesAndDecodesSyntheticValidFrame()
    {
        var bytes = Convert.FromHexString("BC12342110E1008035");
        var samples = Encode(bytes);
        var result = OfflineRawAnalyzer.Analyze(new AnalogSampleStream(Rate, 500, samples));
        var frame = Assert.Single(result.Tp1Candidates, x => x.Classification == OfflineAnalogClassification.TP1_VALID_FRAME);
        Assert.Equal(Convert.ToHexString(bytes), frame.RawHex);
        Assert.NotNull(frame.Telegram);
        Assert.Equal("1.2.52", frame.Telegram!.Source);
        Assert.InRange(frame.TimingRmsMicroseconds, 0, 1);
        Assert.InRange(frame.TimingMaxErrorMicroseconds, 0, 1);
        Assert.True(result.PulseCandidates.Count > 10);
    }

    [Fact]
    public void ReportsParityFailureWithoutPromotingFrame()
    {
        var bytes = Convert.FromHexString("BC12342110E1008035");
        var result = OfflineRawAnalyzer.Analyze(new AnalogSampleStream(Rate, 0, Encode(bytes, corruptParityCharacter: 2)));
        Assert.DoesNotContain(result.Tp1Candidates, x => x.Classification == OfflineAnalogClassification.TP1_VALID_FRAME);
        Assert.Contains(result.Tp1Candidates, x => x.Classification == OfflineAnalogClassification.TP1_INVALID_PARITY);
    }

    [Fact]
    public void RejectsSyntheticFrameWithBadChecksum()
    {
        var bytes = Convert.FromHexString("BC12342110E1008034");
        var result = OfflineRawAnalyzer.Analyze(new AnalogSampleStream(Rate, 0, Encode(bytes)));
        Assert.DoesNotContain(result.Tp1Candidates, x => x.Classification == OfflineAnalogClassification.TP1_VALID_FRAME);
        Assert.Contains(result.Tp1Candidates, x => x.Classification == OfflineAnalogClassification.TP1_INVALID_CHECKSUM);
    }


    [Fact]
    public void ComparesHistoricalAndFieldCandidateWithoutChangingBitValidation()
    {
        var bytes = Convert.FromHexString("BC12342110E1008035");
        var capture = new RawCapture {
            EventId = 1, SampleRateHz = Rate, TriggerIndex = 0,
            Samples = Encode(bytes, weakPulseCharacter: 2, weakPulseBit: 1),
            CrcValid = true
        };
        var comparison = OfflineRawAnalyzer.CompareProfiles(capture);
        var historical = Assert.Single(comparison.Historical.Tp1Candidates);
        var fieldCandidate = Assert.Single(comparison.FieldCandidate.Tp1Candidates);
        Assert.Equal(Tp1AnalogDecodeProfile.Historical, comparison.Historical.Profile);
        Assert.Equal(Tp1AnalogDecodeProfile.FieldCandidate, comparison.FieldCandidate.Profile);
        Assert.Equal(OfflineAnalogClassification.TP1_INVALID_PARITY, historical.Classification);
        Assert.Equal(OfflineAnalogClassification.TP1_VALID_FRAME, fieldCandidate.Classification);
        Assert.Equal(Convert.ToHexString(bytes), fieldCandidate.RawHex);
        Assert.False(comparison.ByteStreamsIdentical);
    }
    private static ushort[] Encode(byte[] bytes, int corruptParityCharacter = -1, int weakPulseCharacter = -1, int weakPulseBit = -1)
    {
        const int bitSamples = 10, characterSamples = 130, lead = 50;
        var samples = Enumerable.Repeat((ushort)2000, lead + bytes.Length * characterSamples + 100).ToArray();
        for (var character = 0; character < bytes.Length; character++) {
            var bits = new bool[11];
            bits[0] = false;
            var ones = 0;
            for (var bit = 0; bit < 8; bit++) { bits[bit + 1] = (bytes[character] & (1 << bit)) != 0; if (bits[bit + 1]) ones++; }
            bits[9] = (ones & 1) != 0;
            if (character == corruptParityCharacter) bits[9] = !bits[9];
            bits[10] = true;
            for (var bit = 0; bit < bits.Length; bit++) if (!bits[bit])
                for (var i = 0; i < 3; i++) samples[lead + character * characterSamples + bit * bitSamples + i] =
                    character == weakPulseCharacter && bit == weakPulseBit ? (ushort)1650 : (ushort)1400;
        }
        return samples;
    }
}
