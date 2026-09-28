using System.IO.Compression;
using KNXAnalyzer.Core.Experimental;
using Xunit.Abstractions;

namespace KNXAnalyzer.Core.Tests;

public sealed class Tp1FixedStartEvidenceDecoderV6Tests(ITestOutputHelper output)
{
    private sealed record Data(RawCapture Capture, Tp1DecodeResult Historical);

    [Fact]
    public void ReplaysFixedHistoricalStructureWithIndependentSlotEvidence()
    {
        WithFixture(folder => {
            var data = Load(folder); GuardHistorical(data);
            var model = Train(data, null); var decoded = Decode(data, model);
            Assert.Equal(622, decoded.Sum(x => x.Characters.Count));
            Assert.Equal(197, decoded.Sum(x => x.Records.Count));
            CompareSlots(data, decoded); CompareCharacters(decoded); CompareRecords(decoded);
            InvalidParity(decoded); KnownFrames(decoded); KnownControls(decoded); DetailedCases(decoded);

            Robust("OFFSET-100", data, model, decoded, -100, 1, 0);
            Robust("OFFSET+100", data, model, decoded, 100, 1, 0);
            Robust("GAIN0.9", data, model, decoded, 0, .9, 0);
            Robust("GAIN1.1", data, model, decoded, 0, 1.1, 0);
            Robust("NOISE5", data, model, decoded, 0, 1, 5);
            foreach (var eventId in new uint[] { 8, 9 }) LeaveOneOut(data, model, decoded, eventId);
        });
    }

    private void CompareSlots(Data[] source, Tp1FixedEvidenceDecodeResult[] decoded)
    {
        foreach (var group in new[] { ("START", new[] { 0 }), ("DATA", Enumerable.Range(1, 8).ToArray()), ("PARITY", new[] { 9 }), ("STOP", new[] { 10 }) }) {
            var pairs = decoded.SelectMany(x => x.Characters).SelectMany(x => group.Item2.Select(i => (H: x.Historical.Slots[i], V: x.Slots[i]))).ToArray();
            output.WriteLine($"SLOTS {group.Item1} H-PULSE->V P/A/N={C(pairs, true, Tp1EvidenceDecision.Pulse)}/{C(pairs, true, Tp1EvidenceDecision.Ambiguous)}/{C(pairs, true, Tp1EvidenceDecision.NoPulse)} " +
                $"H-NO->V N/A/P={C(pairs, false, Tp1EvidenceDecision.NoPulse)}/{C(pairs, false, Tp1EvidenceDecision.Ambiguous)}/{C(pairs, false, Tp1EvidenceDecision.Pulse)}");
        }
        static int C((Tp1BitSlot H, Tp1EvidenceMeasurement V)[] x, bool pulse, Tp1EvidenceDecision d) => x.Count(v => (v.H.SourcePulse is not null) == pulse && v.V.Decision == d);
    }

    private void CompareCharacters(Tp1FixedEvidenceDecodeResult[] decoded)
    {
        var chars = decoded.SelectMany(x => x.Characters).ToArray(); var hv = chars.Where(x => Valid(x.Historical)).ToArray(); var hi = chars.Where(x => !Valid(x.Historical)).ToArray();
        output.WriteLine($"CHAR historical-valid={hv.Length} V6 identical-valid/different-valid/ambiguous/invalid=" +
            $"{hv.Count(x => VValid(x) && !x.DataChanged)}/{hv.Count(x => VValid(x) && x.DataChanged)}/{hv.Count(x => !x.StrictPhysicalValid)}/{hv.Count(x => x.StrictPhysicalValid && !VValid(x))} " +
            $"strict-preserved={hv.Count(x => VValid(x) && !x.DataChanged) * 100.0 / hv.Length:F2}% best-byte-identical={hv.Count(x => !x.DataChanged)} ({hv.Count(x => !x.DataChanged) * 100.0 / hv.Length:F2}%) " +
            $"identical-ambiguous={hv.Count(x => !x.DataChanged && !x.StrictPhysicalValid)} different-ambiguous={hv.Count(x => x.DataChanged && !x.StrictPhysicalValid)} start-mismatch={hv.Count(x => x.StartEvidenceMismatch)}");
        output.WriteLine($"CHAR historical-invalid={hi.Length} V6 valid/ambiguous/invalid={hi.Count(VValid)}/{hi.Count(x => !x.StrictPhysicalValid)}/{hi.Count(x => x.StrictPhysicalValid && !VValid(x))} " +
            $"valid unchanged/data-changed/multi-data-changed={hi.Count(x => VValid(x) && !x.DataChanged)}/{hi.Count(x => VValid(x) && x.DataChanged)}/{hi.Count(x => VValid(x) && DataChanges(x) > 1)}");
    }

