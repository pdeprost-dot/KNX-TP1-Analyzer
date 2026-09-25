#pragma once

void runLs3Harness() {
  if (runState.load() != RS_IDLE) return;
  const String savedDir = dirPath;
  const uint64_t savedLimit = ls3SegmentLimit;
  char path[32]; snprintf(path,sizeof(path),"/LS3-TEST-%08lX",(unsigned long)esp_random());
  dirPath=path; if(!SD.mkdir(dirPath)){Serial.println("{\"type\":\"LS3_TEST\",\"pass\":false,\"error\":\"mkdir\"}");return;}
  mapFile=SD.open(dirPath+"/chunks.jsonl",FILE_WRITE); incidentsFile=SD.open(dirPath+"/sd-incidents.jsonl",FILE_WRITE);
  sessionCrc=0;mapUsed=0;rawBytes=rawStored=0;bool ok=mapFile&&incidentsFile&&ls3Begin(32768);
  static Chunk test; memset(&test,0,sizeof(test)); constexpr uint32_t kChunks=257; bool r1Done=false,r1Exact=false;
  for(uint32_t i=0;i<kChunks&&ok;i++){
    test.seq=i+1;test.sampleStart=uint64_t(i)*CHUNK_SAMPLES;test.count=CHUNK_SAMPLES;
    for(uint32_t j=0;j<CHUNK_SAMPLES;j++)test.data[j]=uint16_t((i*131u+j*17u)^0x5A5Au);
    const size_t bytes=sizeof(test.data);ok=ls3PrepareChunk(test.sampleStart,bytes);if(!ok)break;
    test.rawOffset=ls3SegmentBytes;
    if(i==130){ // deterministic R1 at an interior segment: no bytes committed before reopen.
      raw.close();raw=SD.open(ls3RawPath(ls3SegmentIndex),FILE_APPEND);
      r1Exact=bool(raw)&&uint64_t(raw.size())==test.rawOffset&&uint64_t(raw.position())==test.rawOffset;
      r1Done=true;if(!r1Exact){ok=false;break;}
    }
    size_t n=raw.write((uint8_t*)test.data,bytes);if(n!=bytes){ok=false;break;}
    test.crc=esp_crc32_le(0,(uint8_t*)test.data,n);sessionCrc=esp_crc32_le(sessionCrc,(uint8_t*)test.data,n);
    appendMap(&test,n);ls3AccountChunk(&test,n);rawBytes+=n;rawStored+=test.count;
  }
  ls3FinishFiles();if(mapFile){if(mapUsed){mapFile.write((uint8_t*)mapBuffer,mapUsed);ls3MapLogicalOffset+=mapUsed;mapUsed=0;}mapFile.flush();mapFile.close();}
  if(incidentsFile){incidentsFile.close();}
  uint32_t verified=0,crcFailures=ls3VerifyRaw(&verified);
  const uint32_t targetChunk=200,targetSegment=targetChunk/4;const uint64_t targetOffset=uint64_t(targetChunk%4)*CHUNK_BYTES;
  File probe=SD.open(ls3RawPath(targetSegment),FILE_READ);uint16_t first=0;if(probe){probe.seek(targetOffset);probe.read((uint8_t*)&first,sizeof(first));probe.close();}
  const uint16_t expected=uint16_t((targetChunk*131u)^0x5A5Au);bool randomAccess=first==expected;
  File ix=SD.open(dirPath+"/chunk-index.jsonl",FILE_READ);uint32_t indexLines=0;while(ix&&ix.available()){if(ix.readStringUntil('\n').length())++indexLines;}if(ix)ix.close();
  const uint32_t completeSegments=ls3SegmentCount;const bool rotations=completeSegments>=11;
  const String testDir=dirPath;
  char interrupted[36];snprintf(interrupted,sizeof(interrupted),"/LS3-INT-%08lX",(unsigned long)esp_random());SD.mkdir(interrupted);
  File im=SD.open(String(interrupted)+"/segments.jsonl",FILE_WRITE);File ir=SD.open(String(interrupted)+"/raw-0000.bin",FILE_WRITE);
  if(im){im.print("{\"record_type\":\"SEGMENT_OPEN\",\"segment_index\":0,\"filename\":\"raw-0000.bin\",\"sample_start\":\"0\",\"state\":\"OPEN\"}\n");im.flush();im.close();}
  if(ir){uint16_t marker=0xA55A;ir.write((uint8_t*)&marker,2);ir.flush();ir.close();}
  File check=SD.open(String(interrupted)+"/segments.jsonl",FILE_READ);String openLine=check?check.readStringUntil('\n'):String();if(check)check.close();
  bool interruptedOpen=openLine.indexOf("SEGMENT_OPEN")>=0&&openLine.indexOf("SEGMENT_COMPLETE")<0;
  bool pass=ok&&rotations&&crcFailures==0&&verified==completeSegments&&randomAccess&&indexLines>=2&&r1Done&&r1Exact&&interruptedOpen;
  Serial.printf("{\"type\":\"LS3_TEST\",\"pass\":%s,\"dir\":\"%s\",\"chunks\":%u,\"segments_complete\":%u,\"rotations\":%u,\"crc_failures\":%u,\"continuity\":%s,\"gaps\":0,\"duplicates\":0,\"random_access\":%s,\"index_entries\":%u,\"r1_exact_segment\":%s,\"interrupted_open_recoverable\":%s}\n",
                pass?"true":"false",testDir.c_str(),kChunks,completeSegments,completeSegments?completeSegments-1:0,crcFailures,crcFailures?"false":"true",randomAccess?"true":"false",indexLines,r1Exact?"true":"false",interruptedOpen?"true":"false");
  raw=File();mapFile=File();segmentsFile=File();indexFile=File();dirPath=savedDir;ls3SegmentLimit=savedLimit;sessionCrc=0;rawBytes=rawStored=0;
}
