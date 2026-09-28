using System.IO.Compression;
using KNXAnalyzer.Core.Experimental;
using Xunit.Abstractions;

namespace KNXAnalyzer.Core.Tests;

public sealed class Tp1EvidenceDecoderV5Tests(ITestOutputHelper output)
{
    private sealed record Data(RawCapture Capture, Tp1DecodeResult Historical);

    [Fact]
    public void ReplaysParallelEvidenceDecoderBlindly()
    {
        WithFixture(folder => {
            var data = Load(folder); GuardHistorical(data);
            var model = Train(data, excludedEvent: null);
            var evidence = data.Select(x => Tp1EvidenceDecoder.Decode(x.Capture, model)).ToArray();
            Dump("GLOBAL", evidence);
            CompareCharacters(data, evidence); CompareRecords(data, evidence); KnownControls(data, evidence); KnownTelegrams(evidence);

            foreach (var eventId in new uint[] { 8, 9 }) {
                var looModel = Train(data, eventId);
                var global = evidence.Single(x => x.EventId == eventId);
                var loo = Tp1EvidenceDecoder.Decode(data.Single(x => x.Capture.EventId == eventId).Capture, looModel);
                output.WriteLine($"LOO E{eventId} train-pulses={TrainingPulses(data, eventId).Count()} global={Signature(global)} loo={Signature(loo)} " +
                    $"start-overlap={Overlap(global.Characters.Select(x => x.StartSample), loo.Characters.Select(x => x.StartSample), 5):F3}");
            }

            Robust("OFFSET-100", data, model, evidence, -100, 1, 0);
            Robust("OFFSET+100", data, model, evidence, 100, 1, 0);
            Robust("GAIN0.9", data, model, evidence, 0, .9, 0);
            Robust("GAIN1.1", data, model, evidence, 0, 1.1, 0);
            Robust("NOISE5", data, model, evidence, 0, 1, 5);
        });
    }

    private void Dump(string name, Tp1EvidenceDecodeResult[] result)
    {
        var pulses = result.SelectMany(x => x.Pulses).ToArray(); var chars = result.SelectMany(x => x.Characters).ToArray(); var records = result.SelectMany(x => x.Records).ToArray();
        output.WriteLine($"{name} pulses P/A={pulses.Count(x => x.Decision == Tp1EvidenceDecision.Pulse)}/{pulses.Count(x => x.Decision == Tp1EvidenceDecision.Ambiguous)} " +
            $"characters={chars.Length} parity-valid={chars.Count(x => x.ParityValid)} parity-invalid={chars.Count(x => !x.ParityValid)} " +
            $"stop-invalid={chars.Count(x => !x.StopValid)} ambiguous={chars.Count(x => !x.StrictPhysicalValid)}");
        output.WriteLine($"{name} records={records.Length} " + string.Join(' ', records.GroupBy(x => x.Classification).OrderBy(x => x.Key).Select(x => $"{x.Key}={x.Count()}")) +
            $" ACK={records.Count(x => x.KnownControl == Tp1KnownControl.ACK)} BUSY={records.Count(x => x.KnownControl == Tp1KnownControl.BUSY)} telegrams={records.Count(x => x.Telegram is not null)}");
    }

    private void CompareCharacters(Data[] historical, Tp1EvidenceDecodeResult[] evidence)
    {
        var hhEv = 0; var hiEv = 0; var haEv = 0; var ihEv = 0; var iiEv = 0; var iaEv = 0; var unmatched = 0;
        foreach (var data in historical) foreach (var h in data.Historical.Records.SelectMany(x => x.Characters)) {
            var e = evidence.Single(x => x.EventId == data.Capture.EventId).Characters.OrderBy(x => Math.Abs(x.StartSample - h.StartSample)).FirstOrDefault();
            if (e is null || Math.Abs(e.StartSample - h.StartSample) > 5) { unmatched++; continue; }
            var hv = h.ParityValid && h.ReceivedStop; var ea = !e.StrictPhysicalValid; var ev = e.ParityValid && e.StopValid && !ea;
            if (hv && ev) hhEv++; else if (hv && ea) haEv++; else if (hv) hiEv++;
            else if (ev) ihEv++; else if (ea) iaEv++; else iiEv++;
        }
        output.WriteLine($"CHAR_MATRIX historical-valid evidence-valid/invalid/ambiguous={hhEv}/{hiEv}/{haEv} " +
            $"historical-invalid evidence-valid/invalid/ambiguous={ihEv}/{iiEv}/{iaEv} unmatched={unmatched}");
    }

