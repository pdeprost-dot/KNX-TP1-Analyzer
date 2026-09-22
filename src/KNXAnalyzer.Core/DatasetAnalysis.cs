using System.Text;
using System.Text.Json;

namespace KNXAnalyzer.Core;

public static class DatasetAnalysis
{
    public static IReadOnlyList<Session> Analyzable(IEnumerable<Session> sessions) =>
        sessions.Where(s => s.Candidates.Count > 0 || s.AnalogEvents.Count > 0).ToArray();

    public static string ToJson(IEnumerable<Session> source)
    {
        var sessions = source.ToArray();
        var decoded = sessions.SelectMany(s => s.Candidates.Select(c => new { Session = s, Candidate = c, Telegram = KnxTelegramDecoder.Decode(c) }))
            .Where(x => x.Telegram is not null).ToArray();
        var errors = sessions.SelectMany(s => s.Candidates.Where(c => c.IsError).Select(c => new {
            session_id = s.Id, line = c.Line, timestamp_us = c.MonotonicUs,
            classification = c.Classification, raw_hex = c.RawHex
        })).ToArray();
        var analog = sessions.SelectMany(s => s.AnalogEvents.Where(e => e.CaptureSummary is not null).Select(e => new {
            session_id = s.Id, event_id = e.EventId, peak_to_peak_raw = e.CaptureSummary!.PeakToPeak,
            min_raw = e.CaptureSummary.Minimum, max_raw = e.CaptureSummary.Maximum,
            sample_count = e.CaptureSummary.SampleCount, sample_rate_hz = e.CaptureSummary.SampleRateHz,
            crc_valid = e.CaptureSummary.CrcValid
        })).ToArray();
        var total = sessions.Sum(s => s.Candidates.Count);
        var errorCount = errors.Length;
        var dates = sessions.Select(s => ParseDate(s.DateTime)).Where(d => d is not null).Select(d => d!.Value).Order().ToArray();
        var health = new[] { "adc_overruns", "dma_errors", "sd_write_errors" }.ToDictionary(k => k, k => SumMetadata(sessions, k));
        const long bucketUs = 15L * 60 * 1_000_000;
        var timelineSessions = sessions.Select(s => new { session_id = s.Id, date_time = s.DateTime, duration_ms = s.DurationMs,
            first_timestamp_us = s.Candidates.Where(c => c.MonotonicUs is not null).MinBy(c => c.MonotonicUs)?.MonotonicUs,
            last_timestamp_us = s.Candidates.Where(c => c.MonotonicUs is not null).MaxBy(c => c.MonotonicUs)?.MonotonicUs,
            candidates = s.Candidates.Count, errors = s.Candidates.Count(c => c.IsError) }).ToArray();
        var timelineBuckets = sessions.SelectMany(s => s.Candidates.Where(c => c.MonotonicUs is not null)
            .GroupBy(c => c.MonotonicUs!.Value / bucketUs)
            .Select(g => new { session_id = s.Id, start_timestamp_us = g.Key * bucketUs,
                end_timestamp_us = (g.Key + 1) * bucketUs, candidates = g.Count(),
                valid_telegrams = g.Count(c => c.IsValid && !c.IsAck), acks = g.Count(c => c.Ack == "ACK"),
                errors = g.Count(c => c.IsError) })).OrderBy(x => x.session_id).ThenBy(x => x.start_timestamp_us).ToArray();
        var model = new {
            schema_version = SessionAnalysis.SchemaVersion, studio_version = SessionAnalysis.StudioVersion,
            analysis_scope = "dataset",
            dataset = new { session_count = sessions.Length, analyzable_session_count = Analyzable(sessions).Count,
                closed = sessions.Count(s => s.State == "CLOSED"), interrupted = sessions.Count(s => s.State == "INTERRUPTED"),
                observed_period_start = dates.Length > 0 ? dates[0].ToString("O") : null,
                observed_period_end = dates.Length > 0 ? dates[^1].ToString("O") : null,
                cumulative_duration_ms = sessions.Where(s => s.DurationMs is not null).Sum(s => s.DurationMs!.Value),
                duration_known_sessions = sessions.Count(s => s.DurationMs is not null) },
            acquisition_health = new { adc_overruns = health["adc_overruns"], dma_errors = health["dma_errors"],
                sd_errors = health["sd_write_errors"], source = "recorded session metadata when present" },
            global_statistics = new { total_candidates = total,
                valid_telegrams = sessions.Sum(s => s.Candidates.Count(c => c.IsValid && !c.IsAck)),
                decoded_standard_telegrams = decoded.Length, ack = sessions.Sum(s => s.Candidates.Count(c => c.Ack == "ACK")),
                nak = sessions.Sum(s => s.Candidates.Count(c => c.Ack == "NAK")), busy = sessions.Sum(s => s.Candidates.Count(c => c.Ack == "BUSY")),
                parity_errors = sessions.Sum(s => s.Count("INVALID_PARITY")), checksum_errors = sessions.Sum(s => s.Count("INVALID_CHECKSUM")),
                timing_errors = sessions.Sum(s => s.Count("INVALID_TIMING")), other_errors = sessions.Sum(s => s.Candidates.Count(c => c.IsError && c.Classification is not ("INVALID_PARITY" or "INVALID_CHECKSUM" or "INVALID_TIMING"))),
                error_rate = total == 0 ? 0 : (double)errorCount / total,
                group_traffic = decoded.Count(x => x.Telegram!.DestinationType == "group"), individual_traffic = decoded.Count(x => x.Telegram!.DestinationType == "individual"),
                repeated = decoded.Count(x => x.Telegram!.Repeat), analog_captures = analog.Length },
            participants = decoded.GroupBy(x => x.Telegram!.Source).Select(g => new { address = g.Key, count = g.Count(), sessions = g.Select(x => x.Session.Id).Distinct().Order().ToArray() }).OrderByDescending(x => x.count).ToArray(),
            destinations = decoded.GroupBy(x => (x.Telegram!.Destination, x.Telegram.DestinationType)).Select(g => new { address = g.Key.Destination, type = g.Key.DestinationType, count = g.Count(), sessions = g.Select(x => x.Session.Id).Distinct().Order().ToArray() }).OrderByDescending(x => x.count).ToArray(),
            services = decoded.GroupBy(x => x.Telegram!.Service).Select(g => new { service = g.Key, count = g.Count(), sessions = g.Select(x => x.Session.Id).Distinct().Order().ToArray() }).OrderByDescending(x => x.count).ToArray(),
            timeline = new { bucket_duration_seconds = bucketUs / 1_000_000, sessions = timelineSessions, buckets = timelineBuckets },
            sessions = sessions.Select(s => new { session_id = s.Id, state = s.State, date_time = s.DateTime, duration_ms = s.DurationMs,
                candidates = s.Candidates.Count, analog_captures = s.AnalogEvents.Count, generation = s.Generation }).ToArray(),
            errors,
            analog_summary = new { count = analog.Length, crc_valid = analog.Count(x => x.crc_valid), crc_invalid = analog.Count(x => !x.crc_valid),
                peak_to_peak_min = analog.Length == 0 ? (int?)null : analog.Min(x => x.peak_to_peak_raw),
                peak_to_peak_max = analog.Length == 0 ? (int?)null : analog.Max(x => x.peak_to_peak_raw),
                peak_to_peak_mean = analog.Length == 0 ? (double?)null : analog.Average(x => x.peak_to_peak_raw),
                remarkable = analog.OrderByDescending(x => x.peak_to_peak_raw).Take(10).ToArray() },
            notable_events = errors.Take(100).Select(x => new { kind = "candidate_error", x.session_id, line = (int?)x.line, x.timestamp_us, detail = x.classification })
                .Concat(analog.OrderByDescending(x => x.peak_to_peak_raw).Take(10).Select(x => new { kind = "analog_peak_to_peak", x.session_id, line = (int?)null, timestamp_us = (long?)null, detail = $"event {x.event_id}, P-P {x.peak_to_peak_raw} RAW" })).ToArray(),
            limitations = new[] { "Sessions keep separate provenance and are not merged into a continuous capture.", "Cross-session chronology is unavailable when device date_time is unset.",
                "ACK attribution, DPTs, equipment roles and group meanings are not inferred.", "Analog events are not assumed to represent KNX traffic.",
                "Experimental GPIO5 mV estimates have unknown accuracy and are not bus voltage." }
        };
        return JsonSerializer.Serialize(model, new JsonSerializerOptions { WriteIndented = true });
    }

