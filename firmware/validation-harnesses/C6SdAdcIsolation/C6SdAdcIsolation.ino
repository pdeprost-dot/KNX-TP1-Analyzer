#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <esp_adc/adc_continuous.h>
#include <atomic>

constexpr uint32_t SD_HZ=4000000;
constexpr size_t BLOCK_BYTES=64*1024;
constexpr uint64_t TEST_US=600ULL*1000000ULL;
uint8_t *buffer=nullptr;
adc_continuous_handle_t adcHandle=nullptr;
std::atomic<bool> adcActive{false};
std::atomic<uint64_t> adcSamples{0};
std::atomic<uint32_t> adcOverflow{0},adcReadErrors{0};
volatile uint32_t adcOverflowIsr=0;

bool IRAM_ATTR onAdcOverflow(adc_continuous_handle_t,const adc_continuous_evt_data_t*,void*){++adcOverflowIsr;return false;}
void adcReader(void*){alignas(4) uint8_t dma[1024];while(true){if(!adcActive){vTaskDelay(pdMS_TO_TICKS(5));continue;}uint32_t bytes=0;esp_err_t e=adc_continuous_read(adcHandle,dma,sizeof(dma),&bytes,50);adcOverflow=adcOverflowIsr;if(e==ESP_ERR_TIMEOUT)continue;if(e!=ESP_OK){++adcReadErrors;continue;}for(uint32_t off=0;off+SOC_ADC_DIGI_RESULT_BYTES<=bytes;off+=SOC_ADC_DIGI_RESULT_BYTES){const auto*r=reinterpret_cast<const adc_digi_output_data_t*>(dma+off);if(r->type2.channel==ADC_CHANNEL_5)++adcSamples;}}}
bool initAdc(){adc_continuous_handle_cfg_t h{};h.max_store_buf_size=49152;h.conv_frame_size=1024;if(adc_continuous_new_handle(&h,&adcHandle)!=ESP_OK)return false;adc_digi_pattern_config_t p{};p.atten=ADC_ATTEN_DB_12;p.channel=ADC_CHANNEL_5;p.unit=ADC_UNIT_1;p.bit_width=SOC_ADC_DIGI_MAX_BITWIDTH;adc_continuous_config_t c{};c.pattern_num=1;c.adc_pattern=&p;c.sample_freq_hz=83333;c.conv_mode=ADC_CONV_SINGLE_UNIT_1;c.format=ADC_DIGI_OUTPUT_FORMAT_TYPE2;if(adc_continuous_config(adcHandle,&c)!=ESP_OK)return false;adc_continuous_evt_cbs_t cb{};cb.on_pool_ovf=onAdcOverflow;return adc_continuous_register_event_callbacks(adcHandle,&cb,nullptr)==ESP_OK;}

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
  adcSamples=0;adcOverflow=0;adcReadErrors=0;adcOverflowIsr=0;adcActive=true;if(adc_continuous_start(adcHandle)!=ESP_OK){adcActive=false;f.close();Serial.println("{\"type\":\"ADC_START_ERROR\"}");return;}const uint64_t started=esp_timer_get_time();uint64_t lastFlush=started;bool pass=true;
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
  m.position=f.position();f.close();m.durationUs=esp_timer_get_time()-started;adcActive=false;adc_continuous_stop(adcHandle);
  Serial.printf("{\"type\":\"RESULT\",\"test\":%u,\"target_Bps\":%u,\"duration_us\":%llu,\"requested\":%llu,\"actual\":%llu,\"calls\":%llu,\"short_writes\":%u,\"zero_writes\":%u,\"errors\":%u,\"avg_write_us\":%llu,\"max_write_us\":%u,\"flushes\":%u,\"max_flush_us\":%u,\"position\":%llu,\"Bps\":%.1f,\"adc_samples\":%llu,\"adc_hz\":%.1f,\"dma_overflow\":%u,\"adc_read_errors\":%u,\"pass\":%s,\"file\":\"%s\"}\n",number,targetBps,m.durationUs,m.requested,m.actual,m.calls,m.shortWrites,m.zeroWrites,m.errors,m.calls?m.writeUs/m.calls:0,m.maxWriteUs,m.flushes,m.maxFlushUs,m.position,m.durationUs?double(m.actual)*1000000.0/double(m.durationUs):0,adcSamples.load(),m.durationUs?double(adcSamples.load())*1000000.0/double(m.durationUs):0,adcOverflow.load(),adcReadErrors.load(),(pass&&adcOverflow==0&&adcReadErrors==0)?"true":"false",path);
}

void setup(){
  Serial.begin(115200);delay(1200);pinMode(14,OUTPUT);digitalWrite(14,HIGH);SPI.begin(1,3,2,14);
  const bool sd=SD.begin(4,SPI,SD_HZ);const bool adc=initAdc();xTaskCreate(adcReader,"adc_discard",4096,nullptr,4,nullptr);buffer=static_cast<uint8_t*>(heap_caps_malloc(BLOCK_BYTES,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));if(buffer)fillBuffer();
  Serial.printf("{\"type\":\"BOOT\",\"sd\":%s,\"sd_bytes\":%llu,\"sd_hz\":%u,\"block_bytes\":%u,\"buffer\":%s,\"adc\":%s,\"heap\":%u}\n",sd?"true":"false",sd?SD.cardSize():0,SD_HZ,unsigned(BLOCK_BYTES),buffer?"true":"false",adc?"true":"false",ESP.getFreeHeap());
}
void loop(){if(Serial.available()){String c=Serial.readStringUntil('\n');c.trim();c.toUpperCase();if(c=="RUN")runTest(2,170000);}delay(2);}