    private void CompareRecords(Data[] historical, Tp1EvidenceDecodeResult[] evidence)
    {
        int vv = 0, vi = 0, va = 0, iv = 0, ii = 0, ia = 0, unmatched = 0, strong = 0, weak = 0;
        foreach (var data in historical) foreach (var h in data.Historical.Records) {
            var e = evidence.Single(x => x.EventId == data.Capture.EventId).Records
                .OrderBy(x => Math.Abs(x.Characters[0].StartSample - h.StartSample)).FirstOrDefault();
            if (e is null || Math.Abs(e.Characters[0].StartSample - h.StartSample) > 5) { unmatched++; continue; }
            var hv = h.Classification is Tp1RecordClassification.VALID_KNOWN or Tp1RecordClassification.VALID_UNKNOWN;
            var ea = e.Classification == "AMBIGUOUS"; var ev = e.Classification is "VALID_KNOWN" or "VALID_UNKNOWN";
            if (hv && ev) vv++; else if (hv && ea) va++; else if (hv) vi++; else if (ev) { iv++; if (e.PhysicalConfidence >= Tp1PhysicalConfidence.Medium && (e.ChecksumValid != false)) strong++; else weak++; }
            else if (ea) ia++; else ii++;
        }
        output.WriteLine($"RECORD_MATRIX historical-valid evidence-valid/invalid/ambiguous={vv}/{vi}/{va} " +
            $"historical-invalid evidence-valid/invalid/ambiguous={iv}/{ii}/{ia} unmatched={unmatched} invalid-to-valid strong/weak={strong}/{weak}");
    }

    private void KnownControls(Data[] historical, Tp1EvidenceDecodeResult[] evidence)
    {
        var hAck = historical.SelectMany(x => x.Historical.Records.Select(r => (x.Capture.EventId, r))).Where(x => x.r.KnownControl == Tp1KnownControl.ACK).ToArray();
        var eAck = evidence.SelectMany(x => x.Records.Select(r => (x.EventId, r))).Where(x => x.r.KnownControl == Tp1KnownControl.ACK).ToArray();
        var common = hAck.Count(h => eAck.Any(e => e.EventId == h.EventId && Math.Abs(e.r.Characters[0].StartSample - h.r.StartSample) <= 5));
        output.WriteLine($"ACK historical={hAck.Length} evidence={eAck.Length} common={common} historical-only={hAck.Length - common} evidence-only={eAck.Length - common}");
    }

    private void KnownTelegrams(Tp1EvidenceDecodeResult[] evidence)
    {
        foreach (var hex in new[] { "BCFF160001E10080CA", "9CFF160001E10080EA" }) {
            var found = evidence.SelectMany(x => x.Records).Where(x => Convert.ToHexString(x.Bytes) == hex).ToArray();
            output.WriteLine($"KNOWN {hex} count={found.Length} confidence={string.Join(',', found.Select(x => x.PhysicalConfidence))} " +
                $"parity={found.All(x => x.Characters.All(c => c.ParityValid))} stop={found.All(x => x.Characters.All(c => c.StopValid))} checksum={found.All(x => x.ChecksumValid == true)}");
        }
    }

    private void Robust(string name, Data[] original, Tp1EvidenceModel model, Tp1EvidenceDecodeResult[] baseline, double offset, double gain, double sigma)
    {
        var random = new Random(0x51A7); var transformed = original.Select(x => Transform(x.Capture, offset, gain, sigma, random)).ToArray();
        var decoded = transformed.Select(x => Tp1EvidenceDecoder.Decode(x, model)).ToArray();
        var baseChars = baseline.SelectMany(x => x.Characters.Select(c => (x.EventId, c))).ToArray(); var nowChars = decoded.SelectMany(x => x.Characters.Select(c => (x.EventId, c))).ToArray();
        var overlap = baseChars.Count(x => nowChars.Any(y => y.EventId == x.EventId && Math.Abs(y.c.StartSample - x.c.StartSample) <= 5));
        output.WriteLine($"ROBUST {name} {Signature(decoded)} start-overlap={overlap}/{baseChars.Length} ({overlap * 100.0 / Math.Max(1, baseChars.Length):F2}%)");
    }

