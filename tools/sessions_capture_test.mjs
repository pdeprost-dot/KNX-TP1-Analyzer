const host=process.env.KNX_ANALYZER_HOST||'192.168.4.1';
const base='http://'+host;
const target=Number(process.argv[2]||50);
const sleep=ms=>new Promise(resolve=>setTimeout(resolve,ms));
async function api(path,body){
  const r=await fetch(base+path,{method:body?'POST':'GET',headers:body?{'Content-Type':'application/json'}:{},body:body?JSON.stringify(body):undefined,signal:AbortSignal.timeout(20000),cache:'no-store'});
  if(!r.ok)throw Error(path+' HTTP '+r.status+' '+await r.text());
  return r.json();
}
let status=await api('/api/status');
if(status.analysis.state==='STOPPED'){await api('/api/analysis/start',{});status=await api('/api/status')}
if(status.analysis.state!=='RUNNING')throw Error('analysis not running');
const sessionId=status.analysis.session_id;
const session=await api('/api/sessions/'+sessionId);
const initialPersisted=status.sd.captures_persisted,initialOverruns=status.adc.overruns,initialDma=status.adc.read_errors,initialWriteErrors=status.sd.write_errors;
let created=session.event_count,failures=0;
const begin=Date.now();
while(created<target){
  await api('/api/scope/arm',{mode:'manual'});
  await sleep(1200);
  await api('/api/scope/trigger',{});
  const expected=initialPersisted+(created-session.event_count)+1;
  let finished=false;
  for(let check=0;check<30;check++){
    await sleep(400);
    status=await api('/api/status');
    if(status.adc.overruns!==initialOverruns)throw Error('ADC overrun at event '+(created+1)+': '+status.adc.overruns);
    if(status.adc.read_errors!==initialDma)throw Error('DMA error at event '+(created+1));
    if(status.sd.write_errors!==initialWriteErrors||status.sd.captures_failed)throw Error('SD write failure at event '+(created+1));
    if(status.sd.captures_persisted>=expected&&status.sd.queue_depth===0){finished=true;break}
  }
  if(!finished)throw Error('SD persistence timeout at event '+(created+1));
  created++;
  if(created%5===0||created===target)console.log('PROGRESS '+JSON.stringify({created,elapsed_s:Math.round((Date.now()-begin)/1000),overruns:status.adc.overruns,persisted:status.sd.captures_persisted,heap:status.heap.free,queue_max:status.sd.queue_high_water}));
}
await api('/api/analysis/stop',{});
status=await api('/api/status');
const metadata=await api('/api/sessions/'+sessionId);
const events=await api('/api/sessions/'+sessionId+'/events');
console.log('RESULT '+JSON.stringify({duration_s:Math.round((Date.now()-begin)/1000),session_id:sessionId,state:metadata.state,event_count:metadata.event_count,event_lines:events.count,added_persisted:status.sd.captures_persisted-initialPersisted,captures_failed:status.sd.captures_failed,sd_write_errors:status.sd.write_errors-initialWriteErrors,adc_overruns:status.adc.overruns-initialOverruns,dma_errors:status.adc.read_errors-initialDma,heap_free:status.heap.free,heap_min:status.heap.minimum,queue_max:status.sd.queue_high_water,bytes_written:status.sd.bytes_written}));
