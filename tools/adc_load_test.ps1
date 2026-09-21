param([int]$Seconds = 600)

$port = [System.IO.Ports.SerialPort]::new('COM10', 115200)
$port.ReadTimeout = 500
$port.Open()
$stats = [System.Collections.Generic.List[object]]::new()
$memory = [System.Collections.Generic.List[object]]::new()
$started = [DateTime]::UtcNow
$nextReport = $started.AddSeconds(60)
try {
    Start-Sleep -Milliseconds 1500
    $port.Write('S')
    while (([DateTime]::UtcNow - $started).TotalSeconds -lt $Seconds) {
        try {
            $line = $port.ReadLine().Trim()
            if (-not $line.StartsWith('{')) { continue }
            $item = $line | ConvertFrom-Json
            if ($item.type -eq 'ADC_STATS' -and $item.running) { $stats.Add($item) }
            if ($item.type -eq 'MEMORY_STATS') { $memory.Add($item) }
            if ($item.type -in @('ANALYSIS', 'ADC_ERROR', 'WIFI_STATUS', 'CAPTURE_READY')) { $line }
        } catch [TimeoutException] {} catch { "PARSE_ERROR $($_.Exception.Message)" }
        if ([DateTime]::UtcNow -ge $nextReport) {
            $last = if ($stats.Count) { $stats[$stats.Count - 1] } else { $null }
            "PROGRESS seconds=$([math]::Round(([DateTime]::UtcNow - $started).TotalSeconds)) stats=$($stats.Count) hz=$($last.measured_hz) overruns=$($last.overruns) heap=$($last.heap_free)"
            $nextReport = $nextReport.AddSeconds(60)
        }
    }
    $port.Write('X')
    Start-Sleep -Milliseconds 500
    $rates = @($stats | Where-Object measured_hz -gt 0 | ForEach-Object measured_hz)
    $heaps = @($stats | ForEach-Object heap_free)
    $overruns = if ($stats.Count) { $stats[$stats.Count - 1].overruns - $stats[0].overruns } else { $null }
    "RESULT seconds=$Seconds windows=$($stats.Count) min_hz=$(($rates | Measure-Object -Minimum).Minimum) max_hz=$(($rates | Measure-Object -Maximum).Maximum) avg_hz=$([math]::Round(($rates | Measure-Object -Average).Average)) overruns=$overruns heap_min=$(($heaps | Measure-Object -Minimum).Minimum) heap_max=$(($heaps | Measure-Object -Maximum).Maximum)"
} finally {
    $port.Close()
}
