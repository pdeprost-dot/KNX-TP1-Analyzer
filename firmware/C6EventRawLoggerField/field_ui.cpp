#include "field_ui.h"
#include "ls1_identity.h"

#include <Arduino_GFX_Library.h>
#include <WebServer.h>
#include <uri/UriRegex.h>
#include <WiFi.h>
#include <Wire.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <SD.h>
#include <esp_crc.h>
#include <esp_heap_caps.h>

namespace fieldui {
namespace {
Arduino_DataBus *lcdBus = nullptr;
Arduino_GFX *lcd = nullptr;
WebServer server(80);
bool lcdReady = false, touchReady = false, dirty = true;
uint32_t lastDrawMs = 0, lastTouchMs = 0;
enum class TouchPhase : uint8_t { Up, StartIntent, StopHolding, WaitRelease };
TouchPhase touchPhase = TouchPhase::Up;
uint32_t touchBeganMs = 0, lastTouchSeenMs = 0, lastCommandMs = 0;
uint8_t validTouchCount = 0;
bool touchGapLogged = false;
bool confirmStop = false;
uint32_t confirmOpenedMs = 0, touchLockUntilMs = 0;
volatile bool touchIrqPending = false;
volatile uint32_t touchIrqCount = 0;
uint32_t touchIrqConsumed = 0;
bool touchContactActive = false;
void ARDUINO_ISR_ATTR touchIrqIsr(){touchIrqPending=true;touchIrqCount++;}
FieldState pressedState = FieldState::Idle;
constexpr uint16_t buttonX0=5,buttonX1=167,buttonY0=278,buttonY1=315;
constexpr uint16_t confirmY0=218,confirmY1=255;
constexpr uint32_t touchGapMs=120,stopHoldMs=700,commandDebounceMs=250;
constexpr uint8_t startObservations=2;
constexpr uint32_t touchPollMs=5,touchLockMs=800,confirmGuardMs=300,confirmTimeoutMs=10000;
char apSsid[32] = {};
char hostName[40] = {};
String staSsid[2], staPassword[2], otaPassword;
uint8_t staProfile = 0, staAttempt = 0;
uint32_t staAttemptMs = 0, staRetryMs = 0;
bool otaActive = false, otaServiceRunning = false, mdnsReady = false;
constexpr uint32_t staAttemptTimeoutMs = 12000, staRetryIntervalMs = 30000;
constexpr char firmwareVersion[] = "field-analyzer-v1";
constexpr char benchmarkPath[] = "/LS5B-BENCH-6191CE7C/raw-0000.bin";
constexpr char networkApiVersion[] = "network-session-api-v1";
struct TransferStats {bool valid=false,sdOnly=false,complete=false,clientDisconnected=false;uint32_t block=0,sdErrors=0,networkShortWrites=0,networkErrors=0,crc=0,heapStart=0,heapMin=0,heapEnd=0;uint64_t requestedBytes=0,bytes=0,durationUs=0,sdReadUs=0,networkUs=0;int32_t rssi=0;};
TransferStats transferStats;
constexpr uint16_t black=0x0000,white=0xFFFF,green=0x07E0,cyan=0x07FF,amber=0xFFE0,red=0xF800;

bool transferAllowed(){FieldState s=fieldSnapshot().state;return s==FieldState::Idle||s==FieldState::Closed;}
uint32_t requestedBlock(){uint32_t n=server.hasArg("block")?uint32_t(server.arg("block").toInt()):32768;if(n!=8192&&n!=16384&&n!=32768&&n!=65536)n=32768;return n;}
String transferJson(){const TransferStats&s=transferStats;String o;o.reserve(420);o="{\"valid\":"+String(s.valid?"true":"false")+",\"mode\":\""+String(s.sdOnly?"SD_ONLY":"HTTP")+"\",\"complete\":"+String(s.complete?"true":"false")+",\"block_bytes\":"+String(s.block)+",\"bytes\":\""+String(s.bytes)+"\",\"duration_us\":\""+String(s.durationUs)+"\",\"sd_read_us\":\""+String(s.sdReadUs)+"\",\"network_us\":\""+String(s.networkUs)+"\",\"crc32\":\"";char crc[9];snprintf(crc,sizeof(crc),"%08X",s.crc);o+=crc;o+="\",\"heap_start\":"+String(s.heapStart)+",\"heap_min\":"+String(s.heapMin)+",\"heap_end\":"+String(s.heapEnd)+",\"sd_errors\":"+String(s.sdErrors)+",\"network_short_writes\":"+String(s.networkShortWrites)+",\"client_disconnected\":"+String(s.clientDisconnected?"true":"false")+",\"sta_ip\":\""+WiFi.localIP().toString()+"\",\"rssi\":"+String(s.rssi)+",\"ap_active\":"+String(WiFi.getMode()==WIFI_AP_STA?"true":"false")+"}";return o;}
void resetTransferStats(uint32_t block,bool sdOnly){transferStats=TransferStats{};transferStats.valid=true;transferStats.sdOnly=sdOnly;transferStats.block=block;transferStats.heapStart=ESP.getFreeHeap();transferStats.heapMin=transferStats.heapStart;transferStats.rssi=WiFi.RSSI();}
#if 0
String transferJsonV2(){
  String o=transferJson();o.remove(o.length()-1);
  o+=",\requested_bytes\:\"+String(transferStats.requestedBytes)+"\";
  o+=",\network_errors\:"+String(transferStats.networkErrors)+"}";
  return o;
}
#endif
uint64_t requestedBytes(uint64_t fileBytes){
  if(!server.hasArg("bytes"))return fileBytes;
  const uint64_t n=strtoull(server.arg("bytes").c_str(),nullptr,10);
  return n>0&&n<fileBytes?n:fileBytes;
}
bool writeAll(WiFiClient&client,const uint8_t*data,size_t length){
  size_t sent=0;uint32_t noProgressSince=millis();
  while(sent<length){
    if(!client.connected()){transferStats.clientDisconnected=true;++transferStats.networkErrors;return false;}
    const size_t amount=min<size_t>(1360,length-sent);const uint64_t t=esp_timer_get_time();
    const size_t written=client.write(data+sent,amount);transferStats.networkUs+=esp_timer_get_time()-t;
    if(written<amount)++transferStats.networkShortWrites;
    if(written){sent+=written;transferStats.bytes+=written;noProgressSince=millis();}
    else if(millis()-noProgressSince>=10000){++transferStats.networkErrors;return false;}
    else delay(1);
  }
  return true;
}
void handleBenchmarkRawV2(){
  if(!transferAllowed()){server.send(409,"application/json","{\error\:\capture_active\}");return;}
  const uint32_t block=requestedBlock();File f=SD.open(benchmarkPath,FILE_READ);
  if(!f){server.send(404,"application/json","{\error\:\benchmark_file_missing\}");return;}
  const uint64_t total=requestedBytes(f.size());uint8_t*buffer=(uint8_t*)heap_caps_malloc(block,MALLOC_CAP_8BIT);
  if(!buffer){f.close();server.send(503,"application/json","{\error\:\buffer_allocation_failed\}");return;}
  resetTransferStats(block,false);transferStats.requestedBytes=total;WiFiClient client=server.client();client.setNoDelay(true);
  server.sendHeader("Cache-Control","no-store");server.sendHeader("Content-Disposition","attachment; filename=raw-0000.bin");server.sendHeader("Connection","close");server.setContentLength(total);server.send(200,"application/octet-stream","");
  const uint64_t started=esp_timer_get_time();
  while(transferStats.bytes<total){
    const uint32_t want=uint32_t(min<uint64_t>(block,total-transferStats.bytes));const uint64_t t=esp_timer_get_time();const int n=f.read(buffer,want);transferStats.sdReadUs+=esp_timer_get_time()-t;
    if(n<=0){++transferStats.sdErrors;break;}transferStats.crc=esp_crc32_le(transferStats.crc,buffer,n);
    if(!writeAll(client,buffer,size_t(n)))break;const uint32_t heap=ESP.getFreeHeap();if(heap<transferStats.heapMin)transferStats.heapMin=heap;yield();
  }
  transferStats.durationUs=esp_timer_get_time()-started;transferStats.heapEnd=ESP.getFreeHeap();transferStats.complete=transferStats.bytes==total&&transferStats.sdErrors==0&&transferStats.networkErrors==0;
  f.close();free(buffer);client.stop();Serial.printf("{\type\:\TRANSFER_BENCH\,\mode\:\HTTP\,\block\:%u,\requested\:%llu,\bytes\:%llu,\duration_us\:%llu,\sd_us\:%llu,\network_us\:%llu,\crc32\:\%08X\,\short_writes\:%u,\network_errors\:%u,\complete\:%s}\n",block,total,transferStats.bytes,transferStats.durationUs,transferStats.sdReadUs,transferStats.networkUs,transferStats.crc,transferStats.networkShortWrites,transferStats.networkErrors,transferStats.complete?"true":"false");
}
void handleBenchmarkRaw(){
  if(!transferAllowed()){server.send(409,"application/json","{\"error\":\"capture_active\"}");return;}uint32_t block=requestedBlock();File f=SD.open(benchmarkPath,FILE_READ);if(!f){server.send(404,"application/json","{\"error\":\"benchmark_file_missing\"}");return;}uint64_t total=f.size();uint8_t*buffer=(uint8_t*)heap_caps_malloc(block,MALLOC_CAP_8BIT);if(!buffer){f.close();server.send(503,"application/json","{\"error\":\"buffer_allocation_failed\"}");return;}resetTransferStats(block,false);WiFiClient client=server.client();client.setNoDelay(true);server.sendHeader("Cache-Control","no-store");server.sendHeader("Content-Disposition","attachment; filename=raw-0000.bin");server.sendHeader("Connection","close");server.setContentLength(total);server.send(200,"application/octet-stream","");uint64_t started=esp_timer_get_time();while(transferStats.bytes<total){uint32_t want=uint32_t(min<uint64_t>(block,total-transferStats.bytes));uint64_t t=esp_timer_get_time();int n=f.read(buffer,want);transferStats.sdReadUs+=esp_timer_get_time()-t;if(n<=0){++transferStats.sdErrors;break;}transferStats.crc=esp_crc32_le(transferStats.crc,buffer,n);t=esp_timer_get_time();size_t w=client.write(buffer,n);transferStats.networkUs+=esp_timer_get_time()-t;if(w!=size_t(n)){++transferStats.networkShortWrites;transferStats.bytes+=w;transferStats.clientDisconnected=!client.connected();break;}transferStats.bytes+=w;uint32_t heap=ESP.getFreeHeap();if(heap<transferStats.heapMin)transferStats.heapMin=heap;yield();}transferStats.durationUs=esp_timer_get_time()-started;transferStats.heapEnd=ESP.getFreeHeap();transferStats.complete=transferStats.bytes==total&&transferStats.sdErrors==0&&transferStats.networkShortWrites==0;f.close();free(buffer);client.stop();Serial.printf("{\"type\":\"TRANSFER_BENCH\",\"mode\":\"HTTP\",\"block\":%u,\"bytes\":%llu,\"duration_us\":%llu,\"sd_us\":%llu,\"network_us\":%llu,\"crc32\":\"%08X\",\"complete\":%s}\n",block,transferStats.bytes,transferStats.durationUs,transferStats.sdReadUs,transferStats.networkUs,transferStats.crc,transferStats.complete?"true":"false");
}
void handleBenchmarkSd(){
  if(!transferAllowed()){server.send(409,"application/json","{\"error\":\"capture_active\"}");return;}uint32_t block=requestedBlock();File f=SD.open(benchmarkPath,FILE_READ);if(!f){server.send(404,"application/json","{\"error\":\"benchmark_file_missing\"}");return;}uint64_t total=f.size();uint8_t*buffer=(uint8_t*)heap_caps_malloc(block,MALLOC_CAP_8BIT);if(!buffer){f.close();server.send(503,"application/json","{\"error\":\"buffer_allocation_failed\"}");return;}resetTransferStats(block,true);uint64_t started=esp_timer_get_time();while(transferStats.bytes<total){uint32_t want=uint32_t(min<uint64_t>(block,total-transferStats.bytes));uint64_t t=esp_timer_get_time();int n=f.read(buffer,want);transferStats.sdReadUs+=esp_timer_get_time()-t;if(n<=0){++transferStats.sdErrors;break;}transferStats.crc=esp_crc32_le(transferStats.crc,buffer,n);transferStats.bytes+=n;uint32_t heap=ESP.getFreeHeap();if(heap<transferStats.heapMin)transferStats.heapMin=heap;yield();}transferStats.durationUs=esp_timer_get_time()-started;transferStats.heapEnd=ESP.getFreeHeap();transferStats.complete=transferStats.bytes==total&&transferStats.sdErrors==0;f.close();free(buffer);server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",transferJson());
}

void startStaAttempt(uint8_t profile){
  if(profile>1||staSsid[profile].isEmpty())return;
  staProfile=profile;staAttempt=profile+1;staAttemptMs=millis();
  WiFi.begin(staSsid[profile].c_str(),staPassword[profile].c_str());
  Serial.printf("{\"type\":\"WIFI_STA_ATTEMPT\",\"profile\":%u,\"ssid\":\"%s\"}\n",profile+1,staSsid[profile].c_str());
}
void beginStaCycle(){if(!staSsid[0].isEmpty())startStaAttempt(0);else if(!staSsid[1].isEmpty())startStaAttempt(1);else{staAttempt=0;staRetryMs=millis();}}
void tickSta(){
  if(WiFi.status()==WL_CONNECTED){staAttempt=0;return;}
  uint32_t now=millis();
  if(staAttempt&&now-staAttemptMs>=staAttemptTimeoutMs){WiFi.disconnect(false,false);if(staProfile==0&&!staSsid[1].isEmpty())startStaAttempt(1);else{staAttempt=0;staRetryMs=now;Serial.println("{\"type\":\"WIFI_STA\",\"state\":\"UNAVAILABLE\",\"ap_preserved\":true}");}}
  else if(!staAttempt&&now-staRetryMs>=staRetryIntervalMs)beginStaCycle();
}
void loadNetworkConfig(){
  Preferences p;p.begin("knx-net",false);staSsid[0]=p.getString("ssid1","");staPassword[0]=p.getString("pass1","");staSsid[1]=p.getString("ssid2","");staPassword[1]=p.getString("pass2","");otaPassword=p.getString("ota-pass","");
  if(otaPassword.length()<12){char generated[17];snprintf(generated,sizeof(generated),"%08lX%08lX",(unsigned long)esp_random(),(unsigned long)esp_random());otaPassword=generated;p.putString("ota-pass",otaPassword);Serial.printf("{\"type\":\"OTA_PASSWORD_CREATED\",\"password\":\"%s\"}\n",otaPassword.c_str());}p.end();
}
String networkJson(){String o;o.reserve(480);o="{\"ap_ssid\":\""+String(apSsid)+"\",\"ap_ip\":\""+WiFi.softAPIP().toString()+"\",\"sta_state\":\""+(WiFi.status()==WL_CONNECTED?String("CONNECTED"):staAttempt?String("CONNECTING"):String("DISCONNECTED"))+"\",\"sta_ssid\":\""+(WiFi.status()==WL_CONNECTED?WiFi.SSID():String(""))+"\",\"sta_ip\":\""+(WiFi.status()==WL_CONNECTED?WiFi.localIP().toString():String(""))+"\",\"rssi\":"+String(WiFi.status()==WL_CONNECTED?WiFi.RSSI():0)+",\"hostname\":\""+String(hostName)+"\",\"ssid1\":\""+staSsid[0]+"\",\"ssid2\":\""+staSsid[1]+"\",\"firmware_version\":\""+String(firmwareVersion)+"\",\"ota_available\":"+String((fieldSnapshot().state==FieldState::Idle||fieldSnapshot().state==FieldState::Closed)&&!otaActive?"true":"false")+"}";return o;}

const char *stateName(FieldState s){switch(s){case FieldState::Capturing:return "CAPTURING";case FieldState::Finalizing:return "FINALIZING";case FieldState::Closed:return "CLOSED";default:return "IDLE";}}
void text(int x,int y,const String&v,uint16_t color=white,uint8_t size=1){lcd->setTextColor(color);lcd->setTextSize(size);lcd->setCursor(x,y);lcd->print(v);}
String durationText(uint64_t us){uint32_t s=us/1000000ULL;char out[16];snprintf(out,sizeof(out),"%02lu:%02lu:%02lu",s/3600,(s/60)%60,s%60);return out;}
String sizeText(uint64_t b){return b>=1048576?String(b/1048576.0,2)+" MB":String(b/1024.0,1)+" kB";}
void buttonAt(uint16_t y,const char*label,uint16_t color){lcd->drawRect(5,y,162,37,color);text(max(10,86-int(strlen(label))*3),y+12,label,color);}
void button(const char*label,uint16_t color){buttonAt(278,label,color);}

void draw(){
  if(!lcdReady)return;FieldSnapshot s=fieldSnapshot();lcd->fillScreen(black);text(6,6,"KNX TP1",cyan,2);text(6,25,"ANALYZER",cyan,2);
  if(s.state==FieldState::Capturing&&confirmStop){
    text(6,52,"CONFIRM STOP",amber,2);text(6,92,"STOP CAPTURE?",red,2);text(6,126,"Acquisition continues",white);text(6,144,String("Session ")+s.session);text(6,162,String("Duration ")+durationText(s.durationUs));buttonAt(confirmY0,"CONFIRM STOP",red);buttonAt(buttonY0,"CANCEL",cyan);
  }else{
    uint16_t color=s.state==FieldState::Capturing?green:s.state==FieldState::Finalizing?amber:s.state==FieldState::Closed?cyan:white;text(6,52,stateName(s.state),color,2);
    if(s.state==FieldState::Idle){
      text(6,82,String("SD          ")+(s.sdReady?"READY":"ERROR"),s.sdReady?green:red);text(6,100,String("ADC         ")+(s.adcReady?"READY":"ERROR"),s.adcReady?green:red);text(6,118,"Sample rate ~83.3 kS/s");text(6,136,"Wi-Fi AP    ON",green);text(6,154,String("Sessions SD ")+s.sessions);text(6,181,apSsid,cyan);text(6,199,"192.168.4.1",white,2);button("START CAPTURE",s.sdReady&&s.adcReady?green:red);
    }else if(s.state==FieldState::Capturing||s.state==FieldState::Finalizing){
      text(6,79,String("Session ")+s.session);text(6,97,String("Duration ")+durationText(s.durationUs));text(6,115,String("Events   ")+s.events);text(6,133,String("RAW      ")+sizeText(s.rawBytes));text(6,151,String("D44 max  ")+s.d44Max);text(6,169,String("SD ")+(s.sdErrors?"ERROR":"OK")+"   R1 "+s.recoveredR1,s.sdErrors?red:white);text(6,187,String("Loss ")+s.loss+"   DMA "+s.dmaOverflow,(s.loss||s.dmaOverflow)?red:white);text(6,205,String("Pool ")+s.poolExhaustion,s.poolExhaustion?red:white);if(s.recoveredR1)text(6,231,String("WARNING R1 recovered: ")+s.recoveredR1,amber);button(s.state==FieldState::Capturing?"STOP & CLOSE":"PLEASE WAIT",s.state==FieldState::Capturing?red:amber);
    }else{
      text(6,78,s.closed?"SESSION SAVED":"CAPTURE STOPPED",s.closed?green:red,2);text(6,105,s.session,cyan);text(6,127,String("Duration ")+durationText(s.durationUs));text(6,145,String("Events   ")+s.events);text(6,163,String("RAW      ")+sizeText(s.rawBytes));text(6,181,String("Loss ")+s.loss+" DMA "+s.dmaOverflow);text(6,199,String("Pool ")+s.poolExhaustion+" R1 "+s.recoveredR1);text(6,217,String("SD errors ")+s.sdErrors,s.sdErrors?red:white);button("NEW SESSION",cyan);
    }
  }
  lastDrawMs=millis();dirty=false;
}
enum class TouchSample : uint8_t { None, Error, Released, Point };
TouchSample readTouchSample(uint16_t&x,uint16_t&y,bool latched=false){if(!touchReady||(!latched&&digitalRead(21)!=LOW))return TouchSample::None;Wire.beginTransmission(0x63);Wire.write(0x01);if(Wire.endTransmission(true)!=0||Wire.requestFrom(uint8_t(0x63),uint8_t(6))!=6)return TouchSample::Error;uint8_t d[6];for(uint8_t&v:d)v=Wire.read();if((d[1]&15)==0)return TouchSample::Released;uint16_t rx=((d[2]&15)<<8)|d[3],ry=((d[4]&15)<<8)|d[5];if((!rx&&!ry)||rx>=172||ry>=320)return TouchSample::Error;x=171-rx;y=319-ry;return TouchSample::Point;}
void command(FieldState s){bool ok=s==FieldState::Idle?fieldStartCapture():s==FieldState::Capturing?fieldStopCapture():s==FieldState::Closed?fieldNewSession():false;if(ok)dirty=true;}

String statusJson(){FieldSnapshot s=fieldSnapshot();String o;o.reserve(420);o+="{\"state\":\"";o+=stateName(s.state);o+="\",\"sd\":\"";o+=s.sdReady?"READY":"ERROR";o+="\",\"adc\":\"";o+=s.adcReady?"READY":"ERROR";o+="\",\"sample_rate_hz\":83333,\"session\":\"";o+=s.session;o+="\",\"duration_us\":";o+=String(s.durationUs);o+=",\"events\":";o+=s.events;o+=",\"raw_bytes\":";o+=String(s.rawBytes);o+=",\"d44_max\":";o+=s.d44Max;o+=",\"recovered_R1\":";o+=s.recoveredR1;o+=",\"data_loss\":";o+=String(s.loss);o+=",\"dma_overflow\":";o+=s.dmaOverflow;o+=",\"pool_exhaustion\":";o+=s.poolExhaustion;o+=",\"sd_errors\":";o+=s.sdErrors;o+=",\"sessions\":";o+=s.sessions;o+=",\"closed\":";o+=s.closed?"true}":"false}";return o;}

constexpr char page[] PROGMEM=R"HTML(<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1"><title>KNX TP1 Analyzer</title><style>body{font:16px system-ui;background:#101820;color:#eee;margin:auto;max-width:520px;padding:20px}h1{color:#53d8fb}.card{background:#1c2935;padding:18px;border-radius:14px}.state{font-size:1.7em;color:#5ee381}dl{display:grid;grid-template-columns:1fr 1fr;gap:8px}dt{color:#9fb3c8}dd{margin:0;text-align:right}button{width:100%;padding:16px;margin-top:18px;font-weight:bold;font-size:1em;border:0;border-radius:10px;background:#29c46a}button.stop{background:#ee5353}button.new{background:#53d8fb}button:disabled{background:#667}</style></head><body><h1>KNX TP1 Analyzer</h1><div class=card><div id=state class=state>...</div><div id=session></div><dl id=stats></dl><button id=action disabled>...</button></div><script>const q=s=>document.querySelector(s);let busy=false,failures=0;function size(n){return n>1048576?(n/1048576).toFixed(2)+' MB':(n/1024).toFixed(1)+' kB'}function time(u){let s=Math.floor(u/1e6);return [Math.floor(s/3600),Math.floor(s/60)%60,s%60].map(x=>String(x).padStart(2,'0')).join(':')}async function poll(){try{let c=new AbortController(),t=setTimeout(()=>c.abort(),350);let d=await fetch('/api/status',{cache:'no-store',signal:c.signal}).then(r=>r.json());clearTimeout(t);failures=0;q('#state').textContent=d.state;q('#session').textContent=d.session||'';q('#stats').innerHTML=`<dt>SD / ADC</dt><dd>${d.sd} / ${d.adc}</dd><dt>Sample rate</dt><dd>83.3 kS/s</dd><dt>Duration</dt><dd>${time(d.duration_us)}</dd><dt>Events</dt><dd>${d.events}</dd><dt>RAW</dt><dd>${size(d.raw_bytes)}</dd><dt>D44 max</dt><dd>${d.d44_max}</dd><dt>R1 / SD errors</dt><dd>${d.recovered_R1} / ${d.sd_errors}</dd><dt>Loss / DMA / Pool</dt><dd>${d.data_loss} / ${d.dma_overflow} / ${d.pool_exhaustion}</dd>`;let b=q('#action');b.className='';if(d.state==='IDLE'){b.textContent='START CAPTURE';b.dataset.cmd='start'}else if(d.state==='CAPTURING'){b.textContent='STOP & CLOSE';b.dataset.cmd='stop';b.className='stop'}else if(d.state==='CLOSED'){b.textContent='NEW SESSION';b.dataset.cmd='new';b.className='new'}else{b.textContent='FINALIZING...';b.dataset.cmd=''}b.disabled=busy||!b.dataset.cmd}catch(e){if(q('#state').textContent==='CAPTURING'&&++failures>=2){q('#state').textContent='FINALIZING';q('#action').textContent='PLEASE WAIT';q('#action').dataset.cmd='';q('#action').disabled=true}}setTimeout(poll,500)}q('#action').onclick=async e=>{busy=true;e.target.disabled=true;await fetch('/api/'+e.target.dataset.cmd,{method:'POST'});busy=false};poll()</script></body></html>)HTML";
constexpr char networkPage[] PROGMEM=R"HTML(<!doctype html><html><head><meta name=viewport content="width=device-width,initial-scale=1"><title>KNX Network</title><style>body{font:16px system-ui;background:#101820;color:#eee;margin:auto;max-width:520px;padding:20px}h1{color:#53d8fb}.card{background:#1c2935;padding:18px;border-radius:14px}label{display:block;margin-top:12px;color:#9fb3c8}input{box-sizing:border-box;width:100%;padding:10px;background:#101820;color:#fff;border:1px solid #667;border-radius:6px}button{width:100%;padding:14px;margin-top:18px;background:#53d8fb;border:0;border-radius:8px;font-weight:bold}pre{white-space:pre-wrap}</style></head><body><h1>NETWORK</h1><div class=card><pre id=s>Loading...</pre><form id=f><label>SSID 1<input name=ssid1 id=s1></label><label>Password 1 (blank = unchanged)<input name=pass1 type=password></label><label>SSID 2<input name=ssid2 id=s2></label><label>Password 2 (blank = unchanged)<input name=pass2 type=password></label><label>New OTA password (12+ chars, blank = unchanged)<input name=otapass type=password minlength=12></label><button>SAVE / APPLY</button></form></div><script>let loaded=false;async function poll(){let d=await fetch('/api/network',{cache:'no-store'}).then(r=>r.json());document.querySelector('#s').textContent=`AP: ${d.ap_ssid}  ${d.ap_ip}\nSTA: ${d.sta_state} ${d.sta_ssid} ${d.sta_ip}\nRSSI: ${d.rssi} dBm\nHostname: ${d.hostname}.local\nFirmware: ${d.firmware_version}\nOTA available: ${d.ota_available}`;if(!loaded){s1.value=d.ssid1;s2.value=d.ssid2;loaded=true}setTimeout(poll,1000)}f.onsubmit=async e=>{e.preventDefault();let r=await fetch('/api/network',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(new FormData(f))});if(!r.ok){alert(await r.text());return}location.reload()};poll()</script></body></html>)HTML";
void reply(bool ok,const char*state){server.sendHeader("Cache-Control","no-store");server.send(ok?202:409,"application/json",ok?String("{\"accepted\":true,\"state\":\"")+state+"\"}":"{\"accepted\":false,\"error\":\"invalid_state\"}");if(ok)dirty=true;}
}

#include "network_session_api.h"
bool begin(){
  const char* collectedHeaders[]={"Range"};server.collectHeaders(collectedHeaders,1);
  server.on("/api/analyzer",HTTP_GET,nsAnalyzer);server.on("/api/sessions",HTTP_GET,nsSessions);server.on(UriRegex("^\\/api\\/sessions\\/.*$"),HTTP_GET,[]{nsDispatch();});
  Wire.begin(18,19);Wire.setClock(100000);pinMode(21,INPUT_PULLUP);pinMode(20,OUTPUT);digitalWrite(20,LOW);delay(10);digitalWrite(20,HIGH);delay(100);Wire.beginTransmission(0x63);touchReady=Wire.endTransmission()==0;if(touchReady)attachInterrupt(digitalPinToInterrupt(21),touchIrqIsr,FALLING);
  pinMode(23,OUTPUT);digitalWrite(23,LOW);lcdBus=new Arduino_HWSPI(15,14,1,2,3);lcd=new Arduino_ST7789(lcdBus,22,0,false,172,320,34,0,34,0);lcdReady=lcd&&lcd->begin(40000000);if(lcdReady){lcd->invertDisplay(true);lcd->setRotation(6);digitalWrite(23,HIGH);}
  uint64_t mac=ESP.getEfuseMac();snprintf(apSsid,sizeof(apSsid),"KNX-Analyzer-%04X",uint16_t(mac));snprintf(hostName,sizeof(hostName),"knx-analyzer-%04x",uint16_t(mac));loadNetworkConfig();WiFi.mode(WIFI_AP_STA);WiFi.setHostname(hostName);bool apReady=WiFi.softAP(apSsid);beginStaCycle();
  server.on("/",HTTP_GET,[]{server.send_P(200,"text/html",page);});server.on("/network",HTTP_GET,[]{server.send_P(200,"text/html",networkPage);});server.on("/api/status",HTTP_GET,[]{server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",statusJson());});server.on("/api/network",HTTP_GET,[]{server.sendHeader("Cache-Control","no-store");server.send(200,"application/json",networkJson());});server.on("/api/network",HTTP_POST,[]{Preferences p;p.begin("knx-net",false);String n1=server.arg("ssid1"),n2=server.arg("ssid2"),pw1=server.arg("pass1"),pw2=server.arg("pass2"),op=server.arg("otapass");if(!op.isEmpty()&&op.length()<12){p.end();server.send(400,"application/json","{\"saved\":false,\"error\":\"ota_password_too_short\"}");return;}p.putString("ssid1",n1);p.putString("ssid2",n2);if(n1.isEmpty())p.remove("pass1");else if(!pw1.isEmpty())p.putString("pass1",pw1);if(n2.isEmpty())p.remove("pass2");else if(!pw2.isEmpty())p.putString("pass2",pw2);if(!op.isEmpty())p.putString("ota-pass",op);p.end();staSsid[0]=n1;staSsid[1]=n2;if(n1.isEmpty())staPassword[0]="";else if(!pw1.isEmpty())staPassword[0]=pw1;if(n2.isEmpty())staPassword[1]="";else if(!pw2.isEmpty())staPassword[1]=pw2;if(!op.isEmpty()){otaPassword=op;ArduinoOTA.setPassword(otaPassword.c_str());}WiFi.disconnect(false,false);staAttempt=0;staRetryMs=millis()-staRetryIntervalMs;server.send(200,"application/json","{\"saved\":true,\"ap_preserved\":true}");});server.on("/api/start",HTTP_POST,[]{bool ok=fieldStartCapture();reply(ok,"CAPTURING");});server.on("/api/stop",HTTP_POST,[]{bool ok=fieldStopCapture();reply(ok,"FINALIZING");});server.on("/api/new",HTTP_POST,[]{bool ok=fieldNewSession();reply(ok,"IDLE");});server.onNotFound([]{server.sendHeader("Location","/");server.send(302,"text/plain","");});server.begin();ArduinoOTA.setHostname(hostName);ArduinoOTA.setPassword(otaPassword.c_str());ArduinoOTA.setMdnsEnabled(false);ArduinoOTA.onStart([]{otaActive=true;Serial.printf("{\"type\":\"OTA_START\",\"version\":\"%s\"}\n",firmwareVersion);});ArduinoOTA.onEnd([]{Serial.println("{\"type\":\"OTA_END\",\"validation\":\"OK\",\"reboot\":true}");});ArduinoOTA.onError([](ota_error_t e){otaActive=false;Serial.printf("{\"type\":\"OTA_ERROR\",\"code\":%u}\n",unsigned(e));});mdnsReady=MDNS.begin(hostName);if(mdnsReady){MDNS.addService("http","tcp",80);MDNS.enableArduino(3232,true);}ArduinoOTA.begin();otaServiceRunning=true;dirty=true;draw();Serial.printf("{\"type\":\"FIELD_UI\",\"lcd\":%s,\"touch\":%s,\"ap\":%s,\"ssid\":\"%s\",\"ip\":\"%s\",\"hostname\":\"%s\",\"mdns\":%s,\"firmware_version\":\"%s\",\"ota_auth\":\"PBKDF2-HMAC-SHA256\"}\n",lcdReady?"true":"false",touchReady?"true":"false",apReady?"true":"false",apSsid,WiFi.softAPIP().toString().c_str(),hostName,mdnsReady?"true":"false",firmwareVersion);return lcdReady&&apReady;
}
void tick(){
  server.handleClient();tickSta();FieldState networkState=fieldSnapshot().state;bool otaAllowed=networkState==FieldState::Idle||networkState==FieldState::Closed;if(!otaAllowed&&otaServiceRunning){ArduinoOTA.end();if(mdnsReady)MDNS.disableArduino();otaServiceRunning=false;otaActive=false;Serial.println("{\"type\":\"OTA_SERVICE\",\"state\":\"REFUSED_CAPTURE_ACTIVE\"}");}else if(otaAllowed&&!otaServiceRunning){ArduinoOTA.begin();if(mdnsReady)MDNS.enableArduino(3232,true);otaServiceRunning=true;Serial.println("{\"type\":\"OTA_SERVICE\",\"state\":\"AVAILABLE\"}");}if(otaAllowed&&otaServiceRunning)ArduinoOTA.handle();const uint32_t nowMs=millis();FieldSnapshot snapshot=fieldSnapshot();
  if(confirmStop&&(snapshot.state!=FieldState::Capturing||nowMs-confirmOpenedMs>=confirmTimeoutMs)){Serial.println("{\"type\":\"CONFIRM_STOP_TIMEOUT\"}");confirmStop=false;touchLockUntilMs=nowMs+touchLockMs;dirty=true;}
  bool pending=false;uint32_t irqTotal=0;
  noInterrupts();pending=touchIrqPending;if(pending)touchIrqPending=false;irqTotal=touchIrqCount;interrupts();
  const bool fallback=!pending&&nowMs-lastTouchMs>=20&&digitalRead(21)==LOW;
  if(pending||fallback){
    lastTouchMs=nowMs;touchIrqConsumed=irqTotal;uint16_t x=0,y=0;const TouchSample sample=readTouchSample(x,y,pending);
    if(sample==TouchSample::Released){touchContactActive=false;}
    else if(sample==TouchSample::Point&&!touchContactActive){
      touchContactActive=true;snapshot=fieldSnapshot();
      if(nowMs>=touchLockUntilMs){
        const bool mainHit=x>=buttonX0&&x<=buttonX1&&y>=buttonY0&&y<=buttonY1;
        const bool confirmHit=x>=buttonX0&&x<=buttonX1&&y>=confirmY0&&y<=confirmY1;
        if(confirmStop&&snapshot.state==FieldState::Capturing){
          if(nowMs-confirmOpenedMs>=confirmGuardMs&&confirmHit){Serial.println("{\"type\":\"TOUCH_STOP_CONFIRMED\"}");confirmStop=false;touchLockUntilMs=nowMs+touchLockMs;if(fieldStopCapture())dirty=true;}
          else if(nowMs-confirmOpenedMs>=confirmGuardMs&&mainHit){Serial.println("{\"type\":\"TOUCH_STOP_CANCELLED\"}");confirmStop=false;touchLockUntilMs=nowMs+touchLockMs;dirty=true;}
        }else if(mainHit){
          if(snapshot.state==FieldState::Idle){Serial.println("{\"type\":\"TOUCH_START_CONFIRMED\"}");touchLockUntilMs=nowMs+touchLockMs;if(fieldStartCapture())dirty=true;}
          else if(snapshot.state==FieldState::Closed){Serial.println("{\"type\":\"TOUCH_NEW_SESSION_CONFIRMED\"}");touchLockUntilMs=nowMs+touchLockMs;if(fieldNewSession())dirty=true;}
          else if(snapshot.state==FieldState::Capturing){Serial.println("{\"type\":\"TOUCH_STOP_CONFIRM_OPEN\"}");confirmStop=true;confirmOpenedMs=nowMs;touchLockUntilMs=nowMs+confirmGuardMs;dirty=true;}
        }
      }
    }
  }
  snapshot=fieldSnapshot();uint32_t interval=snapshot.state==FieldState::Capturing?500:1000;if(dirty||(snapshot.state!=FieldState::Capturing&&millis()-lastDrawMs>=interval))draw();
}
bool otaBusy(){return otaActive;}
}