    public static string ToFrenchReport(IEnumerable<Session> source)
    {
        var sessions = source.ToArray();
        using var json = JsonDocument.Parse(ToJson(sessions));
        var root = json.RootElement; var stats = root.GetProperty("global_statistics"); var dataset = root.GetProperty("dataset"); var health = root.GetProperty("acquisition_health");
        var b = new StringBuilder();
        void Section(string title) { b.AppendLine(); b.AppendLine(title); b.AppendLine(new string('-', title.Length)); }
        b.AppendLine("KNX TP1 ANALYZER STUDIO"); b.AppendLine("Rapport global d'analyse du dossier");
        Section("Dataset"); b.AppendLine($"OBSERVÉ — {dataset.GetProperty("session_count")} sessions : {dataset.GetProperty("closed")} CLOSED, {dataset.GetProperty("interrupted")} INTERRUPTED ; {dataset.GetProperty("analyzable_session_count")} analysables.");
        b.AppendLine($"OBSERVÉ — Durée cumulée : {dataset.GetProperty("cumulative_duration_ms").GetInt64() / 1000.0:F1} s ({dataset.GetProperty("duration_known_sessions")} sessions renseignées).");
        b.AppendLine(dataset.GetProperty("observed_period_start").ValueKind == JsonValueKind.Null ? "NON DÉTERMINABLE — Période civile globale : horloges absentes." : $"OBSERVÉ — Période : {dataset.GetProperty("observed_period_start")} à {dataset.GetProperty("observed_period_end")}.");
        Section("État acquisition"); b.AppendLine($"OBSERVÉ — ADC overruns : {health.GetProperty("adc_overruns")} ; DMA errors : {health.GetProperty("dma_errors")} ; SD errors : {health.GetProperty("sd_errors")}.");
        Section("Résumé trafic"); b.AppendLine($"OBSERVÉ — {stats.GetProperty("total_candidates")} candidats ; {stats.GetProperty("valid_telegrams")} télégrammes valides ; ACK {stats.GetProperty("ack")}, NAK {stats.GetProperty("nak")}, BUSY {stats.GetProperty("busy")}.");
        b.AppendLine($"OBSERVÉ — Erreurs parité {stats.GetProperty("parity_errors")}, checksum {stats.GetProperty("checksum_errors")}, timing {stats.GetProperty("timing_errors")}, autres {stats.GetProperty("other_errors")} ; taux {stats.GetProperty("error_rate").GetDouble():P2}.");
        b.AppendLine($"DÉRIVÉ — Groupe {stats.GetProperty("group_traffic")}, individuel {stats.GetProperty("individual_traffic")}, répétitions {stats.GetProperty("repeated")}.");
        AppendTop(b, "Participants les plus actifs", root.GetProperty("participants"), "address");
        AppendTop(b, "Destinations les plus actives", root.GetProperty("destinations"), "address");
        AppendTop(b, "Services / APCI", root.GetProperty("services"), "service");
        Section("Évolution temporelle et erreurs");
        var timeline = root.GetProperty("timeline");
        foreach (var item in timeline.GetProperty("sessions").EnumerateArray().Where(x => x.GetProperty("errors").GetInt32() > 0)) b.AppendLine($"OBSERVÉ — session {item.GetProperty("session_id")} : {item.GetProperty("errors")} erreurs / {item.GetProperty("candidates")} candidats.");
        foreach (var item in timeline.GetProperty("buckets").EnumerateArray().Where(x => x.GetProperty("errors").GetInt32() > 0).OrderByDescending(x => x.GetProperty("errors").GetInt32()).Take(20)) b.AppendLine($"OBSERVÉ — concentration session {item.GetProperty("session_id")}, t={item.GetProperty("start_timestamp_us").GetInt64() / 1_000_000.0:F0}s : {item.GetProperty("errors")} erreurs / {item.GetProperty("candidates")} candidats (fenêtre 15 min).");
        b.AppendLine("DÉRIVÉ — La timeline utilise des fenêtres de 15 minutes dans chaque session, sans fusion inter-session.");
        b.AppendLine("NON DÉTERMINABLE — Continuité temporelle entre sessions sans date_time exploitable.");
        Section("Analyse physique disponible"); var analog = root.GetProperty("analog_summary"); b.AppendLine($"OBSERVÉ — {analog.GetProperty("count")} captures ; CRC valides {analog.GetProperty("crc_valid")}, invalides {analog.GetProperty("crc_invalid")} ; P-P moyen {Number(analog, "peak_to_peak_mean")} RAW, maximum {Number(analog, "peak_to_peak_max")} RAW.");
        foreach (var item in analog.GetProperty("remarkable").EnumerateArray()) b.AppendLine($"OBSERVÉ — session {item.GetProperty("session_id")}, event {item.GetProperty("event_id")} : P-P {item.GetProperty("peak_to_peak_raw")} RAW, CRC {(item.GetProperty("crc_valid").GetBoolean() ? "OK" : "INVALIDE")}.");
        b.AppendLine("ESTIMÉ — Les mV GPIO5 historiques utilisent la calibration expérimentale approximative ; RAW reste autoritatif.");
        Section("Sessions interrompues"); foreach (var s in sessions.Where(s => s.State == "INTERRUPTED")) b.AppendLine($"OBSERVÉ — session {s.Id}.");
        Section("Événements remarquables"); b.AppendLine("OBSERVÉ — Les erreurs et captures remarquables ci-dessus conservent session, ligne ou event_id."); b.AppendLine("NON DÉTERMINABLE — Corrélation causale entre événements analogiques et trafic KNX.");
        Section("Limites"); foreach (var item in root.GetProperty("limitations").EnumerateArray()) b.AppendLine($"NON DÉTERMINABLE — {item.GetString()}");
        Section("Conclusion technique"); b.AppendLine($"OBSERVÉ — Dataset de {sessions.Length} sessions, {stats.GetProperty("total_candidates")} candidats et {analog.GetProperty("count")} captures. Les sessions restent analysées séparément avec leur provenance.");
        return b.ToString();
    }

