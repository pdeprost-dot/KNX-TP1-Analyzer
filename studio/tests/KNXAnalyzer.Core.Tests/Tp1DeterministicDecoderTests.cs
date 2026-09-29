using KNXAnalyzer.Core;

namespace KNXAnalyzer.Core.Tests;

public class Tp1DeterministicDecoderTests
{
    private static readonly byte[] ValidFrame = Convert.FromHexString("BC12342110E1008035");

    [Theory]
    [InlineData(83160u)]
    [InlineData(83333u)]
    [InlineData(96000u)]
    public void DecodesStartLsbParityStopAcrossSampleRates(uint rate)
    {
        var capture = Capture(rate, ValidFrame, absoluteStart: 1_000_000);
        var result = Tp1DeterministicDecoder.Decode(capture, Tp1AnalogDecodeProfile.Historical);

        var record = Assert.Single(result.Records);
        Assert.Equal(Tp1RecordClassification.VALID_UNKNOWN, record.Classification);
        Assert.Equal(ValidFrame, record.Bytes);
        Assert.All(record.Characters, character => {
            Assert.True(character.StartValid);
            Assert.True(character.ParityValid);
            Assert.True(character.ReceivedStop);
            Assert.True(character.TimingValid);
            Assert.Equal(11, character.Slots.Count);
        });
        Assert.Equal(1_000_050UL, record.AbsoluteStartSample);
        Assert.Equal(0, record.RecordIndex);
        Assert.All(record.Characters, character => {
            Assert.Equal(1u, character.EventId);
            Assert.Equal(record.RecordIndex, character.RecordIndex);
        });
        Assert.Same(record.Characters[0].Slots[0].SourcePulse, result.Pulses[0]);
        Assert.Equal("START", record.Characters[0].Slots[0].Name);
        Assert.Equal("D0", record.Characters[0].Slots[1].Name);
        Assert.Equal("PARITY", record.Characters[0].Slots[9].Name);
        Assert.Equal("STOP", record.Characters[0].Slots[10].Name);
        Assert.All(record.Characters.SelectMany(x => x.Slots), slot =>
            Assert.InRange(slot.QuantizationErrorSamples, -0.5, 0.5));
        Assert.InRange(record.Characters[0].Slots[1].ExpectedMicroseconds -
            record.Characters[0].Slots[0].ExpectedMicroseconds,
            1_000_000.0 / 9600 - 0.01, 1_000_000.0 / 9600 + 0.01);
    }

    [Theory]
    [InlineData(0xCC, Tp1KnownControl.ACK)]
    [InlineData(0x0C, Tp1KnownControl.NAK)]
    [InlineData(0xC0, Tp1KnownControl.BUSY)]
    public void RestoresKnownOneByteControls(byte value, Tp1KnownControl expected)
    {
        var record = Assert.Single(Tp1DeterministicDecoder.Decode(
            Capture(83333, [value]), Tp1AnalogDecodeProfile.Historical).Records);
        Assert.Equal(Tp1RecordClassification.VALID_KNOWN, record.Classification);
        Assert.Equal(expected, record.KnownControl);
    }

    [Fact]
    public void PreservesShortAndAnalogUndecodedRecords()
    {
        var shortRecord = Assert.Single(Tp1DeterministicDecoder.Decode(
            Capture(83333, [0x12, 0x34]), Tp1AnalogDecodeProfile.Historical).Records);
        Assert.Equal(Tp1RecordClassification.INCOMPLETE, shortRecord.Classification);
        Assert.Equal("1234", shortRecord.RawHex);

        var unknown = Assert.Single(Tp1DeterministicDecoder.Decode(
            Capture(83333, [0x55]), Tp1AnalogDecodeProfile.Historical).Records);
        Assert.Equal(Tp1RecordClassification.ANALOG_UNDECODED, unknown.Classification);
    }

    [Fact]
    public void DistinguishesParityStopTimingAndChecksumFailures()
    {
        var parity = Assert.Single(Tp1DeterministicDecoder.Decode(
            Capture(83333, ValidFrame, corruptParityCharacter: 2), Tp1AnalogDecodeProfile.Historical).Records);
        Assert.Equal(Tp1RecordClassification.INVALID_PARITY, parity.Classification);
        Assert.False(parity.Characters[2].ParityValid);

        var stop = Assert.Single(Tp1DeterministicDecoder.Decode(
            Capture(83333, ValidFrame, corruptStopCharacter: 1), Tp1AnalogDecodeProfile.Historical).Records);
        Assert.Equal(Tp1RecordClassification.INVALID_TIMING, stop.Classification);
        Assert.False(stop.Characters[1].ReceivedStop);

        var invalidChecksum = ValidFrame.ToArray();
        invalidChecksum[^1] ^= 1;
        var checksum = Assert.Single(Tp1DeterministicDecoder.Decode(
            Capture(83333, invalidChecksum), Tp1AnalogDecodeProfile.Historical).Records);
        Assert.Equal(Tp1RecordClassification.INVALID_CHECKSUM, checksum.Classification);

        var timingCapture = Capture(83333, [0xCC]);
        var stream = new AnalogSampleStream(timingCapture.SampleRateHz, timingCapture.TriggerIndex, timingCapture.Samples);
        var profile = Tp1AnalogDecodeProfile.Historical;
        var pulses = Tp1AnalogPulseExtractor.Extract(timingCapture, profile).Append(Pulse(54, profile)).OrderBy(x => x.StartSample).ToArray();
        var timing = Assert.Single(Tp1DeterministicDecoder.Decode(stream, pulses, profile).Records);
        Assert.Equal(Tp1RecordClassification.INVALID_TIMING, timing.Classification);
        Assert.Contains(timing.Characters[0].Errors, error => error.Contains("timing"));
    }

