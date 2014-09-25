/*
 * Copyright (C) 2014 The MoKee OpenSource Project
 * Copyright (C) 2014 The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/limits.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "adb_install.h"
#include "bmlutils/bmlutils.h"
#include "bootloader.h"
#include "common.h"
#include "cutils/android_reboot.h"
#include "cutils/properties.h"
#include "edify/expr.h"
#include "extendedcommands.h"
#include "firmware.h"
#include "flashutils/flashutils.h"
#include "install.h"
#include "make_ext4fs.h"
#include "minui/minui.h"
#include "minzip/DirUtil.h"
#include "mmcutils/mmcutils.h"
#include "mounts.h"
#include "mtdutils/mtdutils.h"
#include "nandroid.h"
#include "recovery_settings.h"
#include "recovery_ui.h"
#include "roots.h"
#include "voldclient/voldclient.h"

// top fixed menu items, those before extra storage volumes
#define FIXED_TOP_INSTALL_ZIP_MENUS 1
// bottom fixed menu items, those after extra storage volumes
#define FIXED_BOTTOM_INSTALL_ZIP_MENUS 3
#define FIXED_INSTALL_ZIP_MENUS (FIXED_TOP_INSTALL_ZIP_MENUS + FIXED_BOTTOM_INSTALL_ZIP_MENUS)

// number of actions added for each volume by add_nandroid_options_for_volume()
// these go on top of menu list
#define NANDROID_ACTIONS_NUM 4
// number of fixed bottom entries after volume actions
#define NANDROID_FIXED_ENTRIES 2

#if defined(ENABLE_LOKI) && defined(BOARD_NATIVE_DUALBOOT_SINGLEDATA)
#define FIXED_ADVANCED_ENTRIES 10
#elif !defined(ENABLE_LOKI) && defined(BOARD_NATIVE_DUALBOOT_SINGLEDATA)
#define FIXED_ADVANCED_ENTRIES 9
#elif defined(ENABLE_LOKI) && !defined(BOARD_NATIVE_DUALBOOT_SINGLEDATA)
#define FIXED_ADVANCED_ENTRIES 8
#else
#define FIXED_ADVANCED_ENTRIES 7
#endif

extern struct selabel_handle *sehandle;
int signature_check_enabled = 1;

typedef struct {
    char mount[255];
    char unmount[255];
    char path[PATH_MAX];
} MountMenuEntry;

typedef struct {
    char txt[255];
    char path[PATH_MAX];
    char type[255];
} FormatMenuEntry;

typedef struct {
    char *name;
    int can_mount;
    int can_format;
} MFMatrix;

// Prototypes of private functions that are used before defined
static void show_choose_zip_menu(const char *mount_point);
static void format_sdcard(const char* volume);
static int can_partition(const char* volume);
static int is_path_mounted(const char* path);

static int get_filtered_menu_selection(const char** headers, char** items, int menu_only, int initial_selection, int items_count) {
    int index;
    int offset = 0;
    int* translate_table = (int*)malloc(sizeof(int) * items_count);
    char* items_new[items_count];

    for (index = 0; index < items_count; index++) {
        items_new[index] = items[index];
    }

    for (index = 0; index < items_count; index++) {
        if (items_new[index] == NULL)
            continue;
        char *item = items_new[index];
        items_new[index] = NULL;
        items_new[offset] = item;
        translate_table[offset] = index;
        offset++;
    }
    items_new[offset] = NULL;

    initial_selection = translate_table[initial_selection];
    int ret = get_menu_selection(headers, items_new, menu_only, initial_selection);
    if (ret < 0 || ret >= offset) {
        free(translate_table);
        return ret;
    }

    ret = translate_table[ret];
    free(translate_table);
    return ret;
}

static void write_string_to_file(const char* filename, const char* string) {
    ensure_path_mounted(filename);
    char tmp[PATH_MAX];
    sprintf(tmp, "mkdir -p $(dirname %s)", filename);
    __system(tmp);
    FILE *file = fopen(filename, "w");
    if (file != NULL) {
        fprintf(file, "%s", string);
        fclose(file);
    }
}

void write_recovery_version() {
    char path[PATH_MAX];
    sprintf(path, "%s%s%s", get_primary_storage_path(), (is_data_media() ? "/0/" : "/"), RECOVERY_VERSION_FILE);
    write_string_to_file(path, EXPAND(RECOVERY_VERSION) "\n" EXPAND(TARGET_DEVICE));
    // force unmount /data for /data/media devices as we call this on recovery exit
    preserve_data_media(0);
    ensure_path_unmounted(path);
    preserve_data_media(1);
}

static void write_last_install_path(const char* install_path) {
    char path[PATH_MAX];
    sprintf(path, "%s%s%s", get_primary_storage_path(), (is_data_media() ? "/0/" : "/"), RECOVERY_LAST_INSTALL_FILE);
    write_string_to_file(path, install_path);
}

static char* read_last_install_path() {
    static char path[PATH_MAX];
    sprintf(path, "%s%s%s", get_primary_storage_path(), (is_data_media() ? "/0/" : "/"), RECOVERY_LAST_INSTALL_FILE);

    ensure_path_mounted(path);
    FILE *f = fopen(path, "r");
    if (f != NULL) {
        fgets(path, PATH_MAX, f);
        fclose(f);

        return path;
    }
    return NULL;
}

static void toggle_signature_check() {
    signature_check_enabled = !signature_check_enabled;
if ( language== 1 )
    ui_print("Signature Check: %s\n", signature_check_enabled ? "Enabled" : "Disabled");
else
    ui_print("签名校验: %s\n", signature_check_enabled ? "已启用" : "已禁用");

}

#ifdef ENABLE_LOKI
int loki_support_enabled = 1;
void toggle_loki_support() {
    loki_support_enabled = !loki_support_enabled;
if ( language== 1 )
    ui_print("Loki Support: %s\n", loki_support_enabled ? "Enabled" : "Disabled");
else
    ui_print("Loki 支持: %s\n", loki_support_enabled ? "已启用" : "已禁用");

}
#endif

int install_zip(const char* packagefilepath) {
if ( language== 1 )
    ui_print("\n-- Installing: %s\n", packagefilepath);
else
    ui_print("\n-- 正在刷机: %s\n", packagefilepath);

    if (device_flash_type() == MTD) {
        set_sdcard_update_bootloader_message();
    }

    int status = install_package(packagefilepath);
    ui_reset_progress();
    if (status != INSTALL_SUCCESS) {
        ui_set_background(BACKGROUND_ICON_ERROR);
if ( language== 1 )
        ui_print("Installation aborted.\n");
else
        ui_print("刷机已中止。\n");

        return 1;
    }
#ifdef ENABLE_LOKI
    if (loki_support_enabled) {
if ( language== 1 )
        ui_print("Checking if loki-fying is needed\n");
else
        ui_print("正在检查是否需要 loki-fying\n");

        status = loki_check();
        if (status != INSTALL_SUCCESS) {
            ui_set_background(BACKGROUND_ICON_ERROR);
            return 1;
        }
    }
#endif

    ui_set_background(BACKGROUND_ICON_CLOCKWORK);
if ( language== 1 )
    ui_print("\nInstall from sdcard complete.\n");
else
    ui_print("\n已完成 SD 卡中刷机包的刷入。\n");

    return 0;
}

int show_install_update_menu() {
    char buf[100];
    int i = 0, chosen_item = 0;
    static char* install_menu_items[MAX_NUM_MANAGED_VOLUMES + FIXED_INSTALL_ZIP_MENUS + 1];

    char* primary_path = get_primary_storage_path();
    char** extra_paths = get_extra_storage_paths();
    int num_extra_volumes = get_num_extra_volumes();

    memset(install_menu_items, 0, MAX_NUM_MANAGED_VOLUMES + FIXED_INSTALL_ZIP_MENUS + 1);


    static const char* headers[] = { "Install update from zip file", "", NULL };
if ( language== 0 ) {
    headers[0] = "刷入 zip 刷机包";
}else{ 
	headers[0] = "Install update from zip file"; 
	}


    // FIXED_TOP_INSTALL_ZIP_MENUS
if ( language== 1 )
    sprintf(buf, "choose zip from %s", primary_path);
else
    sprintf(buf, "从 %s 中选择刷机包", primary_path);

    install_menu_items[0] = strdup(buf);

    // extra storage volumes (vold managed)
    for (i = 0; i < num_extra_volumes; i++) {
if ( language== 1 )
        sprintf(buf, "choose zip from %s", extra_paths[i]);
else
        sprintf(buf, "从 %s 中选择刷机包", extra_paths[i]);

        install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + i] = strdup(buf);
    }

    // FIXED_BOTTOM_INSTALL_ZIP_MENUS
if ( language== 1 ) {
    install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes] = "choose zip from last install folder";
    install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 1] = "install zip from sideload";
    install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 2] = "toggle signature verification";
} else {
    install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes] = "从上次选定文件夹中选择刷机包";
    install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 1] = "使用 sideload 方式刷机";
    install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 2] = "切换签名校验";
}

    // extra NULL for GO_BACK
    install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 3] = NULL;

    for (;;) {
        chosen_item = get_menu_selection(headers, install_menu_items, 0, 0);
        if (chosen_item == 0) {
            show_choose_zip_menu(primary_path);
        } else if (chosen_item >= FIXED_TOP_INSTALL_ZIP_MENUS && chosen_item < FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes) {
            show_choose_zip_menu(extra_paths[chosen_item - FIXED_TOP_INSTALL_ZIP_MENUS]);
        } else if (chosen_item == FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes) {
            char *last_path_used = read_last_install_path();
            if (last_path_used == NULL)
                show_choose_zip_menu(primary_path);
            else
                show_choose_zip_menu(last_path_used);
        } else if (chosen_item == FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 1) {
            apply_from_adb();
        } else if (chosen_item == FIXED_TOP_INSTALL_ZIP_MENUS + num_extra_volumes + 2) {
            toggle_signature_check();
        } else {
            // GO_BACK or REFRESH (chosen_item < 0)
            goto out;
        }
    }
out:
    free(install_menu_items[0]);
    if (extra_paths != NULL) {
        for (i = 0; i < num_extra_volumes; i++)
            free(install_menu_items[FIXED_TOP_INSTALL_ZIP_MENUS + i]);
    }
    return chosen_item;
}


static void free_string_array(char** array) {
    if (array == NULL)
        return;
    char* cursor = array[0];
    int i = 0;
    while (cursor != NULL) {
        free(cursor);
        cursor = array[++i];
    }
    free(array);
}

static char** gather_files(const char* directory, const char* fileExtensionOrDirectory, int* numFiles) {
    char path[PATH_MAX] = "";
    DIR *dir;
    struct dirent *de;
    int total = 0;
    int i;
    char** files = NULL;
    int pass;
    *numFiles = 0;
    int dirLen = strlen(directory);

    dir = opendir(directory);
    if (dir == NULL) {
if ( language== 1 )
        ui_print("Couldn't open directory.\n");
else
        ui_print("无法打开文件夹。\n");

        return NULL;
    }

    unsigned int extension_length = 0;
    if (fileExtensionOrDirectory != NULL)
        extension_length = strlen(fileExtensionOrDirectory);

    int isCounting = 1;
    i = 0;
    for (pass = 0; pass < 2; pass++) {
        while ((de = readdir(dir)) != NULL) {
            // skip hidden files
            if (de->d_name[0] == '.')
                continue;

            // NULL means that we are gathering directories, so skip this
            if (fileExtensionOrDirectory != NULL) {
                // make sure that we can have the desired extension (prevent seg fault)
                if (strlen(de->d_name) < extension_length)
                    continue;
                // compare the extension
                if (strcmp(de->d_name + strlen(de->d_name) - extension_length, fileExtensionOrDirectory) != 0)
                    continue;
            } else {
                struct stat info;
                char fullFileName[PATH_MAX];
                strcpy(fullFileName, directory);
                strcat(fullFileName, de->d_name);
                lstat(fullFileName, &info);
                // make sure it is a directory
                if (!(S_ISDIR(info.st_mode)))
                    continue;
            }

            if (pass == 0) {
                total++;
                continue;
            }

            files[i] = (char*)malloc(dirLen + strlen(de->d_name) + 2);
            strcpy(files[i], directory);
            strcat(files[i], de->d_name);
            if (fileExtensionOrDirectory == NULL)
                strcat(files[i], "/");
            i++;
        }
        if (pass == 1)
            break;
        if (total == 0)
            break;
        rewinddir(dir);
        *numFiles = total;
        files = (char**)malloc((total + 1) * sizeof(char*));
        files[total] = NULL;
    }

    if (closedir(dir) < 0) {
if ( language== 1 )
        LOGE("Failed to close directory.\n");
else
        LOGE("无法关闭文件夹。\n");

    }

    if (total == 0) {
        return NULL;
    }
    // sort the result
    if (files != NULL) {
        for (i = 0; i < total; i++) {
            int curMax = -1;
            int j;
            for (j = 0; j < total - i; j++) {
                if (curMax == -1 || strcmp(files[curMax], files[j]) < 0)
                    curMax = j;
            }
            char* temp = files[curMax];
            files[curMax] = files[total - i - 1];
            files[total - i - 1] = temp;
        }
    }

    return files;
}

// pass in NULL for fileExtensionOrDirectory and you will get a directory chooser
static char* choose_file_menu(const char* basedir, const char* fileExtensionOrDirectory, const char* headers[]) {
    const char* fixed_headers[20];
    int numFiles = 0;
    int numDirs = 0;
    int i;
    char* return_value = NULL;
    char directory[PATH_MAX];
    int dir_len = strlen(basedir);

    strcpy(directory, basedir);

    // Append a trailing slash if necessary
    if (directory[dir_len - 1] != '/') {
        strcat(directory, "/");
        dir_len++;
    }

    i = 0;
    while (headers[i]) {
        fixed_headers[i] = headers[i];
        i++;
    }
    fixed_headers[i] = directory;
    fixed_headers[i + 1] = "";
    fixed_headers[i + 2] = NULL;

    char** files = gather_files(directory, fileExtensionOrDirectory, &numFiles);
    char** dirs = NULL;
    if (fileExtensionOrDirectory != NULL)
        dirs = gather_files(directory, NULL, &numDirs);
    int total = numDirs + numFiles;
    if (total == 0) {
if ( language== 1 )
        ui_print("No files found.\n");
else
        ui_print("未找到文件。\n");

    } else {
        char** list = (char**)malloc((total + 1) * sizeof(char*));
        list[total] = NULL;


        for (i = 0; i < numDirs; i++) {
            list[i] = strdup(dirs[i] + dir_len);
        }

        for (i = 0; i < numFiles; i++) {
            list[numDirs + i] = strdup(files[i] + dir_len);
        }

        for (;;) {
            int chosen_item = get_menu_selection(fixed_headers, list, 0, 0);
            if (chosen_item == GO_BACK || chosen_item == REFRESH)
                break;
            if (chosen_item < numDirs) {
                char* subret = choose_file_menu(dirs[chosen_item], fileExtensionOrDirectory, headers);
                if (subret != NULL) {
                    return_value = strdup(subret);
                    free(subret);
                    break;
                }
                continue;
            }
            return_value = strdup(files[chosen_item - numDirs]);
            break;
        }
        free_string_array(list);
    }

    free_string_array(files);
    free_string_array(dirs);
    return return_value;
}

static void show_choose_zip_menu(const char *mount_point) {
    if (ensure_path_mounted(mount_point) != 0) {
if ( language== 1 )
        LOGE("Can't mount %s\n", mount_point);
else
        LOGE("无法挂载 %s\n", mount_point);

        return;
    }


    static const char* headers[] = { "Choose a zip to apply", "", NULL };
if ( language== 0 )
    headers[0] = "选择要刷入的刷机包";


    char* file = choose_file_menu(mount_point, ".zip", headers);
    if (file == NULL)
        return;
    char confirm[PATH_MAX];
if ( language== 1 )
    sprintf(confirm, "Yes - Install %s", basename(file));
else
    sprintf(confirm, "是 - 刷入 %s", basename(file));


if ( language== 1 ) {
    if (confirm_selection("Confirm install?", confirm)) {

	install_zip(file);
        write_last_install_path(dirname(file));
    }
	free(file);

}
else {
    if (confirm_selection("确认刷入？", confirm)) {

        install_zip(file);
        write_last_install_path(dirname(file));
    }

    free(file);
	}
}

static void show_nandroid_restore_menu(const char* path) {
    if (ensure_path_mounted(path) != 0) {
if ( language== 1 )
        LOGE("Can't mount %s\n", path);
else
        LOGE("无法挂载 %s\n", path);

        return;
    }


    static const char* headers[] = { "Choose an image to restore", "", NULL };
	if ( language== 0 )
		headers[0] = "选择要还原的镜像";


    char tmp[PATH_MAX];
    sprintf(tmp, "%s/clockworkmod/backup/", path);
    char* file = choose_file_menu(tmp, NULL, headers);
    if (file == NULL)
        return;

	if ( language== 1 ) {
		if (confirm_selection("Confirm restore?", "Yes - Restore")) {
			unsigned char flags = NANDROID_BOOT | NANDROID_SYSTEM | NANDROID_PRELOAD | NANDROID_DATA
								  | NANDROID_CACHE | NANDROID_SDEXT;
			nandroid_restore(file, flags);
		}
	}
	else {
		if (confirm_selection("确认还原？", "是 - 还原")) {
			unsigned char flags = NANDROID_BOOT | NANDROID_SYSTEM | NANDROID_PRELOAD | NANDROID_DATA
								  | NANDROID_CACHE | NANDROID_SDEXT;
			nandroid_restore(file, flags);
		}
	}

    free(file);
	}
}

static void show_nandroid_delete_menu(const char* path) {
    if (ensure_path_mounted(path) != 0) {
		if ( language== 1 )
			LOGE("Can't mount %s\n", path);
		else
			LOGE("无法挂载 %s\n", path);

        return;
    }


    static const char* headers[] = { "Choose an image to delete", "", NULL };
if ( language== 0 )
    headers[0] = "选择要删除的镜像";


    char tmp[PATH_MAX];
    sprintf(tmp, "%s/clockworkmod/backup/", path);
    char* file = choose_file_menu(tmp, NULL, headers);
    if (file == NULL)
        return;

if ( language== 1 ) {
    if (confirm_selection("Confirm delete?", "Yes - Delete")) {

	sprintf(tmp, "rm -rf %s", file);
        __system(tmp);
    	}
	free(file);
  }
else {
    if (confirm_selection("确认删除？", "是 - 删除")) {

        sprintf(tmp, "rm -rf %s", file);
        __system(tmp);
    }

    free(file);
	}
}

static int control_usb_storage(bool on) {
    int i = 0;
    int num = 0;

    for (i = 0; i < get_num_volumes(); i++) {
        Volume *v = get_device_volumes() + i;
        if (fs_mgr_is_voldmanaged(v) && vold_is_volume_available(v->mount_point)) {
            if (on) {
                vold_share_volume(v->mount_point);
            } else {
                vold_unshare_volume(v->mount_point, 1);
            }
            property_set("sys.storage.ums_enabled", on ? "1" : "0");
            num++;
        }
    }
    return num;
}

static void show_mount_usb_storage_menu() {
    // Enable USB storage using vold
    if (!control_usb_storage(true))
        return;


    static const char* headers[] = { "USB Mass Storage device",
                                     "Leaving this menu unmounts",
                                     "your SD card from your PC.",
				     "",
                                     NULL
    					};
if ( language== 0 ) {
    headers[0] = "U 盘模式";
    headers[1] = "离开当前菜单即会停";
    headers[2] = "用 U 盘模式。";
	}
else {
    headers[0] = "USB Mass Storage device";
    headers[1] = "Leaving this menu unmounts";
    headers[2] = "your SD card from your PC.";
	}                               


    static char* list[] = { "Unmount", NULL };
if ( language== 0 )
    list[0] = "停用";


    for (;;) {
        int chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item == GO_BACK || chosen_item == 0)
            break;
    }

    // Disable USB storage
    control_usb_storage(false);
}

int confirm_selection(const char* title, const char* confirm) {
    struct stat info;
    int ret = 0;

    char path[PATH_MAX];
    sprintf(path, "%s%s%s", get_primary_storage_path(), (is_data_media() ? "/0/" : "/"), RECOVERY_NO_CONFIRM_FILE);
    ensure_path_mounted(path);
    if (0 == stat(path, &info))
        return 1;

#ifdef BOARD_NATIVE_DUALBOOT
    char buf[PATH_MAX];
    device_build_selection_title(buf, title);
    title = (char*)&buf;
#endif

    int many_confirm;
    char* confirm_str = strdup(confirm);

    const char* confirm_headers[] = { title, "  THIS CAN NOT BE UNDONE.", "", NULL };
if ( language== 0 ) {
    confirm_headers[1] = "  本操作是不可逆的。";
} else { 
    confirm_headers[1] = "  THIS CAN NOT BE UNDONE.";
}
	int old_val = ui_is_showing_back_button();
    ui_set_showing_back_button(0);

    sprintf(path, "%s%s%s", get_primary_storage_path(), (is_data_media() ? "/0/" : "/"), RECOVERY_MANY_CONFIRM_FILE);
    ensure_path_mounted(path);
    many_confirm = 0 == stat(path, &info);

    if (many_confirm) {

        char* items[] = { "No",
                          "No",
                          "No",
                          "No",
                          "No",
                          "No",
                          "No",
                          confirm_str, // Yes, [7]
                          "No",
                          "No",
                          "No",
				NULL };
if ( language== 0 ) {
        items[0] = "否";
        items[1] = "否";
        items[2] = "否";
        items[3] = "否";
        items[4] = "否";
        items[5] = "否";
        items[6] = "否";
        items[7] = confirm_str; // Yes, [7]
        items[8] =  "否";
        items[9] = "否";
        items[10] = "否";
} else {
        items[0] = "No";
        items[1] = "No";
        items[2] = "No";
        items[3] = "No";
        items[4] = "No";
        items[5] = "No";
        items[6] = "No";
        items[7] = confirm_str; // Yes, [7]
        items[8] = "No";
        items[9] = "No";
        items[10] = "No";
}
        int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
        ret = (chosen_item == 7);
    } else {

        char* items[] = { "No",
		confirm_str, // Yes, [1]
                          NULL };
if ( language== 0 ) {
        items[0] = "否";
} else {
        items[0] = "No";
}
                          
        int chosen_item = get_menu_selection(confirm_headers, items, 0, 0);
        ret = (chosen_item == 1);
    }

    free(confirm_str);
    ui_set_showing_back_button(old_val);
    return ret;
}

int format_device(const char *device, const char *path, const char *fs_type) {
#ifdef BOARD_NATIVE_DUALBOOT_SINGLEDATA
    if(device_truedualboot_format_device(device, path, fs_type) <= 0)
        return 0;
#endif
    if (is_data_media_volume_path(path)) {
        return format_unknown_device(NULL, path, NULL);
    }
    if (strstr(path, "/data") == path && is_data_media()) {
        return format_unknown_device(NULL, path, NULL);
    }

    Volume* v = volume_for_path(path);
    if (v == NULL) {
        // silent failure for sd-ext
        if (strcmp(path, "/sd-ext") != 0)
if ( language== 1 )
            LOGE("unknown volume '%s'\n", path);
else
            LOGE("未知卷 '%s'\n", path);

        return -1;
    }

    if (strcmp(fs_type, "ramdisk") == 0) {
        // you can't format the ramdisk.
if ( language== 1 )
        LOGE("can't format_volume \"%s\"", path);
else
        LOGE("无法格式化卷 \"%s\"", path);

        return -1;
    }

    if (strcmp(fs_type, "rfs") == 0) {
        if (ensure_path_unmounted(path) != 0) {
if ( language== 1 )
            LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
else
            LOGE("format_volume 卸载 \"%s\" 时出错\n", v->mount_point);

            return -1;
        }
        if (0 != format_rfs_device(device, path)) {
if ( language== 1 )
            LOGE("format_volume: format_rfs_device failed on %s\n", device);
else
            LOGE("format_volume: 使用 format_rfs_device 格式化 %s 时出错\n", device);

            return -1;
        }
        return 0;
    }

    if (strcmp(v->mount_point, path) != 0) {
        return format_unknown_device(v->blk_device, path, NULL);
    }

    if (ensure_path_unmounted(path) != 0) {
if ( language== 1 )
        LOGE("format_volume failed to unmount \"%s\"\n", v->mount_point);
else
        LOGE("format_volume 卸载 \"%s\" 时出错\n", v->mount_point);

        return -1;
    }

    if (strcmp(fs_type, "yaffs2") == 0 || strcmp(fs_type, "mtd") == 0) {
        mtd_scan_partitions();
        const MtdPartition* partition = mtd_find_partition_by_name(device);
        if (partition == NULL) {
if ( language== 1 )
            LOGE("format_volume: no MTD partition \"%s\"\n", device);
else
            LOGE("format_volume: 无 MTD 分区 \"%s\"\n", device);

            return -1;
        }

        MtdWriteContext *write = mtd_write_partition(partition);
        if (write == NULL) {
if ( language== 1 )
            LOGW("format_volume: can't open MTD \"%s\"\n", device);
else
            LOGW("format_volume: 无法打开 MTD 设备 \"%s\"\n", device);

            return -1;
        } else if (mtd_erase_blocks(write, -1) == (off_t) - 1) {
if ( language== 1 )
            LOGW("format_volume: can't erase MTD \"%s\"\n", device);
else
            LOGW("format_volume: 无法擦除 MTD 设备 \"%s\"\n", device);

            mtd_write_close(write);
            return -1;
        } else if (mtd_write_close(write)) {
if ( language== 1 )
            LOGW("format_volume: can't close MTD \"%s\"\n", device);
else
            LOGW("format_volume: 无法关闭 MTD 设备 \"%s\"\n", device);

            return -1;
        }
        return 0;
    }

    if (strcmp(fs_type, "ext4") == 0) {
        int length = 0;
        if (strcmp(v->fs_type, "ext4") == 0) {
            // Our desired filesystem matches the one in fstab, respect v->length
            length = v->length;
        }

        int result = make_ext4fs(device, length, v->mount_point, sehandle);
        if (result != 0) {
if ( language== 1 )
            LOGE("format_volume: make_ext4fs failed on %s\n", device);
else
            LOGE("format_volume: 在设备 %s 上执行 make_ext4fs 时出错\n", device);

            return -1;
        }
        return 0;
    }
#ifdef USE_F2FS
    if (strcmp(fs_type, "f2fs") == 0) {
        char* args[] = { "mkfs.f2fs", v->blk_device };
        if (make_f2fs_main(2, args) != 0) {
            LOGE("format_volume: mkfs.f2fs failed on %s\n", v->blk_device);
            return -1;
        }
        return 0;
    }
#endif
    return format_unknown_device(device, path, fs_type);
}

int format_unknown_device(const char *device, const char* path, const char *fs_type) {
if ( language== 1 )
    LOGI("Formatting unknown device.\n");
else
    LOGI("正在格式化未知设备。\n");


    if (fs_type != NULL && get_flash_type(fs_type) != UNSUPPORTED)
        return erase_raw_partition(fs_type, device);

    // if this is SDEXT:, don't worry about it if it does not exist.
    if (0 == strcmp(path, "/sd-ext")) {
        struct stat st;
        Volume *vol = volume_for_path("/sd-ext");
        if (vol == NULL || 0 != stat(vol->blk_device, &st)) {
if ( language== 1 )
            LOGI("No app2sd partition found. Skipping format of /sd-ext.\n");
else
            LOGI("未找到 app2sd 分区。跳过对 /sd-ext 的格式化。\n");

            return 0;
        }
    }

    if (NULL != fs_type) {
        if (strcmp("ext3", fs_type) == 0) {
if ( language== 1 )
            LOGI("Formatting ext3 device.\n");
else
            LOGI("格式化 ext3 设备。\n");

            if (0 != ensure_path_unmounted(path)) {
if ( language== 1 )
                LOGE("Error while unmounting %s.\n", path);
else
                LOGE("卸载 %s 时出错。\n", path);

                return -12;
            }
            return format_ext3_device(device);
        }

        if (strcmp("ext2", fs_type) == 0) {
if ( language== 1 )
            LOGI("Formatting ext2 device.\n");
else
            LOGI("格式化 ext2 设备。\n");

            if (0 != ensure_path_unmounted(path)) {
if ( language== 1 )
                LOGE("Error while unmounting %s.\n", path);
else
                LOGE("卸载 %s 时出错。\n", path);

                return -12;
            }
            return format_ext2_device(device);
        }
    }

    if (0 != ensure_path_mounted(path)) {
if ( language== 1 ) {
        ui_print("Error mounting %s!\n", path);
        ui_print("Skipping format...\n");
} else {
        ui_print("挂载 %s 时出错！\n", path);
        ui_print("跳过格式化...\n");
}
        return 0;
    }

    char tmp[PATH_MAX];
    if (strcmp(path, "/data") == 0) {
        sprintf(tmp, "cd /data ; for f in $(ls -a | grep -v ^media$); do rm -rf $f; done");
        __system(tmp);
        // if the /data/media sdcard has already been migrated for android 4.2,
        // prevent the migration from happening again by writing the .layout_version
        struct stat st;
        if (0 == lstat("/data/media/0", &st)) {
            char* layout_version = "2";
            FILE* f = fopen("/data/.layout_version", "wb");
            if (NULL != f) {
                fwrite(layout_version, 1, 2, f);
                fclose(f);
            } else {
if ( language== 1 )
                LOGI("error opening /data/.layout_version for write.\n");
else
                LOGI("打开 /data/.layout_version 进行写入时出错。\n");

            }
        } else {
if ( language== 1 )
            LOGI("/data/media/0 not found. migration may occur.\n");
else
            LOGI("未找到 /data/media/0。可能已被移动到其他位置。\n");

        }
    } else {
        sprintf(tmp, "rm -rf %s/*", path);
        __system(tmp);
        sprintf(tmp, "rm -rf %s/.*", path);
        __system(tmp);
    }

    ensure_path_unmounted(path);
    return 0;
}

static MFMatrix get_mnt_fmt_capabilities(char *fs_type, char *mount_point) {
    MFMatrix mfm = { mount_point, 1, 1 };

    const int NUM_FS_TYPES = 6;
    MFMatrix *fs_matrix = malloc(NUM_FS_TYPES * sizeof(MFMatrix));
    // Defined capabilities:   fs_type     mnt fmt
    fs_matrix[0] = (MFMatrix){ "bml",       0,  1 };
    fs_matrix[1] = (MFMatrix){ "datamedia", 0,  1 };
    fs_matrix[2] = (MFMatrix){ "emmc",      0,  1 };
    fs_matrix[3] = (MFMatrix){ "mtd",       0,  0 };
    fs_matrix[4] = (MFMatrix){ "ramdisk",   0,  0 };
    fs_matrix[5] = (MFMatrix){ "swap",      0,  0 };

    const int NUM_MNT_PNTS = 6;
    MFMatrix *mp_matrix = malloc(NUM_MNT_PNTS * sizeof(MFMatrix));
    // Defined capabilities:   mount_point   mnt fmt
    mp_matrix[0] = (MFMatrix){ "/misc",       0,  0 };
    mp_matrix[1] = (MFMatrix){ "/radio",      0,  0 };
    mp_matrix[2] = (MFMatrix){ "/bootloader", 0,  0 };
    mp_matrix[3] = (MFMatrix){ "/recovery",   0,  0 };
    mp_matrix[4] = (MFMatrix){ "/efs",        0,  0 };
    mp_matrix[5] = (MFMatrix){ "/wimax",      0,  0 };

    int i;
    for (i = 0; i < NUM_FS_TYPES; i++) {
        if (strcmp(fs_type, fs_matrix[i].name) == 0) {
            mfm.can_mount = fs_matrix[i].can_mount;
            mfm.can_format = fs_matrix[i].can_format;
        }
    }
    for (i = 0; i < NUM_MNT_PNTS; i++) {
        if (strcmp(mount_point, mp_matrix[i].name) == 0) {
            mfm.can_mount = mp_matrix[i].can_mount;
            mfm.can_format = mp_matrix[i].can_format;
        }
    }

    free(fs_matrix);
    free(mp_matrix);

    // User-defined capabilities
    char *custom_mp;
    char custom_forbidden_mount[PROPERTY_VALUE_MAX];
    char custom_forbidden_format[PROPERTY_VALUE_MAX];
    property_get("ro.cwm.forbid_mount", custom_forbidden_mount, "");
    property_get("ro.cwm.forbid_format", custom_forbidden_format, "");

    custom_mp = strtok(custom_forbidden_mount, ",");
    while (custom_mp != NULL) {
        if (strcmp(mount_point, custom_mp) == 0) {
            mfm.can_mount = 0;
        }
        custom_mp = strtok(NULL, ",");
    }

    custom_mp = strtok(custom_forbidden_format, ",");
    while (custom_mp != NULL) {
        if (strcmp(mount_point, custom_mp) == 0) {
            mfm.can_format = 0;
        }
        custom_mp = strtok(NULL, ",");
    }

    return mfm;
}

static int is_ums_capable() {
    // control_usb_storage() only supports vold managed storage
    int i;

    // If USB volume is available, UMS not possible (assumes one USB/device)
    for (i = 0; i < get_num_volumes(); i++) {
        Volume *v = get_device_volumes() + i;
        if (fs_mgr_is_voldmanaged(v) && vold_is_volume_available(v->mount_point)
                && (strcasestr(v->label, "usb") || strcasestr(v->label, "otg")))
            return 0;
    }

    // No USB storage found, look for any other vold managed storage
    for (i = 0; i < get_num_volumes(); i++) {
        Volume *v = get_device_volumes() + i;
        if (fs_mgr_is_voldmanaged(v) && vold_is_volume_available(v->mount_point))
            return 1;
    }

    return 0;
}

int show_partition_menu() {

    static const char* headers[] = { "Mounts and Storage Menu", "", NULL };

    if ( language== 0 ) headers[0] = "挂载及 U 盘模式";



    static char* confirm_format[] = { "Confirm format?" };
    static char* confirm[] = { "Yes - Format" };
if ( language== 0 ) {
    confirm_format[0] = "确认格式化？";
    confirm[0] = "是 - 格式化";
} else {

    confirm_format[0] = "Confirm format?";
    confirm[0] = "Yes - Format";
}
    char confirm_string[255];

    static MountMenuEntry* mount_menu = NULL;
    static FormatMenuEntry* format_menu = NULL;
    static char* list[256];

    int i, mountable_volumes, formatable_volumes;
    int num_volumes;
    int chosen_item = 0;
    int menu_entries = 0;

    struct menu_extras {
        int dm;   // boolean: enable wipe data media
        int ums;  // boolean: enable mount usb mass storage
        int idm;  // index of wipe dm entry in list[]
        int iums; // index of ums entry in list[]
    };
    struct menu_extras me;

    num_volumes = get_num_volumes();

    if (!num_volumes)
        return 0;

    mountable_volumes = 0;
    formatable_volumes = 0;

    mount_menu = malloc(num_volumes * sizeof(MountMenuEntry));
    format_menu = malloc(num_volumes * sizeof(FormatMenuEntry));

    for (i = 0; i < num_volumes; i++) {
        Volume* v = get_device_volumes() + i;

        if (fs_mgr_is_voldmanaged(v) && !vold_is_volume_available(v->mount_point)) {
            continue;
        }

        MFMatrix mfm = get_mnt_fmt_capabilities(v->fs_type, v->mount_point);

        if (mfm.can_mount) {
if ( language== 1 ) {
            sprintf(mount_menu[mountable_volumes].mount, "mount %s", v->mount_point);
            sprintf(mount_menu[mountable_volumes].unmount, "unmount %s", v->mount_point);
}else{
            sprintf(mount_menu[mountable_volumes].mount, "挂载 %s", v->mount_point);
            sprintf(mount_menu[mountable_volumes].unmount, "卸载 %s", v->mount_point);
}
            sprintf(mount_menu[mountable_volumes].path, "%s", v->mount_point);
            ++mountable_volumes;
        }
        if (mfm.can_format) {
if ( language== 1 )
            sprintf(format_menu[formatable_volumes].txt, "format %s", v->mount_point);
else
            sprintf(format_menu[formatable_volumes].txt, "格式化 %s", v->mount_point);

            sprintf(format_menu[formatable_volumes].path, "%s", v->mount_point);
            sprintf(format_menu[formatable_volumes].type, "%s", v->fs_type);
            ++formatable_volumes;
        }
    }

    for (;;) {
        for (i = 0; i < mountable_volumes; i++) {
            MountMenuEntry* e = &mount_menu[i];
            if (is_path_mounted(e->path))
                list[i] = e->unmount;
            else
                list[i] = e->mount;
        }

        for (i = 0; i < formatable_volumes; i++) {
            FormatMenuEntry* e = &format_menu[i];
            list[mountable_volumes + i] = e->txt;
        }

        menu_entries = mountable_volumes + formatable_volumes;
        me = (struct menu_extras){ 0, 0, 0, 0 };

        if (me.dm = is_data_media()) {
            me.idm = menu_entries;
			if ( language== 1 )
				list[me.idm] = "format /data and /data/media (/sdcard)";
			else
				list[me.idm] = "格式化 /data 和 /data/media (/sdcard)";
            menu_entries++;
        }
        if (me.ums = is_ums_capable()) {
            me.iums = menu_entries;
			if ( language== 1 )
				list[me.iums] = "mount USB storage";
			else
				list[me.iums] = "开启 U 盘模式";
            menu_entries++;
        }
        list[menu_entries] = '\0';

        chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item >= menu_entries || chosen_item < 0)
            break;

        if (chosen_item < mountable_volumes) {
            MountMenuEntry* e = &mount_menu[chosen_item];

            if (is_path_mounted(e->path)) {
                preserve_data_media(0);
                if (0 != ensure_path_unmounted(e->path))
if ( language== 1 )
                    ui_print("Error unmounting %s!\n", e->path);
else
                    ui_print("卸载 %s 时出错！\n", e->path);

                preserve_data_media(1);
            } else {
                if (0 != ensure_path_mounted(e->path))
if ( language== 1 )
                    ui_print("Error mounting %s!\n", e->path);
else
                    ui_print("挂载 %s 时出错！\n", e->path);

            }
        } else if (chosen_item < (mountable_volumes + formatable_volumes)) {
            chosen_item = chosen_item - mountable_volumes;
            FormatMenuEntry* e = &format_menu[chosen_item];

            sprintf(confirm_string, "%s - %s", e->path, confirm_format[0]);

            // support user choice fstype when formatting external storage
            // ensure fstype==auto because most devices with internal vfat
            // storage cannot be formatted to other types
            if (strcmp(e->type, "auto") == 0) {
                format_sdcard(e->path);
                continue;
            }

            if (!confirm_selection(confirm_string, confirm[0]))
                continue;
			if ( language== 1 )
				ui_print("Formatting %s...\n", e->path);
			else
				ui_print("正在格式化 %s...\n", e->path);

            if (0 != format_volume(e->path)) {
				if ( language== 1 )
					ui_print("Error formatting %s!\n", e->path);
				else
					ui_print("格式化 %s 时出错！\n", e->path);
			}
            else {
				if ( language== 1 )
					ui_print("Done.\n");
				else
					ui_print("完成。\n");
			}
        } else if (me.dm && chosen_item == me.idm) {
			if ( language== 1 ) {
				if (!confirm_selection("format /data and /data/media (/sdcard)", confirm))
					continue;
			}
			else {
				if (!confirm_selection("格式化/data和/data/media (/sdcard)", confirm))
					continue;
			}
            preserve_data_media(0);
			if ( language== 1 )
				ui_print("Formatting /data...\n");
			else
				ui_print("正在格式化/data...\n");
            if (0 != format_volume("/data")) {
				if ( language== 1 )
					ui_print("Error formatting /data!\n");
				else
					ui_print("格式化/data出错!\n");
			}
            else {
				if ( language== 1 )
					ui_print("Done.\n");
				else
					ui_print("完成。\n");
			}
            preserve_data_media(1);

            // recreate /data/media with proper permissions
            ensure_path_mounted("/data");
            setup_data_media();
        } else if (me.ums && chosen_item == me.iums) {
            show_mount_usb_storage_menu();
        }
    }

    free(mount_menu);
    free(format_menu);
    return chosen_item;
}

static void nandroid_adv_update_selections(char *str[], int listnum, unsigned char *flags) {
    int len = strlen(str[listnum]);
    if (str[listnum][len-2] == ' ') {
        str[listnum][len-1] = ')';
        str[listnum][len-2] = '+';
        str[listnum][len-3] = '(';
    } else {
        str[listnum][len-1] = ' ';
        str[listnum][len-2] = ' ';
        str[listnum][len-3] = ' ';
    }
    switch(listnum) {
        case 0:
            *flags ^= NANDROID_BOOT;
            break;
        case 1:
            *flags ^= NANDROID_SYSTEM;
            break;
        case 2:
            *flags ^= NANDROID_PRELOAD;
            break;
        case 3:
            *flags ^= NANDROID_DATA;
            break;
        case 4:
            *flags ^= NANDROID_CACHE;
            break;
        case 5:
            *flags ^= NANDROID_SDEXT;
            break;
        case 6:
            *flags ^= NANDROID_WIMAX;
            break;
    }
}

int empty_nandroid_bitmask(unsigned char flags) {
    int ret = !(((flags & NANDROID_BOOT) == NANDROID_BOOT) ||
                ((flags & NANDROID_SYSTEM) == NANDROID_SYSTEM) ||
                ((flags & NANDROID_PRELOAD) == NANDROID_PRELOAD) ||
                ((flags & NANDROID_DATA) == NANDROID_DATA) ||
                ((flags & NANDROID_CACHE) == NANDROID_CACHE) ||
                ((flags & NANDROID_SDEXT) == NANDROID_SDEXT) ||
                ((flags & NANDROID_WIMAX) == NANDROID_WIMAX));

    return ret;
}

static void show_nandroid_advanced_restore_menu(const char* path) {
    if (ensure_path_mounted(path) != 0) {
if ( language== 1 )
        LOGE("Can't mount sdcard\n");
else
        LOGE("无法挂载 SD 卡\n");

        return;
    }


    static const char* advancedheaders[] = { "Choose an image to restore",
                                             "",
                                             "Choose an image to restore",
                                             "first. The next menu will",
                                             "show you more options.",
						"",
                                             NULL };
if ( language== 0 ) {
    advancedheaders[0] = "选择一个要还原的镜像";
    advancedheaders[2] = "首先选择一个要还原的镜";
    advancedheaders[3] = "像。接下来的菜单会有更";
    advancedheaders[4] = "多的选项以供选择。";
} else {
    advancedheaders[0] = "Choose an image to restore";
    advancedheaders[2] = "Choose an image to restore";
    advancedheaders[3] = "first. The next menu will";
    advancedheaders[4] = "show you more options.";
} 
                                             

    char tmp[PATH_MAX];
    sprintf(tmp, "%s/clockworkmod/backup/", path);
    char* file = choose_file_menu(tmp, NULL, advancedheaders);
    if (file == NULL)
        return;

    static char* headers[] = { "Advanced Restore",
                                     "",
                                     "Select image(s) to restore:",
                                     NULL };
	if ( language == 0 )
		headers[0] = "高级还原";

    int disable_wimax = 0;
    if (0 != get_partition_device("wimax", tmp))
        disable_wimax = 1;

    char *list[9 - disable_wimax];
    // Dynamically allocated entries will have (+) added/removed to end
    // Leave space at end of string  so terminator doesn't need to move
	if ( language== 0 ) {
		list[0] = malloc(sizeof("Restore boot    "));
		list[1] = malloc(sizeof("Restore system    "));
		list[2] = malloc(sizeof("Restore preload    "));
		list[3] = malloc(sizeof("Restore data    "));
		list[4] = malloc(sizeof("Restore cache    "));
		list[5] = malloc(sizeof("Restore sd-ext    "));
		if (!disable_wimax)
			list[6] = malloc(sizeof("Restore wimax    "));
		list[7 - disable_wimax] = "Start restore";
		list[8 - disable_wimax] = NULL;

		sprintf(list[0], "Restore boot    ");
		sprintf(list[1], "Restore system    ");
		sprintf(list[2], "Restore preload    ");
		sprintf(list[3], "Restore data    ");
		sprintf(list[4], "Restore cache    ");
		sprintf(list[5], "Restore sd-ext    ");
		if (!disable_wimax)
			sprintf(list[6], "Restore wimax    ");
	} else {
		list[0] = malloc(sizeof("还原 boot    "));
		list[1] = malloc(sizeof("还原 system    "));
		list[2] = malloc(sizeof("还原 preload    "));
		list[3] = malloc(sizeof("还原 data    "));
		list[4] = malloc(sizeof("还原 cache    "));
		list[5] = malloc(sizeof("还原 sd-ext    "));
		if (!disable_wimax)
			list[6] = malloc(sizeof("还原 wimax    "));
		list[7 - disable_wimax] = "开始还原";
		list[8 - disable_wimax] = NULL;

		sprintf(list[0], "还原 boot    ");
		sprintf(list[1], "还原 system    ");
		sprintf(list[2], "还原 preload    ");
		sprintf(list[3], "还原 data    ");
		sprintf(list[4], "还原 cache    ");
		sprintf(list[5], "还原 sd-ext    ");
		if (!disable_wimax)
			sprintf(list[6], "还原 wimax    ");
	}

    unsigned char flags = NANDROID_NONE;
    int reload_menu;
    int start_restore = 6-disable_wimax;
    int chosen_item;

    do {
        reload_menu = 0;
        chosen_item = get_menu_selection(headers, list, 0, 0);
        if (chosen_item < 0 || chosen_item > start_restore)
            break;

        if (chosen_item < start_restore) {
            nandroid_adv_update_selections(list, chosen_item, &flags);
        } else if ((chosen_item == start_restore) && empty_nandroid_bitmask(flags)) {
			if ( language== 1 )
				ui_print("No image(s) selected!\n");
			else
				ui_print("没有选择镜像!\n");
			reload_menu = 1;
        }
    } while ((chosen_item >=0 && chosen_item < start_restore) || reload_menu);

    if (chosen_item == start_restore)
        nandroid_restore(file, flags);

    free(file);
    int i;
    for (i = 0; i < (5-disable_wimax); i++) {
        free(list[i]);
    }
}

static void run_dedupe_gc() {
    char path[PATH_MAX];
    char* fmt = "%s/clockworkmod/blobs";
    char* primary_path = get_primary_storage_path();
    char** extra_paths = get_extra_storage_paths();
    int i = 0;

    sprintf(path, fmt, primary_path);
    ensure_path_mounted(primary_path);
    nandroid_dedupe_gc(path);

    if (extra_paths != NULL) {
        for (i = 0; i < get_num_extra_volumes(); i++) {
            ensure_path_mounted(extra_paths[i]);
            sprintf(path, fmt, extra_paths[i]);
            nandroid_dedupe_gc(path);
        }
    }
}

static void choose_default_backup_format() {

    static const char* headers[] = { "Default Backup Format", "", NULL };
if ( language== 0 ) { 
	headers[0] = "默认备份格式";
} else { 
	headers[0] = "Default Backup Format";
}

    int fmt = nandroid_get_default_backup_format();

    char **list;

    char* list_tar_default[] = { "tar (default)",
                                 "dup",
				"tar + gzip",
                                 NULL };
if ( language== 0 ) { 
	list_tar_default[0] = "tar (默认)";
        list_tar_default[1] = "增量备份";
} else { 
	list_tar_default[0] = "tar (default)";
        list_tar_default[1] = "dup";
}
    char* list_dup_default[] = { "tar",

                                 "dup (default)",


                                 "tar + gzip",
                                 NULL };
if ( language== 0 ) { 
	list_dup_default[2] = "增量备份 (默认)";
} else { 
	list_dup_default[2] = "dup (default)";
}
    char* list_tgz_default[] = { "tar",

                                 "dup",
                                 "tar + gzip (default)",

                                 NULL };
if ( language== 0 ) {
	list_tgz_default[1] = "增量备份";
	list_tgz_default[2] = "tar + gzip (默认)";
} else {
	list_tgz_default[1] = "dup";
	list_tgz_default[2] = "tar + gzip (default)";
}
    if (fmt == NANDROID_BACKUP_FORMAT_DUP) {
        list = list_dup_default;
    } else if (fmt == NANDROID_BACKUP_FORMAT_TGZ) {
        list = list_tgz_default;
    } else {
        list = list_tar_default;
    }

    char path[PATH_MAX];
    sprintf(path, "%s%s%s", get_primary_storage_path(), (is_data_media() ? "/0/" : "/"), NANDROID_BACKUP_FORMAT_FILE);
    int chosen_item = get_menu_selection(headers, list, 0, 0);
    switch (chosen_item) {
        case 0: {
            write_string_to_file(path, "tar");
if ( language== 1 )
            ui_print("Default backup format set to tar.\n");
else
            ui_print("默认的备份格式已设置为 tar。\n");

            break;
        }
        case 1: {
            write_string_to_file(path, "dup");
if ( language== 1 )
            ui_print("Default backup format set to dedupe.\n");
else
            ui_print("默认的备份格式已设置为增量备份。\n");

            break;
        }
        case 2: {
            write_string_to_file(path, "tgz");
if ( language== 1 )
            ui_print("Default backup format set to tar + gzip.\n");
else
            ui_print("默认的备份格式已设置为 tar + gzip。\n");

            break;
        }
    }
}

static void add_nandroid_options_for_volume(char** menu, char* path, int offset) {
    char buf[100];

if ( language== 1 )
    sprintf(buf, "backup to %s", path);
else
    sprintf(buf, "备份到 %s", path);

    menu[offset] = strdup(buf);

if ( language== 1 )
    sprintf(buf, "restore from %s", path);
else
    sprintf(buf, "[普通]从 %s 中选择备份", path);

    menu[offset + 1] = strdup(buf);

if ( language== 1 )
    sprintf(buf, "delete from %s", path);
else
    sprintf(buf, "删除 %s 中的备份文件", path);

    menu[offset + 2] = strdup(buf);

if ( language== 1 )
    sprintf(buf, "advanced restore from %s", path);
else
    sprintf(buf, "[高级]从 %s 中选择备份", path);

    menu[offset + 3] = strdup(buf);
}

int show_nandroid_menu() {
    char* primary_path = get_primary_storage_path();
    char** extra_paths = get_extra_storage_paths();
    int num_extra_volumes = get_num_extra_volumes();
    int i = 0, offset = 0, chosen_item = 0;
    char* chosen_path = NULL;
    int action_entries_num = (num_extra_volumes + 1) * NANDROID_ACTIONS_NUM;


    static const char* headers[] = { "Backup and Restore", "", NULL };
if ( language== 0 ) { 
	headers[0] = "备份和还原";
} else { 
	headers[0] = "Backup and Restore";
}


    // (MAX_NUM_MANAGED_VOLUMES + 1) for primary_path (/sdcard)
    // + 1 for extra NULL entry
    static char* list[((MAX_NUM_MANAGED_VOLUMES + 1) * NANDROID_ACTIONS_NUM) + NANDROID_FIXED_ENTRIES + 1];

    // actions for primary_path
    add_nandroid_options_for_volume(list, primary_path, offset);
    offset += NANDROID_ACTIONS_NUM;

    // actions for voldmanaged volumes
    if (extra_paths != NULL) {
        for (i = 0; i < num_extra_volumes; i++) {
            add_nandroid_options_for_volume(list, extra_paths[i], offset);
            offset += NANDROID_ACTIONS_NUM;
        }
    }
    // fixed bottom entries
if ( language== 1 ) {
    list[offset] = "free unused backup data";
    list[offset + 1] = "choose default backup format";
}else{
    list[offset] = "释放未使用的备份数据";
    list[offset + 1] = "选择默认的备份格式";
}
    offset += NANDROID_FIXED_ENTRIES;

#ifdef RECOVERY_EXTEND_NANDROID_MENU
    extend_nandroid_menu(list, offset, sizeof(list) / sizeof(char*));
    offset++;
#endif

    // extra NULL for GO_BACK
    list[offset] = NULL;
    offset++;

    for (;;) {
        chosen_item = get_filtered_menu_selection(headers, list, 0, 0, offset);
        if (chosen_item == GO_BACK || chosen_item == REFRESH)
            break;

        // fixed bottom entries
        if (chosen_item == action_entries_num) {
            run_dedupe_gc();
        } else if (chosen_item == (action_entries_num + 1)) {
            choose_default_backup_format();
        } else if (chosen_item < action_entries_num) {
            // get nandroid volume actions path
            if (chosen_item < NANDROID_ACTIONS_NUM) {
                chosen_path = primary_path;
            } else if (extra_paths != NULL) {
                chosen_path = extra_paths[(chosen_item / NANDROID_ACTIONS_NUM) - 1];
            }
            // process selected nandroid action
            int chosen_subitem = chosen_item % NANDROID_ACTIONS_NUM;
            switch (chosen_subitem) {
                case 0: {
                    char backup_path[PATH_MAX];
                    time_t t = time(NULL);
                    struct tm *tmp = localtime(&t);
                    if (tmp == NULL) {
                        struct timeval tp;
                        gettimeofday(&tp, NULL);
                        sprintf(backup_path, "%s/clockworkmod/backup/%ld", chosen_path, tp.tv_sec);
                    } else {
                        char path_fmt[PATH_MAX];
                        strftime(path_fmt, sizeof(path_fmt), "clockworkmod/backup/%F.%H.%M.%S", tmp);
                        // this sprintf results in:
                        // clockworkmod/backup/%F.%H.%M.%S (time values are populated too)
                        sprintf(backup_path, "%s/%s", chosen_path, path_fmt);
                    }
                    if (confirm_selection("Confirm backup?", "Yes - Backup")) {
                        nandroid_backup(backup_path);
                    }
                    break;
                }
                case 1:
                    show_nandroid_restore_menu(chosen_path);
                    break;
                case 2:
                    show_nandroid_delete_menu(chosen_path);
                    break;
                case 3:
                    show_nandroid_advanced_restore_menu(chosen_path);
                    break;
                default:
                    break;
            }
        } else {
#ifdef RECOVERY_EXTEND_NANDROID_MENU
            handle_nandroid_menu(action_entries_num + NANDROID_FIXED_ENTRIES, chosen_item);
#endif
            goto out;
        }
    }
out:
    for (i = 0; i < action_entries_num; i++)
        free(list[i]);
    return chosen_item;
}

static void format_sdcard(const char* volume) {
    if (is_data_media_volume_path(volume))
        return;

    Volume *v = volume_for_path(volume);
    if (v == NULL || strcmp(v->fs_type, "auto") != 0)
        return;
    if (!fs_mgr_is_voldmanaged(v) && !can_partition(volume))
        return;


    const char* headers[] = { "Format device:", volume, "", NULL };

    static char* list[] = { "default",
 			    "vfat",
                            "exfat",
                            "ntfs",
                            "ext4",
                            "ext3",
                            "ext2",
                            NULL };
if ( language== 0 ) {
    headers[0] = "格式化设备:";

    list[0] = "默认格式";
} else {
    headers[0] = "Format device:";

    list[0] = "default";
}
                           

    int ret = -1;
    char cmd[PATH_MAX];
    int chosen_item = get_menu_selection(headers, list, 0, 0);
    if (chosen_item < 0) // REFRESH or GO_BACK
        return;
if ( language== 1 )
    if (!confirm_selection("Confirm formatting?", "Yes - Format device"))
	return;
else
    if (!confirm_selection("确认需要格式化？", "是 - 格式化此设备"))

        return;

    if (ensure_path_unmounted(v->mount_point) != 0)
        return;

    switch (chosen_item) {
        case 0:
            ret = format_volume(v->mount_point);
            break;
        case 1:
        case 2:
        case 3:
        case 4: {
            if (fs_mgr_is_voldmanaged(v)) {
                ret = vold_custom_format_volume(v->mount_point, list[chosen_item], 1) == CommandOkay ? 0 : -1;
            } else if (strcmp(list[chosen_item], "vfat") == 0) {
                sprintf(cmd, "/sbin/newfs_msdos -F 32 -O android -c 8 %s", v->blk_device);
                ret = __system(cmd);
            } else if (strcmp(list[chosen_item], "exfat") == 0) {
                sprintf(cmd, "/sbin/mkfs.exfat %s", v->blk_device);
                ret = __system(cmd);
            } else if (strcmp(list[chosen_item], "ntfs") == 0) {
                sprintf(cmd, "/sbin/mkntfs -f %s", v->blk_device);
                ret = __system(cmd);
            } else if (strcmp(list[chosen_item], "ext4") == 0) {
                char *secontext = NULL;
                if (selabel_lookup(sehandle, &secontext, v->mount_point, S_IFDIR) < 0) {
                    LOGE("cannot lookup security context for %s\n", v->mount_point);
                    ret = make_ext4fs(v->blk_device, v->length, volume, NULL);
                } else {
                    ret = make_ext4fs(v->blk_device, v->length, volume, sehandle);
                    freecon(secontext);
                }
            }
            break;
        }
        case 5:
        case 6: {
            // workaround for new vold managed volumes that cannot be recognized by prebuilt ext2/ext3 bins
            const char *device = v->blk_device2;
            if (device == NULL)
                device = v->blk_device;
            ret = format_unknown_device(device, v->mount_point, list[chosen_item]);
            break;
        }
    }
if ( language== 1 ) {
    if (ret)

        ui_print("Could not format %s (%s)\n", volume, list[chosen_item]);
    else
        ui_print("Done formatting %s (%s)\n", volume, list[chosen_item]);


	} else {
		if (ret)

        
        ui_print("无法格式化 %s (%s)\n", volume, list[chosen_item]);
    else
        ui_print("成功完成格式化 %s (%s)\n", volume, list[chosen_item]);

  	}
}

static void partition_sdcard(const char* volume) {
    if (!can_partition(volume)) {
if ( language== 1 )
        ui_print("Can't partition device: %s\n", volume);
else
        ui_print("无法对设备进行分区: %s\n", volume);

        return;
    }

    static char* ext_sizes[] = { "128M",
                                 "256M",
                                 "512M",
                                 "1024M",
                                 "2048M",
                                 "4096M",
                                 NULL };

    static char* swap_sizes[] = { "0M",
                                  "32M",
                                  "64M",
                                  "128M",
                                  "256M",
                                  NULL };

    static char* partition_types[] = { "ext3",
                                       "ext4",
                                       NULL };


    static const char* ext_headers[] = { "Ext Size", "", NULL };
    static const char* swap_headers[] = { "Swap Size", "", NULL };
    static const char* fstype_headers[] = { "Partition Type", "", NULL };
if ( language== 0 ) {
    ext_headers[0] = "Ext 大小";
    swap_headers[0] = "Swap 大小";
    fstype_headers[0] = "分区类型";
} else {
    ext_headers[0] = "Ext Size";
    swap_headers[0] = "Swap Size";
    fstype_headers[0] = "Partition Type";
}

    int ext_size = get_menu_selection(ext_headers, ext_sizes, 0, 0);
    if (ext_size < 0)
        return;

    int swap_size = get_menu_selection(swap_headers, swap_sizes, 0, 0);
    if (swap_size < 0)
        return;

    int partition_type = get_menu_selection(fstype_headers, partition_types, 0, 0);
    if (partition_type < 0)
        return;

    char sddevice[256];
    Volume *vol = volume_for_path(volume);

    // can_partition() ensured either blk_device or blk_device2 has /dev/block/mmcblk format
    if (strstr(vol->blk_device, "/dev/block/mmcblk") != NULL)
        strcpy(sddevice, vol->blk_device);
    else
        strcpy(sddevice, vol->blk_device2);

    // we only want the mmcblk, not the partition
    sddevice[strlen("/dev/block/mmcblkX")] = '\0';
    char cmd[PATH_MAX];
    setenv("SDPATH", sddevice, 1);
    sprintf(cmd, "sdparted -es %s -ss %s -efs %s -s", ext_sizes[ext_size], swap_sizes[swap_size], partition_types[partition_type]);
if ( language== 1 )
    ui_print("Partitioning SD Card... please wait...\n");
else
    ui_print("正在对 SD 卡进行分区... 请稍等...\n");

    if (0 == __system(cmd))
if ( language== 1 )
        ui_print("Done!\n");
else
        ui_print("完成！\n");

    else
if ( language== 1 )
        ui_print("An error occured while partitioning your SD Card. Please see /tmp/recovery.log for more details.\n");
else
        ui_print("对 SD 卡进行分区出现了一个错误。请查看 /tmp/recovery.log 以了解更多细节。\n");

}

static int can_partition(const char* volume) {
    if (is_data_media_volume_path(volume))
        return 0;

    Volume *vol = volume_for_path(volume);
    if (vol == NULL) {
if ( language== 1 )
        LOGI("Can't format unknown volume: %s\n", volume);
else
        LOGI("无法格式化未知卷: %s\n", volume);

        return 0;
    }
    if (strcmp(vol->fs_type, "auto") != 0) {
if ( language== 1 )
        LOGI("Can't partition non-vfat: %s (%s)\n", volume, vol->fs_type);
else
        LOGI("无法对非 vfat 设备进行分区: %s (%s)\n", volume, vol->fs_type);

        return 0;
    }
    // do not allow partitioning of a device that isn't mmcblkX or mmcblkXp1
    // needed with new vold managed volumes and virtual device path links
    int vol_len;
    char *device = NULL;
    if (strstr(vol->blk_device, "/dev/block/mmcblk") != NULL) {
        device = vol->blk_device;
    } else if (vol->blk_device2 != NULL && strstr(vol->blk_device2, "/dev/block/mmcblk") != NULL) {
        device = vol->blk_device2;
    } else {
if ( language== 1 )
        LOGI("Can't partition non mmcblk device: %s\n", vol->blk_device);
else
        LOGI("无法对非 mmcblk 设备进行分区: %s\n", vol->blk_device);

        return 0;
    }

    vol_len = strlen(device);
    if (device[vol_len - 2] == 'p' && device[vol_len - 1] != '1') {
if ( language== 1 )
        LOGI("Can't partition unsafe device: %s\n", device);
else
        LOGI("无法为不安全的设备分区: %s\n", device);

        return 0;
    }

    return 1;
}

int show_advanced_menu() {
    char buf[80];
    int i = 0, j = 0, chosen_item = 0, list_index = 0;
    /* Default number of entries if no compile-time extras are added */
    static char* list[MAX_NUM_MANAGED_VOLUMES + FIXED_ADVANCED_ENTRIES + 1];

    char* primary_path = get_primary_storage_path();
    char** extra_paths = get_extra_storage_paths();
    int num_extra_volumes = get_num_extra_volumes();


    static const char* headers[] = { "Advanced Menu", "", NULL };
