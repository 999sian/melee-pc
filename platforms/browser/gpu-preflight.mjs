// SPDX-License-Identifier: GPL-3.0-or-later
/** Keep in sync with extern/aurora/lib/webgpu/gpu.cpp and emdawnwebgpu. */
export async function checkGraphics(gpu = navigator.gpu) {
 try {
  if (!gpu) throw Error('navigator.gpu is unavailable');
  const adapter = await gpu.requestAdapter({featureLevel:'compatibility',powerPreference:'high-performance',forceFallbackAdapter:false});
  if (!adapter) throw Error('requestAdapter returned null');
  const requiredLimits = {maxStorageBuffersPerShaderStage:2,
   minUniformBufferOffsetAlignment:Math.max(64,adapter.limits.minUniformBufferOffsetAlignment),
   minStorageBufferOffsetAlignment:Math.max(16,adapter.limits.minStorageBufferOffsetAlignment)};
  for (const name of ['maxTextureDimension1D','maxTextureDimension2D','maxTextureDimension3D','maxTextureArrayLayers']) {
   if (adapter.limits[name]) requiredLimits[name] = adapter.limits[name];
  }
  const requiredFeatures = ['core-features-and-limits','texture-compression-bc','texture-compression-astc','texture-component-swizzle'].filter(name=>adapter.features.has(name));
  const device = await adapter.requestDevice({requiredFeatures,requiredLimits});
  device.destroy();
 } catch (cause) {
  throw new Error(`Melee cannot initialize WebGPU on this browser/GPU. Check browser hardware acceleration and graphics driver support. Linux/NVIDIA support depends on the browser and driver configuration. Details: ${cause?.message || String(cause)}`, {cause});
 }
}
