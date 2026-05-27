#pragma once

#include <jni.h>

namespace hide {

__attribute__((visibility("default")))
bool init();

__attribute__((visibility("default")))
bool hide_handle(void *handle);

__attribute__((visibility("default")))
void restore_handle(void *handle);

__attribute__((visibility("default")))
bool setup_maps_hide();

__attribute__((visibility("default")))
void add_maps_filter(const char *pattern);

__attribute__((visibility("default")))
void set_maps_hide_active(bool active);

__attribute__((visibility("default")))
void cleanup();

} // namespace hide

void hook_java_system_properties(
        JNIEnv *env,
        void (*hooker)(JNIEnv *, const char *, JNINativeMethod *, int));