    private void InvalidParity(Tp1FixedEvidenceDecodeResult[] decoded)
    {
        var chars = decoded.SelectMany(x => x.Records.Where(r => r.Historical.Classification == Tp1RecordClassification.INVALID_PARITY).SelectMany(r => r.Characters.Where(c => !c.Historical.ParityValid))).ToArray();
        var repaired = chars.Count(x => x.StrictPhysicalValid && x.ParityValid && !x.DataChanged);
        var dataValid = chars.Count(x => x.StrictPhysicalValid && x.ParityValid && x.DataChanged);
        var dataInvalid = chars.Count(x => x.StrictPhysicalValid && !x.ParityValid && x.DataChanged);
        var ambiguous = chars.Count(x => !x.StrictPhysicalValid); var still = chars.Count(x => x.StrictPhysicalValid && !x.ParityValid && !x.DataChanged);
        var missed = chars.Where(x => x.Historical.Slots[9].SourcePulse is null && x.Slots[9].Decision == Tp1EvidenceDecision.Pulse).ToArray();
        output.WriteLine($"INVALID_PARITY characters={chars.Length} repaired-parity-only={repaired} still-invalid={still} ambiguous={ambiguous} data-changed-total={chars.Count(x => x.DataChanged)} data-changed-valid={dataValid} data-changed-invalid={dataInvalid} analog-missed-evidence={missed.Length} analog-missed-complete-valid={missed.Count(VValid)}");
    }

    private void CompareRecords(Tp1FixedEvidenceDecodeResult[] decoded)
    {
        var records = decoded.SelectMany(x => x.Records).ToArray();
        output.WriteLine("RECORDS " + string.Join(' ', records.GroupBy(x => x.StrictClassification).OrderBy(x => x.Key).Select(x => $"{x.Key}={x.Count()}")) +
            $" ACK={records.Count(x => x.KnownControl == Tp1KnownControl.ACK)} BUSY={records.Count(x => x.KnownControl == Tp1KnownControl.BUSY)} parser-success={records.Count(x => x.Telegram is not null)} boundaries={records.Length}/{records.Count(x => x.Historical is not null)}");
    }

    private void KnownFrames(Tp1FixedEvidenceDecodeResult[] decoded)
    {
        foreach (var hex in new[] { "BCFF160001E10080CA", "9CFF160001E10080EA" }) {
            var records = decoded.SelectMany(x => x.Records).Where(x => x.Historical.RawHex == hex).ToArray();
            foreach (var r in records) output.WriteLine($"KNOWN {hex} E{r.Historical.ProvenanceEventIds[0]}/R{r.Historical.RecordIndex} V6={Convert.ToHexString(r.BestEvidenceBytes)} class={r.StrictClassification} " +
                $"amb-slots={r.Characters.Sum(x => x.AmbiguousSlotCount)} min-confidence={r.Characters.SelectMany(x => x.Slots).Min(x => x.Confidence):F3} " +
                $"start={string.Join(',', r.Characters.Select(x => x.Slots[0].Decision))} parity={r.Characters.All(x => x.ParityValid)} stop={r.Characters.All(x => x.StopValid)} checksum={r.ChecksumValid}");
        }
    }

