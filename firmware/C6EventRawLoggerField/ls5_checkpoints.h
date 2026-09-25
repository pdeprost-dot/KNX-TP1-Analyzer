#pragma once

constexpr uint32_t LS5_CHECKPOINT_CHUNKS = 64;
File checkpointsFile;
uint64_t ls5CheckpointId=0,ls5CheckpointChunkOrdinal=0,ls5LastChunkSeq=0,ls5LastSampleEnd=0;
uint32_t ls5CheckpointCount=0,ls5CheckpointUsMax=0;
uint64_t ls5CheckpointUsTotal=0;

bool ls5Begin(const String &directory){
  checkpointsFile=SD.open(directory+"/checkpoints.jsonl",FILE_WRITE);
  ls5CheckpointId=ls5CheckpointChunkOrdinal=ls5LastChunkSeq=ls5LastSampleEnd=0;
  ls5CheckpointCount=ls5CheckpointUsMax=0;ls5CheckpointUsTotal=0;
  return bool(checkpointsFile);
}

bool ls5FlushDurableMetadata(){
  if(mapUsed){size_t n=mapFile.write((uint8_t*)mapBuffer,mapUsed);if(n!=mapUsed)return false;ls3MapLogicalOffset+=mapUsed;mapUsed=0;}
  if(raw)raw.flush();if(mapFile)mapFile.flush();if(indexFile)indexFile.flush();
  if(!flushEventBuffer())return false;if(eventsFile)eventsFile.flush();
  return true;
}

bool ls5WriteCheckpoint(){
  if(!ls3StoredChunkOrdinal||ls5CheckpointChunkOrdinal==ls3StoredChunkOrdinal)return true;
  const uint64_t t0=esp_timer_get_time();if(!ls5FlushDurableMetadata())return false;
  char prefix[480];const uint64_t checkpointId=++ls5CheckpointId;
  int n=snprintf(prefix,sizeof(prefix),"{\"record_type\":\"CHECKPOINT\",\"checkpoint_id\":\"%llu\",\"session_id\":\"%s\",\"segment_index\":%u,\"last_chunk_ordinal\":\"%llu\",\"last_chunk_seq\":\"%llu\",\"sample_end\":\"%llu\",\"segment_raw_bytes\":\"%llu\",\"session_raw_bytes\":\"%llu\",\"segment_crc32\":\"%08X\",\"session_crc32\":\"%08X\",\"completed_events_observed\":%u,\"timestamp_sample\":\"%llu\"",
    checkpointId,ls1SessionId(),ls3SegmentIndex,ls3StoredChunkOrdinal,ls5LastChunkSeq,ls5LastSampleEnd,ls3SegmentBytes,rawBytes.load(),ls3SegmentCrc,sessionCrc,completedEventCount,acquired.load());
  if(n<=0||size_t(n)>=sizeof(prefix))return false;uint32_t crc=esp_crc32_le(0,(uint8_t*)prefix,n);
  bool ok=checkpointsFile&&checkpointsFile.printf("%s,\"record_crc32\":\"%08X\"}\n",prefix,crc)>0;if(ok)checkpointsFile.flush();
  const uint32_t us=uint32_t(esp_timer_get_time()-t0);ls5CheckpointUsTotal+=us;++ls5CheckpointCount;if(us>ls5CheckpointUsMax)ls5CheckpointUsMax=us;
  if(ok)ls5CheckpointChunkOrdinal=ls3StoredChunkOrdinal;return ok;
}

void ls5OnStoredChunk(const Chunk *chunk){
  ls5LastChunkSeq=chunk->seq;ls5LastSampleEnd=chunk->sampleStart+chunk->count;
  if((ls3StoredChunkOrdinal%LS5_CHECKPOINT_CHUNKS)==0&&!ls5WriteCheckpoint()){++sdErrors;sdIncident=true;runState=RS_STOP_REQUESTED;active=false;}
}

void ls5Finish(){ls5WriteCheckpoint();if(checkpointsFile){checkpointsFile.flush();checkpointsFile.close();}}

void ls5DetectInterrupted(){
  File root=SD.open("/");if(!root)return;uint32_t count=0;
  for(File item=root.openNextFile();item;item=root.openNextFile()){
    if(item.isDirectory()){String name=item.name();if(name.startsWith("EVENT-")||name.startsWith("/EVENT-")){String base=name.startsWith("/")?name:String("/")+name;File start=SD.open(base+"/session-start.json",FILE_READ);File result=SD.open(base+"/test-result.json",FILE_READ);if(start&&!result){++count;Serial.printf("{\"type\":\"PREVIOUS_SESSION_INTERRUPTED\",\"dir\":\"%s\"}\n",base.c_str());}if(start)start.close();if(result)result.close();}}
    item.close();
  }root.close();Serial.printf("{\"type\":\"LS5_BOOT_SCAN\",\"interrupted_sessions\":%u}\n",count);
}
