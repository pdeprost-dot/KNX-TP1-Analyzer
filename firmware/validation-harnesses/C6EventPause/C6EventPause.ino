#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <esp_adc/adc_continuous.h>
#include <esp_heap_caps.h>
#include <esp_crc.h>
#include <atomic>

constexpr uint32_t SAMPLE_RATE=83333, SD_HZ=4000000;
constexpr uint64_t CYCLE_US=10000000ULL, RAW_US=1000000ULL, TEST_US=600000000ULL;
constexpr size_t BUFFER_BYTES=16*1024, SAMPLES_PER_BUFFER=BUFFER_BYTES/2, BUFFER_COUNT=2;
constexpr size_t DMA_BYTES=1024, DMA_POOL_BYTES=49152;
constexpr uint8_t PERIOD_COUNT=120;

struct Period {
  uint64_t sampleStart=0,sampleEnd=0,startUs=0,endUs=0,count=0,sum=0,ignored=0,lost=0,rawStored=0,rawBytes=0,writes=0,writeUs=0;
  uint32_t min=UINT32_MAX,max=0,crc=0,shortWrites=0,zeroWrites=0,sdErrors=0,writeMaxUs=0,bufferOverflow=0,dmaOverflow=0;
};
struct Buffer { uint32_t count=0; uint8_t period=0; uint16_t samples[SAMPLES_PER_BUFFER]; };

Period periods[PERIOD_COUNT]; Buffer *buffers[BUFFER_COUNT]{};
QueueHandle_t freeQ=nullptr,readyQ=nullptr; adc_continuous_handle_t adc=nullptr; File raw;
std::atomic<bool> active{false},adcDrained{true},stopping{false},windowDone{false};
std::atomic<uint64_t> acquired{0},rawStored{0},ignored{0},lost{0},rawBytes{0},writes{0},writeUs{0};
std::atomic<uint32_t> dmaOverflow{0},bufferOverflow{0},sdErrors{0},shortWrites{0},zeroWrites{0},writeMaxUs{0};
volatile uint32_t dmaOverflowIsr=0; uint64_t startedUs=0; String dirPath;

bool IRAM_ATTR onOverflow(adc_continuous_handle_t,const adc_continuous_evt_data_t*,void*){++dmaOverflowIsr;return false;}
bool initAdc(){
  adc_continuous_handle_cfg_t h{};h.max_store_buf_size=DMA_POOL_BYTES;h.conv_frame_size=DMA_BYTES;if(adc_continuous_new_handle(&h,&adc)!=ESP_OK)return false;
  adc_digi_pattern_config_t p{};p.atten=ADC_ATTEN_DB_12;p.channel=ADC_CHANNEL_5;p.unit=ADC_UNIT_1;p.bit_width=SOC_ADC_DIGI_MAX_BITWIDTH;
  adc_continuous_config_t c{};c.pattern_num=1;c.adc_pattern=&p;c.sample_freq_hz=SAMPLE_RATE;c.conv_mode=ADC_CONV_SINGLE_UNIT_1;c.format=ADC_DIGI_OUTPUT_FORMAT_TYPE2;
  if(adc_continuous_config(adc,&c)!=ESP_OK)return false;adc_continuous_evt_cbs_t cb{};cb.on_pool_ovf=onOverflow;return adc_continuous_register_event_callbacks(adc,&cb,nullptr)==ESP_OK;
}
void maxAtomic(std::atomic<uint32_t>&v,uint32_t n){uint32_t o=v.load();while(n>o&&!v.compare_exchange_weak(o,n)){} }
void queuePartial(Buffer*&b){if(b&&b->count){xQueueSend(readyQ,&b,portMAX_DELAY);b=nullptr;}}

void adcTask(void*){
  alignas(4) uint8_t dma[DMA_BYTES];Buffer*cur=nullptr;int current=-1;uint32_t lastDma=0;
  while(true){
    if(!active){queuePartial(cur);adcDrained=true;vTaskDelay(pdMS_TO_TICKS(2));continue;}
    uint32_t bytes=0;esp_err_t e=adc_continuous_read(adc,dma,sizeof(dma),&bytes,50);dmaOverflow=dmaOverflowIsr;if(e==ESP_ERR_TIMEOUT)continue;if(e!=ESP_OK)continue;
    const uint64_t now=esp_timer_get_time(),elapsed=now-startedUs;int target=int((elapsed/CYCLE_US)*2+((elapsed%CYCLE_US)>=RAW_US?1:0));
    if(target>=PERIOD_COUNT){if(current>=0&&periods[current].endUs==0){periods[current].sampleEnd=acquired.load();periods[current].endUs=now;periods[current].dmaOverflow=dmaOverflow.load()-lastDma;}queuePartial(cur);windowDone=true;continue;}
    if(target!=current){queuePartial(cur);if(current>=0){periods[current].sampleEnd=acquired.load();periods[current].endUs=now;periods[current].dmaOverflow=dmaOverflow.load()-lastDma;lastDma=dmaOverflow.load();}current=target;periods[current].sampleStart=acquired.load();periods[current].startUs=now;}
    Period &p=periods[current];const bool capture=(current%2)==0;
    for(uint32_t off=0;off+SOC_ADC_DIGI_RESULT_BYTES<=bytes;off+=SOC_ADC_DIGI_RESULT_BYTES){
      const auto*r=reinterpret_cast<const adc_digi_output_data_t*>(dma+off);if(r->type2.channel!=ADC_CHANNEL_5)continue;const uint16_t v=r->type2.data;++acquired;++p.count;p.sum+=v;if(v<p.min)p.min=v;if(v>p.max)p.max=v;
      if(!capture){++ignored;++p.ignored;continue;}
      if(!cur&&xQueueReceive(freeQ,&cur,0)!=pdTRUE){++lost;++p.lost;++bufferOverflow;++p.bufferOverflow;continue;}
      if(cur->count==0)cur->period=current;cur->samples[cur->count++]=v;if(cur->count==SAMPLES_PER_BUFFER){xQueueSend(readyQ,&cur,portMAX_DELAY);cur=nullptr;}
    }
  }
}