    public static void WriteBatch(string directory, IEnumerable<Session> source)
    {
        var sessions = source.ToArray(); Directory.CreateDirectory(directory);
        File.WriteAllText(Path.Combine(directory, "rapport-global.txt"), ToFrenchReport(sessions), Encoding.UTF8);
        File.WriteAllText(Path.Combine(directory, "analysis-global.json"), ToJson(sessions), Encoding.UTF8);
        foreach (var session in Analyzable(sessions)) {
            var target = Path.Combine(directory, "sessions", SafeName(session.Id)); Directory.CreateDirectory(target);
            File.WriteAllText(Path.Combine(target, "rapport.txt"), SessionAnalysis.ToFrenchReport(session), Encoding.UTF8);
            File.WriteAllText(Path.Combine(target, "analysis.json"), SessionAnalysis.ToJson(session), Encoding.UTF8);
        }
    }

    private static long? SumMetadata(IEnumerable<Session> sessions, string name)
    {
        long total = 0; var found = false;
        foreach (var session in sessions) if (session.Metadata.TryGetProperty(name, out var value) && value.TryGetInt64(out var number)) { total += number; found = true; }
        return found ? total : null;
    }
    private static DateTimeOffset? ParseDate(string? value) => DateTimeOffset.TryParse(value, out var date) ? date : null;
    private static string SafeName(string value) => string.Concat(value.Select(c => Path.GetInvalidFileNameChars().Contains(c) ? '_' : c));
    private static string Number(JsonElement parent, string name) => parent.GetProperty(name).ValueKind == JsonValueKind.Null ? "indisponible" : parent.GetProperty(name).ToString();
    private static void AppendTop(StringBuilder b, string title, JsonElement items, string key) { b.AppendLine(); b.AppendLine(title); b.AppendLine(new string('-', title.Length)); foreach (var item in items.EnumerateArray().Take(20)) b.AppendLine($"DÉRIVÉ — {item.GetProperty(key)} : {item.GetProperty("count")}"); }
}
