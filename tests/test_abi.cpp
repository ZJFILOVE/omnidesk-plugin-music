#include <dlfcn.h>
#include <iostream>
#include <string>
#include <omnidesk/plugin_abi.h>
#include <omnidesk/plugin.hpp>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: test_abi <so_path> <expected_id>\n";
        return 2;
    }
    void* h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        std::cerr << "dlopen failed: " << dlerror() << "\n";
        return 1;
    }
    auto info_fn = reinterpret_cast<OmniDeskAbiInfoFn>(dlsym(h, "omnidesk_plugin_abi_info"));
    auto create_fn = reinterpret_cast<OmniDeskPluginCreateFn>(dlsym(h, "omnidesk_plugin_create"));
    auto destroy_fn = reinterpret_cast<OmniDeskPluginDestroyFn>(dlsym(h, "omnidesk_plugin_destroy"));
    if (!info_fn || !create_fn || !destroy_fn) {
        std::cerr << "Missing ABI symbol\n";
        dlclose(h);
        return 1;
    }
    const OmniDeskPluginInfo* info = info_fn();
    if (!info || info->abi_version != OMNIDESK_PLUGIN_ABI_VERSION ||
        info->struct_size < sizeof(OmniDeskPluginInfo) || !info->id ||
        std::string(info->id) != argv[2] ||
        !info->sdk_abi_signature ||
        std::string(info->sdk_abi_signature) != omnidesk::sdkAbiSignature()) {
        std::cerr << "ABI metadata mismatch\n";
        dlclose(h);
        return 1;
    }
    void* inst = create_fn();
    if (!inst) {
        std::cerr << "create returned null\n";
        dlclose(h);
        return 1;
    }
    destroy_fn(inst);
    dlclose(h);
    return 0;
}