if ( language== 0 ) { 
	headers[0] = "高级功能菜单";
} else { 
	headers[0] = "Advanced Menu";
}


    memset(list, 0, MAX_NUM_MANAGED_VOLUMES + FIXED_ADVANCED_ENTRIES + 1);

if ( language== 1 )
    list[list_index++] = "reboot recovery";
else
    list[list_index++] = "重启 recovery";


    char bootloader_mode[PROPERTY_VALUE_MAX];
    property_get("ro.bootloader.mode", bootloader_mode, "");
    if (!strcmp(bootloader_mode, "download")) {
if ( language== 1 )
        list[list_index++] = "reboot to download mode";
else
        list[list_index++] = "重启到下载模式";

    } else {
if ( language== 1 )
        list[list_index++] = "reboot to bootloader";
else
        list[list_index++] = "重启到 bootloader";

    }

if ( language== 1 ) {
    list[list_index++] = "power off";
    list[list_index++] = "wipe dalvik cache";
    list[list_index++] = "report error";
    list[list_index++] = "key test";
    list[list_index++] = "show log";
}else{
    list[list_index++] = "关机";
    list[list_index++] = "清除 dalvik 缓存";
    list[list_index++] = "报告错误";
    list[list_index++] = "键值测试";
    list[list_index++] = "显示日志";
}
#ifdef BOARD_NATIVE_DUALBOOT_SINGLEDATA
    int index_tdb = list_index++;
    int index_bootmode = list_index++;
