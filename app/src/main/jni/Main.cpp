#include <list>
#include <vector>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <string>
#include <mutex>
#include <jni.h>
#include <unistd.h>
#include <fstream>
#include <iostream>
#include <dlfcn.h>
#include "Includes/Logger.h"
#include "Includes/obfuscate.h"
#include "Includes/Utils.hpp"
#include "Menu/Menu.hpp"
#include "Menu/Jni.hpp"
#include "Includes/Macros.h"

// ---------------------------------------------------------------------------
// State (set from the menu)
// ---------------------------------------------------------------------------
bool godMode = false;          // featNum 0
bool logExec = false;          // featNum 3 (recon: log lines passed to ExecuteCmd)
int runRequest = 0;            // 0 = none, 1 = run menu text (featNum 2), 2 = run cmd.txt (featNum 4)
std::string cmdText;           // featNum 1
std::mutex cmdMtx;

// ---------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------
jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jobjectArray ret;

    const char *features[] = {
            OBFUSCATE("Toggle_God Mode (NPC damage)"),        // 0
            OBFUSCATE("InputText_Triggerbox command"),        // 1
            OBFUSCATE("Button_Run command"),                  // 2
            OBFUSCATE("Toggle_Log ExecuteCmd lines (logcat)"),// 3
            OBFUSCATE("Button_Run cmd.txt (multi-line)"),     // 4
    };

    int Total_Feature = (sizeof features / sizeof features[0]);
    ret = (jobjectArray)
            env->NewObjectArray(Total_Feature, env->FindClass(OBFUSCATE("java/lang/String")),
                                env->NewStringUTF(""));

    for (int i = 0; i < Total_Feature; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));

    return (ret);
}

void Changes(JNIEnv *env, jclass clazz, jobject obj, jint featNum, jstring featName, jint value, jlong Lvalue, jboolean boolean, jstring text) {
    switch (featNum) {
        case 0:
            godMode = boolean;
            LOGI(OBFUSCATE("godMode = %d"), (int) boolean);
            break;
        case 1: {
            if (text != nullptr) {
                const char *s = env->GetStringUTFChars(text, 0);
                if (s) {
                    std::lock_guard<std::mutex> lk(cmdMtx);
                    cmdText = s;
                    env->ReleaseStringUTFChars(text, s);
                }
            }
            break;
        }
        case 2:
            runRequest = 1;
            break;
        case 3:
            logExec = boolean;
            break;
        case 4:
            runRequest = 2;
            break;
    }
}

// ---------------------------------------------------------------------------
// il2cpp helpers (resolved by name from libil2cpp.so exports)
// ---------------------------------------------------------------------------
typedef void *(*il2cpp_string_new_t)(const char *);
typedef void *(*il2cpp_array_new_t)(void *klass, uintptr_t n);
typedef void *(*il2cpp_get_corlib_t)();
typedef void *(*il2cpp_class_from_name_t)(void *image, const char *ns, const char *name);

il2cpp_string_new_t      p_string_new = nullptr;
il2cpp_array_new_t       p_array_new = nullptr;
void *                   g_stringClass = nullptr;

// Il2CppString layout (64-bit): klass(8) monitor(8) length(int @0x10) chars(UTF-16 @0x14)
std::string Il2CppStr(void *s) {
    if (!s) return "";
    int len = *(int *) ((uintptr_t) s + 0x10);
    uint16_t *ch = (uint16_t *) ((uintptr_t) s + 0x14);
    std::string out;
    for (int i = 0; i < len && i < 512; i++)
        out += (ch[i] < 128) ? (char) ch[i] : '?';
    return out;
}

