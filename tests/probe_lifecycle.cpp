// SPDX-License-Identifier: MIT
// Exercise the probe's real shutdown with callable code in a separately loaded DSO.
#define main pixel_bc_probe_main
#include "probe.cpp"
#undef main
#include <fstream>
#include <iterator>
namespace {
const char *log_path;
void verify_exit() {
    std::ifstream log(log_path); std::string events((std::istreambuf_iterator<char>(log)),{});
    const std::string expected="load\nwait\npool\ndevice\ninstance\nhal-close\nunload\nload\ninstance\nhal-close\nunload\nload\nhal-close\nunload\nload\ndevice\nhal-close\nunload\nload\nwait\ndevice\nhal-close\nunload\n";
    if(events!=expected) { std::fprintf(stderr,"Lifecycle callback order/count failure:\n%s",events.c_str()); std::abort(); }
    std::printf("LIFECYCLE: full/partial/repeated shutdown, wait-error and missing-HAL-close reporting, natural exit passed\n");
}
void open_fixture(App &a,const char *path) {
    a.library=dlopen(path,RTLD_NOW|RTLD_LOCAL); if(!a.library) throw std::runtime_error(dlerror());
    a.hal=reinterpret_cast<hw_device_t*(*)()>(dlsym(a.library,"fixture_hal"))();
    a.DestroyInstance=reinterpret_cast<PFN_vkDestroyInstance>(dlsym(a.library,"fixture_instance"));
    a.DestroyDevice=reinterpret_cast<PFN_vkDestroyDevice>(dlsym(a.library,"fixture_device"));
    a.DeviceWaitIdle=reinterpret_cast<PFN_vkDeviceWaitIdle>(dlsym(a.library,"fixture_wait"));
    a.DestroyCommandPool=reinterpret_cast<PFN_vkDestroyCommandPool>(dlsym(a.library,"fixture_pool"));
}
}
int main(int argc,char **argv) {
    if(argc!=3) return 2;
    log_path=argv[2]; std::remove(log_path); setenv("PVB_LIFECYCLE_LOG",log_path,1); std::atexit(verify_exit);
    try {
        { App a; open_fixture(a,argv[1]); a.device=reinterpret_cast<VkDevice>(uintptr_t(1)); a.instance=reinterpret_cast<VkInstance>(uintptr_t(2)); a.pool=reinterpret_cast<VkCommandPool>(uintptr_t(3)); a.shutdown(); a.shutdown(); }
        try { App a; open_fixture(a,argv[1]); a.instance=reinterpret_cast<VkInstance>(uintptr_t(2)); throw std::runtime_error("Injected dispatch resolution failure after instance creation"); } catch(const std::runtime_error&) {}
        { App a; open_fixture(a,argv[1]); }
        { App a; open_fixture(a,argv[1]); a.device=reinterpret_cast<VkDevice>(uintptr_t(1)); a.DeviceWaitIdle=nullptr; }
        { App a; open_fixture(a,argv[1]); a.device=reinterpret_cast<VkDevice>(uintptr_t(1)); reinterpret_cast<void(*)()>(dlsym(a.library,"fixture_wait_error"))(); a.shutdown(); }
        if(failed!=1) return 1; // Failed wait must be counted despite completing cleanup.
        { hw_device_t missing_close{}; App a; a.hal=&missing_close; a.shutdown(); }
        if(failed!=2) return 1;
        failed=0; // Both explicitly injected failures were detected as expected.
    } catch(const std::exception &e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
    return failed?1:0;
}
