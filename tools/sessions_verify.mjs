const host=process.env.KNX_ANALYZER_HOST||'192.168.4.1';
const base='http://'+host;
const sessionId=process.argv[2];
const expectedCount=Number(process.argv[3]||50);
if(!sessionId||!/^[A-Za-z0-9-]+$/.test(sessionId))throw Error('session id required');
const crc32=bytes=>{let c=0xFFFFFFFF;for(const b of bytes){c^=b;for(let i=0;i<8;i++)c=(c>>>1)^((c&1)?0xEDB88320:0)}return(c^0xFFFFFFFF)>>>0};
async function get(path,binary=false){const r=await fetch(base+path,{signal:AbortSignal.timeout(30000),cache:'no-store'});if(!r.ok)throw Error(path+' HTTP '+r.status);return binary?Buffer.from(await r.arrayBuffer()):r.json()}
const session=await get('/api/sessions/'+sessionId);
const listing=await get('/api/sessions/'+sessionId+'/events');
if(session.state!=='CLOSED'||session.event_count!==expectedCount||listing.count!==expectedCount)throw Error('metadata count/state mismatch');
let bytesTotal=0,minRate=Infinity,maxRate=0;
for(let i=0;i<listing.events.length;i++){
  const event=listing.events[i];
  if(!event.raw_persisted)throw Error('RAW not persisted '+event.event_id);
  const b=await get('/api/sessions/'+sessionId+'/events/'+event.event_id+'/raw',true);
  if(b.length!==100048||b.toString('ascii',0,7)!=='KNXADC1'||b.readUInt8(7)!==0)throw Error('header/size '+event.event_id);
  const h={version:b.readUInt16LE(8),headerBytes:b.readUInt16LE(10),eventId:b.readUInt32LE(12),rate:b.readUInt32LE(16),samples:b.readUInt32LE(20),trigger:b.readUInt32LE(24),pre:b.readUInt32LE(28),post:b.readUInt32LE(32),min:b.readUInt16LE(36),max:b.readUInt16LE(38),crc:b.readUInt32LE(40)};
  if(h.version!==1||h.headerBytes!==48||h.eventId!==event.event_id||h.rate!==event.sample_rate_hz||h.samples!==50000||h.trigger!==35000||h.pre!==35000||h.post!==15000)throw Error('header fields '+event.event_id);
  if(crc32(b.subarray(48))!==h.crc)throw Error('CRC '+event.event_id);
  let min=65535,max=0,preSum=0,postSum=0;
  for(let j=0;j<50000;j++){const v=b.readUInt16LE(48+j*2);if(v<min)min=v;if(v>max)max=v;if(j<35000)preSum+=v;else postSum+=v}
  if(min!==h.min||max!==h.max||min!==event.adc_min||max!==event.adc_max||Math.floor(preSum/35000)!==event.adc_mean_before||Math.floor(postSum/15000)!==event.adc_mean_after)throw Error('sample statistics '+event.event_id);
  bytesTotal+=b.length;minRate=Math.min(minRate,h.rate);maxRate=Math.max(maxRate,h.rate);
  if((i+1)%10===0)console.log('PROGRESS '+JSON.stringify({verified:i+1,bytes:bytesTotal}));
}
const oldest=await get('/api/sessions/'+sessionId+'/events/'+listing.events[0].event_id+'/capture');
if(oldest.raw_source!=='SD'||oldest.samples!==50000||oldest.low.length!==160)throw Error('oldest waveform not served from SD');
console.log('RESULT '+JSON.stringify({session_id:sessionId,state:session.state,event_count:session.event_count,files_verified:listing.count,total_raw_bytes:bytesTotal,min_rate_hz:minRate,max_rate_hz:maxRate,oldest_wave_source:oldest.raw_source,sd_write_errors:session.sd_write_errors,captures_failed:session.captures_failed}));
