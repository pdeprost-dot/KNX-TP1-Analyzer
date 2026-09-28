using System.IO.Compression;
using KNXAnalyzer.Core;
using Xunit.Abstractions;

namespace KNXAnalyzer.Core.Tests;

public sealed class PhysicalDecoderInvestigationV2Tests(ITestOutputHelper output)
{
    private sealed record EventData(RawCapture Capture, Tp1DecodeResult Decode);
    private sealed record Shape(double Baseline, double Minimum, double Depth, double RelativeDepth,
        double Width, double Width25, double Width50, double Width75, double Fall, double Rise,
        double Area, double TemplateScore, int Sample);

    [Fact]
    public void MeasuresPhysicalDecoderCorpusWithoutChangingProductionDecoding()
    {
        WithFixture(folder => {
            var session = EventRawV2Reader.OpenSession(folder);
            var events = session.AnalogEvents.OrderBy(x => x.EventId).Select(item => {
                var capture = EventRawV2Reader.ReadCapture(folder, item.EventId);
                return new EventData(capture, Tp1DeterministicDecoder.Decode(capture, Tp1AnalogDecodeProfile.FieldCandidate));
            }).ToArray();
            var records = events.SelectMany(x => x.Decode.Records).ToArray();

            // Immutable baseline guard for every experimental run.
            Assert.Equal(2577, events.Sum(x => x.Decode.Pulses.Count));
            Assert.Equal(622, events.Sum(x => x.Decode.CharacterCount));
            Assert.Equal(197, records.Length);

            var certainPulseRefs = records
                .Where(x => x.Classification is Tp1RecordClassification.VALID_KNOWN or Tp1RecordClassification.VALID_UNKNOWN)
                .SelectMany(x => x.Characters).SelectMany(x => x.Slots)
                .Where(x => x.SourcePulse is not null).Select(x => x.SourcePulse!).ToArray();
            var templateVectors = certainPulseRefs.Select(p => Vector(Event(events, p.EventId).Capture, p.StartSample)).ToArray();
            var template = Enumerable.Range(0, 16).Select(i => Median(templateVectors.Select(x => x[i]))).ToArray();

            Shape PulseShape(Tp1Pulse pulse)
            {
                var capture = Event(events, pulse.EventId).Capture;
                return ShapeAt(capture, pulse.StartSample, pulse.StartSample, pulse.EndSample, template);
            }

            var telegramPulses = records.Where(x => x.Classification == Tp1RecordClassification.VALID_UNKNOWN)
                .SelectMany(x => x.Characters).SelectMany(x => x.Slots).Where(x => x.SourcePulse is not null)
                .Select(x => PulseShape(x.SourcePulse!)).ToArray();
            var controlPulses = records.Where(x => x.Classification == Tp1RecordClassification.VALID_KNOWN)
                .SelectMany(x => x.Characters).SelectMany(x => x.Slots).Where(x => x.SourcePulse is not null)
                .Select(x => PulseShape(x.SourcePulse!)).ToArray();
            var invalidPulses = records.Where(x => x.Classification == Tp1RecordClassification.INVALID_PARITY)
                .SelectMany(x => x.Characters).SelectMany(x => x.Slots).Where(x => x.SourcePulse is not null)
                .Select(x => PulseShape(x.SourcePulse!)).ToArray();

            var trueAbsences = new List<Shape>();
            var parityMissing = new List<Shape>();
            var validAbsentByCharacter = new List<Shape[]>();
            var invalidAbsentByCharacter = new List<Shape[]>();
            var unexpectedParityShapes = new List<Shape>();
            var missingParityBelowLow = 0;
            var missingParityWithinDetectedPulse = 0;
            var invalidCharacters = records.Where(x => x.Classification == Tp1RecordClassification.INVALID_PARITY)
                .SelectMany(x => x.Characters).Where(x => !x.ParityValid).ToArray();
            foreach (var data in events) {
                foreach (var character in data.Decode.Records
                             .Where(x => x.Classification is Tp1RecordClassification.VALID_KNOWN or Tp1RecordClassification.VALID_UNKNOWN)
                             .SelectMany(x => x.Characters)) {
                    var absent = character.Slots.Skip(1).Take(9).Where(x => x.SourcePulse is null)
                        .Select(slot => Candidate(data.Capture, slot.ExpectedSample, template)).ToArray();
                    trueAbsences.AddRange(absent); validAbsentByCharacter.Add(absent);
                }
                foreach (var character in data.Decode.Records.Where(x => x.Classification == Tp1RecordClassification.INVALID_PARITY)
                             .SelectMany(x => x.Characters).Where(x => !x.ParityValid)) {
                    invalidAbsentByCharacter.Add(character.Slots.Skip(1).Take(9).Where(x => x.SourcePulse is null)
                        .Select(slot => Candidate(data.Capture, slot.ExpectedSample, template)).ToArray());
                    if (character.Slots[9].SourcePulse is null && !character.ExpectedParity) {
                        var candidate = Candidate(data.Capture, character.Slots[9].ExpectedSample, template);
                        parityMissing.Add(candidate);
                        if (candidate.Minimum < Tp1AnalogDecodeProfile.FieldCandidate.LowThreshold) missingParityBelowLow++;
                        if (data.Decode.Pulses.Any(p => character.Slots[9].ExpectedSample >= p.StartSample &&
                            character.Slots[9].ExpectedSample <= p.EndSample)) missingParityWithinDetectedPulse++;
                    }
                    if (character.Slots[9].SourcePulse is { } unexpected && character.ExpectedParity)
                        unexpectedParityShapes.Add(PulseShape(unexpected));
                }
            }

            var missingParityPulse = invalidCharacters.Count(x => x.Slots[9].SourcePulse is null && !x.ExpectedParity);
            var unexpectedParityPulse = invalidCharacters.Count(x => x.Slots[9].SourcePulse is not null && x.ExpectedParity);
            var badStop = invalidCharacters.Count(x => !x.ReceivedStop);
            var withUnassigned = invalidCharacters.Count(x => x.UnassignedPulses.Count > 0);
            var withDuplicates = invalidCharacters.Count(x => x.DuplicateSlotPulses.Count > 0);
            var weakThreshold = Quantile(trueAbsences.Select(x => x.Depth), .99);
            var certainTemplateFloor = Quantile(telegramPulses.Concat(controlPulses).Select(x => x.TemplateScore), .10);
            var weakCompatible = parityMissing.Count(x => x.Depth > weakThreshold && x.TemplateScore >= certainTemplateFloor);
            var certainDepthFloor = Quantile(telegramPulses.Concat(controlPulses).Select(x => x.RelativeDepth), .10);
            bool PulseLike(Shape x) => x.RelativeDepth >= certainDepthFloor && x.TemplateScore >= certainTemplateFloor;
            var pulseLikeMissing = parityMissing.Count(PulseLike);
            var pulseLikeAbsence = trueAbsences.Count(PulseLike);
            var invalidCharactersWithPulseLikeAbsent = invalidAbsentByCharacter.Count(x => x.Any(PulseLike));
            var validCharactersWithPulseLikeAbsent = validAbsentByCharacter.Count(x => x.Any(PulseLike));

            var validCharacters = records
                .Where(x => x.Classification is Tp1RecordClassification.VALID_KNOWN or Tp1RecordClassification.VALID_UNKNOWN)
                .SelectMany(x => x.Characters).ToArray();
            var invalidAssignedByCharacter = invalidCharacters.Select(character => character.Slots
                .Where(x => x.SourcePulse is not null).Select(x => PulseShape(x.SourcePulse!)).ToArray()).ToArray();
            var physicallyWeakInvalidCharacters = invalidAssignedByCharacter.Count(shapes => shapes.Length > 0 &&
                shapes.Count(x => x.RelativeDepth < certainDepthFloor || x.TemplateScore < certainTemplateFloor) >= Math.Max(1, shapes.Length / 2));
            var parityUnassignedNearby = invalidCharacters.Count(character => character.UnassignedPulses.Any(p =>
                Math.Abs(p.StartSample - character.Slots[9].ExpectedSample) <= 9));
            var boundaryParityCharacters = records.Where(x => x.Classification == Tp1RecordClassification.INVALID_PARITY)
                .Sum(record => record.Characters.Select((character, index) => (character, index))
                    .Count(x => !x.character.ParityValid && (x.index == 0 || x.index == record.Characters.Count - 1)));

            var startAlternatives = 0;
            var startStrictImprovements = 0;
            var interiorStartAlternatives = 0;
            var interiorStartImprovements = 0;
            var improvedStartOffsets = new List<int>();
            foreach (var data in events) {
                var pulses = data.Decode.Pulses;
                foreach (var record in data.Decode.Records.Where(x => x.Classification == Tp1RecordClassification.INVALID_PARITY)) {
                    for (var index = 0; index < record.Characters.Count; index++) {
                        var character = record.Characters[index]; if (character.ParityValid) continue;
                        var current = StartScore(character);
                        var lower = index == 0 ? character.StartSample - 18 : record.Characters[index - 1].StartSample + 90;
                        var upper = index == record.Characters.Count - 1 ? character.StartSample + 18 : record.Characters[index + 1].StartSample - 90;
                        var alternatives = pulses.Where(x => x.StartSample != character.StartSample &&
                            x.StartSample >= lower && x.StartSample <= upper && Math.Abs(x.StartSample - character.StartSample) <= 18)
                            .Select(x => (Offset: x.StartSample - character.StartSample,
                                Evaluation: HypotheticalCharacter(pulses, x.StartSample, data.Capture.SampleRateHz))).ToArray();
                        if (alternatives.Length == 0) continue;
                        startAlternatives++;
                        var best = alternatives.Where(x => x.Evaluation.Valid && x.Evaluation.Cost + .25 < current)
                            .OrderBy(x => x.Evaluation.Cost).FirstOrDefault();
                        var improved = best != default;
                        if (improved) startStrictImprovements++;
                        if (improved) improvedStartOffsets.Add(best.Offset);
                        if (index > 0 && index < record.Characters.Count - 1) {
                            interiorStartAlternatives++;
                            if (improved) interiorStartImprovements++;
                        }
                    }
                }
            }

            output.WriteLine("BASELINE " + Summary(records, events.Sum(x => x.Decode.Pulses.Count), events.Sum(x => x.Decode.CharacterCount)));
            output.WriteLine($"INVALID_PARITY records={records.Count(x => x.Classification == Tp1RecordClassification.INVALID_PARITY)} characters={invalidCharacters.Length} " +
                $"missing-parity-pulse={missingParityPulse} unexpected-parity-pulse={unexpectedParityPulse} bad-stop={badStop} unassigned={withUnassigned} duplicate={withDuplicates}");
            Dump("VALID_TELEGRAM_PULSES", telegramPulses);
            Dump("VALID_CONTROL_PULSES", controlPulses);
            Dump("INVALID_ASSIGNED_PULSES", invalidPulses);
            Dump("VALID_ABSENT_SLOTS", trueAbsences.ToArray());
            Dump("INVALID_MISSING_PARITY_CANDIDATES", parityMissing.ToArray());
            Dump("INVALID_UNEXPECTED_PARITY_PULSES", unexpectedParityShapes.ToArray());
            output.WriteLine($"WEAK_SIGNATURE depth-q99-absence={weakThreshold:F1} template-q10-certain={certainTemplateFloor:F3} compatible={weakCompatible}/{parityMissing.Count}");
            output.WriteLine($"MISSING_PARITY_ANALOG below-LOW={missingParityBelowLow}/{parityMissing.Count} " +
                $"inside-detected-low-interval={missingParityWithinDetectedPulse}/{parityMissing.Count}");
            output.WriteLine($"PULSE_LIKE relDepth-q10-certain={certainDepthFloor:F3} missing={pulseLikeMissing}/{parityMissing.Count} " +
                $"valid-absence-false-positive={pulseLikeAbsence}/{trueAbsences.Count} invalid-chars-any={invalidCharactersWithPulseLikeAbsent}/{invalidAbsentByCharacter.Count} " +
                $"valid-chars-any-control={validCharactersWithPulseLikeAbsent}/{validAbsentByCharacter.Count}");
            output.WriteLine($"TEMPLATE_AUC certain-vs-absence={Auc(telegramPulses.Concat(controlPulses).Select(x => x.TemplateScore), trueAbsences.Select(x => x.TemplateScore)):F3} " +
                $"depth-relative-auc={Auc(telegramPulses.Concat(controlPulses).Select(x => x.RelativeDepth), trueAbsences.Select(x => x.RelativeDepth)):F3} " +
                $"certain-vs-invalid-template={Auc(telegramPulses.Concat(controlPulses).Select(x => x.TemplateScore), invalidPulses.Select(x => x.TemplateScore)):F3} " +
                $"certain-vs-invalid-relDepth={Auc(telegramPulses.Concat(controlPulses).Select(x => x.RelativeDepth), invalidPulses.Select(x => x.RelativeDepth)):F3}");
            output.WriteLine($"CHAR_TIMING valid-rms={QD(validCharacters.Select(x => x.TimingRmsSamples))} invalid-rms={QD(invalidCharacters.Select(x => x.TimingRmsSamples))} " +
                $"valid-max={QD(validCharacters.Select(x => x.TimingMaximumSamples))} invalid-max={QD(invalidCharacters.Select(x => x.TimingMaximumSamples))}");
            output.WriteLine($"CAUSE_FLAGS weak-shape={physicallyWeakInvalidCharacters}/{invalidCharacters.Length} near-parity-unassigned={parityUnassignedNearby} " +
                $"boundary={boundaryParityCharacters} bad-stop={badStop}");
            output.WriteLine($"START boundary-constrained-alternatives={startAlternatives} strict-valid-improvements={startStrictImprovements}");
            output.WriteLine($"START_INTERIOR alternatives={interiorStartAlternatives} strict-valid-improvements={interiorStartImprovements}");
            output.WriteLine($"START_IMPROVEMENT_OFFSETS samples={QD(improvedStartOffsets.Select(x => (double)x))} " +
                $"bit-like={improvedStartOffsets.Count(x => Math.Abs(Math.Abs(x) - events[0].Capture.SampleRateHz / 9600.0) < 2)}");
            var timingInvalid = records.Where(x => x.Classification == Tp1RecordClassification.INVALID_TIMING).ToArray();
            output.WriteLine($"INVALID_TIMING records={timingInvalid.Length} chars={timingInvalid.Sum(x => x.Characters.Count)} " +
                $"unassigned={timingInvalid.Sum(x => x.Characters.Sum(c => c.UnassignedPulses.Count))} bad-stop={timingInvalid.Sum(x => x.Characters.Count(c => !c.ReceivedStop))}");

            foreach (var low in new ushort[] { 1650, 1700, 1715, 1750, 1800 }) {
                var profile = new Tp1AnalogDecodeProfile($"study-{low}", "counterfactual", low, 1815, false);
                var decodes = events.Select(x => Tp1DeterministicDecoder.Decode(x.Capture, profile)).ToArray();
                var trial = decodes.SelectMany(x => x.Records).ToArray();
                output.WriteLine($"LOW={low} " + Summary(trial, decodes.Sum(x => x.Pulses.Count), decodes.Sum(x => x.CharacterCount)));
            }
            foreach (var rate in new uint[] { 83315, 83330, 83333 }) {
                var decodes = events.Select(x => Tp1DeterministicDecoder.Decode(
                    new AnalogSampleStream(rate, x.Capture.TriggerIndex, x.Capture.Samples), x.Decode.Pulses,
                    Tp1AnalogDecodeProfile.FieldCandidate, eventId: x.Capture.EventId, absoluteSampleStart: x.Capture.SampleStart)).ToArray();
                var trial = decodes.SelectMany(x => x.Records).ToArray();
                output.WriteLine($"RATE={rate} " + Summary(trial, decodes.Sum(x => x.Pulses.Count), decodes.Sum(x => x.CharacterCount)));
            }
        });
    }

