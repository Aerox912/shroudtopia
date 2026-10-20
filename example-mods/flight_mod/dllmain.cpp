#include "pch.h"
#include <shroudtopia.h>
#include "flight_patch.h"
#include "notice.h"

class FlightMod : public Mod {
    Flight::Patch patch;
    Flight::Toggle toggle;
    FlightNotice notice;
    bool prepared=false;
    int key=VK_F6;
public:
    ModMetaData GetMetaData() override {return {"Flight Mod","Toggle free glider flight. Starts off on activation.","1.1.0","s0T7x; maintained by Aerox912","0.1.1",true,false};}
    void Load(ModContext* c) override {
        if(!c->game.isClient || prepared)return;
        prepared=patch.prepare(Flight::FindTarget(reinterpret_cast<const std::uint8_t*>(GetModuleHandleW(nullptr))));
        c->Log(prepared ? "[Flight] verified client 1076226; press F6 (or configured key) to toggle" : "[Flight] unsupported layout; vanilla flight retained");
    }
    void Activate(ModContext* c) override {
        if(active||!prepared)return;
        active=patch.install();toggle={};patch.enable(false);
        if(!active)c->Log("[Flight] patch installation failed; toggle unavailable");
    }
    void Deactivate(ModContext*) override {patch.enable(false);toggle={};notice.dismiss();active=false;}
    void Unload(ModContext*) override {notice.release();patch.release();prepared=false;active=false;loaded=false;}
    void Update(ModContext* c) override {
        if(!active)return;
        const int next=c->config.GetInt ? c->config.GetInt("Flight Mod","toggle_key",VK_F6) : VK_F6;
        if(next!=key){key=next;toggle.armed=false;}
        auto game=GetForegroundWindow();DWORD pid=0;GetWindowThreadProcessId(game,&pid);
        CURSORINFO cursor{sizeof(CURSORINFO)};
        const bool focused=pid==GetCurrentProcessId() && (!GetCursorInfo(&cursor)||(cursor.flags&CURSOR_SHOWING)==0);
        const bool down=key>0&&key<255&&(GetAsyncKeyState(key)&0x8000)!=0;
        if(toggle.sample(down,focused)) {patch.enable(toggle.on);notice.show(toggle.on);c->Log(toggle.on ? "[Flight] on" : "[Flight] off");}
        notice.update(focused?game:nullptr);
    }
};
extern "C" __declspec(dllexport) Mod* CreateModInstance(){return new FlightMod();}
BOOL APIENTRY DllMain(HMODULE,DWORD,LPVOID){return TRUE;}