    private static string Signature(IEnumerable<Tp1EvidenceDecodeResult> result)
    {
        var x = result.ToArray(); return $"P={x.Sum(r => r.DefinitePulseCount)} A={x.Sum(r => r.AmbiguousPulseCount)} chars={x.Sum(r => r.Characters.Count)} " +
            $"charAmb={x.Sum(r => r.Characters.Count(c => !c.StrictPhysicalValid))} records={x.Sum(r => r.Records.Count)}";
    }
    private static string Signature(Tp1EvidenceDecodeResult x) => Signature([x]);
    private static double Overlap(IEnumerable<int> a, IEnumerable<int> b, int tolerance)
    {
        var right = b.ToArray(); var left = a.ToArray(); return left.Length == 0 ? 1 : left.Count(x => right.Any(y => Math.Abs(x - y) <= tolerance)) / (double)left.Length;
    }

    private static Tp1EvidenceModel Train(Data[] data, uint? excludedEvent) => Tp1EvidenceDecoder.TrainTemplate(TrainingPulses(data, excludedEvent));
    private static IEnumerable<(RawCapture Capture, Tp1Pulse Pulse)> TrainingPulses(Data[] data, uint? excludedEvent) => data
        .Where(x => x.Capture.EventId != excludedEvent)
        .SelectMany(x => x.Historical.Records.Where(r => r.Classification is Tp1RecordClassification.VALID_KNOWN or Tp1RecordClassification.VALID_UNKNOWN)
            .SelectMany(r => r.Characters).SelectMany(c => c.Slots).Where(s => s.SourcePulse is not null).Select(s => (x.Capture, s.SourcePulse!)));

    private static RawCapture Transform(RawCapture source, double offset, double gain, double sigma, Random random)
    {
        var baseline = source.Samples.Order().ElementAt((int)(source.Samples.Length * .75));
        var samples = source.Samples.Select(x => {
            var noise = sigma == 0 ? 0 : sigma * Math.Sqrt(-2 * Math.Log(Math.Max(1e-12, random.NextDouble()))) * Math.Cos(2 * Math.PI * random.NextDouble());
            return (ushort)Math.Clamp(Math.Round(baseline + gain * (x - baseline) + offset + noise), 0, ushort.MaxValue);
        }).ToArray();
        return new RawCapture { EventId = source.EventId, SampleRateHz = source.SampleRateHz, TriggerIndex = source.TriggerIndex,
            SampleStart = source.SampleStart, SampleEnd = source.SampleEnd, TriggerSample = source.TriggerSample, Samples = samples,
            Minimum = samples.Min(), Maximum = samples.Max(), Mean = samples.Average(x => (double)x), CrcValid = source.CrcValid, Integrity = source.Integrity };
    }

    private static Data[] Load(string folder)
    {
        var session = EventRawV2Reader.OpenSession(folder); return session.AnalogEvents.OrderBy(x => x.EventId).Select(x => {
            var capture = EventRawV2Reader.ReadCapture(folder, x.EventId); return new Data(capture, Tp1DeterministicDecoder.Decode(capture, Tp1AnalogDecodeProfile.FieldCandidate)); }).ToArray();
    }
    private static void GuardHistorical(Data[] data)
    {
        Assert.Equal(2577, data.Sum(x => x.Historical.Pulses.Count)); Assert.Equal(622, data.Sum(x => x.Historical.CharacterCount)); Assert.Equal(197, data.Sum(x => x.Historical.Records.Count));
        var e8 = data.Single(x => x.Capture.EventId == 8).Historical; Assert.Equal(990, e8.Pulses.Count); Assert.Equal(220, e8.CharacterCount); Assert.Equal(54, e8.Records.Count);
        Assert.Equal(19, e8.AckCount); Assert.Equal(7, e8.Records.Count(x => x.Telegram is not null)); Assert.Contains(e8.Records, x => x.RawHex == "BCFF160001E10080CA"); Assert.Contains(e8.Records, x => x.RawHex == "9CFF160001E10080EA");
    }
    private static void WithFixture(Action<string> action)
    {
        var archive = Path.Combine(AppContext.BaseDirectory, "Fixtures", "KNX-F35F14A4.zip"); var folder = Path.Combine(Path.GetTempPath(), "knx-evidence-v5-" + Guid.NewGuid().ToString("N"));
        try { ZipFile.ExtractToDirectory(archive, folder); action(folder); } finally { if (Directory.Exists(folder)) Directory.Delete(folder, true); }
    }
}