    private void Dump(string name, Shape[] values)
    {
        output.WriteLine($"{name} n={values.Length} " +
            $"depth={Q(values, x => x.Depth)} relDepth={Q(values, x => x.RelativeDepth)} width={Q(values, x => x.Width)} " +
            $"w25={Q(values, x => x.Width25)} w50={Q(values, x => x.Width50)} w75={Q(values, x => x.Width75)} " +
            $"fall={Q(values, x => x.Fall)} rise={Q(values, x => x.Rise)} area={Q(values, x => x.Area)} template={Q(values, x => x.TemplateScore)}");
    }

    private static string Summary(Tp1DecodedRecord[] records, int pulses, int characters) =>
        $"pulses={pulses} chars={characters} records={records.Length} " +
        string.Join(' ', Enum.GetValues<Tp1RecordClassification>().Select(c => $"{c}={records.Count(x => x.Classification == c)}"));

    private static EventData Event(EventData[] events, uint id) => events.Single(x => x.Capture.EventId == id);

    private static Shape ShapeAt(RawCapture capture, int anchor, int start, int end, double[] template)
    {
        var samples = capture.Samples;
        start = Math.Clamp(start, 0, samples.Length - 1); end = Math.Clamp(end, start + 1, samples.Length);
        var local = Enumerable.Range(Math.Max(0, anchor - 16), Math.Min(samples.Length, anchor + 20) - Math.Max(0, anchor - 16))
            .Select(i => (double)samples[i]).Order().ToArray();
        var baseline = Quantile(local, .75);
        var minimum = samples.Skip(start).Take(end - start).Min(x => (double)x);
        var depth = Math.Max(1, baseline - minimum);
        var minIndex = Enumerable.Range(Math.Max(0, anchor - 3), Math.Min(samples.Length, anchor + 10) - Math.Max(0, anchor - 3))
            .MinBy(i => samples[i]);
        double fall = 0, rise = 0, area = 0;
        for (var i = Math.Max(1, anchor - 4); i <= Math.Min(samples.Length - 2, anchor + 12); i++) {
            fall = Math.Max(fall, samples[i - 1] - (double)samples[i]);
            rise = Math.Max(rise, samples[i + 1] - (double)samples[i]);
            area += Math.Max(0, baseline - samples[i]);
        }
        double Width(double fraction) => Enumerable.Range(Math.Max(0, anchor - 3), Math.Min(samples.Length, anchor + 13) - Math.Max(0, anchor - 3))
            .Count(i => samples[i] <= baseline - depth * fraction);
        return new(baseline, minimum, depth, depth / Math.Max(1, baseline), end - start,
            Width(.25), Width(.50), Width(.75), fall, rise, area, BestTemplateScore(capture, anchor, template), minIndex);
    }

