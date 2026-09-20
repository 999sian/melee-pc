// Emscripten JS library for frame.cpp (linked with --js-library, see
// aurora_core.cmake). Kept out of an EM_JS block so
// platforms/browser/tests/staging.test.mjs can run this exact source.
addToLibrary({
  // Queue writes copy only the used bytes. Mapping a staging buffer instead
  // waits for prior GPU work (shader compilation included) and stalls the
  // simulation and audio with it.
  //
  // entries: `count` triples of {wgpu buffer handle, data pointer, byte size}.
  browser_upload_pools__deps: ['$WebGPU'],
  browser_upload_pools: (queue, entries, count) => {
    const gpuQueue = WebGPU.getJsObject(queue);
    for (let i = 0; i < count; i++) {
      const at = (entries >>> 2) + i * 3;
      const size = HEAPU32[at + 2], data = HEAPU32[at + 1];
      if (!size) continue;
      // WebGPU needs an ordinary buffer, not the shared wasm heap. Reuse one
      // scratch allocation: writeBuffer snapshots it before returning.
      let scratch = Module.gpuUploadScratch;
      if (!scratch || scratch.length < size) {
        scratch = Module.gpuUploadScratch = new Uint8Array(2 ** Math.ceil(Math.log2(size)));
      }
      scratch.set(HEAPU8.subarray(data, data + size));
      gpuQueue.writeBuffer(WebGPU.getJsObject(HEAPU32[at]), 0, scratch, 0, size);
    }
  },
});
