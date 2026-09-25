#pragma once
#include <esp_vfs_fat.h>

void runLs5bBenchmark() {
  if (runState.load() != RS_IDLE) return;
  char leaf[32]; snprintf(leaf,sizeof(leaf),"LS5B-BENCH-%08lX",(unsigned long)esp_random());
  const String directory=String("/")+leaf, relative=directory+"/raw-0000.bin", absolute=String("/sd")+relative;
  constexpr uint64_t capacity=512ULL*1024ULL*1024ULL;
  uint64_t t=esp_timer_get_time(); bool made=SD.mkdir(directory); uint64_t mkdirUs=esp_timer_get_time()-t;
  if(!made){Serial.printf("{\"type\":\"LS5B_BENCH\",\"pass\":false,\"stage\":\"mkdir\",\"dir\":\"%s\"}\n",directory.c_str());return;}
  t=esp_timer_get_time();errno=0;esp_err_t er=esp_vfs_fat_create_contiguous_file("/sd",absolute.c_str(),capacity,true);int savedErrno=errno;uint64_t expandUs=esp_timer_get_time()-t;
  uint64_t observed=0,reopenUs=0,ioUs=0;bool reopenOk=false,seekOk=false,markerOk=false,readbackOk=false;
  static const uint8_t marker[16]={'L','S','5','B','-','P','R','E','P','A','R','E','D','\r','\n',0};
  if(er==ESP_OK){t=esp_timer_get_time();File f=SD.open(relative,"r+");reopenUs=esp_timer_get_time()-t;reopenOk=bool(f);if(f){observed=f.size();t=esp_timer_get_time();seekOk=f.seek(0);markerOk=seekOk&&f.write(marker,sizeof(marker))==sizeof(marker);if(markerOk)f.flush();ioUs=esp_timer_get_time()-t;f.close();}}
  File c=SD.open(relative,FILE_READ);if(c){uint8_t got[sizeof(marker)]={};readbackOk=c.read(got,sizeof(got))==sizeof(got)&&memcmp(got,marker,sizeof(got))==0;c.close();}
  bool pass=er==ESP_OK&&observed==capacity&&reopenOk&&seekOk&&markerOk&&readbackOk;
  Serial.printf("{\"type\":\"LS5B_BENCH\",\"pass\":%s,\"dir\":\"%s\",\"capacity_bytes\":\"%llu\",\"observed_size\":\"%llu\",\"mkdir_us\":\"%llu\",\"expand_close_sync_us\":\"%llu\",\"reopen_us\":\"%llu\",\"seek_write_flush_us\":\"%llu\",\"expand_result\":%d,\"errno\":%d,\"readback\":%s}\n",pass?"true":"false",directory.c_str(),capacity,observed,mkdirUs,expandUs,reopenUs,ioUs,int(er),savedErrno,readbackOk?"true":"false");
}