    private void KnownControls(Tp1FixedEvidenceDecodeResult[] decoded)
    {
        foreach (var control in new[] { Tp1KnownControl.ACK, Tp1KnownControl.BUSY }) {
            var records = decoded.SelectMany(x => x.Records).Where(x => x.Historical.KnownControl == control).ToArray();
            output.WriteLine($"CONTROL {control} historical={records.Length} identical={records.Count(x => x.BestEvidenceBytes.SequenceEqual(x.Historical.Bytes) && x.StrictClassification == "VALID_KNOWN")} " +
                $"ambiguous={records.Count(x => x.StrictClassification == "AMBIGUOUS")} modified={records.Count(x => !x.BestEvidenceBytes.SequenceEqual(x.Historical.Bytes))}");
        }
    }

    private void DetailedCases(Tp1FixedEvidenceDecodeResult[] decoded)
    {
        var all = decoded.SelectMany(x => x.Records.SelectMany(r => r.Characters.Select(c => (Event: x.EventId, Record: r, Character: c)))).ToArray();
        Detail("VALID_IDENTICAL", all.FirstOrDefault(x => Valid(x.Character.Historical) && VValid(x.Character) && !x.Character.DataChanged));
        foreach (var hex in new[] { "BCFF160001E10080CA", "9CFF160001E10080EA" }) {
            var r = decoded.SelectMany(x => x.Records.Select(r => (x.EventId, R: r))).First(x => x.R.Historical.RawHex == hex);
            Detail(hex[..2], (r.EventId, r.R, r.R.Characters[0]));
        }
        Detail("ACK", all.FirstOrDefault(x => x.Record.Historical.KnownControl == Tp1KnownControl.ACK && VValid(x.Character)));
        Detail("ANALOG_PULSE_MISSED", all.FirstOrDefault(x => !x.Character.Historical.ParityValid && x.Character.Historical.Slots[9].SourcePulse is null && x.Character.Slots[9].Decision == Tp1EvidenceDecision.Pulse));
        Detail("INVALID_TO_VALID", all.FirstOrDefault(x => !Valid(x.Character.Historical) && VValid(x.Character)));
        Detail("VALID_TO_AMBIGUOUS", all.FirstOrDefault(x => Valid(x.Character.Historical) && !x.Character.StrictPhysicalValid));
        Detail("VALID_TO_DIFFERENT", all.FirstOrDefault(x => Valid(x.Character.Historical) && x.Character.DataChanged));
        Detail("PHYSICALLY_CORRUPTED", all.FirstOrDefault(x => !Valid(x.Character.Historical) && x.Character.StrictPhysicalValid && !VValid(x.Character)));
        Detail("NO_PHYSICAL_EVIDENCE", all.FirstOrDefault(x => !Valid(x.Character.Historical) && x.Character.Slots.Count(s => s.Decision == Tp1EvidenceDecision.NoPulse) >= 9));
    }

    private void Detail(string name, (uint Event, Tp1FixedEvidenceRecord Record, Tp1FixedEvidenceCharacter Character) x)
    {
        if (x.Character is null) { output.WriteLine($"CASE {name} none"); return; }
        output.WriteLine($"CASE {name} E{x.Event}/R{x.Record.Historical.RecordIndex}/C{x.Character.Historical.Index} H={x.Character.Historical.Value:X2} V={x.Character.BestEvidenceValue:X2} " +
            $"strict={x.Character.StrictPhysicalValid} parity={x.Character.ParityValid} stop={x.Character.StopValid} startMismatch={x.Character.StartEvidenceMismatch} slots=" +
            string.Join(' ', x.Character.Slots.Select(s => $"{x.Character.Historical.Slots[s.Index].Name}:{Short(s.Decision)}:{s.Score:F3}")));
    }

