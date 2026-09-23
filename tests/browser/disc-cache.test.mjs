// SPDX-License-Identifier: GPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import {createDiscCache} from '../../platforms/browser/disc-cache.mjs';
test('exact cached reads, cross-block boundaries, short final block, and bounded eviction',async()=>{
 const bytes=Uint8Array.from({length:35},(_,i)=>i),cache=createDiscCache(new Blob([bytes]),{blockBytes:8,maxBytes:16});
 assert.deepEqual(await cache.read(5,13),bytes.slice(5,18));
 assert(cache.read(12,5) instanceof Uint8Array);assert.deepEqual(cache.read(12,5),bytes.slice(12,17));
 assert.deepEqual(await cache.read(32,3),bytes.slice(32));assert(cache.residentBytes<=16);
 assert.deepEqual(await cache.read(0,4),bytes.slice(0,4));assert.throws(()=>cache.read(34,2),/bounds/);
});
test('coalesces concurrent reads without corrupting cache accounting',async()=>{
 let reads=0;const blob=new Blob([new Uint8Array(64)]);const file={size:64,slice(...args){reads++;return blob.slice(...args)}};
 const cache=createDiscCache(file,{blockBytes:16,maxBytes:32});
 await Promise.all([cache.read(0,8),cache.read(2,8)]);assert.equal(reads,1);assert.equal(cache.residentBytes,16);
});
