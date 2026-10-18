#pragma once
namespace FirstPerson {
// Serialized by the owning mod lifecycle lock. A selected view never disables
// the host, otherwise a host that repeatedly activates enabled mods resets HOME.
class ViewMode {
 bool host_=false,first_=false,retired_=false;
public:
 bool HostActive() const noexcept{return host_;}
 template<class Apply> void Activate(Apply apply){
  if(host_)return;
  host_=true;first_=!retired_;apply(first_);
 }
 template<class Apply> void Consume(unsigned count,Apply apply){
  if(!host_||retired_||!(count&1u))return;
  first_=!first_;apply(first_);
 }
 template<class Apply> void Deactivate(Apply apply){host_=false;first_=false;apply(false);}
 template<class Apply> void RetireInput(Apply apply){retired_=true;first_=false;apply(false);}
};
}