    private void Robust(string name, Data[] source, Tp1EvidenceModel model, Tp1FixedEvidenceDecodeResult[] baseline, double offset, double gain, double sigma)
    {
        var random = new Random(0x6606); var changedSlots = 0; var totalSlots = 0; var sameStrict = 0; var sameBest = 0; var sameRecords = 0; var totalChars = 0; var totalRecords = 0;
        foreach (var data in source) {
            var capture = Transform(data.Capture, offset, gain, sigma, random); var reference = PulseReference(capture, TrainingPulses(data.Historical));
            var now = Tp1FixedStartEvidenceDecoder.Decode(capture, data.Historical, model, reference); var before = baseline.Single(x => x.EventId == data.Capture.EventId);
            foreach (var pair in before.Characters.Zip(now.Characters)) { totalChars++; if (pair.First.StrictPhysicalValid == pair.Second.StrictPhysicalValid) sameStrict++; if (pair.First.BestEvidenceValue == pair.Second.BestEvidenceValue) sameBest++; foreach (var s in pair.First.Slots.Zip(pair.Second.Slots)) { totalSlots++; if (s.First.Decision != s.Second.Decision) changedSlots++; } }
            foreach (var pair in before.Records.Zip(now.Records)) { totalRecords++; if (pair.First.StrictClassification == pair.Second.StrictClassification && pair.First.BestEvidenceBytes.SequenceEqual(pair.Second.BestEvidenceBytes)) sameRecords++; }
        }
        output.WriteLine($"ROBUST {name} slot={100.0 * (totalSlots - changedSlots) / totalSlots:F2}% strict={100.0 * sameStrict / totalChars:F2}% best-bytes={100.0 * sameBest / totalChars:F2}% records={100.0 * sameRecords / totalRecords:F2}%");
    }

    private void LeaveOneOut(Data[] source, Tp1EvidenceModel globalModel, Tp1FixedEvidenceDecodeResult[] global, uint eventId)
    {
        var data = source.Single(x => x.Capture.EventId == eventId); var loo = Tp1FixedStartEvidenceDecoder.Decode(data.Capture, data.Historical, Train(source, eventId), PulseReference(data.Capture, TrainingPulses(data.Historical)));
        var g = global.Single(x => x.EventId == eventId); var slots = g.Characters.SelectMany(x => x.Slots).Zip(loo.Characters.SelectMany(x => x.Slots)).ToArray(); var chars = g.Characters.Zip(loo.Characters).ToArray();
        output.WriteLine($"LOO E{eventId} slot-agreement={100.0 * slots.Count(x => x.First.Decision == x.Second.Decision) / slots.Length:F2}% char-strict={100.0 * chars.Count(x => x.First.StrictPhysicalValid == x.Second.StrictPhysicalValid) / chars.Length:F2}% " +
            $"byte-agreement={100.0 * chars.Count(x => x.First.BestEvidenceValue == x.Second.BestEvidenceValue) / chars.Length:F2}% records-global/loo={g.Records.Count}/{loo.Records.Count}");
    }

    private static Tp1FixedEvidenceDecodeResult[] Decode(Data[] source, Tp1EvidenceModel model) => source.Select(x => Tp1FixedStartEvidenceDecoder.Decode(x.Capture, x.Historical, model, PulseReference(x.Capture, TrainingPulses(x.Historical)))).ToArray();
    private static Tp1EvidenceModel Train(Data[] source, uint? excluded) => Tp1EvidenceDecoder.TrainTemplate(source.Where(x => x.Capture.EventId != excluded).SelectMany(x => TrainingPulses(x.Historical).Select(p => (x.Capture, p))));
    private static IEnumerable<Tp1Pulse> TrainingPulses(Tp1DecodeResult result) => result.Records.Where(r => r.Classification is Tp1RecordClassification.VALID_KNOWN or Tp1RecordClassification.VALID_UNKNOWN).SelectMany(r => r.Characters).SelectMany(c => c.Slots).Where(s => s.SourcePulse is not null).Select(s => s.SourcePulse!);
    private static double PulseReference(RawCapture capture, IEnumerable<Tp1Pulse> pulses) { var x = pulses.Select(p => RawDepth(capture, p.StartSample)).Order().ToArray(); return x.Length == 0 ? 1 : Q(x, .5); }
    private static double RawDepth(RawCapture capture, int anchor) { var from = Math.Max(0, anchor - 20); var to = Math.Min(capture.Samples.Length, anchor + 21); var baseline = Q(capture.Samples.Skip(from).Take(to - from).Select(x => (double)x), .75); var lo = Math.Max(0, anchor - 2); var hi = Math.Min(capture.Samples.Length, anchor + 10); return Math.Max(1, baseline - Enumerable.Range(lo, hi - lo).Min(i => capture.Samples[i])); }
    private static double Q(IEnumerable<double> source, double p) { var x = source.Order().ToArray(); var at = p * (x.Length - 1); var lo = (int)Math.Floor(at); var hi = (int)Math.Ceiling(at); return x[lo] + (x[hi] - x[lo]) * (at - lo); }
    private static bool Valid(Tp1Character x) => x.ParityValid && x.ReceivedStop;
    private static bool VValid(Tp1FixedEvidenceCharacter x) => x.StrictPhysicalValid && x.ParityValid && x.StopValid;
    private static int DataChanges(Tp1FixedEvidenceCharacter x) => Enumerable.Range(1, 8).Count(i => (x.Historical.Slots[i].SourcePulse is not null) != (x.Slots[i].Score >= .5));
    private static char Short(Tp1EvidenceDecision x) => x switch { Tp1EvidenceDecision.Pulse => 'P', Tp1EvidenceDecision.NoPulse => 'N', _ => 'A' };

