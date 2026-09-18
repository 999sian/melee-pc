/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Android platform hooks that have to go through the Java side. The CMake
 * glob (CMakeLists.txt:141) compiles this everywhere, so everything outside
 * __ANDROID__ is a no-op and callers need no #ifdef. */
#include "android_hooks.h"

#if defined(__ANDROID__)

#include "pc.h"

#include <SDL3/SDL_system.h>
#include <jni.h>
#include <mutex>

/* Every signature below was taken from android-34 android.jar with
 * `javap -s -constants`, not from memory:
 *   Context.getApplicationContext   ()Landroid/content/Context;
 *   Context.getSystemService        (Ljava/lang/String;)Ljava/lang/Object;
 *   Context.WIFI_SERVICE            = "wifi"
 *   WifiManager.createMulticastLock (Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;
 *   MulticastLock.setReferenceCounted (Z)V
 *   MulticastLock.acquire           ()V
 *   MulticastLock.release           ()V
 *   Context.getContentResolver      ()Landroid/content/ContentResolver;
 *   Settings$Global.getString       (Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;  [static]
 *   Settings$Global.DEVICE_NAME     = "device_name"
 *   Build.MODEL                     Ljava/lang/String;  [static]
 * The lock needs android.permission.CHANGE_WIFI_MULTICAST_STATE
 * (AndroidManifest.xml:16); it is a `normal` permission, so it is granted at
 * install and there is nothing to request at runtime. Reading a Settings
 * global and Build.MODEL needs no permission at all. */

