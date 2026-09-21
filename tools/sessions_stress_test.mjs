const host=process.env.KNX_ANALYZER_HOST||'192.168.4.1';
const base='http://'+host;
const durationMs=Number(process.argv[2]||600)*1000;
const intervalMs=Number(process.argv[3]||25)*1000;
const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function api(path,body){const r=await fetch(base+path,{method:body?'POST':'GET',headers:body?{'Content-Type':'application/json'}:{},body:body?JSON.stringify(body):undefined,signal:AbortSignal.timeout(20000),cache:'no-store'});if(!r.ok)throw Error(path+' HTTP '+r.status+' '+await r.text());return path==='/'?r.text():r.json()}
let ws,wsMessages=0,wsErrors=0,wsOpen=false,wsReconnects=0,wsStopping=false;
function connectWs(){ws=new WebSocket('ws://'+host+':81/');ws.onopen=()=>{wsOpen=true};ws.onmessage=e=>{try{if(JSON.parse(e.data).type==='status')wsMessages++}catch{wsErrors++}};ws.onerror=()=>{wsErrors++};ws.onclose=()=>{if(!wsStopping){wsReconnects++;setTimeout(connectWs,2000)}}}connectWs();
let start=await api('/api/status');
if(start.analysis.state!=='RUNNING'){await api('/api/analysis/start',{});start=await api('/api/status')}
const sessionId=start.analysis.session_id;
const begin=Date.now();let nextCapture=begin+5000,lastReport=0,lastUptime=start.uptime_ms;
const result={session_id:sessionId,duration_s:0,polls:0,sessionPolls:0,scopePolls:0,htmlPolls:0,httpFailures:0,capturesRequested:0,capturesPersisted:0,minHz:Infinity,maxHz:0,sumHz:0,hzSamples:0,minEventHz:Infinity,maxEventHz:0,sumEventHz:0,minHeap:Infinity,minLargest:Infinity,maxOverruns:start.adc.overruns,maxDmaErrors:start.adc.read_errors,maxSdErrors:start.sd.write_errors,reboots:0};
while(Date.now()-begin<durationMs){
  try{
    const status=await api('/api/status');
    if(status.uptime_ms<lastUptime)result.reboots++;
    lastUptime=status.uptime_ms;
    result.polls++;
    if(status.adc.running&&status.adc.measured_hz>70000){result.minHz=Math.min(result.minHz,status.adc.measured_hz);result.maxHz=Math.max(result.maxHz,status.adc.measured_hz);result.sumHz+=status.adc.measured_hz;result.hzSamples++}
    result.minHeap=Math.min(result.minHeap,status.heap.free);
    result.minLargest=Math.min(result.minLargest,status.heap.largest);
    result.maxOverruns=Math.max(result.maxOverruns,status.adc.overruns);
    result.maxDmaErrors=Math.max(result.maxDmaErrors,status.adc.read_errors);
    result.maxSdErrors=Math.max(result.maxSdErrors,status.sd.write_errors);
    if(result.polls%3===0){const s=await api('/api/sessions/'+sessionId);if(s.state!=='RUNNING')throw Error('session state');result.sessionPolls++}
    if(result.polls%4===0){const wave=await api('/api/scope/capture');if(wave.low.length!==160)throw Error('scope shape');result.scopePolls++}
    if(result.polls%6===0){const html=await api('/');if(!html.includes('sessionsList'))throw Error('sessions UI missing');result.htmlPolls++}
    if(Date.now()>=nextCapture&&status.sd.queue_depth===0){
      await api('/api/scope/arm',{mode:'manual'});
      await sleep(1200);
      const armed=await api('/api/status');
      if(armed.adc.measured_hz>70000){result.minHz=Math.min(result.minHz,armed.adc.measured_hz);result.maxHz=Math.max(result.maxHz,armed.adc.measured_hz);result.sumHz+=armed.adc.measured_hz;result.hzSamples++}
      await api('/api/scope/trigger',{});
      result.capturesRequested++;
      const target=start.sd.captures_persisted+result.capturesRequested;
      let saved=false;
      for(let j=0;j<25;j++){await sleep(400);const check=await api('/api/status');if(check.sd.captures_persisted>=target&&check.sd.queue_depth===0){saved=true;result.capturesPersisted++;break}if(check.sd.captures_failed>start.sd.captures_failed)throw Error('capture failed')}
      if(!saved)throw Error('capture persistence timeout');
      const latest=(await api('/api/events')).events[0];
      result.minEventHz=Math.min(result.minEventHz,latest.sample_rate_hz);
      result.maxEventHz=Math.max(result.maxEventHz,latest.sample_rate_hz);
      result.sumEventHz+=latest.sample_rate_hz;
      nextCapture+=intervalMs;
    }
  }catch(e){result.httpFailures++;console.log('FAIL '+e.message)}
  const seconds=Math.floor((Date.now()-begin)/1000);
  if(seconds-lastReport>=60){lastReport=seconds;console.log('PROGRESS '+JSON.stringify({seconds,polls:result.polls,captures:result.capturesPersisted,failures:result.httpFailures,overruns:result.maxOverruns-start.adc.overruns,sdErrors:result.maxSdErrors-start.sd.write_errors,heap:result.minHeap,wsMessages,wsErrors}))}
  await sleep(1000);
}
wsStopping=true;ws?.close();
await api('/api/analysis/stop',{});
const finish=await api('/api/status');
const session=await api('/api/sessions/'+sessionId);
result.duration_s=Math.round((Date.now()-begin)/1000);
console.log('RESULT '+JSON.stringify({...result,avgHz:Math.round(result.sumHz/result.hzSamples),avgEventHz:Math.round(result.sumEventHz/result.capturesPersisted),adcOverrunDelta:finish.adc.overruns-start.adc.overruns,dmaErrorDelta:finish.adc.read_errors-start.adc.read_errors,sdErrorDelta:finish.sd.write_errors-start.sd.write_errors,captureFailedDelta:finish.sd.captures_failed-start.sd.captures_failed,bytesWritten:finish.sd.bytes_written-start.sd.bytes_written,freeHeapFinal:finish.heap.free,minFreeHeapInternal:finish.heap.minimum,largestBlockFinal:finish.heap.largest,httpErrorsDelta:finish.http.errors-start.http.errors,wsOpen,wsMessages,wsErrors,wsReconnects,finalState:finish.analysis.state,sessionState:session.state}));
