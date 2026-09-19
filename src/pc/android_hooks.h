/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Platform hooks the LAN lobby needs from the Android Java side
 * (src/pc/android_compat.cpp). Every one of these is a no-op, or returns
 * NULL, on every other platform, so callers need no #ifdef. */
#ifndef PC_ANDROID_HOOKS_H
#define PC_ANDROID_HOOKS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Hold a WifiManager.MulticastLock while LAN discovery runs. Without it the
 * Wi-Fi stack drops every multicast frame not addressed to this device, so
 * the mDNS group is joined, our announces go out, and no answer ever comes
 * back: pc_lan_discovery_unavailable() turns true after 5 s and the lobby
 * stays empty (docs/netcode-plan.md §8).
 *
 * Nesting is counted: the OS lock is taken on the 0 -> 1 acquire and dropped
 * on the 1 -> 0 release, so repeated pc_lan_start()/pc_lan_stop() cycles in
 * one process are safe, as is the atexit(pc_lan_stop) path. An unbalanced
 * release is ignored rather than letting MulticastLock.release() throw.
 * Safe to call from any thread. */
void pc_android_multicast_lock_acquire(void);
void pc_android_multicast_lock_release(void);

/* The user's device name ("Sian's Pixel 7") reduced to a DNS label, or NULL
 * when there is none to be had. gethostname() is no use on Android: stock
 * devices leave the kernel hostname at "localhost", so every instance would
 * announce localhost-<id>._meleepc._udp.local. and claim localhost.local.
 * The result is cached; the buffer is owned by the callee. */
const char* pc_android_device_name(void);

#ifdef __cplusplus
}
#endif

#endif
