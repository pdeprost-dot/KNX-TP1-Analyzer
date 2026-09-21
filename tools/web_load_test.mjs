const host = process.env.KNX_ANALYZER_HOST || '192.168.4.1';
const base = 'http://' + host;
const durationMs = Number(process.argv[2] ?? 600) * 1000;
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const get = async path => {
  const response = await fetch(base + path, { signal: AbortSignal.timeout(5000), cache: 'no-store' });
  if (!response.ok) throw new Error(path + ' HTTP ' + response.status);
  return path === '/' ? response.text() : response.json();
};
const post = async path => {
  const response = await fetch(base + path, { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: '{}', signal: AbortSignal.timeout(5000) });
  if (!response.ok) throw new Error(path + ' HTTP ' + response.status);
  return response.json();
};
let wsMessages = 0, wsErrors = 0, wsOpen = false;
const ws = new WebSocket('ws://' + host + ':81/');
ws.onopen = () => { wsOpen = true; };
ws.onmessage = event => { try { const item = JSON.parse(event.data); if (item.type === 'status') wsMessages++; } catch { wsErrors++; } };
ws.onerror = () => { wsErrors++; };
const start = await get('/api/status');
if (start.analysis.state !== 'RUNNING') await post('/api/analysis/start');
const begin = Date.now();
const results = { polls: 0, scopePolls: 0, htmlPolls: 0, failures: 0, minHz: Infinity, maxHz: 0, totalHz: 0, minHeap: Infinity, maxHeap: 0, maxOverruns: 0, maxErrors: 0, minLargest: Infinity, bootUptime: start.uptime_ms, lastUptime: start.uptime_ms, reboots: 0 };
let lastReport = 0;
while (Date.now() - begin < durationMs) {
  try {
    const status = await get('/api/status');
    const wave = await get('/api/scope/capture');
    if (wave.low.length !== 160 || wave.high.length !== 160 || wave.samples !== 50000) throw new Error('scope shape');
    if (status.analysis.state !== 'RUNNING') throw new Error('analysis stopped');
    results.polls++;
    results.scopePolls++;
    results.minHz = Math.min(results.minHz, status.adc.measured_hz);
    results.maxHz = Math.max(results.maxHz, status.adc.measured_hz);
    results.totalHz += status.adc.measured_hz;
    results.minHeap = Math.min(results.minHeap, status.heap.free);
    results.maxHeap = Math.max(results.maxHeap, status.heap.free);
    results.minLargest = Math.min(results.minLargest, status.heap.largest);
    results.maxOverruns = Math.max(results.maxOverruns, status.adc.overruns);
    results.maxErrors = Math.max(results.maxErrors, status.adc.read_errors);
    if (status.uptime_ms < results.lastUptime) results.reboots++;
    results.lastUptime = status.uptime_ms;
    if (results.polls % 5 === 1) {
      const html = await get('/');
      if (!html.includes('KNX ANALYZER') || !html.includes('Dashboard') || !html.includes('Scope')) throw new Error('incomplete HTML');
      results.htmlPolls++;
    }
  } catch (error) {
    results.failures++;
    console.log('FAIL ' + error.message);
  }
  const seconds = Math.floor((Date.now() - begin) / 1000);
  if (seconds - lastReport >= 60) {
    lastReport = seconds;
    console.log('PROGRESS ' + JSON.stringify({ seconds, polls: results.polls, failures: results.failures, hz: Math.round(results.totalHz / results.polls), overruns: results.maxOverruns, heap: results.minHeap, wsMessages, wsErrors }));
  }
  await sleep(2000);
}
ws.close();
const stopped = await post('/api/analysis/stop');
const finish = await get('/api/status');
console.log('RESULT ' + JSON.stringify({ duration_s: Math.round((Date.now() - begin) / 1000), ...results, avgHz: Math.round(results.totalHz / results.polls), wsOpen, wsMessages, wsErrors, stop: stopped.state, finalState: finish.analysis.state, finalHeap: finish.heap.free, finalMinHeap: finish.heap.minimum, finalOverruns: finish.adc.overruns, finalErrors: finish.adc.read_errors }));
