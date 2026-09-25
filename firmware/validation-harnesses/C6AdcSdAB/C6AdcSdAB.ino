#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <esp_adc/adc_continuous.h>
#include <esp_heap_caps.h>
#include <atomic>

constexpr uint32_t SAMPLE_RATE=83333, SD_HZ=4000000;
constexpr size_t BUFFER_BYTES=16*1024, SAMPLES_PER_BUFFER=BUFFER_BYTES/2, BUFFER_COUNT=2;
constexpr size_t DMA_BYTES=1024, DMA_POOL_BYTES=49152;
constexpr uint64_t TEST_US=600ULL*1000000ULL;

struct Buffer { uint32_t count; uint16_t samples[SAMPLES_PER_BUFFER]; };
Buffer *buffers[BUFFER_COUNT]{};
QueueHandle_t freeQ=nullptr, readyQ=nullptr;
adc_continuous_handle_t adc=nullptr;
File raw;
std::atomic<bool> active{false}, stopping{false}, substitute{false}, adcDrained{true};
std::atomic<uint64_t> acquired{0}, written{0}, sdBytes{0}, writes{0};
std::atomic<uint32_t> dmaOverflow{0}, bufferOverflow{0}, shortWrites{0}, zeroWrites{0}, sdErrors{0};
volatile uint32_t dmaOverflowIsr=0;
uint64_t startedUs=0; uint32_t writeDurations[8192]{};
std::atomic<uint64_t> writeTotalUs{0}, writeStartedUs{0}, firstOverflowAtUs{0}, firstOverflowAcquired{0}, firstOverflowWritten{0};
std::atomic<uint32_t> writeMaxUs{0}, lastWriteUs{0}, over50{0}, over100{0}, over200{0}, over500{0}, over1000{0}, firstOverflowWriteUs{0}, firstOverflowReady{0}, firstOverflowFree{0}, firstOverflowWriteIndex{0};

bool IRAM_ATTR onOverflow(adc_continuous_handle_t,const adc_continuous_evt_data_t*,void*) { ++dmaOverflowIsr; return false; }

bool initAdc() {
  adc_continuous_handle_cfg_t h{}; h.max_store_buf_size=DMA_POOL_BYTES; h.conv_frame_size=DMA_BYTES;
  if(adc_continuous_new_handle(&h,&adc)!=ESP_OK) return false;
  adc_digi_pattern_config_t p{}; p.atten=ADC_ATTEN_DB_12; p.channel=ADC_CHANNEL_5; p.unit=ADC_UNIT_1; p.bit_width=SOC_ADC_DIGI_MAX_BITWIDTH;
  adc_continuous_config_t c{}; c.pattern_num=1; c.adc_pattern=&p; c.sample_freq_hz=SAMPLE_RATE; c.conv_mode=ADC_CONV_SINGLE_UNIT_1; c.format=ADC_DIGI_OUTPUT_FORMAT_TYPE2;
  if(adc_continuous_config(adc,&c)!=ESP_OK) return false;
  adc_continuous_evt_cbs_t cb{}; cb.on_pool_ovf=onOverflow;
  return adc_continuous_register_event_callbacks(adc,&cb,nullptr)==ESP_OK;
}

void logStage(const char *stage) { Serial.printf("{\"type\":\"STOP_STAGE\",\"stage\":\"%s\",\"acquired\":%llu,\"written\":%llu,\"buffer_overflow\":%u,\"dma_overflow\":%u,\"ready_depth\":%u,\"free_depth\":%u}\n",stage,acquired.load(),written.load(),bufferOverflow.load(),dmaOverflow.load(),unsigned(uxQueueMessagesWaiting(readyQ)),unsigned(uxQueueMessagesWaiting(freeQ))); } void adcTask(void*) {
  alignas(4) uint8_t dma[DMA_BYTES]; Buffer *cur=nullptr;
  while(true) {
    if(!active) { if(cur&&cur->count){xQueueSend(readyQ,&cur,portMAX_DELAY);cur=nullptr;} adcDrained=true; vTaskDelay(pdMS_TO_TICKS(2)); continue; }
    uint32_t bytes=0; esp_err_t e=adc_continuous_read(adc,dma,sizeof(dma),&bytes,50); dmaOverflow=dmaOverflowIsr;
    if(e==ESP_ERR_TIMEOUT) continue; if(e!=ESP_OK) continue;
    for(uint32_t off=0; off+SOC_ADC_DIGI_RESULT_BYTES<=bytes; off+=SOC_ADC_DIGI_RESULT_BYTES) {
      const auto *r=reinterpret_cast<const adc_digi_output_data_t*>(dma+off);
      if(r->type2.channel!=ADC_CHANNEL_5) continue;
      ++acquired;
      if(!cur && xQueueReceive(freeQ,&cur,0)!=pdTRUE) { if(bufferOverflow.fetch_add(1)==0){const uint64_t now=esp_timer_get_time(),ws=writeStartedUs.load();firstOverflowAtUs=now-startedUs;firstOverflowAcquired=acquired.load();firstOverflowWritten=written.load();firstOverflowWriteUs=ws?uint32_t(now-ws):lastWriteUs.load();firstOverflowReady=uxQueueMessagesWaiting(readyQ);firstOverflowFree=uxQueueMessagesWaiting(freeQ);firstOverflowWriteIndex=uint32_t(writes.load());} continue; }
      cur->samples[cur->count++]=r->type2.data;
      if(cur->count==SAMPLES_PER_BUFFER) { xQueueSend(readyQ,&cur,portMAX_DELAY); cur=nullptr; }
    }
  }
}