void writerTask(void*){
  while(true){Buffer*b=nullptr;if(xQueueReceive(readyQ,&b,pdMS_TO_TICKS(20))==pdTRUE){Period&p=periods[b->period];size_t req=b->count*2;uint64_t wb=esp_timer_get_time();size_t n=raw?raw.write((uint8_t*)b->samples,req):0;uint32_t dt=uint32_t(esp_timer_get_time()-wb);++writes;++p.writes;writeUs+=dt;p.writeUs+=dt;maxAtomic(writeMaxUs,dt);if(dt>p.writeMaxUs)p.writeMaxUs=dt;size_t ns=n/2;rawStored+=ns;rawBytes+=n;p.rawStored+=ns;p.rawBytes+=n;p.crc=esp_crc32_le(p.crc,(uint8_t*)b->samples,n);if(n!=req){++shortWrites;++sdErrors;++p.shortWrites;++p.sdErrors;if(n==0){++zeroWrites;++p.zeroWrites;}uint64_t missing=b->count-ns;lost+=missing;p.lost+=missing;Serial.printf("{\"type\":\"SD_ERROR\",\"requested\":%u,\"actual\":%u}\n",unsigned(req),unsigned(n));}b->count=0;xQueueSend(freeQ,&b,portMAX_DELAY);}if(stopping&&uxQueueMessagesWaiting(readyQ)==0){if(raw)raw.flush();stopping=false;}}
}

void writeMetadata(uint64_t duration){
  File f=SD.open(dirPath+"/periods.jsonl",FILE_WRITE);uint64_t metaBytes=0;
  for(uint8_t i=0;i<PERIOD_COUNT;i++){Period&p=periods[i];char line[640];int n=snprintf(line,sizeof(line),"{\"index\":%u,\"type\":\"%s\",\"sample_start\":%llu,\"sample_end\":%llu,\"sample_count\":%llu,\"duration_us\":%llu,\"adc_min\":%u,\"adc_max\":%u,\"adc_mean\":%.3f,\"intentionally_not_stored\":%llu,\"samples_lost\":%llu,\"dma_overflow\":%u,\"buffer_overflow\":%u,\"sd_errors\":%u,\"raw_stored\":%llu,\"raw_bytes\":%llu,\"crc32\":\"%08X\",\"writes\":%llu,\"short_writes\":%u,\"zero_writes\":%u,\"write_avg_us\":%llu,\"write_max_us\":%u}\n",i,(i%2)==0?"CAPTURED_RAW":"SILENCE_SUMMARIZED",p.sampleStart,p.sampleEnd,p.count,p.endUs-p.startUs,p.min==UINT32_MAX?0:p.min,p.max,p.count?double(p.sum)/p.count:0,p.ignored,p.lost,p.dmaOverflow,p.bufferOverflow,p.sdErrors,p.rawStored,p.rawBytes,p.crc,p.writes,p.shortWrites,p.zeroWrites,p.writes?p.writeUs/p.writes:0,p.writeMaxUs);if(f)n=f.write((uint8_t*)line,n);metaBytes+=n;Serial.print(line);}if(f){f.flush();f.close();}
  uint32_t rawPeriodsWithLoss=0;uint64_t worstRawLoss=0;for(uint8_t i=0;i<PERIOD_COUNT;i+=2){if(periods[i].lost){++rawPeriodsWithLoss;if(periods[i].lost>worstRawLoss)worstRawLoss=periods[i].lost;}}bool invariant=acquired==rawStored+ignored+lost;File e=SD.open(dirPath+"/session-end.json",FILE_WRITE);char end[512];int n=snprintf(end,sizeof(end),"{\"status\":\"%s\",\"duration_us\":%llu,\"samples_acquired\":%llu,\"samples_raw_stored\":%llu,\"samples_intentionally_not_stored\":%llu,\"samples_lost\":%llu,\"raw_bytes\":%llu,\"dma_overflow\":%u,\"buffer_overflow\":%u,\"sd_errors\":%u,\"short_writes\":%u,\"zero_writes\":%u,\"writes\":%llu,\"raw_periods_with_loss\":%u,\"worst_raw_loss\":%llu,\"write_avg_us\":%llu,\"write_max_us\":%u,\"invariant\":%s}\n",invariant&&dmaOverflow==0&&lost==0&&sdErrors==0?"CLOSED":"FAILED",duration,acquired.load(),rawStored.load(),ignored.load(),lost.load(),rawBytes.load(),dmaOverflow.load(),bufferOverflow.load(),sdErrors.load(),shortWrites.load(),zeroWrites.load(),writes.load(),rawPeriodsWithLoss,worstRawLoss,writes?writeUs/writes:0,writeMaxUs.load(),invariant?"true":"false");if(e){e.write((uint8_t*)end,n);e.flush();e.close();}metaBytes+=n;Serial.printf("{\"type\":\"RESULT\",\"duration_us\":%llu,\"adc_hz\":%.3f,\"acquired\":%llu,\"raw_stored\":%llu,\"ignored\":%llu,\"lost\":%llu,\"raw_bytes\":%llu,\"metadata_bytes\":%llu,\"session_bytes\":%llu,\"dma_overflow\":%u,\"buffer_overflow\":%u,\"sd_errors\":%u,\"short_writes\":%u,\"zero_writes\":%u,\"writes\":%llu,\"raw_periods_with_loss\":%u,\"worst_raw_loss\":%llu,\"invariant\":%s,\"closed\":true,\"pass\":%s}\n",duration,double(acquired.load())*1000000.0/duration,acquired.load(),rawStored.load(),ignored.load(),lost.load(),rawBytes.load(),metaBytes,rawBytes.load()+metaBytes,dmaOverflow.load(),bufferOverflow.load(),sdErrors.load(),shortWrites.load(),zeroWrites.load(),writes.load(),rawPeriodsWithLoss,worstRawLoss,invariant?"true":"false",invariant&&dmaOverflow==0&&lost==0&&sdErrors==0?"true":"false");
}