#endif
#ifdef ENABLE_LOKI

    int index_loki = list_index++;
if ( language== 1 )
    list[index_loki] = "toggle loki support";
else
    list[index_loki] = "开关 loki 支持";

#endif


    char list_prefix[] = "partition ";
if ( language== 0 ) { 
	list_prefix[0] = "分区 ";
} else { 
	list_prefix[0] = "partition ";
} 

    if (can_partition(primary_path)) {
        sprintf(buf, "%s%s", list_prefix, primary_path);
        list[FIXED_ADVANCED_ENTRIES] = strdup(buf);
        j++;
    }

    if (extra_paths != NULL) {
        for (i = 0; i < num_extra_volumes; i++) {
            if (can_partition(extra_paths[i])) {
                sprintf(buf, "%s%s", list_prefix, extra_paths[i]);
                list[FIXED_ADVANCED_ENTRIES + j] = strdup(buf);
                j++;
            }
        }
    }
    list[FIXED_ADVANCED_ENTRIES + j] = NULL;

    for (;;) {
#ifdef BOARD_NATIVE_DUALBOOT_SINGLEDATA
        char tdb_name[PATH_MAX];
        device_get_truedualboot_entry(tdb_name);
        list[index_tdb] = &tdb_name;

        char bootmode_name[PATH_MAX];
        device_get_bootmode(bootmode_name);
        list[index_bootmode] = &bootmode_name;
#endif
        chosen_item = get_filtered_menu_selection(headers, list, 0, 0, sizeof(list) / sizeof(char*));
        if (chosen_item == GO_BACK || chosen_item == REFRESH)
            break;
        switch (chosen_item) {
            case 0: {
if ( language== 1 )
                ui_print("Rebooting recovery...\n");
else
                ui_print("正在重启 recovery...\n");

                reboot_main_system(ANDROID_RB_RESTART2, 0, "recovery");
                break;
            }
            case 1: {
                if (!strcmp(bootloader_mode, "download")) {
if ( language== 1 )
                    ui_print("Rebooting to download mode...\n");
else
                    ui_print("正在重启到下载模式...\n");

                    reboot_main_system(ANDROID_RB_RESTART2, 0, "download");
                } else {
if ( language== 1 )
                    ui_print("Rebooting to bootloader...\n");
else
                    ui_print("正在重启到 bootloader...\n");

                    reboot_main_system(ANDROID_RB_RESTART2, 0, "bootloader");
                }
                break;
            }
            case 2: {
if ( language== 1 )
                ui_print("Shutting down...\n");
else
                ui_print("关机中...\n");

                reboot_main_system(ANDROID_RB_POWEROFF, 0, 0);
                break;
            }
            case 3: {
                if (0 != ensure_path_mounted("/data"))
                    break;
                if (volume_for_path("/sd-ext") != NULL)
                    ensure_path_mounted("/sd-ext");
                ensure_path_mounted("/cache");
if ( language== 1 )
                if (confirm_selection("Confirm wipe?", "Yes - Wipe Dalvik Cache")) {
		__system("rm -r /data/dalvik-cache");
	        __system("rm -r /cache/dalvik-cache");
                __system("rm -r /sd-ext/dalvik-cache");
                    ui_print("Dalvik Cache wiped.\n");
		}
else {
                if (confirm_selection("确认清除？", "是 - 清除 Dalvik 缓存")) {

                    __system("rm -r /data/dalvik-cache");
                    __system("rm -r /cache/dalvik-cache");
                    __system("rm -r /sd-ext/dalvik-cache");
                    ui_print("已清除 Dalvik 缓存。\n");

                }
	}
                ensure_path_unmounted("/data");
                break;
            }
            case 4:
                handle_failure(1);
                break;
            case 5: {
if ( language== 1 ) {
                ui_print("Outputting key codes.\n");
                ui_print("Go back to end debugging.\n");
}else{
                ui_print("正在监测键值。\n");
                ui_print("按返回键来结束调试。\n");
}
                int key;
                int action;
                do {
                    key = ui_wait_key();
                    action = device_handle_key(key, 1);
if ( language== 1 )
                    ui_print("Key: %d\n", key);
else
                    ui_print("键值: %d\n", key);

                } while (action != GO_BACK);
                break;
            }
            case 6:
                ui_printlogtail(24);
                ui_wait_key();
                ui_clear_key_queue();
                break;
            default:
#ifdef BOARD_NATIVE_DUALBOOT_SINGLEDATA
            if(chosen_item==index_tdb) {
                device_toggle_truedualboot();
                break;
            }
            if(chosen_item==index_bootmode) {
                device_choose_bootmode();
                break;
            }
#endif
#ifdef ENABLE_LOKI
            if(chosen_item==index_loki) {
                toggle_loki_support();
                break;
            }
#endif
            partition_sdcard(list[chosen_item] + strlen(list_prefix));
            break;
        }
    }

    for (; j > 0; --j) {
        free(list[FIXED_ADVANCED_ENTRIES + j - 1]);
    }
    return chosen_item;
}