void writerTask(void*) {
  while(true) {
    Buffer *b=nullptr;
    if(xQueueReceive(readyQ,&b,pdMS_TO_TICKS(20))==pdTRUE) {
      if(substitute) for(uint32_t i=0;i<b->count;i++) b->samples[i]=0x5555;
      const size_t req=b->count*sizeof(uint16_t); const bool partial=req<BUFFER_BYTES; const uint64_t before=raw?raw.position():0;
      if(partial) Serial.printf("{\"type\":\"FINAL_WRITE_BEFORE\",\"buffer\":\"%p\",\"offset\":0,\"length\":%u,\"position\":%llu,\"file\":%s,\"heap_ok\":%s}\n",b->samples,unsigned(req),before,raw?"true":"false",heap_caps_check_integrity_all(false)?"true":"false");
      const uint64_t wb=esp_timer_get_time(); writeStartedUs=wb; const size_t n=raw?raw.write(reinterpret_cast<uint8_t*>(b->samples),req):0; const uint32_t writeUs=uint32_t(esp_timer_get_time()-wb); writeStartedUs=0; lastWriteUs=writeUs; writeTotalUs+=writeUs; if(writeUs>writeMaxUs)writeMaxUs=writeUs; if(writeUs>50000)++over50; if(writeUs>100000)++over100; if(writeUs>200000)++over200; if(writeUs>500000)++over500; if(writeUs>1000000)++over1000; const uint64_t wi=writes.fetch_add(1); if(wi<8192)writeDurations[wi]=writeUs;
      if(partial) { const uint64_t after=raw?uint64_t(raw.position()):0; Serial.printf("{\"type\":\"FINAL_WRITE_AFTER\",\"result\":%u,\"position\":%llu,\"file\":%s,\"heap_ok\":%s}\n",unsigned(n),after,raw?"true":"false",heap_caps_check_integrity_all(false)?"true":"false"); logStage("FINAL_WRITE_DONE"); }
      sdBytes+=n; written+=n/sizeof(uint16_t);
      if(n!=req) { ++shortWrites; if(n==0)++zeroWrites; ++sdErrors; Serial.printf("{\"type\":\"SD_ERROR\",\"requested\":%u,\"actual\":%u}\n",unsigned(req),unsigned(n)); }
      b->count=0; xQueueSend(freeQ,&b,portMAX_DELAY);
    }
    if(stopping&&uxQueueMessagesWaiting(readyQ)==0) { if(raw)raw.flush(); stopping=false; }
  }
}

