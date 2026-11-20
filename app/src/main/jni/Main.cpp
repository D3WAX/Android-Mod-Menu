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
