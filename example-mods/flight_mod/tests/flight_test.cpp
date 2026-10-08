#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <thread>
#include <atomic>
#include "../flight_patch.h"

static int checks=0;
static void Check(bool ok,const char* message) {
    ++checks;if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);std::exit(1);}
}
int main() {
    Flight::Toggle toggle;
    Check(!toggle.sample(true,true) && !toggle.on,"held key at activation does not toggle");
    Check(!toggle.sample(false,true) && toggle.sample(true,true) && toggle.on,"released then pressed enables flight");
    Check(!toggle.sample(true,true) && toggle.on,"held key does not repeat");
    Check(!toggle.sample(true,false) && toggle.on,"focus loss keeps selected state");
    Check(!toggle.sample(true,true) && toggle.on,"refocus while held does not toggle");
    toggle.sample(false,true);
    Check(toggle.sample(true,true) && !toggle.on,"next press disables flight");
    std::vector<std::uint8_t> image(0x2DA7000);
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(image.data());dos->e_magic=IMAGE_DOS_SIGNATURE;dos->e_lfanew=0x80;
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(image.data()+0x80);
    nt->Signature=IMAGE_NT_SIGNATURE;nt->FileHeader.Machine=IMAGE_FILE_MACHINE_AMD64;
    nt->OptionalHeader.Magic=IMAGE_NT_OPTIONAL_HDR64_MAGIC;nt->FileHeader.TimeDateStamp=1782724296;nt->OptionalHeader.SizeOfImage=0x2DA7000;
    const std::uint8_t expected[]={0xF3,0x0F,0x10,0x05,0x47,0x53,0x1D,0x01,0xF2,0x0F,0x11,0x4C,0x24,0x40,0x77,0x15};
    const float original=0.0872664600610733f;
    std::memcpy(image.data()+Flight::Site,expected,sizeof(expected));std::memcpy(image.data()+Flight::Constant,&original,4);
    Check(Flight::FindTarget(image.data())==reinterpret_cast<uintptr_t>(image.data()+Flight::Site),"verified layout accepted");
    nt->FileHeader.TimeDateStamp++;Check(!Flight::FindTarget(image.data()),"unknown game build rejected");nt->FileHeader.TimeDateStamp--;
    for(std::size_t i=0;i<sizeof(expected);++i){image[Flight::Site+i]^=1;Check(!Flight::FindTarget(image.data()),"changed instruction rejected");image[Flight::Site+i]^=1;}
    image[Flight::Constant]^=1;Check(!Flight::FindTarget(image.data()),"changed original constant rejected");
    Check(!Flight::FindTarget(nullptr),"missing image rejected");
    Flight::Patch empty;Check(!empty.prepare(0) && !empty.install(),"missing target cannot activate");empty.enable(true);empty.release();
    auto fn=static_cast<std::uint8_t*>(VirtualAlloc(nullptr,4096,MEM_RESERVE|MEM_COMMIT,PAGE_EXECUTE_READWRITE));
    Check(fn!=nullptr,"allocate synthetic function");
    std::uint8_t code[]={0xF3,0x0F,0x10,0x05,8,0,0,0,0xC3};
    std::memcpy(fn,code,sizeof(code));std::memcpy(fn+16,&original,4);
    using Function=float(*)();auto call=reinterpret_cast<Function>(fn);
    Check(call()==original,"synthetic original instruction executes");
    Flight::Patch patch;Check(patch.prepare(reinterpret_cast<uintptr_t>(fn)) && patch.install(),"production detour installed");
    Check(call()==original,"starts with vanilla glide");
    std::uint8_t installed[8];std::memcpy(installed,fn,8);
    patch.enable(true);Check(call()==-1.57f,"on executes free-flight constant");
    patch.enable(false);Check(call()==original,"off executes original constant");
    std::atomic<bool> bad{false};
    std::thread reader([&]{for(int i=0;i<100000;++i){auto v=call();if(v!=original && v!=-1.57f)bad=true;}});
    for(int i=0;i<20000;++i)patch.enable(i%2==0);
    reader.join();Check(!bad,"concurrent changes only expose complete values");
    Check(!std::memcmp(fn,installed,8),"toggles never rewrite executable instructions");
    patch.release();Check(!std::memcmp(fn,code,8) && call()==original,"unload restores original instructions");
    patch.release();VirtualFree(fn,0,MEM_RELEASE);
    std::printf("%d flight checks passed\n",checks);return 0;
}
