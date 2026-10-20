#pragma once
#include <windows.h>
#include <string>
#include <utility>
#pragma comment(lib,"user32.lib")
#pragma comment(lib,"gdi32.lib")

// Owned and pumped by the mod update thread; never activates or captures input.
class FlightNotice {
    HWND window=nullptr; ULONGLONG until=0; bool on=false;
    std::wstring message;
    static LRESULT CALLBACK Proc(HWND w,UINT m,WPARAM p,LPARAM l) {
        if(m==WM_NCHITTEST)return HTTRANSPARENT;
        if(m==WM_MOUSEACTIVATE)return MA_NOACTIVATE;
        if(m==WM_ERASEBKGND)return 1;
        if(m==WM_PAINT) {
            auto self=reinterpret_cast<FlightNotice*>(GetWindowLongPtrW(w,GWLP_USERDATA));
            PAINTSTRUCT ps;auto dc=BeginPaint(w,&ps);RECT r;GetClientRect(w,&r);
            SetDCBrushColor(dc,RGB(24,27,30));FillRect(dc,&r,(HBRUSH)GetStockObject(DC_BRUSH));
            SetDCBrushColor(dc,self && self->on ? RGB(112,196,142) : RGB(181,186,192));FrameRect(dc,&r,(HBRUSH)GetStockObject(DC_BRUSH));
            SetBkMode(dc,TRANSPARENT);SetTextColor(dc,RGB(239,242,244));
            auto font=CreateFontW(-MulDiv(16,GetDpiForWindow(w),96),0,0,0,FW_MEDIUM,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
            auto old=SelectObject(dc,font);DrawTextW(dc,self ? self->message.c_str() : L"",-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            SelectObject(dc,old);DeleteObject(font);EndPaint(w,&ps);return 0;
        }
        return DefWindowProcW(w,m,p,l);
    }
public:
    void dismiss(){until=0;if(window)ShowWindow(window,SW_HIDE);}
    void show_text(std::wstring text,bool enabled=false){message=std::move(text);on=enabled;until=GetTickCount64()+1500;}
    void show(bool enabled){show_text(enabled?L"Always Flying: On":L"Always Flying: Off",enabled);}
    void update(HWND game) {
        if(!game || IsIconic(game) || GetTickCount64()>=until){dismiss();return;}
        if(!window) {
            HMODULE module=nullptr;GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&Proc),&module);
            WNDCLASSW c{};c.lpfnWndProc=Proc;c.hInstance=module;c.lpszClassName=L"AeroxFlightToggleNotice";
            if(!RegisterClassW(&c) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
            window=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT,c.lpszClassName,L"Always Flying",WS_POPUP,0,0,240,38,nullptr,nullptr,module,nullptr);
            if(!window)return;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(this));
        }
        SetWindowTextW(window,message.c_str());
        RECT r{};POINT origin{};
        if(!GetClientRect(game,&r)||!ClientToScreen(game,&origin)||r.right<=0||r.bottom<=0){dismiss();return;}
        int dpi=GetDpiForWindow(game);if(!dpi)dpi=96;
        const int width=MulDiv(240,dpi,96),height=MulDiv(38,dpi,96);
        SetWindowPos(window,HWND_TOPMOST,origin.x+(r.right-width)/2,origin.y+r.bottom*2/3,width,height,SWP_NOACTIVATE|SWP_SHOWWINDOW);
        InvalidateRect(window,nullptr,FALSE);UpdateWindow(window);
        MSG msg{};while(PeekMessageW(&msg,window,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
    }
    void release(){if(window){auto module=reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window,GWLP_HINSTANCE));DestroyWindow(window);window=nullptr;UnregisterClassW(L"AeroxFlightToggleNotice",module);}until=0;}
};
