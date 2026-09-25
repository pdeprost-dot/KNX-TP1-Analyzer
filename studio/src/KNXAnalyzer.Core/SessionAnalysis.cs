using System.Text;
using System.Text.Json;

namespace KNXAnalyzer.Core;

public static class SessionAnalysis
{
    public const string SchemaVersion = "1.0";
    public const string StudioVersion = "0-preview";

    public static object Create(Session session)
    {
        var decodedAll = session.Candidates.Select(c => (Candidate: c, Telegram: KnxTelegramDecoder.Decode(c)))
            .Where(x => x.Telegram is not null).ToArray();
        var decoded = decodedAll.Where(x => !x.Candidate.SyntheticTest).ToArray();
        var analog = session.AnalogEvents.Select(e => new {
            event_id = e.EventId, raw_persisted = e.RawPersisted, sample_count = e.CaptureSummary?.SampleCount,
            sample_rate_hz = e.CaptureSummary?.SampleRateHz, min_raw = e.CaptureSummary?.Minimum,
            max_raw = e.CaptureSummary?.Maximum, peak_to_peak_raw = e.CaptureSummary?.PeakToPeak,
            mean_raw = e.CaptureSummary?.Mean, crc_valid = e.CaptureSummary?.CrcValid,
            recorded = e.Original
        }).ToArray();
        return new {
            schema_version = SchemaVersion, studio_version = StudioVersion,
            session = new { id = session.Id, state = session.State, date_time = session.DateTime,
                duration_ms = session.DurationMs, generation = session.Generation, recorded = session.Metadata },
            statistics = new { total_candidates = session.Candidates.Count,
                valid_telegrams = session.Candidates.Count(c => c.IsValid && !c.IsAck && !c.SyntheticTest),
                recorded_valid_telegrams_total = session.Candidates.Count(c => c.IsValid && !c.IsAck),
                decoded_standard_telegrams = decoded.Length,
                synthetic_test_telegrams = decodedAll.Count(x => x.Candidate.SyntheticTest),
                ack = session.Candidates.Count(c => c.Ack == "ACK"),
                nak = session.Candidates.Count(c => c.Ack == "NAK"),
                busy = session.Candidates.Count(c => c.Ack == "BUSY"),
                parity_errors = session.Count("INVALID_PARITY"), checksum_errors = session.Count("INVALID_CHECKSUM"),
                timing_errors = session.Count("INVALID_TIMING"),
                other_errors = session.Candidates.Count(c => c.IsError && c.Classification is not ("INVALID_PARITY" or "INVALID_CHECKSUM" or "INVALID_TIMING")),
                group_traffic = decoded.Count(x => x.Telegram!.DestinationType == "group"),
                individual_traffic = decoded.Count(x => x.Telegram!.DestinationType == "individual"),
                repeated = decoded.Count(x => x.Telegram!.Repeat), analog_captures = analog.Length },
            participants = decoded.GroupBy(x => x.Telegram!.Source).Select(g => new { address = g.Key, count = g.Count() }).OrderByDescending(x => x.count).ToArray(),
            destinations = decoded.GroupBy(x => (x.Telegram!.Destination, x.Telegram.DestinationType))
                .Select(g => new { address = g.Key.Destination, type = g.Key.DestinationType, count = g.Count() }).OrderByDescending(x => x.count).ToArray(),
            telegrams = decodedAll.Select(x => new { line = x.Candidate.Line, timestamp_us = x.Candidate.MonotonicUs,
                synthetic_test = x.Candidate.SyntheticTest,
                raw_hex = x.Candidate.RawHex, firmware_classification = x.Candidate.Classification,
                recorded = x.Candidate.Original, derived = x.Telegram,
                ack_relation = "indeterminate" }).ToArray(),
            acks = session.Candidates.Where(c => c.IsAck).Select(c => new { line = c.Line,
                timestamp_us = c.MonotonicUs, kind = c.Ack, raw_hex = c.RawHex,
                preceding_telegram = "indeterminate", recorded = c.Original }).ToArray(),
            errors = session.Candidates.Where(c => c.IsError).Select(c => new { line = c.Line,
                timestamp_us = c.MonotonicUs, classification = c.Classification, raw_hex = c.RawHex,
                recorded = c.Original }).ToArray(),
            services = decoded.GroupBy(x => x.Telegram!.Service).Select(g => new { service = g.Key, count = g.Count() }).OrderByDescending(x => x.count).ToArray(),
            analog_captures = analog,
            notable_events = analog.Where(x => x.peak_to_peak_raw is > 0).OrderByDescending(x => x.peak_to_peak_raw).Take(5)
                .Select(x => new { kind = "analog_peak_to_peak", event_id = x.event_id, peak_to_peak_raw = x.peak_to_peak_raw }).ToArray(),
            limitations = new[] { "No ETS project, DPT or equipment role is inferred.",
                "ACK proximity does not prove a particular telegram was acknowledged.",
                "Analog events are not assumed to represent KNX traffic.",
                "Historical RAW captures have no recorded ADC calibration; experimental mV display has unknown accuracy." }
        };
    }