static void write_fstab_root(char *path, FILE *file) {
    Volume *vol = volume_for_path(path);
    if (vol == NULL) {
if ( language== 1 )
        LOGW("Unable to get recovery.fstab info for %s during fstab generation!\n", path);
else
        LOGW("无法在 fstab 生成过程中获取 recovery.fstab 中 %s 的信息！\n", path);

        return;
    }

    char device[200];
    if (vol->blk_device[0] != '/')
        get_partition_device(vol->blk_device, device);
    else
        strcpy(device, vol->blk_device);

    fprintf(file, "%s ", device);
    fprintf(file, "%s ", path);
    // special case rfs cause auto will mount it as vfat on samsung.
    fprintf(file, "%s rw\n", vol->fs_type2 != NULL && strcmp(vol->fs_type, "rfs") != 0 ? "auto" : vol->fs_type);
}

static void create_fstab() {
    struct stat info;
    __system("touch /etc/mtab");
    FILE *file = fopen("/etc/fstab", "w");
    if (file == NULL) {
if ( language== 1 )
        LOGW("Unable to create /etc/fstab!\n");
else
        LOGW("无法创建 /etc/fstab!\n");

        return;
    }
    Volume *vol = volume_for_path("/boot");
    if (NULL != vol && strcmp(vol->fs_type, "mtd") != 0 && strcmp(vol->fs_type, "emmc") != 0 && strcmp(vol->fs_type, "bml") != 0)
        write_fstab_root("/boot", file);
    write_fstab_root("/cache", file);
    write_fstab_root("/data", file);
    write_fstab_root("/datadata", file);
    write_fstab_root("/emmc", file);
    write_fstab_root("/system", file);
    write_fstab_root("/preload", file);
    write_fstab_root("/sdcard", file);
    write_fstab_root("/sd-ext", file);
    write_fstab_root("/external_sd", file);
    fclose(file);
if ( language== 1 )
    LOGI("Completed outputting fstab.\n");
else
    LOGI("fstab 输出已完成。\n");

}

