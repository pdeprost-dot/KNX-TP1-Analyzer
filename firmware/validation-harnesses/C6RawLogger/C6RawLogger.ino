#include <Arduino.h>
#include <SPI.h>
#include <SD.h>
#include <Arduino_GFX_Library.h>
#include <esp_adc/adc_continuous.h>
#include <esp_heap_caps.h>
#include <atomic>

constexpr uint32_t SAMPLE_RATE = 83333;
constexpr gpio_num_t ADC_GPIO = GPIO_NUM_5;
constexpr adc_channel_t ADC_CHANNEL = ADC_CHANNEL_5;
constexpr size_t SAMPLES_PER_BLOCK = 512;
constexpr size_t BLOCK_COUNT = 128;
constexpr size_t DMA_FRAME_BYTES = 1024;
constexpr size_t DMA_POOL_BYTES = 49152;
constexpr uint32_t SD_HZ = 4000000;
constexpr uint64_t SYNC_BYTES = 100000;

struct Block { uint32_t count; uint16_t samples[SAMPLES_PER_BLOCK]; };
struct RawHeader {
  char magic[8];
  uint32_t version;
  uint32_t headerBytes;
  uint32_t requestedHz;
  uint32_t adcGpio;
  uint32_t adcBits;
  uint64_t startMonotonicUs;
  uint8_t reserved[472];
};
static_assert(sizeof(RawHeader) == 512, "RAW header must be 512 bytes");

Arduino_DataBus *lcdBus = new Arduino_HWSPI(15, 14, 1, 2, 3);
Arduino_GFX *lcd = new Arduino_ST7789(lcdBus, 22, 0, false, 172, 320, 34, 0, 34, 0);
bool lcdReady = false;

adc_continuous_handle_t adcHandle = nullptr;
QueueHandle_t freeQueue = nullptr, readyQueue = nullptr;
TaskHandle_t adcTaskHandle = nullptr, writerTaskHandle = nullptr;
Block *blocks[BLOCK_COUNT]{};
File rawFile;
String sessionDir, rawPath;
std::atomic<bool> recording{false}, stopping{false}, stopRequested{false};
std::atomic<uint64_t> acquired{0}, written{0}, lost{0}, rawBytes{0};
std::atomic<uint32_t> dmaOverflow{0}, bufferOverflow{0}, sdErrors{0};
std::atomic<uint32_t> queueMax{0}, writeMaxUs{0}, syncMaxUs{0};
std::atomic<uint64_t> writeCalls{0}, writeTimeUs{0};
volatile uint32_t dmaOverflowIsr = 0;
uint64_t startUs = 0;

void max32(std::atomic<uint32_t> &v, uint32_t n) { uint32_t old=v.load(); while(n>old && !v.compare_exchange_weak(old,n)){} }

bool IRAM_ATTR onPoolOverflow(adc_continuous_handle_t, const adc_continuous_evt_data_t *, void *) {
  ++dmaOverflowIsr;
  return false;
}

bool initAdc() {
  adc_continuous_handle_cfg_t hc{};
  hc.max_store_buf_size = DMA_POOL_BYTES;
  hc.conv_frame_size = DMA_FRAME_BYTES;
  if (adc_continuous_new_handle(&hc, &adcHandle) != ESP_OK) return false;
  adc_digi_pattern_config_t pattern{};
  pattern.atten = ADC_ATTEN_DB_12;
  pattern.channel = ADC_CHANNEL;
  pattern.unit = ADC_UNIT_1;
  pattern.bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;
  adc_continuous_config_t config{};
  config.pattern_num = 1;
  config.adc_pattern = &pattern;
  config.sample_freq_hz = SAMPLE_RATE;
  config.conv_mode = ADC_CONV_SINGLE_UNIT_1;
  config.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
  if (adc_continuous_config(adcHandle, &config) != ESP_OK) return false;
  adc_continuous_evt_cbs_t callbacks{};
  callbacks.on_pool_ovf = onPoolOverflow;
  return adc_continuous_register_event_callbacks(adcHandle, &callbacks, nullptr) == ESP_OK;
}

void adcTask(void *) {
  alignas(4) uint8_t dma[DMA_FRAME_BYTES];
  Block *current = nullptr;
  while (true) {
    if (!recording) { if (current && current->count) { xQueueSend(readyQueue, &current, portMAX_DELAY); current = nullptr; } vTaskDelay(pdMS_TO_TICKS(5)); continue; }
    uint32_t bytes = 0;
    const esp_err_t err = adc_continuous_read(adcHandle, dma, sizeof(dma), &bytes, 50);
    dmaOverflow = dmaOverflowIsr;
    if (err == ESP_ERR_TIMEOUT) continue;
    if (err != ESP_OK) { vTaskDelay(1); continue; }
    for (uint32_t off=0; off+SOC_ADC_DIGI_RESULT_BYTES<=bytes; off+=SOC_ADC_DIGI_RESULT_BYTES) {
      const auto *r = reinterpret_cast<const adc_digi_output_data_t *>(dma+off);
      if (r->type2.channel != ADC_CHANNEL) continue;
      ++acquired;
      if (!current && xQueueReceive(freeQueue, &current, 0) != pdTRUE) {
        ++lost; ++bufferOverflow; continue;
      }
      current->samples[current->count++] = r->type2.data;
      if (current->count == SAMPLES_PER_BLOCK) {
        xQueueSend(readyQueue, &current, portMAX_DELAY);
        current = nullptr;
      }
    }
  }
}