void finish(){if(!active&&!stopping)return;active=false;adc_continuous_stop(adc);uint32_t d=millis()+1000;while(!adcDrained&&int32_t(d-millis())>0)delay(1);stopping=true;d=millis()+5000;while(stopping&&int32_t(d-millis())>0)delay(2);if(raw){raw.flush();raw.close();}uint64_t duration=esp_timer_get_time()-startedUs;writeMetadata(duration);}
void start(){char d[32];snprintf(d,sizeof(d),"/EVT-%08lX",(unsigned long)esp_random());dirPath=d;if(!SD.mkdir(dirPath)){Serial.println("{\"type\":\"MKDIR_ERROR\"}");return;}raw=SD.open(dirPath+"/raw.bin",FILE_WRITE);if(!raw){Serial.println("{\"type\":\"OPEN_ERROR\"}");return;}memset(periods,0,sizeof(periods));for(auto&p:periods)p.min=UINT32_MAX;acquired=rawStored=ignored=lost=rawBytes=writes=writeUs=0;dmaOverflow=bufferOverflow=sdErrors=shortWrites=zeroWrites=writeMaxUs=0;dmaOverflowIsr=0;windowDone=false;adcDrained=false;startedUs=esp_timer_get_time();if(adc_continuous_start(adc)!=ESP_OK){raw.close();Serial.println("{\"type\":\"ADC_START_ERROR\"}");return;}active=true;Serial.printf("{\"type\":\"START\",\"dir\":\"%s\"}\n",dirPath.c_str());}
void setup(){Serial.begin(115200);delay(1200);pinMode(14,OUTPUT);digitalWrite(14,HIGH);SPI.begin(1,3,2,14);bool sd=SD.begin(4,SPI,SD_HZ);freeQ=xQueueCreate(BUFFER_COUNT,sizeof(Buffer*));readyQ=xQueueCreate(BUFFER_COUNT,sizeof(Buffer*));for(size_t i=0;i<BUFFER_COUNT;i++){buffers[i]=(Buffer*)heap_caps_calloc(1,sizeof(Buffer),MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT);xQueueSend(freeQ,&buffers[i],portMAX_DELAY);}bool a=initAdc();xTaskCreate(adcTask,"adc",4096,nullptr,4,nullptr);xTaskCreate(writerTask,"writer",4096,nullptr,2,nullptr);Serial.printf("{\"type\":\"BOOT\",\"sd\":%s,\"adc\":%s}\n",sd?"true":"false",a?"true":"false");}
void loop(){if(active&&(windowDone||sdErrors))finish();if(Serial.available()){String c=Serial.readStringUntil('\n');c.trim();c.toUpperCase();if(c=="RUN")start();else if(c=="STOP")finish();}delay(1);}
