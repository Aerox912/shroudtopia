#include "view_mode.h"
#include <iostream>
#include <cstdlib>
void check(bool ok,const char* name){if(!ok){std::cerr<<"FAIL "<<name<<'\n';std::exit(1);}std::cout<<"PASS "<<name<<'\n';}
int main(){
 FirstPerson::ViewMode mode;
 bool camera=false,gauge=false;unsigned writes=0;
 auto apply=[&](bool value){camera=value;gauge=value;++writes;};
 mode.Consume(1,apply);check(!camera&&!gauge&&writes==0,"inactive input ignored");
 mode.Activate(apply);check(mode.HostActive()&&camera&&gauge,"activation starts first person and gauge together");
 mode.Consume(1,apply);check(mode.HostActive()&&!camera&&!gauge,"HOME third person retains host activation");
 auto before=writes;mode.Activate(apply);check(!camera&&!gauge&&writes==before,"repeated host activation preserves selected third person");
 mode.Consume(2,apply);check(!camera&&!gauge&&writes==before,"two rapid accepted presses preserve parity");
 mode.Consume(3,apply);check(camera&&gauge,"odd rapid presses switch once to selected mode");
 mode.Deactivate(apply);check(!mode.HostActive()&&!camera&&!gauge,"host disable clears both controls");
 mode.Consume(1,apply);check(!camera&&!gauge,"queued input after disable ignored");
 mode.Activate(apply);check(camera&&gauge,"host reenable resets initial view");
 mode.RetireInput(apply);check(mode.HostActive()&&!camera&&!gauge,"retired window fails to native without host churn");
 before=writes;mode.Activate(apply);mode.Consume(1,apply);check(writes==before&&!camera&&!gauge,"retired input never reopens from host activation");
 mode.Deactivate(apply);mode.Activate(apply);check(!camera&&!gauge,"retirement persists until process restart");
}
