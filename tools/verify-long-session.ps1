param([Parameter(Mandatory=$true)][string]$SessionPath)
$ErrorActionPreference='Stop'
$root=(Resolve-Path -LiteralPath $SessionPath).Path
Add-Type -TypeDefinition @'
using System;
using System.IO;
public static class KnxCrc32 {
  static readonly uint[] T=Build();
  static uint[] Build(){var t=new uint[256];for(uint i=0;i<256;i++){uint c=i;for(int k=0;k<8;k++)c=(c&1)!=0?0xEDB88320U^(c>>1):c>>1;t[i]=c;}return t;}
  public static uint Update(uint seed, byte[] b, int n){uint c=~seed;for(int i=0;i<n;i++)c=T[(c^b[i])&255]^(c>>8);return ~c;}
  public static uint File(string path, ref uint session){using(var f=System.IO.File.OpenRead(path)){var b=new byte[1048576];int n;uint c=0;while((n=f.Read(b,0,b.Length))>0){c=Update(c,b,n);session=Update(session,b,n);}return c;}}
  public static uint Range(string path,long offset,int length){using(var f=System.IO.File.OpenRead(path)){f.Position=offset;var b=new byte[65536];int left=length;uint c=0;while(left>0){int n=f.Read(b,0,Math.Min(left,b.Length));if(n<=0)throw new EndOfStreamException();c=Update(c,b,n);left-=n;}return c;}}
}
'@
function U64($v){[Convert]::ToUInt64([string]$v,[Globalization.CultureInfo]::InvariantCulture)}
$startFile=Join-Path $root 'session-start.json'
if(!(Test-Path -LiteralPath $startFile)){
  $openManifest=Join-Path $root 'segments.jsonl';$hasOpen=$false
  if(Test-Path -LiteralPath $openManifest){$hasOpen=Select-String -LiteralPath $openManifest -SimpleMatch 'SEGMENT_OPEN' -Quiet}
  [pscustomobject]@{pass=$false;state='INTERRUPTED';schema_version=$null;session_id=$null;segments=0;chunks=0;raw_bytes=0;errors=@('session-start.json missing');open_segment=$hasOpen;media_verification='NOT_APPLICABLE_INCOMPLETE_SESSION'}|ConvertTo-Json -Depth 4
  exit 2
}
$start=Get-Content -Raw -LiteralPath $startFile|ConvertFrom-Json
$manifest=Join-Path $root 'segments.jsonl';if(!(Test-Path -LiteralPath $manifest)){throw 'segments.jsonl missing'}
$records=@(Get-Content -LiteralPath $manifest|Where-Object{$_}|ForEach-Object{$_|ConvertFrom-Json})
$opens=@{};$complete=@{}
foreach($r in $records){$i=[uint32]$r.segment_index;if($r.record_type-eq'SEGMENT_OPEN'){$opens[$i]=$r}elseif($r.record_type-eq'SEGMENT_COMPLETE'){$complete[$i]=$r}}
$unclosed=@($opens.Keys|Where-Object{-not $complete.ContainsKey($_)})
$fail=[Collections.Generic.List[string]]::new();$sessionCrc=[uint32]0;$previousEnd=$null;$segments=0
foreach($i in @($complete.Keys|Sort-Object)){
  if($i-ne$segments){$fail.Add("segment index discontinuity at $i")}
  $r=$complete[$i];$file=Join-Path $root ([string]$r.filename)
  if(!(Test-Path -LiteralPath $file)){$fail.Add("missing $($r.filename)");continue}
  $bytes=U64 $r.raw_bytes;$actual=(Get-Item -LiteralPath $file).Length;if($actual-ne$bytes){$fail.Add("size mismatch segment $i")}
  $crc=[KnxCrc32]::File($file,[ref]$sessionCrc);if(('{0:X8}'-f$crc)-ne([string]$r.crc32).ToUpperInvariant()){$fail.Add("CRC segment $i")}
  $ss=U64 $r.sample_start;$se=U64 $r.sample_end;if($null-ne$previousEnd-and$previousEnd-ne$ss){$fail.Add("segment sample boundary $($i-1)->$i")};$previousEnd=$se;++$segments
}
$chunksFile=Join-Path $root 'chunks.jsonl';$segmentOffsets=@{};$previousSampleEnd=$null;$sampleGaps=0L;$duplicates=0L;$chunkCount=0
if(Test-Path -LiteralPath $chunksFile){
  foreach($line in [IO.File]::ReadLines($chunksFile)){if(!$line){continue};$c=$line|ConvertFrom-Json;$si=[uint32]$c.segment_index;$off=U64 $c.segment_offset;$n=[int](U64 $c.raw_bytes);$ss=U64 $c.sample_start;$se=U64 $c.sample_end
    $expected=if($segmentOffsets.ContainsKey($si)){$segmentOffsets[$si]}else{[uint64]0};if($off-ne$expected){$fail.Add("raw offset discontinuity chunk $chunkCount")};$segmentOffsets[$si]=$off+[uint64]$n
    if($null-ne$previousSampleEnd){if($ss-lt$previousSampleEnd){++$duplicates}elseif($ss-gt$previousSampleEnd){++$sampleGaps}};$previousSampleEnd=$se
    $segPath=Join-Path $root ('raw-{0:D4}.bin'-f$si);$crc=[KnxCrc32]::Range($segPath,[long]$off,$n);if(('{0:X8}'-f$crc)-ne([string]$c.crc32).ToUpperInvariant()){$fail.Add("CRC chunk $chunkCount")};++$chunkCount
  }
}
$indexFile=Join-Path $root 'chunk-index.jsonl';$indexCount=0
if(Test-Path -LiteralPath $indexFile){$cf=[IO.File]::OpenRead($chunksFile);try{foreach($line in [IO.File]::ReadLines($indexFile)){if(!$line){continue};$ix=$line|ConvertFrom-Json;$cf.Position=[long](U64 $ix.chunks_jsonl_offset);$sr=[IO.StreamReader]::new($cf,[Text.Encoding]::UTF8,$false,4096,$true);$mapped=$sr.ReadLine()|ConvertFrom-Json;$sr.Dispose();if((U64 $mapped.sample_start)-ne(U64 $ix.sample_start)-or[uint32]$mapped.segment_index-ne[uint32]$ix.segment_index){$fail.Add("sparse index entry $indexCount")};++$indexCount}}finally{$cf.Dispose()}}
$resultFile=Join-Path $root 'test-result.json';$closed=$false;if(Test-Path -LiteralPath $resultFile){$tr=Get-Content -Raw -LiteralPath $resultFile|ConvertFrom-Json;$closed=[bool]$tr.closed}
$state=if($unclosed.Count){'INTERRUPTED'}elseif($closed){'CLOSED'}else{'INCOMPLETE'}
$pass=$fail.Count-eq0-and$unclosed.Count-eq0-and$closed
[pscustomobject]@{pass=$pass;state=$state;schema_version=$start.schema_version;session_id=$start.session_id;segments=$segments;chunks=$chunkCount;raw_bytes=($complete.Values|ForEach-Object{U64 $_.raw_bytes}|Measure-Object -Sum).Sum;segment_crc_failures=@($fail|Where-Object{$_-like'CRC segment*'}).Count;chunk_crc_failures=@($fail|Where-Object{$_-like'CRC chunk*'}).Count;sample_gaps=$sampleGaps;sample_overlaps=$duplicates;sparse_index_entries=$indexCount;session_crc32=('{0:X8}'-f$sessionCrc);errors=@($fail);media_verification='PERFORMED_OFFLINE'}|ConvertTo-Json -Depth 4
if(!$pass){exit 2}
