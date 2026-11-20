#include <list>
#include <vector>
#include <cstring>
#include <pthread.h>
#include <thread>
#include <cstring>
#include <string>
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

// State of the God Mode toggle (set from the menu)
bool godMode = false;

// Feature list shown in the menu.
// Feature numbers count from 0 in the order below (Category/ButtonLink are not counted).
jobjectArray GetFeatureList(JNIEnv *env, jobject context) {
    jobjectArray ret;

    const char *features[] = {
            OBFUSCATE("Toggle_God Mode (NPC damage)"),   // featNum 0
    };

    int Total_Feature = (sizeof features / sizeof features[0]);
    ret = (jobjectArray)
            env->NewObjectArray(Total_Feature, env->FindClass(OBFUSCATE("java/lang/String")),
                                env->NewStringUTF(""));

    for (int i = 0; i < Total_Feature; i++)
        env->SetObjectArrayElement(ret, i, env->NewStringUTF(features[i]));

    return (ret);
}

// Called by the menu when a feature is changed
void Changes(JNIEnv *env, jclass clazz, jobject obj, jint featNum, jstring featName, jint value, jlong Lvalue, jboolean boolean, jstring text) {
    switch (featNum) {
        case 0:
            godMode = boolean;
            LOGI(OBFUSCATE("godMode = %d"), (int) boolean);
            break;
    }
}

// PlayerHealth$$NpcDamage, RVA 0x113B130 (from dump.cs / script.json)
// void NpcDamage(PlayerHealth *this, int32_t _damage, String *_killFeedType, MethodInfo *method)
void (*old_NpcDamage)(void *instance, int damage, void *killFeedType, void *method);

void NpcDamage(void *instance, int damage, void *killFeedType, void *method) {
    // Log only the first call so logcat is not spammed; confirms the hook is alive
    static bool loggedOnce = false;
    if (!loggedOnce) {
        loggedOnce = true;
        LOGI(OBFUSCATE("NpcDamage hook reached: damage=%d godMode=%d"), damage, (int) godMode);
    }

    if (instance != nullptr && godMode) {
        return; // ignore damage coming from NPCs
    }
    return old_NpcDamage(instance, damage, killFeedType, method);
}

// Target lib here
#define targetLibName OBFUSCATE("libil2cpp.so")

ElfScanner g_il2cppELF;

// We run our hacks in a new thread so the wait loop does not block the process main thread
void *hack_thread(void *) {
    LOGI(OBFUSCATE("pthread created"));

    // Wait until libil2cpp.so is loaded
    do {
        sleep(1);
        // createWithPath can also find the lib base if it was loaded from the zipped base.apk
        g_il2cppELF = ElfScanner::createWithPath(targetLibName);
    } while (!g_il2cppELF.isValid());

    LOGI(OBFUSCATE("%s has been loaded"), (const char *) targetLibName);

#if defined(__aarch64__)
    // Il2Cpp: use the RVA offset
    HOOK(targetLibName, str2Offset(OBFUSCATE("0x113B130")), NpcDamage, old_NpcDamage);

#elif defined(__arm__)
    // Nothing for armv7 (the RVAs above are for the arm64 libil2cpp.so)
#endif

    LOGI(OBFUSCATE("Done"));
    return nullptr;
}

__attribute__((constructor))
void lib_main() {
    // Create a new thread so it does not block the main thread, so the game does not freeze
    pthread_t ptid;
    pthread_create(&ptid, NULL, hack_thread, NULL);
}