static int bml_check_volume(const char *path) {
if ( language== 1 )
    ui_print("Checking %s...\n", path);
else
    ui_print("正在检查 %s...\n", path);

    ensure_path_unmounted(path);
    if (0 == ensure_path_mounted(path)) {
        ensure_path_unmounted(path);
        return 0;
    }

    Volume *vol = volume_for_path(path);
    if (vol == NULL) {
if ( language== 1 )
        LOGE("Unable process volume! Skipping...\n");
else
        LOGE("无法处理卷！跳过...\n");

        return 0;
    }

if ( language== 1 )
    ui_print("%s may be rfs. Checking...\n", path);
else
    ui_print("%s 的格式可能为 rfs。检查中...\n", path);

    char tmp[PATH_MAX];
    sprintf(tmp, "mount -t rfs %s %s", vol->blk_device, path);
    int ret = __system(tmp);
    printf("%d\n", ret);
    return ret == 0 ? 1 : 0;
}

void process_volumes() {
    create_fstab();

    if (is_data_media()) {
        setup_data_media();
    }

    return;
}

void handle_failure(int ret) {
    if (ret == 0)
        return;
    if (0 != ensure_path_mounted(get_primary_storage_path()))
        return;
    mkdir("/sdcard/clockworkmod", S_IRWXU | S_IRWXG | S_IRWXO);
    __system("cp /tmp/recovery.log /sdcard/clockworkmod/recovery.log");
if ( language== 1 )
    ui_print("/tmp/recovery.log was copied to /sdcard/clockworkmod/recovery.log. Please open ROM Manager to report the issue.\n");
else
    ui_print("已将 /tmp/recovery.log 复制到 /sdcard/clockworkmod/recovery.log。请打开ROM管理器来反馈这个问题。\n");

}

