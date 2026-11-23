#include "test.h"
#include "prism.h"

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    return Test("prism.txt", [](FILE* result) {
        auto dll = LoadLibraryW(L".\\prism.dll");
        Require(dll != nullptr, "load PRISM");
        // Use the version-2 config retained by PRISM for the D4 Assist binding.
        // The v0.18.3 config_init returns a newer, larger structure by value.
        PrismConfig config{};
        config.version = PRISM_CONFIG_VERSION;
        auto context = Load<decltype(&prism_init)>(dll, "prism_init")(&config);
        Require(context != nullptr, "prism_init");
        auto backend = Load<decltype(&prism_registry_acquire_best)>(dll, "prism_registry_acquire_best")(context);
        Require(backend != nullptr, "prism_registry_acquire_best");
        const char* name = Load<decltype(&prism_backend_name)>(dll, "prism_backend_name")(backend);
        fprintf(result, "PRISM selected %s\n", name ? name : "(none)");
        // Only the NVDA backend is safe to call in this silent test.
        Require(name && std::string(name) == "NVDA", "selected backend is NVDA");
        const auto initialized = Load<decltype(&prism_backend_initialize)>(dll, "prism_backend_initialize")(backend);
        fprintf(result, "Initialize status = %d\n", initialized);
        Require(initialized == PRISM_OK || initialized == PRISM_ERROR_ALREADY_INITIALIZED, "initialize NVDA");
        Require(Load<decltype(&prism_backend_speak)>(dll, "prism_backend_speak")(backend, "PRISM plain", true) == PRISM_OK,
                "PRISM speech");
        bool speaking = true;
        Require(Load<decltype(&prism_backend_is_speaking)>(dll, "prism_backend_is_speaking")(backend, &speaking) == PRISM_OK,
                "PRISM speaking status");
        Require(!speaking, "silent speaker is not speaking");
        Require(Load<decltype(&prism_backend_stop)>(dll, "prism_backend_stop")(backend) == PRISM_OK, "PRISM stop");
        Load<decltype(&prism_backend_free)>(dll, "prism_backend_free")(backend);
        Load<decltype(&prism_shutdown)>(dll, "prism_shutdown")(context);
        FreeLibrary(dll);
    });
}