    [Fact]
    public void DiagnosticFieldsExplainParityWithoutChangingDecodedByteOrClassification()
    {
        var result = Tp1DeterministicDecoder.Decode(
            Capture(83333, [0xFB], absoluteStart: 20_000, eventId: 8, corruptParityCharacter: 0),
            Tp1AnalogDecodeProfile.FieldCandidate);

        var record = Assert.Single(result.Records);
        var character = Assert.Single(record.Characters);
        Assert.Equal(Tp1RecordClassification.INVALID_PARITY, record.Classification);
        Assert.Equal(0xFB, character.Value);
        Assert.Equal("1 1 0 1 1 1 1 1", character.DataBitsLsb);
        Assert.Equal(7, character.DataOneCount);
        Assert.True(character.ExpectedParity);
        Assert.False(character.ReceivedParity);
        Assert.False(character.ParityValid);
        Assert.Equal(8u, character.EventId);
        Assert.Equal(0, character.RecordIndex);
        Assert.Equal(20_050UL, character.AbsoluteStartSample);
        Assert.Equal(11, character.Slots.Count);
        Assert.True(character.TimingMaximumSamples <= 0.5);
    }

    [Fact]
    public void DiagnosticFieldsReportUnassignedAndDuplicatePulsesWithoutChangingClassificationPriority()
    {
        var capture = Capture(83333, [0xCC]);
        var stream = new AnalogSampleStream(capture.SampleRateHz, capture.TriggerIndex, capture.Samples);
        var profile = Tp1AnalogDecodeProfile.Historical;
        var original = Tp1AnalogPulseExtractor.Extract(capture, profile).ToList();
        original.Add(Pulse(original[0].StartSample + 1, profile)); // Same START slot: diagnostic duplicate.
        original.Add(Pulse(original[0].StartSample + 4, profile)); // Outside the 3-sample tolerance.

        var record = Assert.Single(Tp1DeterministicDecoder.Decode(stream,
            original.OrderBy(x => x.StartSample).ToArray(), profile).Records);
        var character = Assert.Single(record.Characters);
        Assert.Equal("CC", record.RawHex);
        Assert.Equal(Tp1RecordClassification.INVALID_TIMING, record.Classification);
        Assert.Single(character.DuplicateSlotPulses);
        Assert.Single(character.UnassignedPulses);
    }

    [Fact]
    public void ConvertsHistoricalSampleConstantsToRateIndependentDurations()
    {
        var timing = Tp1TimingProfile.C6Validated;
        Assert.Equal(9600, timing.BitRate);
        Assert.InRange(timing.CharacterEndMicroseconds, 1151.9, 1152.1);
        Assert.InRange(timing.RecordGapMicroseconds, 1919.9, 1920.1);
        Assert.InRange(timing.TimingToleranceMicroseconds, 35.9, 36.1);
        Assert.InRange(timing.CharacterEndMicroseconds * 96000 / 1_000_000, 110.5, 110.7);
    }

    [Fact]
    public void MergesOnlyExactlyIdenticalOverlapRecordsAndKeepsProvenance()
    {
        var first = Tp1DeterministicDecoder.Decode(Capture(83333, [0xCC], 10_000, 5), Tp1AnalogDecodeProfile.Historical);
        var second = Tp1DeterministicDecoder.Decode(Capture(83333, [0xCC], 10_000, 6), Tp1AnalogDecodeProfile.Historical);
        var merged = Assert.Single(Tp1DeterministicDecoder.MergeExactOverlaps([first, second]));
        Assert.Equal<uint>([5, 6], merged.ProvenanceEventIds);
    }

    private static RawCapture Capture(uint rate, byte[] bytes, ulong absoluteStart = 0, uint eventId = 1,
        int corruptParityCharacter = -1, int corruptStopCharacter = -1)
    {
        var bitSamples = rate / 9600.0;
        const int lead = 50;
        var length = lead + (int)Math.Ceiling(bytes.Length * 13 * bitSamples) + 300;
        var samples = Enumerable.Repeat((ushort)2000, length).ToArray();
        for (var character = 0; character < bytes.Length; ++character) {
            var bits = new bool[11];
            bits[0] = false;
            var ones = 0;
            for (var bit = 0; bit < 8; ++bit) {
                bits[bit + 1] = (bytes[character] & (1 << bit)) != 0;
                if (bits[bit + 1]) ones++;
            }
            bits[9] = (ones & 1) != 0;
            bits[10] = true;
            if (character == corruptParityCharacter) bits[9] = !bits[9];
            if (character == corruptStopCharacter) bits[10] = false;
            var start = lead + (int)Math.Round(character * 13 * bitSamples);
            for (var bit = 0; bit < bits.Length; ++bit) if (!bits[bit]) {
                var pulse = start + (int)Math.Round(bit * bitSamples);
                for (var i = 0; i < 3; ++i) samples[pulse + i] = 1400;
            }
        }
        return new RawCapture { EventId = eventId, SampleRateHz = rate, SampleStart = absoluteStart,
            SampleEnd = absoluteStart + (ulong)samples.Length, Samples = samples, CrcValid = true };
    }

    private static Tp1Pulse Pulse(int start, Tp1AnalogDecodeProfile profile) =>
        new(0, start, start + 1, (ulong)start, (ulong)(start + 1), 12, 1400, profile);
}