    private static RawCapture Transform(RawCapture source, double offset, double gain, double sigma, Random random) { var baseline = source.Samples.Order().ElementAt((int)(source.Samples.Length * .75)); var samples = source.Samples.Select(x => { var noise = sigma == 0 ? 0 : sigma * Math.Sqrt(-2 * Math.Log(Math.Max(1e-12, random.NextDouble()))) * Math.Cos(2 * Math.PI * random.NextDouble()); return (ushort)Math.Clamp(Math.Round(baseline + gain * (x - baseline) + offset + noise), 0, ushort.MaxValue); }).ToArray(); return new RawCapture { EventId = source.EventId, SampleRateHz = source.SampleRateHz, TriggerIndex = source.TriggerIndex, SampleStart = source.SampleStart, SampleEnd = source.SampleEnd, TriggerSample = source.TriggerSample, Samples = samples, Minimum = samples.Min(), Maximum = samples.Max(), Mean = samples.Average(x => (double)x), CrcValid = source.CrcValid, Integrity = source.Integrity }; }
    private static Data[] Load(string folder) { var session = EventRawV2Reader.OpenSession(folder); return session.AnalogEvents.OrderBy(x => x.EventId).Select(x => { var capture = EventRawV2Reader.ReadCapture(folder, x.EventId); return new Data(capture, Tp1DeterministicDecoder.Decode(capture, Tp1AnalogDecodeProfile.FieldCandidate)); }).ToArray(); }
    private static void GuardHistorical(Data[] data) { Assert.Equal(2577, data.Sum(x => x.Historical.Pulses.Count)); Assert.Equal(622, data.Sum(x => x.Historical.CharacterCount)); Assert.Equal(197, data.Sum(x => x.Historical.Records.Count)); var e8 = data.Single(x => x.Capture.EventId == 8).Historical; Assert.Equal(990, e8.Pulses.Count); Assert.Equal(220, e8.CharacterCount); Assert.Equal(54, e8.Records.Count); Assert.Equal(19, e8.AckCount); Assert.Equal(7, e8.Records.Count(x => x.Telegram is not null)); Assert.Contains(e8.Records, x => x.RawHex == "BCFF160001E10080CA"); Assert.Contains(e8.Records, x => x.RawHex == "9CFF160001E10080EA"); }
    private static void WithFixture(Action<string> action) { var archive = Path.Combine(AppContext.BaseDirectory, "Fixtures", "KNX-F35F14A4.zip"); var folder = Path.Combine(Path.GetTempPath(), "knx-evidence-v6-" + Guid.NewGuid().ToString("N")); try { ZipFile.ExtractToDirectory(archive, folder); action(folder); } finally { if (Directory.Exists(folder)) Directory.Delete(folder, true); } }
}