namespace {

std::mutex g_mutex;
int g_depth;        /* acquire() nesting, guarded by g_mutex */
jobject g_lock;     /* global ref: WifiManager$MulticastLock */
jmethodID g_acquire;
jmethodID g_release;
bool g_failed;      /* the lookup failed once: do not report it again */

/* True (and clears the exception) when the last JNI call left one pending. */
bool threw(JNIEnv* env, const char* what) {
    if (env->ExceptionCheck() == JNI_FALSE) {
        return false;
    }
    env->ExceptionDescribe(); /* the stack trace goes to logcat */
    env->ExceptionClear();
    pc_log_line("lan: multicast lock: %s threw", what);
    return true;
}

/* Create the lock once and keep it: SDL's activity is a Context, so
 * getApplicationContext() -> getSystemService("wifi") ->
 * createMulticastLock("melee-lan"). Local refs live in a pushed frame,
 * including the one SDL_GetAndroidActivity() hands back. */
bool lock_open(JNIEnv* env) {
    if (g_lock != nullptr) {
        return true;
    }
    if (g_failed) {
        return false;
    }
    if (env->PushLocalFrame(16) != JNI_OK) {
        threw(env, "PushLocalFrame");
        g_failed = true;
        return false;
    }
    bool ok = [env]() -> bool {
        jobject activity = (jobject) SDL_GetAndroidActivity();
        if (activity == nullptr) {
            pc_log_line("lan: no Android activity: no multicast lock, mDNS answers may be filtered");
            return false;
        }
        jclass ctx_cls = env->FindClass("android/content/Context");
        jclass wifi_cls = env->FindClass("android/net/wifi/WifiManager");
        jclass lock_cls = env->FindClass("android/net/wifi/WifiManager$MulticastLock");
        if (ctx_cls == nullptr || wifi_cls == nullptr || lock_cls == nullptr) {
            return false;
        }
        jmethodID app_ctx = env->GetMethodID(ctx_cls, "getApplicationContext", "()Landroid/content/Context;");
        jmethodID get_svc = env->GetMethodID(ctx_cls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
        jmethodID create = env->GetMethodID(wifi_cls, "createMulticastLock",
                                            "(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;");
        jmethodID counted = env->GetMethodID(lock_cls, "setReferenceCounted", "(Z)V");
        g_acquire = env->GetMethodID(lock_cls, "acquire", "()V");
        g_release = env->GetMethodID(lock_cls, "release", "()V");
        if (app_ctx == nullptr || get_svc == nullptr || create == nullptr || counted == nullptr ||
            g_acquire == nullptr || g_release == nullptr) {
            return false;
        }
        jstring service = env->NewStringUTF("wifi"); /* Context.WIFI_SERVICE */
        jstring tag = env->NewStringUTF("melee-lan");
        if (service == nullptr || tag == nullptr) {
            return false;
        }
        /* From the application context: before API 24 a manager taken from
         * the activity context keeps the activity alive. */
        jobject app = env->CallObjectMethod(activity, app_ctx);
        if (threw(env, "getApplicationContext") || app == nullptr) {
            return false;
        }
        jobject wifi = env->CallObjectMethod(app, get_svc, service);
        if (threw(env, "getSystemService")) {
            return false;
        }
        if (wifi == nullptr) { /* Wi-Fi-less device, or the service is gone */
            pc_log_line("lan: no WifiManager: no multicast lock, mDNS answers may be filtered");
            return false;
        }
        jobject lock = env->CallObjectMethod(wifi, create, tag);
        if (threw(env, "createMulticastLock") || lock == nullptr) {
            return false;
        }
        /* The nesting is counted here (android_multicast.h), so the Java
         * lock stays a plain on/off and an unbalanced release can never
         * throw out of MulticastLock.release(). */
        env->CallVoidMethod(lock, counted, JNI_FALSE);
        if (threw(env, "setReferenceCounted")) {
            return false;
        }
        g_lock = env->NewGlobalRef(lock);
        return g_lock != nullptr;
    }();
    if (!ok) {
        threw(env, "multicast lock lookup");
    }
    env->PopLocalFrame(nullptr);
    g_failed = !ok;
    return ok;
}

/* A device name is free text ("Sian's Pixel 7"); a DNS label is not. Keep
 * [A-Za-z0-9-], map the rest to '-', collapse runs, trim the ends. */
void dns_label(const char* in, char* out, size_t cap) {
    size_t n = 0;
    for (const char* p = in; *p != '\0' && n + 1 < cap; p++) {
        bool keep = (*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z');
        if (keep) {
            out[n++] = *p;
        } else if (n > 0 && out[n - 1] != '-') {
            out[n++] = '-';
        }
    }
    while (n > 0 && out[n - 1] == '-') {
        n--;
    }
    out[n] = '\0';
}

/* Settings.Global.DEVICE_NAME is the name the user typed under Settings >
 * About phone; it is null on a device where none was ever set, so fall back
 * to Build.MODEL ("Pixel 7"). Neither read needs a permission. The local
 * ref returned lives in the caller's frame. */
jstring device_name_string(JNIEnv* env) {
    jobject activity = (jobject) SDL_GetAndroidActivity();
    jclass ctx_cls = env->FindClass("android/content/Context");
    jclass set_cls = env->FindClass("android/provider/Settings$Global");
    if (activity != nullptr && ctx_cls != nullptr && set_cls != nullptr) {
        jmethodID resolver = env->GetMethodID(ctx_cls, "getContentResolver", "()Landroid/content/ContentResolver;");
        jmethodID get = env->GetStaticMethodID(set_cls, "getString",
                                               "(Landroid/content/ContentResolver;Ljava/lang/String;)Ljava/lang/String;");
        jstring key = env->NewStringUTF("device_name"); /* Settings.Global.DEVICE_NAME */
        if (resolver != nullptr && get != nullptr && key != nullptr) {
            jobject cr = env->CallObjectMethod(activity, resolver);
            if (!threw(env, "getContentResolver") && cr != nullptr) {
                jstring name = (jstring) env->CallStaticObjectMethod(set_cls, get, cr, key);
                if (!threw(env, "Settings.Global.getString") && name != nullptr && env->GetStringLength(name) > 0) {
                    return name;
                }
            }
        }
    }
    threw(env, "device name lookup");
    jclass build_cls = env->FindClass("android/os/Build");
    jfieldID model = build_cls != nullptr ? env->GetStaticFieldID(build_cls, "MODEL", "Ljava/lang/String;") : nullptr;
    jstring name = model != nullptr ? (jstring) env->GetStaticObjectField(build_cls, model) : nullptr;
    threw(env, "Build.MODEL");
    return name;
}

}  // namespace

void pc_android_multicast_lock_acquire(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_depth++ > 0) {
        return;
    }
    JNIEnv* env = (JNIEnv*) SDL_GetAndroidJNIEnv();
    if (env == nullptr || !lock_open(env)) {
        return; /* the depth still counts, so the release below stays balanced */
    }
    env->CallVoidMethod(g_lock, g_acquire);
    if (!threw(env, "MulticastLock.acquire")) {
        pc_log_line("lan: multicast lock held");
    }
}

void pc_android_multicast_lock_release(void) {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (g_depth == 0 || --g_depth > 0 || g_lock == nullptr) {
        return;
    }
    JNIEnv* env = (JNIEnv*) SDL_GetAndroidJNIEnv();
    if (env == nullptr) {
        return;
    }
    env->CallVoidMethod(g_lock, g_release);
    if (!threw(env, "MulticastLock.release")) {
        pc_log_line("lan: multicast lock released");
    }
}

const char* pc_android_device_name(void) {
    static char s_label[64];
    static bool s_done;
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!s_done) {
        s_done = true;
        JNIEnv* env = (JNIEnv*) SDL_GetAndroidJNIEnv();
        if (env != nullptr && env->PushLocalFrame(16) == JNI_OK) {
            jstring text = device_name_string(env);
            const char* utf = text != nullptr ? env->GetStringUTFChars(text, nullptr) : nullptr;
            if (utf != nullptr) {
                dns_label(utf, s_label, sizeof s_label);
                env->ReleaseStringUTFChars(text, utf);
            }
            env->PopLocalFrame(nullptr);
        }
        if (s_label[0] == '\0') { /* once per process */
            pc_log_line("lan: no Android device name: using the kernel hostname");
        }
    }
    return s_label[0] != '\0' ? s_label : nullptr;
}

#else

void pc_android_multicast_lock_acquire(void) {}
void pc_android_multicast_lock_release(void) {}

const char* pc_android_device_name(void) {
    return nullptr;
}

#endif