static int is_path_mounted(const char* path) {
    Volume* v = volume_for_path(path);
    if (v == NULL) {
        return 0;
    }
    if (strcmp(v->fs_type, "ramdisk") == 0) {
        // the ramdisk is always mounted.
        return 1;
    }

    if (scan_mounted_volumes() < 0)
        return 0;

    const MountedVolume* mv = find_mounted_volume_by_mount_point(v->mount_point);
    if (mv) {
        // volume is already mounted
        return 1;
    }
    return 0;
}

int has_datadata() {
    Volume *vol = volume_for_path("/datadata");
    return vol != NULL;
}

int volume_main(int argc, char **argv) {
    load_volume_table();
    return 0;
}

int verify_root_and_recovery() {
    if (ensure_path_mounted("/system") != 0)
        return 0;

    int ret = 0;
    struct stat st;
    // check to see if install-recovery.sh is going to clobber recovery
    // install-recovery.sh is also used to run the su daemon on stock rom for 4.3+
    // so verify that doesn't exist...
    if (0 != lstat("/system/etc/.installed_su_daemon", &st)) {
        // check install-recovery.sh exists and is executable
        if (0 == lstat("/system/etc/install-recovery.sh", &st)) {
            if (st.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) {
                ui_show_text(1);
                ret = 1;
if ( language== 1 )
                if (confirm_selection("ROM may flash stock recovery on boot. Fix?", "Yes - Disable recovery flash")) {
__system("chmod -x /system/etc/install-recovery.sh");
                }else {
                if (confirm_selection("ROM 可能会在开机时将 recovery 还原为官方版本。是否修正？", "是 - 禁用自动还原")) {

                    __system("chmod -x /system/etc/install-recovery.sh");
                	}
		}
            }
        }
    }

    int exists = 0;
    if (0 == lstat("/system/bin/su", &st)) {
        exists = 1;
        if (S_ISREG(st.st_mode)) {
            if ((st.st_mode & (S_ISUID | S_ISGID)) != (S_ISUID | S_ISGID)) {
                ui_show_text(1);
                ret = 1;
if ( language== 1 )
                if (confirm_selection("Root access possibly lost. Fix?", "Yes - Fix root (/system/bin/su)")) {
__system("chmod 6755 /system/bin/su");
                } else {
                if (confirm_selection("root 访问权限设置不当。是否修正？", "是 - 修正 root (/system/bin/su)")) {

                    __system("chmod 6755 /system/bin/su");
                }
			}
            }
        }
    }

    if (0 == lstat("/system/xbin/su", &st)) {
        exists = 1;
        if (S_ISREG(st.st_mode)) {
            if ((st.st_mode & (S_ISUID | S_ISGID)) != (S_ISUID | S_ISGID)) {
                ui_show_text(1);
                ret = 1;
if ( language== 1 )
                if (confirm_selection("Root access possibly lost. Fix?", "Yes - Fix root (/system/xbin/su)")) {
__system("chmod 6755 /system/xbin/su");
                } else {
                if (confirm_selection("root 访问权限设置不当。是否修正？", "是 - 修正 root (/system/xbin/su)")) {

                    __system("chmod 6755 /system/xbin/su");
                }
			}
            }
        }
    }

    if (!exists) {
        ui_show_text(1);
        ret = 1;

if ( language== 1 )
        if (confirm_selection("Root access is missing. Root device?", "Yes - Root device (/system/xbin/su)")) {
__system("/sbin/install-su.sh");
        } else {
        if (confirm_selection("未添加 root 访问所需文件。是否添加？", "是 - 确认添加 (/system/xbin/su)")) {

            __system("/sbin/install-su.sh");
        }
		}
    }

    ensure_path_unmounted("/system");
    return ret;
}
