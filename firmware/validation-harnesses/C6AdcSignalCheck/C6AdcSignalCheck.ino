#include <Arduino.h>
#include <esp_adc/adc_continuous.h>

constexpr uint32_t SAMPLE_RATE = 83333;
constexpr uint32_t FRAME_BYTES = 1024;
constexpr uint32_t POOL_BYTES = 49152;
constexpr uint64_t REST_US = 5000000ULL;
constexpr uint64_t TRAFFIC_US = 8000000ULL;
adc_continuous_handle_t adc = nullptr;
volatile uint32_t dmaOverflow = 0;
alignas(4) uint8_t frame[FRAME_BYTES];

enum class State { REST, WAIT_TRAFFIC, TRAFFIC, DONE };
State state = State::REST;
uint64_t phaseStartUs = 0, totalCount = 0, totalSum = 0;
uint16_t totalMin = UINT16_MAX, totalMax = 0;
uint64_t windowStartUs = 0, windowCount = 0, windowSum = 0;
uint16_t windowMin = UINT16_MAX, windowMax = 0;

bool IRAM_ATTR onOverflow(adc_continuous_handle_t, const adc_continuous_evt_data_t*, void*) { ++dmaOverflow; return false; }
float volts(uint16_t raw) { return raw * 3.3f / 4095.0f; }
void resetStats() { totalCount=totalSum=windowCount=windowSum=0; totalMin=windowMin=UINT16_MAX; totalMax=windowMax=0; phaseStartUs=windowStartUs=esp_timer_get_time(); }
void addSample(uint16_t v) { ++totalCount; totalSum+=v; if(v<totalMin)totalMin=v; if(v>totalMax)totalMax=v; ++windowCount; windowSum+=v; if(v<windowMin)windowMin=v; if(v>windowMax)windowMax=v; }
void printWindow() { if(!windowCount)return; Serial.printf("WIN ms=%llu n=%llu avg=%.2f min=%u max=%u amp=%u approxV(avg/min/max)=%.3f/%.3f/%.3f\n",(esp_timer_get_time()-phaseStartUs)/1000,windowCount,double(windowSum)/windowCount,windowMin,windowMax,windowMax-windowMin,volts(uint16_t(windowSum/windowCount)),volts(windowMin),volts(windowMax)); windowCount=windowSum=0; windowMin=UINT16_MAX; windowMax=0; windowStartUs=esp_timer_get_time(); }
void printSummary(const char* phase) { Serial.printf("SUMMARY %s duration_ms=%llu samples=%llu avg=%.3f min=%u max=%u amplitude=%u approxV(avg/min/max)=%.3f/%.3f/%.3f dma_overflow=%u\n",phase,(esp_timer_get_time()-phaseStartUs)/1000,totalCount,double(totalSum)/totalCount,totalMin,totalMax,totalMax-totalMin,volts(uint16_t(totalSum/totalCount)),volts(totalMin),volts(totalMax),dmaOverflow); }
bool initAdc() { adc_continuous_handle_cfg_t h{}; h.max_store_buf_size=POOL_BYTES; h.conv_frame_size=FRAME_BYTES; if(adc_continuous_new_handle(&h,&adc)!=ESP_OK)return false; adc_digi_pattern_config_t p{}; p.atten=ADC_ATTEN_DB_12; p.channel=ADC_CHANNEL_5; p.unit=ADC_UNIT_1; p.bit_width=SOC_ADC_DIGI_MAX_BITWIDTH; adc_continuous_config_t c{}; c.pattern_num=1; c.adc_pattern=&p; c.sample_freq_hz=SAMPLE_RATE; c.conv_mode=ADC_CONV_SINGLE_UNIT_1; c.format=ADC_DIGI_OUTPUT_FORMAT_TYPE2; if(adc_continuous_config(adc,&c)!=ESP_OK)return false; adc_continuous_evt_cbs_t cb{}; cb.on_pool_ovf=onOverflow; return adc_continuous_register_event_callbacks(adc,&cb,nullptr)==ESP_OK; }
void setup() { Serial.begin(115200); delay(1200); if(!initAdc() || adc_continuous_start(adc)!=ESP_OK){Serial.println("ADC_INIT_ERROR");return;} resetStats(); Serial.println("ADC GPIO5 REST measurement: 5 seconds"); }
void loop() { if(state==State::WAIT_TRAFFIC || state==State::DONE){ if(Serial.available()){String c=Serial.readStringUntil('\n');c.trim();c.toUpperCase();if(state==State::WAIT_TRAFFIC && (c=="GO"||c=="TRAFFIC")){resetStats();state=State::TRAFFIC;Serial.println("TRAFFIC measurement: 8 seconds");}} delay(1);return;} uint32_t bytes=0; esp_err_t er=adc_continuous_read(adc,frame,sizeof(frame),&bytes,50); if(er==ESP_OK){for(uint32_t off=0;off+SOC_ADC_DIGI_RESULT_BYTES<=bytes;off+=SOC_ADC_DIGI_RESULT_BYTES){auto*r=(adc_digi_output_data_t*)(frame+off);if(r->type2.channel==ADC_CHANNEL_5)addSample(r->type2.data);}} uint64_t now=esp_timer_get_time(); if(now-windowStartUs>=100000)printWindow(); uint64_t limit=state==State::REST?REST_US:TRAFFIC_US; if(now-phaseStartUs>=limit){printWindow();printSummary(state==State::REST?"REST":"TRAFFIC");if(state==State::REST){state=State::WAIT_TRAFFIC;Serial.println(">>> PROVOQUE MAINTENANT DU TRAFIC SUR LE BUS KNX <<<");Serial.println("Puis envoie GO + Entree pour mesurer 8 secondes.");}else{state=State::DONE;adc_continuous_stop(adc);Serial.println("TEST_TERMINE");}} }