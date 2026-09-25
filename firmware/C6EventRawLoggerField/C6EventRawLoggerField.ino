void runLs1TimestampSelfTest();
void runLs2EventStress(uint32_t target);
#include "field_ui.h"
#include "imu_safety.h"
#include "ls1_identity.h"

// Compile the validated event-raw-v2 engine verbatim. Only its Arduino entry
// points are renamed so the field control layer can run before/after them.
#define setup eventRawEngineSetup
#define loop eventRawEngineLoop
#include "event_raw_instrumented.h"
#undef setup
#undef loop
#include "ls2_stress.h"
#include "ls3_harness.h"
#include "ls4_harness.h"

namespace {
uint32_t fieldSessionCount = 0;
uint64_t closedDurationUs = 0;
RunState previousState = RS_IDLE;
constexpr uint64_t FIELD_SESSION_DURATION_CRITERION = 0;
uint32_t countSessions(){uint32_t count=0;File root=SD.open("/");if(!root)return 0;for(File item=root.openNextFile();item;item=root.openNextFile()){String name=item.name();if(item.isDirectory()&&(name.startsWith("EVENT-")||name.startsWith("/EVENT-")))++count;item.close();}root.close();return count;}
}

FieldSnapshot fieldSnapshot(){RunState state=runState.load();FieldState visible=FieldState::Idle;if(state==RS_RUNNING)visible=FieldState::Capturing;else if(state==RS_CLOSED)visible=FieldState::Closed;else if(state!=RS_IDLE)visible=FieldState::Finalizing;uint64_t elapsed=state==RS_IDLE?0:state==RS_CLOSED?closedDurationUs:esp_timer_get_time()-startedUs;return{visible,SD.cardType()!=CARD_NONE,adc!=nullptr,dirPath.c_str(),fieldSessionCount,elapsed,acquired.load(),static_cast<uint32_t>(eventCount),rawBytes.load(),globalD44Max,retrySuccess,lost.load(),dmaOverflow.load(),poolExhaustion,sdErrors.load(),state==RS_CLOSED};}
bool fieldStartCapture(){if(runState.load()!=RS_IDLE||SD.cardType()==CARD_NONE||adc==nullptr||fieldui::otaBusy())return false;ls3SegmentLimit=LS3_PRODUCTION_SEGMENT_BYTES;ls1PrepareSession();resetDiagnosticRun();start(TEST_US);if(runState.load()!=RS_RUNNING)return false;if(!ls1WriteSessionStart(dirPath,SAMPLE_RATE,D44_THRESHOLD,PRE_SAMPLES,POST_SAMPLES,CHUNK_SAMPLES,CHUNK_COUNT,ls3SegmentLimit))Serial.println("{\"type\":\"LS1_ERROR\",\"error\":\"session_start_write_failed\"}");++fieldSessionCount;return true;}
bool fieldStopCapture(){RunState expected=RS_RUNNING;if(!runState.compare_exchange_strong(expected,RS_STOP_REQUESTED))return false;testUs=FIELD_SESSION_DURATION_CRITERION;active=false;return true;}
bool fieldNewSession(){RunState expected=RS_CLOSED;if(!runState.compare_exchange_strong(expected,RS_IDLE))return false;dirPath="";closedDurationUs=0;return true;}

void runLs1TimestampSelfTest(){static_assert(sizeof(Event::triggerUs)==sizeof(uint64_t),"Event.triggerUs must be uint64_t");constexpr uint64_t values[]={7200000000ULL,28800000000ULL,86400000000ULL};constexpr uint32_t hours[]={2,8,24};for(uint8_t i=0;i<3;i++){Event e{};e.triggerUs=values[i];char serialized[32];snprintf(serialized,sizeof(serialized),"%llu",e.triggerUs);Serial.printf("{\"type\":\"LS1_TIMESTAMP_TEST\",\"hours\":%u,\"trigger_timestamp_us\":\"%s\",\"pass\":%s}\n",hours[i],serialized,strtoull(serialized,nullptr,10)==values[i]?"true":"false");}}

void setup(){eventRawEngineSetup();bool identityReady=ls1IdentityBegin();runLs1TimestampSelfTest();Serial.println("{\"type\":\"BOOT_PHASE\",\"phase\":\"imu\"}");bool imuSafe=ensureImuHighZ();Serial.println("{\"type\":\"BOOT_PHASE\",\"phase\":\"count_sessions\"}");fieldSessionCount=countSessions();Serial.println("{\"type\":\"BOOT_PHASE\",\"phase\":\"ls5_scan\"}");ls5DetectInterrupted();previousState=runState.load();Serial.println("{\"type\":\"BOOT_PHASE\",\"phase\":\"field_ui\"}");fieldui::begin();Serial.printf("{\"type\":\"GPIO5_SAFETY\",\"imu_interrupts_high_z\":%s,\"identity_ready\":%s}\n",imuSafe?"true":"false",identityReady?"true":"false");}
void loop(){fieldui::tick();eventRawEngineLoop();RunState now=runState.load();if(now==RS_CLOSED&&previousState!=RS_CLOSED)closedDurationUs=esp_timer_get_time()-startedUs;previousState=now;}