void writerTask(void *) {
  uint64_t sinceSync = 0;
  while (true) {
    Block *block = nullptr;
    if (xQueueReceive(readyQueue, &block, pdMS_TO_TICKS(20)) == pdTRUE) {
      const size_t requested = block->count * sizeof(uint16_t);
      const uint64_t began = esp_timer_get_time();
      const size_t actual = rawFile ? rawFile.write(reinterpret_cast<uint8_t *>(block->samples), requested) : 0;
      const uint32_t elapsed = uint32_t(esp_timer_get_time()-began);
      ++writeCalls; writeTimeUs += elapsed; max32(writeMaxUs, elapsed);
      if (actual == requested) { written += block->count; rawBytes += actual; sinceSync += actual; }
      else {
        lost += block->count;
        ++sdErrors;
        stopRequested = true;
        Serial.printf("{\"type\":\"SD_ERROR\",\"requested\":%u,\"actual\":%u,\"samples\":%llu,\"written\":%llu,\"queue\":%u}\n",
                      unsigned(requested), unsigned(actual), acquired.load(), written.load(), unsigned(uxQueueMessagesWaiting(readyQueue)));
      }
      block->count = 0;
      xQueueSend(freeQueue, &block, portMAX_DELAY);
      max32(queueMax, uxQueueMessagesWaiting(readyQueue));
      if (sinceSync >= SYNC_BYTES && rawFile) {
        const uint64_t syncBegan = esp_timer_get_time(); rawFile.flush();
        max32(syncMaxUs, uint32_t(esp_timer_get_time()-syncBegan)); sinceSync = 0;
      }
    }
    if (stopping && uxQueueMessagesWaiting(readyQueue)==0) {
      if (rawFile) rawFile.flush();
      stopping = false;
    }
  }
}

bool startRecording() {
  if (recording || !SD.cardSize()) return false;
  char id[32]; snprintf(id,sizeof(id),"S-%08lX%08lX",(unsigned long)esp_random(),(unsigned long)esp_random());
  sessionDir = String("/C6RAW/")+id;
  if (!SD.exists("/C6RAW")) SD.mkdir("/C6RAW");
  if (!SD.mkdir(sessionDir)) return false;
  rawPath=sessionDir+"/raw.bin";
  rawFile=SD.open(rawPath,FILE_WRITE);
  if(!rawFile) return false;
  acquired=written=lost=rawBytes=0; dmaOverflow=bufferOverflow=sdErrors=0;
  queueMax=writeMaxUs=syncMaxUs=0; writeCalls=writeTimeUs=0; dmaOverflowIsr=0; stopRequested=false;
  RawHeader h{}; memcpy(h.magic,"C6RAW1",6); h.version=1; h.headerBytes=sizeof(h); h.requestedHz=SAMPLE_RATE; h.adcGpio=5; h.adcBits=12; h.startMonotonicUs=esp_timer_get_time();
  if(rawFile.write(reinterpret_cast<uint8_t*>(&h),sizeof(h))!=sizeof(h)){rawFile.close();++sdErrors;return false;}
  rawBytes=sizeof(h); startUs=esp_timer_get_time(); recording=true;
  if(adc_continuous_start(adcHandle)!=ESP_OK){recording=false;rawFile.close();return false;}
  Serial.printf("{\"type\":\"START\",\"session\":\"%s\"}\n",sessionDir.c_str());
  return true;
}

void stopRecording() {
  if(!recording)return;
  recording=false; adc_continuous_stop(adcHandle); delay(20); stopping=true;
  const uint32_t deadline=millis()+5000; while(stopping && int32_t(deadline-millis())>0) delay(5);
  if(rawFile){rawFile.flush();rawFile.close();}
  const uint64_t duration=esp_timer_get_time()-startUs;
  File f=SD.open(sessionDir+"/result.txt",FILE_WRITE);
  if(f){f.printf("duration_us=%llu\nsamples=%llu\nwritten=%llu\nlost=%llu\ndma_overflow=%u\nbuffer_overflow=%u\nsd_errors=%u\nraw_bytes=%llu\n",duration,acquired.load(),written.load(),lost.load(),dmaOverflow.load(),bufferOverflow.load(),sdErrors.load(),rawBytes.load());f.close();}
  Serial.printf("{\"type\":\"STOP\",\"duration_us\":%llu,\"samples\":%llu,\"written\":%llu,\"lost\":%llu,\"dma_overflow\":%u,\"buffer_overflow\":%u,\"sd_errors\":%u,\"raw_bytes\":%llu}\n",duration,acquired.load(),written.load(),lost.load(),dmaOverflow.load(),bufferOverflow.load(),sdErrors.load(),rawBytes.load());
}