// Il2CppArray layout (64-bit): klass(8) monitor(8) bounds(8) max_length(@0x18) items(@0x20)
void LogLines(void *arr) {
    if (!arr) return;
    uintptr_t n = *(uintptr_t *) ((uintptr_t) arr + 0x18);
    void **items = (void **) ((uintptr_t) arr + 0x20);
    for (uintptr_t i = 0; i < n && i < 64; i++)
        LOGI(OBFUSCATE("ExecuteCmd line[%d] = %s"), (int) i, Il2CppStr(items[i]).c_str());
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

// PlayerHealth$$NpcDamage, RVA 0x113B130
void (*old_NpcDamage)(void *instance, int damage, void *killFeedType, void *method);

void NpcDamage(void *instance, int damage, void *killFeedType, void *method) {
    if (instance != nullptr && godMode) {
        return; // ignore damage coming from NPCs
    }
    return old_NpcDamage(instance, damage, killFeedType, method);
}

// EditorTriggerBox$$ExecuteCmd(string[] commandLines), RVA 0x11697D8
// Recon hook: shows in logcat what format the game passes in.
void (*old_ExecuteCmd)(void *self, void *lines, void *method);

void ExecuteCmd_hook(void *self, void *lines, void *method) {
    if (logExec) LogLines(lines);
    return old_ExecuteCmd(self, lines, method);
}

// Multi-line input.
// - Menu text: type a literal "\n" (backslash + n) where you want a new line.
// - cmd.txt:   real new lines, edit it with any editor.
#define CMD_FILE "/sdcard/Android/data/com.MA.LAC/files/cmd.txt"

std::vector<std::string> SplitLines(std::string t) {
    auto rep = [&](const std::string &a, const std::string &b) {
        size_t p = 0;
        while ((p = t.find(a, p)) != std::string::npos) {
            t.replace(p, a.size(), b);
            p += b.size();
        }
    };
    rep("\r", "");
    rep("\\n", "\n"); // literal backslash-n -> real new line

    std::vector<std::string> out;
    size_t start = 0;
    while (start <= t.size()) {
        size_t end = t.find('\n', start);
        std::string line = t.substr(start, end == std::string::npos ? std::string::npos : end - start);
        size_t a = line.find_first_not_of(" \t");
        size_t b = line.find_last_not_of(" \t");
        if (a != std::string::npos) out.push_back(line.substr(a, b - a + 1));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}

std::string ReadCmdFile() {
    std::ifstream f(CMD_FILE);
    if (!f.is_open()) {
        LOGE(OBFUSCATE("Cannot open %s"), CMD_FILE);
        return "";
    }
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Build a string[] (one element per line) and run it through the game's own ExecuteCmd.
// Must be called on the Unity main thread (we do it from the Tick hook).
void RunCommand(void *box, const std::vector<std::string> &lines) {
    if (!p_string_new || !p_array_new || !g_stringClass || !old_ExecuteCmd) {
        LOGE(OBFUSCATE("il2cpp API not ready"));
        return;
    }
    if (lines.empty()) return;
    void *arr = p_array_new(g_stringClass, lines.size());
    void **items = (void **) ((uintptr_t) arr + 0x20);
    for (size_t i = 0; i < lines.size(); i++)
        items[i] = p_string_new(lines[i].c_str());
    old_ExecuteCmd(box, arr, nullptr); // call the original directly so it is not logged
}

// EditorTriggerBox$$Tick(), RVA 0x1166344: runs every frame per box on the main thread
void (*old_Tick)(void *self, void *method);

void Tick_hook(void *self, void *method) {
    if (self != nullptr && runRequest != 0) {
        int req = runRequest;
        runRequest = 0;
        std::string text;
        if (req == 1) {
            std::lock_guard<std::mutex> lk(cmdMtx);
            text = cmdText;
        } else {
            text = ReadCmdFile();
        }
        std::vector<std::string> lines = SplitLines(text);
        LOGI(OBFUSCATE("Running %d line(s)"), (int) lines.size());
        RunCommand(self, lines);
    }
    return old_Tick(self, method);
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
#define targetLibName OBFUSCATE("libil2cpp.so")

ElfScanner g_il2cppELF;

void *hack_thread(void *) {
    LOGI(OBFUSCATE("pthread created"));

    do {
        sleep(1);
        g_il2cppELF = ElfScanner::createWithPath(targetLibName);
    } while (!g_il2cppELF.isValid());

    LOGI(OBFUSCATE("%s has been loaded"), (const char *) targetLibName);

#if defined(__aarch64__)
    // Resolve il2cpp API by name
    void *h = dlopen(targetLibName, RTLD_NOW);
    if (h) {
        p_string_new = (il2cpp_string_new_t) dlsym(h, "il2cpp_string_new");
        p_array_new = (il2cpp_array_new_t) dlsym(h, "il2cpp_array_new");
        il2cpp_get_corlib_t get_corlib = (il2cpp_get_corlib_t) dlsym(h, "il2cpp_get_corlib");
        il2cpp_class_from_name_t class_from_name = (il2cpp_class_from_name_t) dlsym(h, "il2cpp_class_from_name");
        if (get_corlib && class_from_name)
            g_stringClass = class_from_name(get_corlib(), "System", "String");
    }
    LOGI(OBFUSCATE("il2cpp api: string_new=%p array_new=%p stringClass=%p"),
         (void *) p_string_new, (void *) p_array_new, g_stringClass);

    // Il2Cpp: RVA offsets from dump.cs
    HOOK(targetLibName, str2Offset(OBFUSCATE("0x113B130")), NpcDamage, old_NpcDamage);
    HOOK(targetLibName, str2Offset(OBFUSCATE("0x11697D8")), ExecuteCmd_hook, old_ExecuteCmd);
    HOOK(targetLibName, str2Offset(OBFUSCATE("0x1166344")), Tick_hook, old_Tick);

#elif defined(__arm__)
    // Nothing for armv7 (RVAs are for the arm64 libil2cpp.so)
#endif

    LOGI(OBFUSCATE("Done"));
    return nullptr;
}

__attribute__((constructor))
void lib_main() {
    pthread_t ptid;
    pthread_create(&ptid, NULL, hack_thread, NULL);
}