void finish() {
  if(!active&&!stopping) return;
  logStage("STOP_REQUEST"); active=false; adc_continuous_stop(adc); logStage("ADC_STOPPED"); uint32_t adcDeadline=millis()+1000; while(!adcDrained&&int32_t(adcDeadline-millis())>0) delay(1); logStage("ADC_DRAINED"); stopping=true;
  uint32_t deadline=millis()+5000; while(stopping&&int32_t(deadline-millis())>0) delay(2); logStage("WRITER_DRAINED");
  if(raw){raw.flush();raw.close();} logStage("CLOSED"); Serial.printf("{\"type\":\"WRITE_STATS\",\"avg_us\":%llu,\"max_us\":%u,\"gt50\":%u,\"gt100\":%u,\"gt200\":%u,\"gt500\":%u,\"gt1000\":%u}\n",writes.load()?writeTotalUs.load()/writes.load():0,writeMaxUs.load(),over50.load(),over100.load(),over200.load(),over500.load(),over1000.load()); Serial.printf("{\"type\":\"FIRST_OVERFLOW\",\"at_us\":%llu,\"acquired\":%llu,\"written\":%llu,\"write_us\":%u,\"ready\":%u,\"free\":%u,\"last5\":[%u,%u,%u,%u,%u]}\n",firstOverflowAtUs.load(),firstOverflowAcquired.load(),firstOverflowWritten.load(),firstOverflowWriteUs.load(),firstOverflowReady.load(),firstOverflowFree.load(),firstOverflowWriteIndex.load()>4?writeDurations[firstOverflowWriteIndex.load()-5]:0,firstOverflowWriteIndex.load()>3?writeDurations[firstOverflowWriteIndex.load()-4]:0,firstOverflowWriteIndex.load()>2?writeDurations[firstOverflowWriteIndex.load()-3]:0,firstOverflowWriteIndex.load()>1?writeDurations[firstOverflowWriteIndex.load()-2]:0,firstOverflowWriteIndex.load()>0?writeDurations[firstOverflowWriteIndex.load()-1]:0); Serial.print("{\"type\":\"WRITE_DURATIONS_US\",\"values\":["); for(uint64_t i=0;i<writes.load()&&i<8192;i++){if(i)Serial.print(',');Serial.print(writeDurations[i]);} Serial.println("]}");
  const uint64_t duration=esp_timer_get_time()-startedUs;
  const bool pass=duration>=TEST_US&&written==acquired&&dmaOverflow==0&&bufferOverflow==0&&sdErrors==0&&sdBytes==written*2;
  Serial.printf("{\"type\":\"RESULT\",\"test\":\"%s\",\"duration_us\":%llu,\"samples\":%llu,\"written\":%llu,\"bytes\":%llu,\"writes\":%llu,\"short\":%u,\"zero\":%u,\"errors\":%u,\"dma_overflow\":%u,\"buffer_overflow\":%u,\"pass\":%s}\n",substitute?"A":"B",duration,acquired.load(),written.load(),sdBytes.load(),writes.load(),shortWrites.load(),zeroWrites.load(),sdErrors.load(),dmaOverflow.load(),bufferOverflow.load(),pass?"true":"false");
}

void start(bool usePattern) {
  char path[40]; snprintf(path,sizeof(path),"/AB-%c-%08lX.bin",usePattern?'A':'B',(unsigned long)esp_random()); raw=SD.open(path,FILE_WRITE);
  if(!raw){Serial.println("{\"type\":\"OPEN_ERROR\"}");return;}
  acquired=written=sdBytes=writes=writeTotalUs=writeStartedUs=firstOverflowAtUs=firstOverflowAcquired=firstOverflowWritten=0; dmaOverflow=bufferOverflow=shortWrites=zeroWrites=sdErrors=writeMaxUs=lastWriteUs=over50=over100=over200=over500=over1000=firstOverflowWriteUs=firstOverflowReady=firstOverflowFree=firstOverflowWriteIndex=0; dmaOverflowIsr=0; substitute=usePattern;
  while(uxQueueMessagesWaiting(readyQ)){Buffer*b; xQueueReceive(readyQ,&b,0);b->count=0;xQueueSend(freeQ,&b,0);}
  adcDrained=false; startedUs=esp_timer_get_time(); if(adc_continuous_start(adc)!=ESP_OK){adcDrained=true;raw.close();Serial.println("{\"type\":\"ADC_START_ERROR\"}");return;} active=true;
  Serial.printf("{\"type\":\"START\",\"test\":\"%c\",\"file\":\"%s\"}\n",usePattern?'A':'B',path);
}

void setup() {
  Serial.begin(115200); delay(1200); pinMode(14,OUTPUT); digitalWrite(14,HIGH); SPI.begin(1,3,2,14);
  bool sd=SD.begin(4,SPI,SD_HZ); freeQ=xQueueCreate(BUFFER_COUNT,sizeof(Buffer*)); readyQ=xQueueCreate(BUFFER_COUNT,sizeof(Buffer*));
  for(size_t i=0;i<BUFFER_COUNT;i++){buffers[i]=static_cast<Buffer*>(heap_caps_calloc(1,sizeof(Buffer),MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT));xQueueSend(freeQ,&buffers[i],portMAX_DELAY);}
  bool a=initAdc(); xTaskCreate(adcTask,"adc",4096,nullptr,4,nullptr); xTaskCreate(writerTask,"writer",4096,nullptr,2,nullptr);
  Serial.printf("{\"type\":\"BOOT\",\"sd\":%s,\"adc\":%s,\"buffer_bytes\":%u}\n",sd?"true":"false",a?"true":"false",unsigned(BUFFER_BYTES));
}

void loop() {
  if(active&&(sdErrors||esp_timer_get_time()-startedUs>=TEST_US)) finish();
  if(Serial.available()){String c=Serial.readStringUntil('\n');c.trim();c.toUpperCase();if(c=="A")start(true);else if(c=="B")start(false);else if(c=="STOP")finish();}
  delay(2);
}