void printStatus() {
  const uint64_t us=recording?esp_timer_get_time()-startUs:0;
  const double hz=us?double(acquired.load())*1000000.0/double(us):0;
  Serial.printf("{\"type\":\"STATUS\",\"recording\":%s,\"duration_us\":%llu,\"requested_hz\":%u,\"measured_hz\":%.1f,\"samples\":%llu,\"written\":%llu,\"lost\":%llu,\"dma_overflow\":%u,\"buffer_overflow\":%u,\"sd_errors\":%u,\"raw_bytes\":%llu,\"raw_Bps\":%.1f,\"write_avg_us\":%llu,\"write_max_us\":%u,\"sync_max_us\":%u,\"queue_max\":%u,\"queue_capacity\":%u}\n",
    recording?"true":"false",us,SAMPLE_RATE,hz,acquired.load(),written.load(),lost.load(),dmaOverflow.load(),bufferOverflow.load(),sdErrors.load(),rawBytes.load(),us?double(rawBytes.load())*1000000.0/double(us):0,writeCalls?writeTimeUs/writeCalls:0,writeMaxUs.load(),syncMaxUs.load(),queueMax.load(),unsigned(BLOCK_COUNT));
}

void updateLcd() {
  if(!lcdReady)return;
  lcd->fillScreen(0x0000); lcd->setTextColor(0xFFFF); lcd->setTextSize(2); lcd->setCursor(8,30); lcd->println(recording?"RECORDING":"STOPPED");
  lcd->setTextSize(1); lcd->setCursor(8,70); lcd->printf("Time %llu s",recording?(esp_timer_get_time()-startUs)/1000000:0); lcd->setCursor(8,90); lcd->printf("Samples %llu",acquired.load()); lcd->setCursor(8,110); lcd->printf("LOST %llu",lost.load()); lcd->setCursor(8,130); lcd->printf("SD ERR %u",sdErrors.load());
}

void setup(){
  Serial.begin(115200);delay(1200);
  pinMode(4,OUTPUT);digitalWrite(4,HIGH);pinMode(23,OUTPUT);digitalWrite(23,LOW);
  SPI.begin(1,3,2,14);lcdReady=lcd->begin(40000000);if(lcdReady){lcd->invertDisplay(true);lcd->setRotation(6);digitalWrite(23,HIGH);}
  digitalWrite(14,HIGH);const bool sd=SD.begin(4,SPI,SD_HZ);
  freeQueue=xQueueCreate(BLOCK_COUNT,sizeof(Block*));readyQueue=xQueueCreate(BLOCK_COUNT,sizeof(Block*));
  for(size_t i=0;i<BLOCK_COUNT;i++){blocks[i]=static_cast<Block*>(heap_caps_calloc(1,sizeof(Block),MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));if(!blocks[i]){Serial.println("BLOCK_ALLOC_FAIL");while(true)delay(1000);}xQueueSend(freeQueue,&blocks[i],portMAX_DELAY);}
  const bool adc=initAdc();
  xTaskCreate(adcTask,"adc",4096,nullptr,4,&adcTaskHandle);xTaskCreate(writerTask,"writer",4096,nullptr,2,&writerTaskHandle);
  Serial.printf("{\"type\":\"BOOT\",\"sd\":%s,\"sd_bytes\":%llu,\"sd_hz\":%u,\"adc\":%s,\"gpio\":5,\"requested_hz\":%u,\"block_bytes\":1024,\"blocks\":%u,\"heap\":%u}\n",sd?"true":"false",sd?SD.cardSize():0,SD_HZ,adc?"true":"false",SAMPLE_RATE,unsigned(BLOCK_COUNT),ESP.getFreeHeap());
  updateLcd();
}

void loop(){
  if(stopRequested && recording)stopRecording();
  if(Serial.available()){String c=Serial.readStringUntil('\n');c.trim();c.toUpperCase();if(c=="START")Serial.println(startRecording()?"OK START":"ERROR START");else if(c=="STOP"){stopRecording();Serial.println("OK STOP");}else if(c=="STATUS")printStatus();}
  static uint32_t lastLcd=0;if(!recording && millis()-lastLcd>=1000){lastLcd=millis();updateLcd();}
  delay(2);
}