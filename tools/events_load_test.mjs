const host = process.env.KNX_ANALYZER_HOST || '192.168.4.1';
const base = 'http://' + host;
const durationMs = Number(process.argv[2] ?? 600) * 1000;
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const request = async (path, method = 'GET', body) => {
  const response = await fetch(base + path, {
    method, headers: method === 'POST' ? { 'Content-Type': 'application/json' } : {},
    body: method === 'POST' ? JSON.stringify(body ?? {}) : undefined,
    signal: AbortSignal.timeout(6000), cache: 'no-store'
  });
  if (!response.ok) throw new Error(path + ' HTTP ' + response.status);
  return path === '/' ? response.text() : response.json();
};
let wsMessages = 0, wsErrors = 0, wsOpen = false, wsReconnects = 0, wsStopping = false;
let ws;
function connectWs() {
  ws = new WebSocket('ws://' + host + ':81/');
  ws.onopen = () => { wsOpen = true; };
  ws.onmessage = event => { try { if (JSON.parse(event.data).type === 'status') wsMessages++; } catch { wsErrors++; } };
  ws.onerror = () => { wsErrors++; };
  ws.onclose = () => { if (!wsStopping) { wsReconnects++; setTimeout(connectWs, 2000); } };
}
connectWs();
const start = await request('/api/status');
if (start.analysis.state !== 'RUNNING') await request('/api/analysis/start', 'POST');
await request('/api/scope/arm', 'POST', { mode: 'manual' });
for (let i = 0; i < 20; i++) {
  if ((await request('/api/status')).adc.ring_valid === 50000) break;
  await sleep(250);
}
await sleep(6000); // let the rolling ADC rate reach a complete measurement window
const begin = Date.now();
const result = { polls: 0, eventPolls: 0, htmlPolls: 0, failures: 0, minHz: Infinity, maxHz: 0, sumHz: 0, minHeap: Infinity, minLargest: Infinity, maxOverruns: 0, maxReadErrors: 0, maxHttpErrors: 0, minEvents: Infinity, maxEvents: 0, reboots: 0, lastUptime: start.uptime_ms };
let lastReport = 0;
while (Date.now() - begin < durationMs) {
  try {
    const status = await request('/api/status');
    const scope = await request('/api/scope/capture');
    const events = await request('/api/events');
    if (status.analysis.state !== 'RUNNING') throw new Error('analysis stopped');
    if (scope.low.length !== 160 || scope.high.length !== 160 || scope.samples !== 50000) throw new Error('scope shape');
    if (events.total !== status.events || events.retained !== events.events.length) throw new Error('event count mismatch');
    if (events.events.length) {
      const event = await request('/api/events/' + events.events[0].event_id);
      if (event.event_id !== events.events[0].event_id || event.capture_state !== 'NOT_RETAINED') throw new Error('event detail mismatch');
    }
    result.polls++; result.eventPolls++;
    const hz = status.adc.measured_hz;
    if (hz > 0) { result.minHz = Math.min(result.minHz, hz); result.maxHz = Math.max(result.maxHz, hz); result.sumHz += hz; }
    result.minHeap = Math.min(result.minHeap, status.heap.free);
    result.minLargest = Math.min(result.minLargest, status.heap.largest);
    result.maxOverruns = Math.max(result.maxOverruns, status.adc.overruns);
    result.maxReadErrors = Math.max(result.maxReadErrors, status.adc.read_errors);
    result.maxHttpErrors = Math.max(result.maxHttpErrors, status.http.errors);
    result.minEvents = Math.min(result.minEvents, status.events);
    result.maxEvents = Math.max(result.maxEvents, status.events);
    if (status.uptime_ms < result.lastUptime) result.reboots++;
    result.lastUptime = status.uptime_ms;
    if (result.polls % 5 === 1) {
      const html = await request('/');
      if (!html.includes('eventsList') || !html.includes('openEvent') || !html.includes('KNX ANALYZER')) throw new Error('incomplete HTML');
      result.htmlPolls++;
    }
  } catch (error) { result.failures++; console.log('FAIL ' + error.message); }
  const seconds = Math.floor((Date.now() - begin) / 1000);
  if (seconds - lastReport >= 60) {
    lastReport = seconds;
    console.log('PROGRESS ' + JSON.stringify({ seconds, polls: result.polls, failures: result.failures, hz: Math.round(result.sumHz / result.polls), overruns: result.maxOverruns, heap: result.minHeap, events: result.maxEvents, wsMessages, wsErrors }));
  }
  await sleep(2000);
}
wsStopping = true;
ws.close();
let stop = 'FAILED';
try { stop = (await request('/api/analysis/stop', 'POST')).state; } catch (error) { console.log('STOP_FAIL ' + error.message); }
const finish = await request('/api/status');
console.log('RESULT ' + JSON.stringify({ duration_s: Math.round((Date.now() - begin) / 1000), ...result, avgHz: Math.round(result.sumHz / result.polls), wsOpen, wsMessages, wsErrors, wsReconnects, stop, finalState: finish.analysis.state, finalHeap: finish.heap.free, finalMinHeap: finish.heap.minimum, finalOverruns: finish.adc.overruns, finalReadErrors: finish.adc.read_errors, finalHttpErrors: finish.http.errors }));
