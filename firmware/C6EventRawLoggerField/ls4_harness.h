#pragma once

void runLs4ScaleHarness() {
  if(runState.load()!=RS_IDLE)return;
  static Chunk test; memset(&test,0,sizeof(test));
  const uint32_t mib[]={1,10,50}; const String savedDir=dirPath; const uint64_t savedLimit=ls3SegmentLimit;
  for(uint8_t caseIndex=0;caseIndex<3;caseIndex++){
    char path[36];snprintf(path,sizeof(path),"/LS4-SCALE-%u-%08lX",mib[caseIndex],(unsigned long)esp_random());dirPath=path;
    bool ok=SD.mkdir(dirPath);mapFile=SD.open(dirPath+"/chunks.jsonl",FILE_WRITE);sessionCrc=0;mapUsed=0;rawBytes=rawStored=0;ls3SegmentLimit=LS3_PRODUCTION_SEGMENT_BYTES;
    ok=ok&&mapFile&&ls3Begin(ls3SegmentLimit);const uint32_t chunksToWrite=mib[caseIndex]*128U;
    for(uint32_t i=0;i<chunksToWrite&&ok;i++){
      test.seq=i+1;test.sampleStart=uint64_t(i)*CHUNK_SAMPLES;test.count=CHUNK_SAMPLES;
      for(uint32_t j=0;j<CHUNK_SAMPLES;j++)test.data[j]=uint16_t((i*73u+j*29u)^0x4C53u);
      const size_t n=sizeof(test.data);ok=ls3PrepareChunk(test.sampleStart,n);if(!ok)break;test.rawOffset=ls3SegmentBytes;
      if(raw.write((uint8_t*)test.data,n)!=n){ok=false;break;}test.crc=esp_crc32_le(0,(uint8_t*)test.data,n);sessionCrc=esp_crc32_le(sessionCrc,(uint8_t*)test.data,n);appendMap(&test,n);ls3AccountChunk(&test,n);rawBytes+=n;rawStored+=test.count;
    }
    const uint64_t total0=esp_timer_get_time();const uint64_t close0=esp_timer_get_time();ls3FinishFiles();const uint64_t closeUs=esp_timer_get_time()-close0;
    const uint64_t metadata0=esp_timer_get_time();if(mapFile){if(mapUsed){mapFile.write((uint8_t*)mapBuffer,mapUsed);ls3MapLogicalOffset+=mapUsed;mapUsed=0;}mapFile.flush();mapFile.close();}const uint64_t metadataUs=esp_timer_get_time()-metadata0;
    const uint64_t json0=esp_timer_get_time();File summary=SD.open(dirPath+"/scale-result.json",FILE_WRITE);if(summary){summary.printf("{\"raw_bytes\":%llu,\"closed\":true,\"media_verification\":\"NOT_PERFORMED_ON_DEVICE\"}\n",rawBytes.load());summary.flush();summary.close();}else ok=false;const uint64_t jsonUs=esp_timer_get_time()-json0;
    const uint64_t totalUs=esp_timer_get_time()-total0;
    Serial.printf("{\"type\":\"LS4_SCALE\",\"pass\":%s,\"raw_mib\":%u,\"raw_bytes\":%llu,\"writer_drain_us\":0,\"metadata_drain_us\":%llu,\"segment_close_flush_us\":%llu,\"final_metadata_us\":%llu,\"stop_to_closed_us\":%llu}\n",ok?"true":"false",mib[caseIndex],rawBytes.load(),metadataUs,closeUs,jsonUs,totalUs);
    raw=File();mapFile=File();segmentsFile=File();indexFile=File();
  }
  dirPath=savedDir;ls3SegmentLimit=savedLimit;sessionCrc=0;rawBytes=rawStored=0;
}
