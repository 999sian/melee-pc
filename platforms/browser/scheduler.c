/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <emscripten.h>

/* Yield to GPU promises without the nested setTimeout minimum delay. */
// clang-format off
EM_ASYNC_JS(void, browser_yield, (void), {
  await new Promise(resolve => {
    if (!Module.browserYieldChannel) {
      const channel = new MessageChannel();
      Module.browserYieldQueue = [];
      channel.port1.onmessage = () => Module.browserYieldQueue.shift()();
      Module.browserYieldChannel = channel;
    }
    Module.browserYieldQueue.push(resolve);
    Module.browserYieldChannel.port2.postMessage(0);
  });
});
// clang-format on
