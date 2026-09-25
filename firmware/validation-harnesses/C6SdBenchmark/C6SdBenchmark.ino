#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <esp_heap_caps.h>

constexpr uint32_t SD_HZ=4000000;
constexpr size_t BLOCK_BYTES=64*1024;
constexpr uint64_t TEST_US=600ULL*1000000ULL;
uint8_t *buffer=nullptr;

struct Metrics {
  uint64_t requested=0,actual=0,calls=0,writeUs=0;
  uint32_t maxWriteUs=0,shortWrites=0,zeroWrites=0,errors=0,flushes=0,maxFlushUs=0;
  uint64_t durationUs=0,position=0;
};

void fillBuffer(){uint32_t x=0x13579BDF;for(size_t i=0;i<BLOCK_BYTES;i++){x=x*1664525u+1013904223u;buffer[i]=uint8_t(x>>24);}}

void runTest(uint8_t number,uint32_t targetBps){
  char path[64];snprintf(path,sizeof(path),"/SDBENCH-T%u-%08lX.bin",number,(unsigned long)esp_random());
  File f=SD.open(path,FILE_WRITE);Metrics m;
  if(!f){Serial.printf("{\"type\":\"RESULT\",\"test\":%u,\"open\":false,\"errors\":1,\"pass\":false}\n",number);return;}
  const uint64_t started=esp_timer_get_time();uint64_t lastFlush=started;bool pass=true;
  while(true){
    const uint64_t now=esp_timer_get_time();const uint64_t elapsed=now-started;if(elapsed>=TEST_US)break;
    if(targetBps){
      const uint64_t allowed=(elapsed*targetBps)/1000000ULL;
      if(m.requested+BLOCK_BYTES>allowed){delay(2);continue;}
    }
    const uint64_t began=esp_timer_get_time();const size_t n=f.write(buffer,BLOCK_BYTES);const uint32_t dt=uint32_t(esp_timer_get_time()-began);
    m.requested+=BLOCK_BYTES;m.actual+=n;++m.calls;m.writeUs+=dt;if(dt>m.maxWriteUs)m.maxWriteUs=dt;
    if(n<BLOCK_BYTES){++m.shortWrites;if(n==0)++m.zeroWrites;++m.errors;m.position=f.position();pass=false;Serial.printf("{\"type\":\"SD_ERROR\",\"test\":%u,\"requested\":%u,\"actual\":%u,\"position\":%llu,\"elapsed_us\":%llu}\n",number,unsigned(BLOCK_BYTES),unsigned(n),m.position,esp_timer_get_time()-started);break;}
    const uint64_t after=esp_timer_get_time();
    if(after-lastFlush>=1000000ULL){const uint64_t fb=esp_timer_get_time();f.flush();const uint32_t fd=uint32_t(esp_timer_get_time()-fb);++m.flushes;if(fd>m.maxFlushUs)m.maxFlushUs=fd;lastFlush=esp_timer_get_time();if(!f){++m.errors;pass=false;break;}}
  }
  const uint64_t flushBegan=esp_timer_get_time();f.flush();const uint32_t flushDt=uint32_t(esp_timer_get_time()-flushBegan);++m.flushes;if(flushDt>m.maxFlushUs)m.maxFlushUs=flushDt;
  m.position=f.position();f.close();m.durationUs=esp_timer_get_time()-started;
  Serial.printf("{\"type\":\"RESULT\",\"test\":%u,\"target_Bps\":%u,\"duration_us\":%llu,\"requested\":%llu,\"actual\":%llu,\"calls\":%llu,\"short_writes\":%u,\"zero_writes\":%u,\"errors\":%u,\"avg_write_us\":%llu,\"max_write_us\":%u,\"flushes\":%u,\"max_flush_us\":%u,\"position\":%llu,\"Bps\":%.1f,\"pass\":%s,\"file\":\"%s\"}\n",number,targetBps,m.durationUs,m.requested,m.actual,m.calls,m.shortWrites,m.zeroWrites,m.errors,m.calls?m.writeUs/m.calls:0,m.maxWriteUs,m.flushes,m.maxFlushUs,m.position,m.durationUs?double(m.actual)*1000000.0/double(m.durationUs):0,pass?"true":"false",path);
}

void setup(){
  Serial.begin(115200);delay(1200);pinMode(14,OUTPUT);digitalWrite(14,HIGH);SPI.begin(1,3,2,14);
  const bool sd=SD.begin(4,SPI,SD_HZ);buffer=static_cast<uint8_t*>(heap_caps_malloc(BLOCK_BYTES,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));if(buffer)fillBuffer();
  Serial.printf("{\"type\":\"BOOT\",\"sd\":%s,\"sd_bytes\":%llu,\"sd_hz\":%u,\"block_bytes\":%u,\"buffer\":%s,\"heap\":%u}\n",sd?"true":"false",sd?SD.cardSize():0,SD_HZ,unsigned(BLOCK_BYTES),buffer?"true":"false",ESP.getFreeHeap());
}
void loop(){if(Serial.available()){String c=Serial.readStringUntil('\n');c.trim();c.toUpperCase();if(c=="T1")runTest(1,0);else if(c=="T2")runTest(2,170000);else if(c=="T3")runTest(3,250000);}delay(2);}