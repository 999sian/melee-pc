// SPDX-License-Identifier: GPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';

// Run the engine's own upload bridge against a queue that snapshots
// writeBuffer's input, as WebGPU does. Real-driver ordering is checked by the
// headed run. The file is an Emscripten JS library, so load it the way the
// linker does: it registers itself through addToLibrary.
const library = {};
new Function('addToLibrary', readFileSync(new URL('../../../extern/aurora/lib/gfx/browser_upload.js', import.meta.url), 'utf8'))(
  (entries) => Object.assign(library, entries));
const upload = library.browser_upload_pools;
test('queued staging snapshots only used bytes without waiting for GPU mappings',()=>{
 const keys=['Module','HEAPU8','HEAPU32','WebGPU'];const saved=Object.fromEntries(keys.map(k=>[k,globalThis[k]]));
 const heap=new SharedArrayBuffer(2048),bytes=new Uint8Array(heap),words=new Uint32Array(heap),writes=[];
 const buffers=[{},{}],queue={writeBuffer(buffer,offset,data,start,size){
  assert.ok(data.buffer instanceof ArrayBuffer,'shared WASM memory must not reach WebGPU');
  writes.push({buffer,offset,bytes:Array.from(data.subarray(start,start+size))});
 }};
 Object.assign(globalThis,{Module:{},HEAPU8:bytes,HEAPU32:words,WebGPU:{getJsObject:id=>id===3?queue:buffers[id-1]}});
 try{
  words.set([1,512,8,2,1024,20]);bytes.set([1,2,3,4,5,6,7,8],512);bytes.fill(42,1024,1088);
  assert.equal(upload(3,0,2),undefined,'upload must not suspend simulation');
  assert.deepEqual(writes.map(w=>w.bytes),[[1,2,3,4,5,6,7,8],Array(20).fill(42)]);
  assert.deepEqual(writes.map(w=>w.buffer),buffers);
  const scratch=Module.gpuUploadScratch;
  bytes.fill(9,512,520);upload(3,0,2);
  assert.equal(Module.gpuUploadScratch,scratch,'steady frames reuse CPU storage');
  assert.deepEqual(writes[0].bytes,[1,2,3,4,5,6,7,8],'later writes cannot change earlier submissions');
  words[5]=40;upload(3,0,2);assert.equal(Module.gpuUploadScratch.length,64);
  const grown=Module.gpuUploadScratch;words[5]=4;upload(3,0,2);assert.equal(Module.gpuUploadScratch,grown);
  words[2]=0;words[5]=0;const before=writes.length;upload(3,0,2);assert.equal(writes.length,before,'empty pools are skipped');
 }finally{Object.assign(globalThis,saved);}
});
