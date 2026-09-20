// SPDX-License-Identifier: GPL-3.0-or-later
/** Bounded local-disc cache. Hits return bytes synchronously to avoid a WASM suspension. */
export function createDiscCache(file,{blockBytes=512*1024,maxBytes=32*1024*1024}={}){
 if(!file||!Number.isSafeInteger(file.size)||blockBytes<1||maxBytes<blockBytes)throw Error('Invalid disc cache configuration.');
 const blocks=new Map(),pending=new Map();let bytes=0;
 const stats={reads:0,hits:0,sourceBytes:0};
 function touch(index,data){blocks.delete(index);blocks.set(index,data);}
 async function fetchBlock(index){
  if(pending.has(index))return pending.get(index);
  const promise=(async()=>{const start=index*blockBytes,end=Math.min(start+blockBytes,file.size),data=new Uint8Array(await file.slice(start,end).arrayBuffer());if(data.length!==end-start)throw Error('Short disc read.');stats.sourceBytes+=data.length;touch(index,data);bytes+=data.length;while(bytes>maxBytes){const [key,old]=blocks.entries().next().value;blocks.delete(key);bytes-=old.length;}return data;})();
  pending.set(index,promise);try{return await promise;}finally{pending.delete(index);}
 }
 function read(offset,size){
  if(!Number.isSafeInteger(offset)||!Number.isSafeInteger(size)||offset<0||size<0||offset+size>file.size)throw Error('Disc read is out of bounds.');
  stats.reads++;if(!size)return new Uint8Array();
  const first=Math.floor(offset/blockBytes),last=Math.floor((offset+size-1)/blockBytes);
  const pieces=[];let missing=false;
  for(let index=first;index<=last;index++){const data=blocks.get(index);if(data){stats.hits++;touch(index,data);pieces.push(data);}else{missing=true;pieces.push(fetchBlock(index));}}
  const combine=values=>{if(first===last)return values[0].subarray(offset-first*blockBytes,offset-first*blockBytes+size);const result=new Uint8Array(size);let written=0;values.forEach((data,i)=>{const start=i===0?offset-first*blockBytes:0,n=Math.min(data.length-start,size-written);result.set(data.subarray(start,start+n),written);written+=n;});return result;};
  return missing?Promise.all(pieces).then(combine):combine(pieces);
 }
 return {read,stats,get residentBytes(){return bytes;}};
}
