#pragma once

#include <stdint.h>
#include <jni.h>
#include <vector>

extern void *self_handle;

void hook_functions();

void revert_unmount_ksu();

void revert_unmount_apatch();

void revert_unmount_magisk();

void hide_from_maps();

void hide_adb_in_memory();

void hook_native_properties();

void rehook_properties_for_app();

void hook_dlopen_for_rehook();

void set_hide_active(bool active);

bool patch_prop_data(char *data, size_t size);