    private static Shape Candidate(RawCapture capture, double expected, double[] template)
    {
        var center = (int)Math.Round(expected);
        var from = Math.Max(0, center - 3); var to = Math.Min(capture.Samples.Length - 1, center + 3);
        var minimum = Enumerable.Range(from, to - from + 1).MinBy(i => capture.Samples[i]);
        return ShapeAt(capture, minimum, Math.Max(0, minimum - 1), Math.Min(capture.Samples.Length, minimum + 2), template);
    }

    private static double[] Vector(RawCapture capture, int start)
    {
        var samples = capture.Samples;
        var from = Math.Max(0, start - 16); var to = Math.Min(samples.Length, start + 20);
        var baseline = Quantile(samples.Skip(from).Take(to - from).Select(x => (double)x), .75);
        var min = Enumerable.Range(Math.Max(0, start - 2), Math.Min(samples.Length, start + 10) - Math.Max(0, start - 2)).Min(i => (double)samples[i]);
        var depth = Math.Max(1, baseline - min);
        return Enumerable.Range(-3, 16).Select(offset => {
            var i = Math.Clamp(start + offset, 0, samples.Length - 1);
            return (baseline - samples[i]) / depth;
        }).ToArray();
    }

    private static double BestTemplateScore(RawCapture capture, int expected, double[] template) =>
        Enumerable.Range(-3, 7).Max(shift => Cosine(Vector(capture, expected + shift), template));

