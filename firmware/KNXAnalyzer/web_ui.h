#pragma once
#include <Arduino.h>

static const char kWebPage[] PROGMEM = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>KNX Analyzer</title><style>
:root{font-family:system-ui,sans-serif;color:#e8f2f5;background:#0b1720}*{box-sizing:border-box}body{margin:0}header{padding:18px 22px;background:#122632;border-bottom:1px solid #29505d}h1{font-size:1.3rem;margin:0;color:#68d9e7}small,.muted{color:#9db6bd}nav{display:flex;gap:4px;overflow:auto;padding:9px 12px;background:#10232f}nav button{white-space:nowrap;background:transparent;border:0;color:#b9cdd1;padding:9px 12px;border-radius:7px}nav button.active{background:#1d4351;color:white}main{max-width:1000px;margin:auto;padding:18px}h2{font-size:1.2rem}section{display:none}section.active{display:block}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px}.card{background:#142b37;border:1px solid #2d4b55;border-radius:10px;padding:16px;min-height:95px}.label{font-size:.78rem;text-transform:uppercase;letter-spacing:.1em;color:#9bb8be}.value{font-size:1.35rem;margin-top:8px;word-break:break-word}.actions{display:flex;gap:10px;flex-wrap:wrap;margin:16px 0}button.action{background:#1b7284;color:white;border:0;border-radius:8px;padding:12px 16px;font-weight:700;cursor:pointer}button.action.alt{background:#344a54}button.action.warn{background:#9d494b}button:disabled{opacity:.45}canvas{width:100%;height:330px;background:#07141b;border:1px solid #34515b;border-radius:8px}input,select{display:block;width:100%;padding:11px;background:#09202a;color:white;border:1px solid #41616a;border-radius:7px;margin:6px 0 12px}form{max-width:550px}.notice{padding:10px;background:#1a3944;border-radius:7px;margin:12px 0}.mono{font-family:ui-monospace,monospace}footer{padding:20px;color:#78989f;text-align:center}
@media(max-width:600px){.grid{grid-template-columns:1fr}}
</style></head><body><header><h1>KNX ANALYZER</h1><small id="connection">Connecting…</small></header><nav id="nav"></nav><main>
<section id="dashboard" class="active"><h2>Dashboard</h2><div class="grid" id="cards"></div><div class="actions"><button class="action" onclick="sendAnalyzerCommand('/api/analysis/start')">START ANALYSIS</button><button class="action warn" onclick="sendAnalyzerCommand('/api/analysis/stop')">STOP ANALYSIS</button></div></section>
<section id="scope"><h2>GPIO5 Scope</h2><p id="scopeNotice" class="notice" role="status">Loading analysis state…</p><div class="actions"><button id="scopeStart" class="action" onclick="sendAnalyzerCommand('/api/analysis/start')">START ANALYSIS</button><button id="scopeStop" class="action warn" onclick="sendAnalyzerCommand('/api/analysis/stop')">STOP ANALYSIS</button></div><p id="scopeMeta" class="mono">Waiting for data…</p><canvas id="plot" width="800" height="330" role="img" aria-label="GPIO5 ADC waveform with relative time axis"></canvas><p id="scopeTime" class="mono muted">Relative time: waiting for capture</p><div class="actions"><button id="armRising" class="action" onclick="arm('rising')">ARM RISING</button><button id="armFalling" class="action" onclick="arm('falling')">ARM FALLING</button><button id="armManual" class="action alt" onclick="arm('manual')">ARM MANUAL</button><button id="manualTrigger" class="action alt" onclick="sendAnalyzerCommand('/api/scope/trigger')">MANUAL TRIGGER</button><button id="scopeClear" class="action alt" onclick="sendAnalyzerCommand('/api/scope/clear')">CLEAR</button></div></section>
<section id="events"><h2>Events</h2><p class="notice">Recent metadata in RAM; RAW is retained in RAM or in its SD session.</p><div id="eventsList" class="grid"></div><div id="eventDetail" class="notice">Select an Event.</div></section>
<section id="sessions"><h2>Sessions</h2><p class="notice">Persistent diagnostics on the project SD directory.</p><div id="sessionsList" class="grid"></div><div id="sessionDetail" class="notice">Select a session.</div><div id="sessionEvents" class="grid"></div><div id="sessionEventDetail" class="notice">Select a session Event.</div></section>
<section id="configuration"><h2>Wi-Fi configuration</h2><p class="notice">Primary and backup profiles are stored on this device. Passwords are never shown here.</p><button class="action alt" onclick="scan()">SCAN NETWORKS</button><div id="networks" class="muted"></div><form onsubmit="saveWifi(event)"><label>Profile<select id="slot"><option value="1">Primary</option><option value="2">Backup</option></select></label><label>SSID<input id="ssid" maxlength="32" required></label><label>Password<input id="password" type="password" maxlength="63" autocomplete="new-password"></label><button class="action" type="submit">SAVE & CONNECT</button></form><p id="wifiMessage"></p></section>
<section id="system"><h2>System</h2><div class="grid" id="systemCards"></div></section>
</main><footer>LAN only · GPIO5 is a test input, not KNX · Event captures saved to SD</footer><script>
const names=['dashboard','scope','events','sessions','configuration','system'];let page='dashboard',latest=null,ws=null,displayedEventId=0,displayedSessionId='',scopeMessage='',scopeMessageUntil=0,waveRequestInFlight=false,displayedWaveKey='',scopeCommandInFlight=false;const $=x=>document.getElementById(x);
function show(n,eventId=0,sessionId=''){page=n;for(const id of names)$(id).classList.toggle('active',id===n);for(const b of $('nav').children)b.classList.toggle('active',b.dataset.page===n);if(n==='scope'){displayedEventId=eventId;displayedSessionId=sessionId;displayedWaveKey='';renderScopeControls(latest);loadWave()}if(n==='events')loadEvents();if(n==='sessions')loadSessions()}
for(const n of names){const b=document.createElement('button');b.textContent=n[0].toUpperCase()+n.slice(1);b.dataset.page=n;b.onclick=()=>show(n);$('nav').appendChild(b)}show(location.pathname==='/sessions'?'sessions':location.pathname==='/scope'?'scope':'dashboard');
const esc=s=>String(s??'—').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
function card(k,v){return '<div class="card"><div class="label">'+esc(k)+'</div><div class="value">'+esc(v)+'</div></div>'}
function scopeFeedback(message){scopeMessage=message;scopeMessageUntil=Date.now()+4500;renderScopeControls(latest)}
function renderScopeControls(s){
  const analysis=s?.analysis?.state||'CONNECTING',adc=s?.adc||{},sd=s?.sd||{};
  const active=analysis==='RUNNING',busy=!!sd.queue_depth,armed=active&&!!adc.running;
  const ready=armed&&Number.isFinite(adc.pre_samples)&&(adc.ring_valid||0)>adc.pre_samples;
  const set=(id,enabled,reason)=>{const b=$(id);b.disabled=!enabled;b.title=enabled?'':reason};
  set('scopeStart',analysis==='STOPPED'&&!waveRequestInFlight&&!scopeCommandInFlight,waveRequestInFlight?'Wait for waveform transfer':analysis==='RUNNING'?'Analysis already running':'Analysis unavailable');
  set('scopeStop',active&&!scopeCommandInFlight,'No active analysis');
  for(const id of ['armRising','armFalling','armManual'])set(id,active&&!busy&&!waveRequestInFlight&&!scopeCommandInFlight,waveRequestInFlight?'Wait for waveform transfer':active?'Saving capture to SD':'Start analysis first');
  set('manualTrigger',ready&&!busy&&!scopeCommandInFlight,!active?'Start analysis first':busy?'Saving capture to SD':armed?'Filling pre-trigger buffer':'Arm manual first');
  set('scopeClear',!!adc.captured&&!busy&&!waveRequestInFlight&&!scopeCommandInFlight,waveRequestInFlight?'Wait for waveform transfer':busy?'Saving capture to SD':'No capture to clear');
  let message=analysis==='STOPPED'?'Analysis stopped — Start analysis to use Scope':
    analysis==='ERROR'?'Analysis error — check SD and system status':
    busy?'Saving capture to SD…':
    adc.captured?'CAPTURED — waveform available below':
    armed?(ready?'Scope armed — manual trigger ready':'Scope armed — filling pre-trigger buffer'):
    active?'Analysis running — choose an ARM mode':'Connecting to analyzer…';
  if(scopeMessage&&Date.now()<scopeMessageUntil)message=scopeMessage;
  $('scopeNotice').textContent=message;
}

function render(s){const eventsChanged=latest&&latest.events!==s.events;const adcReleased=latest?.adc?.running&&!s.adc.running;latest=s;if(s.adc.captured&&scopeMessage.startsWith('Trigger accepted'))scopeMessage='';$('connection').textContent=s.wifi.mode+' · '+(s.wifi.ip||'no IP')+' · '+(ws&&ws.readyState===1?'live':'polling');$('cards').innerHTML=card('Analysis',s.analysis.state)+card('ADC',s.adc.ready?'READY':'ERROR')+card('Sample rate',((s.adc.measured_hz||0)/1000).toFixed(1)+' kS/s')+card('Overruns',s.adc.overruns)+card('Heap free',s.heap.free+' B')+card('Wi-Fi',s.wifi.ssid||s.wifi.mode)+card('IP / RSSI',(s.wifi.ip||'—')+' / '+(s.wifi.rssi||'—')+' dBm')+card('SD',s.sd.ready?'READY':'UNAVAILABLE')+card('KNX','NOT CONNECTED')+card('VBUS','NOT CONNECTED')+card('Events',s.events)+card('Session',s.analysis.session_id||'none');$('systemCards').innerHTML=card('Firmware',s.firmware)+card('Uptime',Math.floor(s.uptime_ms/1000)+' s')+card('Heap minimum',s.heap.minimum+' B')+card('Largest block',s.heap.largest+' B')+card('Flash',s.flash_bytes+' B')+card('Wi-Fi',s.wifi.mode)+card('HTTP errors',s.http.errors)+card('Free heap',s.heap.free+' B')+card('ADC rate',((s.adc.measured_hz||0)/1000).toFixed(1)+' kS/s')+card('ADC overruns',s.adc.overruns)+card('DMA errors',s.adc.read_errors)+card('Events',s.events)+card('SD detected',s.sd.detected?'YES':'NO')+card('SD mounted',s.sd.mounted?'YES':'NO')+card('SD type',(['NONE','MMC','SD','SDHC'][s.sd.type]||s.sd.type))+card('SD card size',s.sd.card_bytes+' B')+card('SD volume',s.sd.total_bytes+' B')+card('SD free',s.sd.free_bytes+' B')+card('Current session',s.sd.current_session||'none')+card('SD write errors',s.sd.write_errors)+card('SD queue',s.sd.queue_depth+' / '+s.sd.queue_high_water)+card('Captures on SD',s.sd.captures_persisted)+card('Capture failures',s.sd.captures_failed);if(eventsChanged&&page==='events')loadEvents();if(adcReleased&&page==='sessions')loadSessions();if(page==='scope'){renderScopeControls(s);if(adcReleased||s.adc.captured)loadWave()}}
async function get(path){const r=await fetch(path,{cache:'no-store'});if(!r.ok)throw Error(await r.text());return r.json()}
async function sendAnalyzerCommand(path,body={}){
  const scopeAction=page==='scope';
  if(scopeAction){
    if(scopeCommandInFlight)return;
    scopeCommandInFlight=true;
    scopeFeedback(path.includes('/trigger')?'Trigger requested…':path.includes('/arm')?'Arming '+body.mode+'…':path.includes('/start')?'Starting analysis…':path.includes('/stop')?'Stopping analysis…':'Clearing capture…');
  }
  try{
    if(scopeAction&&waveRequestInFlight&&(path.includes('/start')||path.includes('/arm'))){
      scopeFeedback('Waiting for waveform transfer to finish…');
      const began=Date.now();
      while(waveRequestInFlight&&Date.now()-began<12000)await new Promise(resolve=>setTimeout(resolve,100));
      if(waveRequestInFlight)throw Error('waveform_transfer_pending');
    }
    const r=await fetch(path,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});
    if(!r.ok)throw Error(await r.text());
    if(scopeAction)scopeFeedback(path.includes('/trigger')?'Trigger accepted — waiting for capture…':path.includes('/arm')?'Armed '+body.mode+' — waiting for pre-trigger samples…':path.includes('/start')?'Analysis started — choose an ARM mode':path.includes('/stop')?'Analysis stopped':'Capture cleared');
    await refresh();
    if(page==='scope')loadWave();
  }catch(e){
    let reason=e.message;try{reason=JSON.parse(reason).error||reason}catch(_){}
    const message=reason==='analysis_stopped'?'Analysis stopped — Start analysis to use Scope':
      reason==='not_armed_or_prebuffer_empty'?'Arm manual and wait for the pre-trigger buffer':
      reason==='sd_capture_pending'?'Saving capture to SD — retry shortly':
      reason==='waveform_transfer_pending'?'Waveform transfer still active — retry shortly':'Action failed: '+reason;
    if(scopeAction)scopeFeedback(message);else alert(message);
    await refresh();
  }finally{
    if(scopeAction){scopeCommandInFlight=false;renderScopeControls(latest)}
  }
}
const arm=mode=>sendAnalyzerCommand('/api/scope/arm',{mode});
async function refresh(){try{render(await get('/api/status'))}catch(e){$('connection').textContent='Disconnected'}}
async function loadWave(){
  if(page!=='scope'||displayedEventId===-1||!latest)return;
  if(latest.adc.running){
    $('scopeMeta').textContent=displayedEventId?'ADC active · stored RAW available after capture':
      'ARMED · '+latest.adc.ring_valid+' samples in ring · '+((latest.adc.measured_hz||0)/1000).toFixed(1)+' kS/s · waiting for capture';
    $('scopeTime').textContent='Relative time: waiting for trigger';
    return;
  }
  if(!displayedEventId&&!latest.adc.captured){
    displayedWaveKey='';drawWave({});
    $('scopeMeta').textContent='STOPPED · 0 samples · min/max 0/0 · 0.0 kS/s · RAW: unavailable · pre/post 0/0';
    return;
  }
  const key=displayedSessionId?'session:'+displayedSessionId+':'+displayedEventId:
    displayedEventId?'event:'+displayedEventId:'capture:'+latest.adc.capture_number;
  if(waveRequestInFlight||key===displayedWaveKey)return;
  waveRequestInFlight=true;renderScopeControls(latest);
  try{
    const d=await get(displayedSessionId?'/api/sessions/'+encodeURIComponent(displayedSessionId)+'/events/'+displayedEventId+'/capture':displayedEventId?'/api/events/'+displayedEventId+'/capture':'/api/scope/capture');
    if(!displayedEventId&&latest.adc.running)return;
    drawWave(d);displayedWaveKey=key;
    $('scopeMeta').textContent=(d.state||'EMPTY')+' · '+d.samples+' samples · min/max '+d.min_raw+'/'+d.max_raw+' · '+((d.sample_rate_hz||0)/1000).toFixed(1)+' kS/s · RAW: '+(d.samples?d.raw_source:'unavailable')+' · pre/post '+d.pre_samples+'/'+d.post_samples;
  }catch(e){
    if(displayedEventId>0&&(e.message.includes('raw_capture_not_retained')||e.message.includes('raw_capture_unavailable'))){displayedEventId=-1;drawWave({});$('scopeMeta').textContent='RAW capture not retained'}
    else $('scopeMeta').textContent='Scope unavailable: '+e.message;
  }finally{waveRequestInFlight=false;renderScopeControls(latest)}
}
function drawWave(d){
  const c=$('plot'),g=c.getContext('2d'),w=c.width,h=c.height,left=54,right=w-22,top=18,bottom=h-45,plotW=right-left,plotH=bottom-top;
  g.fillStyle='#07141b';g.fillRect(0,0,w,h);
  g.strokeStyle='#25434d';g.lineWidth=1;
  for(let i=0;i<=4;i++){const y=top+i*plotH/4;g.beginPath();g.moveTo(left,y);g.lineTo(right,y);g.stroke()}
  g.strokeStyle='#78989f';g.beginPath();g.moveTo(left,top);g.lineTo(left,bottom);g.lineTo(right,bottom);g.stroke();
  if(!d.low?.length){$('scopeTime').textContent='Relative time: waiting for capture';return}
  const y=v=>bottom-3-Math.max(0,Math.min(4095,v))*(plotH-6)/4095;
  g.strokeStyle='#55dfec';g.beginPath();
  for(let i=0;i<d.low.length;i++){const x=left+i*plotW/(d.low.length-1);g.moveTo(x,y(d.low[i]));g.lineTo(x,y(d.high[i]))}
  g.stroke();
  g.beginPath();
  for(let i=0;i<d.low.length;i++){const x=left+i*plotW/(d.low.length-1),mid=(d.low[i]+d.high[i])/2;if(i===0)g.moveTo(x,y(mid));else g.lineTo(x,y(mid))}
  g.stroke();
  const captured=d.state==='CAPTURED'&&d.sample_rate_hz>0&&d.samples>0;
  g.font='12px system-ui';g.fillStyle='#b9cdd1';
  if(captured){
    const trigger=d.trigger_position??d.pre_samples,triggerX=left+plotW*trigger/d.samples;
    const beforeMs=1000*trigger/d.sample_rate_hz,afterMs=1000*(d.samples-trigger)/d.sample_rate_hz;
    g.strokeStyle='#ff6770';g.lineWidth=2;g.beginPath();g.moveTo(triggerX,top);g.lineTo(triggerX,bottom);g.stroke();g.lineWidth=1;
    g.textAlign='left';g.fillText('-'+beforeMs.toFixed(1)+' ms',left,bottom+22);
    g.textAlign='center';g.fillText('t = 0',triggerX,bottom+22);
    g.textAlign='right';g.fillText('+'+afterMs.toFixed(1)+' ms',right,bottom+22);
    $('scopeTime').textContent='Relative time: -'+beforeMs.toFixed(1)+' ms / t = 0 / +'+afterMs.toFixed(1)+' ms';
  }else{
    g.textAlign='left';g.fillText('Live ADC samples',left,bottom+22);
    $('scopeTime').textContent='Live view · trigger not set';
  }
}
async function loadEvents(){try{const d=await get('/api/events');$('eventsList').innerHTML=d.events.map(e=>'<button class="card" style="color:inherit;text-align:left;cursor:pointer" onclick="openEvent('+e.event_id+')"><div class="label">Event #'+e.event_id+' | '+esc(e.trigger)+'</div><div class="value">'+esc(e.adc_min)+' / '+esc(e.adc_max)+'</div><small>RAW: '+esc(e.raw_source)+' | '+esc(e.uptime_ms)+' ms</small></button>').join('')||'<p>No Events captured yet.</p>'}catch(e){$('eventsList').textContent='Events unavailable'}}
async function openEvent(id){try{const e=await get('/api/events/'+id);$('eventDetail').innerHTML='<strong>Event #'+e.event_id+'</strong><p>Session '+esc(e.session_id)+' | trigger '+esc(e.trigger)+' | '+esc(e.uptime_ms)+' ms uptime | RAW: '+esc(e.raw_source)+'</p><p>'+esc(e.sample_count)+' samples | trigger at '+esc(e.trigger_index)+' | pre/post '+esc(e.pre_trigger_samples)+'/'+esc(e.post_trigger_samples)+' | '+((e.sample_rate_hz||0)/1000).toFixed(1)+' kS/s</p><p>ADC min/max '+esc(e.adc_min)+'/'+esc(e.adc_max)+' | mean before/after '+esc(e.adc_mean_before)+'/'+esc(e.adc_mean_after)+'</p>'+(e.raw_source!=='unavailable'?'<button class="action" onclick="show(\'scope\','+e.event_id+',\''+e.session_id+'\')">OPEN RAW CAPTURE</button>':'<p>RAW: unavailable</p>')}catch(e){$('eventDetail').textContent='Event unavailable'}}
async function loadSessions(){if(latest?.adc?.running){$('sessionsList').textContent='ADC active · SD history available after capture';return}try{const d=await get('/api/sessions');$('sessionsList').innerHTML=d.sessions.map(s=>'<button class="card" style="color:inherit;text-align:left;cursor:pointer" onclick="openSession(\''+s.session_id+'\')"><div class="label">'+esc(s.session_id)+'</div><div class="value">'+esc(s.state)+'</div><small>'+esc(s.event_count)+' Events | '+(s.duration_ms==null?'unknown duration':esc(s.duration_ms)+' ms')+' | errors '+esc(s.sd_write_errors)+'</small></button>').join('')||'<p>No sessions on SD yet.</p>'}catch(e){$('sessionsList').textContent='Sessions unavailable'}}
async function openSession(id){if(latest?.adc?.running){$('sessionDetail').textContent='ADC active · retry after capture';return}try{const s=await get('/api/sessions/'+encodeURIComponent(id));const d=await get('/api/sessions/'+encodeURIComponent(id)+'/events');$('sessionDetail').innerHTML='<strong>Session '+esc(s.session_id)+'</strong><p>'+esc(s.state)+' | duration '+esc(s.duration_ms??'unknown')+' ms | Events '+esc(s.event_count)+'</p><p>ADC '+((s.adc_sample_rate_hz||0)/1000).toFixed(1)+' kS/s | overruns '+esc(s.adc_overruns)+' | DMA errors '+esc(s.dma_errors)+' | SD failures '+esc(s.captures_failed)+'</p>';$('sessionEvents').innerHTML=d.events.map(e=>'<button class="card" style="color:inherit;text-align:left;cursor:pointer" onclick="openSessionEvent(\''+id+'\','+e.event_id+')"><div class="label">Event #'+e.event_id+' | '+esc(e.trigger)+'</div><div class="value">'+esc(e.adc_min)+' / '+esc(e.adc_max)+'</div><small>RAW: '+esc(e.raw_source)+' | '+esc(e.uptime_ms)+' ms</small></button>').join('')||'<p>No Events in this session.</p>';$('sessionEventDetail').textContent='Select a session Event.'}catch(e){$('sessionDetail').textContent='Session unavailable'}}
async function openSessionEvent(id,eventId){if(latest?.adc?.running){$('sessionEventDetail').textContent='ADC active · retry after capture';return}try{const e=await get('/api/sessions/'+encodeURIComponent(id)+'/events/'+eventId);$('sessionEventDetail').innerHTML='<strong>Event #'+e.event_id+'</strong><p>Trigger '+esc(e.trigger)+' | '+esc(e.sample_count)+' samples | '+((e.sample_rate_hz||0)/1000).toFixed(1)+' kS/s | RAW: '+esc(e.raw_source)+'</p><p>Pre/post '+esc(e.pre_trigger_samples)+'/'+esc(e.post_trigger_samples)+' | ADC min/max '+esc(e.adc_min)+'/'+esc(e.adc_max)+'</p>'+(e.raw_source!=='unavailable'?'<button class="action" onclick="show(\'scope\','+e.event_id+',\''+id+'\')">OPEN WAVEFORM</button>':'<p>RAW: unavailable</p>')}catch(e){$('sessionEventDetail').textContent='Session Event unavailable'}}
async function scan(){await get('/api/wifi/networks?refresh=1');for(let i=0;i<12;i++){const d=await get('/api/wifi/networks');if(!d.scanning){$('networks').replaceChildren();for(const n of d.networks){const b=document.createElement('button');b.className='action alt';b.textContent=n.ssid+' ('+n.rssi+' dBm)';b.onclick=()=>{$('ssid').value=n.ssid};$('networks').appendChild(b)}if(!d.networks.length)$('networks').textContent='No networks found';return}$('networks').textContent='Scanning...';await new Promise(resolve=>setTimeout(resolve,1000))}}
async function saveWifi(e){e.preventDefault();const body={slot:Number($('slot').value),ssid:$('ssid').value,password:$('password').value};try{const r=await fetch('/api/wifi/profiles',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)});if(!r.ok)throw Error(await r.text());$('wifiMessage').textContent='Saved. Connecting; this page may disconnect while the ESP joins your LAN.';$('password').value=''}catch(err){$('wifiMessage').textContent=err.message}}
function connect(){try{ws=new WebSocket('ws://'+location.hostname+':81/');ws.onmessage=e=>{try{const d=JSON.parse(e.data);if(d.type==='status')render(d.data)}catch(_){}};ws.onclose=()=>setTimeout(connect,2000)}catch(_){setTimeout(connect,2000)}}connect();refresh();setInterval(()=>{if(!ws||ws.readyState!==1)refresh();if(page==='scope')loadWave()},1500);
</script></body></html>)HTML";