    public static string ToJson(Session session) => JsonSerializer.Serialize(Create(session), new JsonSerializerOptions { WriteIndented = true });

    public static string ToFrenchReport(Session session)
    {
        var decodedItems = session.Candidates.Select(c => (Candidate: c, Telegram: KnxTelegramDecoder.Decode(c))).Where(x => x.Telegram is not null).ToArray();
        var decoded = decodedItems.Where(x => !x.Candidate.SyntheticTest).Select(x => x.Telegram!).ToArray();
        var syntheticCount = decodedItems.Count(x => x.Candidate.SyntheticTest);
        var b = new StringBuilder();
        void Section(string name) { b.AppendLine(); b.AppendLine(name); b.AppendLine(new string('-', name.Length)); }
        b.AppendLine("KNX TP1 ANALYZER STUDIO"); b.AppendLine("Rapport d'analyse");
        Section("Session"); b.AppendLine($"OBSERVÉ — ID : {session.Id} ; état : {session.State} ; durée : {(session.DurationMs is long ms ? $"{ms / 1000.0:F1} s" : "indisponible")}");
        b.AppendLine($"OBSERVÉ — Génération indicative : {session.Generation} ; horloge : {session.DateTime ?? "indisponible"}");
        Section("Résumé"); b.AppendLine($"OBSERVÉ — {session.Candidates.Count} candidats ; {session.Candidates.Count(c => c.IsValid && !c.IsAck)} trames valides ; {session.Candidates.Count(c => c.Ack == "ACK")} ACK.");
        b.AppendLine($"OBSERVÉ — Parité : {session.Count("INVALID_PARITY")} ; checksum : {session.Count("INVALID_CHECKSUM")} ; timing : {session.Count("INVALID_TIMING")}.");
        Section("Trafic KNX"); b.AppendLine($"OBSERVÉ — trafic terrain décodé : {decoded.Length} trames ; synthetic_test exclu : {syntheticCount}.");
        b.AppendLine($"DÉRIVÉ — {decoded.Length} trames standard complètes décodées ; groupe : {decoded.Count(t => t.DestinationType == "group")} ; individuel : {decoded.Count(t => t.DestinationType == "individual")} ; répétées : {decoded.Count(t => t.Repeat)}.");
        Section("Participants observés"); foreach (var g in decoded.GroupBy(t => t.Source).OrderByDescending(g => g.Count()).Take(20)) b.AppendLine($"DÉRIVÉ — {g.Key} : {g.Count()}");
        Section("Adresses de groupe observées"); foreach (var g in decoded.Where(t => t.DestinationType == "group").GroupBy(t => t.Destination).OrderByDescending(g => g.Count()).Take(20)) b.AppendLine($"DÉRIVÉ — {g.Key} : {g.Count()}");
        Section("Services / APCI"); foreach (var g in decoded.GroupBy(t => t.Service).OrderByDescending(g => g.Count())) b.AppendLine($"DÉRIVÉ — {g.Key} : {g.Count()}");
        Section("Erreurs et anomalies"); b.AppendLine($"OBSERVÉ — {session.Candidates.Count(c => c.IsError)} candidats en erreur ; {session.Diagnostics.Count} avertissements de lecture.");
        Section("Acquittements"); b.AppendLine($"OBSERVÉ — ACK : {session.Candidates.Count(c => c.Ack == "ACK")} ; NAK : {session.Candidates.Count(c => c.Ack == "NAK")} ; BUSY : {session.Candidates.Count(c => c.Ack == "BUSY")}.");
        b.AppendLine("NON DÉTERMINABLE — Attribution certaine des ACK à une trame précise.");
        Section("Analyse physique disponible"); b.AppendLine($"OBSERVÉ — {session.AnalogEvents.Count} événements analogiques ; {session.AnalogEvents.Count(e => e.CaptureSummary?.CrcValid == true)} RAW avec CRC valide.");
        foreach (var e in session.AnalogEvents.Where(e => e.CaptureSummary is not null).OrderByDescending(e => e.CaptureSummary!.PeakToPeak).Take(5)) b.AppendLine($"OBSERVÉ — Événement {e.EventId} : P-P {e.CaptureSummary!.PeakToPeak} RAW ; CRC {(e.CaptureSummary.CrcValid ? "OK" : "INVALIDE")}");
        b.AppendLine("ESTIMÉ — L'affichage mV GPIO5 des anciennes captures utilise une calibration expérimentale basée sur un point approximatif (RAW 1888 ≈ 1930 mV) et une hypothèse proportionnelle ; précision inconnue.");
        Section("Événements remarquables"); b.AppendLine("NON DÉTERMINABLE — Lien causal entre captures analogiques et trafic KNX.");
        Section("Limites de l'analyse"); b.AppendLine("NON DÉTERMINABLE — DPT, rôle des équipements, signification métier des adresses, tension du bus et provenance exacte du build historique.");
        Section("Conclusion technique"); b.AppendLine($"OBSERVÉ — Session {session.State} avec {session.Candidates.Count} candidats et {session.AnalogEvents.Count} événements analogiques. Aucune fonction d'équipement n'est déduite.");
        return b.ToString();
    }
}
