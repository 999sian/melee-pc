// SPDX-License-Identifier: GPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import {checkGraphics} from '../gpu-preflight.mjs';

test('missing WebGPU and null adapters give actionable errors',async()=>{
 await assert.rejects(checkGraphics(null),/cannot initialize WebGPU.*navigator.gpu is unavailable/);
 await assert.rejects(checkGraphics({requestAdapter:async()=>null}),/cannot initialize WebGPU.*requestAdapter returned null/);
});
test('requests Aurora features and limits and releases the probe device',async()=>{
 let destroyed=false;
 await checkGraphics({requestAdapter:async options=>{
  assert.deepEqual(options,{featureLevel:'compatibility',powerPreference:'high-performance',forceFallbackAdapter:false});
  return {features:new Set(['texture-compression-bc','unrelated']),limits:{minUniformBufferOffsetAlignment:256,minStorageBufferOffsetAlignment:256,maxTextureDimension2D:8192},requestDevice:async descriptor=>{
   assert.deepEqual(descriptor,{requiredFeatures:['texture-compression-bc'],requiredLimits:{maxStorageBuffersPerShaderStage:2,minUniformBufferOffsetAlignment:256,minStorageBufferOffsetAlignment:256,maxTextureDimension2D:8192}});
   return {destroy(){destroyed=true;}};
  }};
 }});
 assert.equal(destroyed,true);
});
test('device rejection retains its underlying cause',async()=>{
 const cause=Error('driver device creation failed');
 await assert.rejects(checkGraphics({requestAdapter:async()=>({features:new Set(),limits:{},requestDevice:async()=>{throw cause;}})}),error=>error.cause===cause&&error.message.includes(cause.message));
});