    private static double Cosine(double[] a, double[] b)
    {
        double dot = 0, aa = 0, bb = 0;
        for (var i = 0; i < a.Length; i++) { dot += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i]; }
        return aa == 0 || bb == 0 ? 0 : dot / Math.Sqrt(aa * bb);
    }

    private static double StartScore(Tp1Character character) =>
        character.TimingRmsSamples + (character.ParityValid ? 0 : 10) + (character.ReceivedStop ? 0 : 10);

    private static (bool Valid, double Cost) HypotheticalCharacter(IReadOnlyList<Tp1Pulse> pulses, int start, uint rate)
    {
        var bitSamples = rate / 9600.0; var slots = new Tp1Pulse?[11]; slots[0] = pulses.First(x => x.StartSample == start);
        foreach (var pulse in pulses.Where(x => x.StartSample > start && x.StartSample <= start + 10.5 * bitSamples)) {
            var slot = (int)Math.Floor((pulse.StartSample - start) / bitSamples + .5);
            if (slot is < 1 or > 10) continue;
            var error = Math.Abs(pulse.StartSample - (start + slot * bitSamples));
            if (error <= 3 && (slots[slot] is null || error < Math.Abs(slots[slot]!.StartSample - (start + slot * bitSamples)))) slots[slot] = pulse;
        }
        byte value = 0; for (var bit = 0; bit < 8; bit++) if (slots[bit + 1] is null) value |= (byte)(1 << bit);
        var expectedParity = (System.Numerics.BitOperations.PopCount(value) & 1) != 0;
        var parity = slots[9] is null; var stop = slots[10] is null;
        var errors = slots.Select((p, i) => p is null ? (double?)null : p.StartSample - (start + i * bitSamples)).Where(x => x.HasValue).Select(x => x!.Value).ToArray();
        var rms = Math.Sqrt(errors.Select(x => x * x).DefaultIfEmpty(0).Average());
        return (parity == expectedParity && stop, rms + (parity == expectedParity ? 0 : 10) + (stop ? 0 : 10));
    }

    private static string Q(IEnumerable<Shape> values, Func<Shape, double> selector)
    {
        var data = values.Select(selector).Order().ToArray();
        return data.Length == 0 ? "—" : $"{Quantile(data, .10):F3}/{Quantile(data, .50):F3}/{Quantile(data, .90):F3}";
    }

    private static string QD(IEnumerable<double> source)
    {
        var data = source.Order().ToArray();
        return data.Length == 0 ? "—" : $"{Quantile(data, .10):F3}/{Quantile(data, .50):F3}/{Quantile(data, .90):F3}";
    }

    private static double Quantile(IEnumerable<double> source, double p)
    {
        var data = source.Order().ToArray(); if (data.Length == 0) return 0;
        var position = p * (data.Length - 1); var lower = (int)Math.Floor(position); var upper = (int)Math.Ceiling(position);
        return data[lower] + (data[upper] - data[lower]) * (position - lower);
    }

    private static double Median(IEnumerable<double> source) => Quantile(source, .5);

    private static double Auc(IEnumerable<double> positive, IEnumerable<double> negative)
    {
        var p = positive.ToArray(); var n = negative.ToArray(); if (p.Length == 0 || n.Length == 0) return 0;
        double wins = 0; foreach (var a in p) foreach (var b in n) wins += a > b ? 1 : a == b ? .5 : 0;
        return wins / (p.Length * (double)n.Length);
    }

    private static void WithFixture(Action<string> action)
    {
        var archive = Path.Combine(AppContext.BaseDirectory, "Fixtures", "KNX-F35F14A4.zip");
        var folder = Path.Combine(Path.GetTempPath(), "knx-physical-study-" + Guid.NewGuid().ToString("N"));
        try { ZipFile.ExtractToDirectory(archive, folder); action(folder); }
        finally { if (Directory.Exists(folder)) Directory.Delete(folder, true); }
    }
}
