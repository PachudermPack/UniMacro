// UniMacro - main.cpp
// Fixed: consistent K/D flags (keepOriginal/dropOriginal), apply to AutoClick and Bind.
// Compile on Windows with MSVC or MinGW (link winmm.lib).

#include <windows.h>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <atomic>

#ifndef LLKHF_INJECTED
#define LLKHF_INJECTED 0x10
#endif
#ifndef LLMHF_INJECTED
#define LLMHF_INJECTED 0x00000001
#endif

struct Macro {
    enum ActionType { ACTION_AUTOCLICK = 0, ACTION_BIND = 1 } action = ACTION_AUTOCLICK;
    // autoclick: true = HOLD, false = TOGGLE
    bool clickHold = true;

    // old-style fields (kept for compatibility)
    bool bindKeepOriginal = false; // K (legacy)
    bool bindDropOriginal = false; // D (legacy)

    // unified K/D flags used by logic
    bool keepOriginal = false; // K
    bool dropOriginal = false; // D

    DWORD triggerCfg = 0;   // config code for trigger (from KeyMapping.cfg or literal)
    DWORD targetCfg = 0;    // config code to send when triggered
    DWORD intervalMs = 0;   // autoclick interval

    std::atomic_bool active{false};         // autoclick currently active (HOLD or TOGGLE)
    std::atomic_bool bindTargetDown{false}; // state for bind target (to send matching up)
    MMRESULT timerId{0};
};

static std::vector<Macro*> macros;
static std::unordered_map<std::string,int> keyMap; // name(lower)->code

// -------- utilities --------
static inline std::string trim(const std::string &s){
    size_t a=0, b=s.size();
    while(a<b && std::isspace((unsigned char)s[a])) ++a;
    while(b>a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a,b-a);
}
static inline std::string toLowerStr(std::string s){
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return s;
}

// find a file in current working directory or next to the exe
static std::string findFileNearby(const std::string &name){
    std::ifstream f(name);
    if(f.good()){ f.close(); return name; }
    char path[MAX_PATH];
    if(GetModuleFileNameA(NULL,path,MAX_PATH) > 0){
        std::string full(path);
        size_t p = full.find_last_of("\\/");
        if(p != std::string::npos){
            std::string folder = full.substr(0,p+1);
            std::string cand = folder + name;
            std::ifstream f2(cand);
            if(f2.good()){ f2.close(); return cand; }
        }
    }
    return "";
}

// -------- KeyMapping.cfg loader --------
static bool LoadKeyMap(const std::string &path){
    std::ifstream in(path);
    if(!in.is_open()) return false;
    std::string line;
    size_t lineno = 0;
    while(std::getline(in,line)){
        ++lineno;
        // strip comments starting with # or ;
        size_t pos = line.find_first_of("#;");
        std::string s = (pos==std::string::npos) ? line : line.substr(0,pos);
        s = trim(s);
        if(s.empty()) continue;
        size_t eq = s.find('=');
        if(eq==std::string::npos) continue;
        std::string name = trim(s.substr(0,eq));
        std::string rhs  = trim(s.substr(eq+1));
        if(name.empty() || rhs.empty()) continue;
        // take first token from rhs (in case there are trailing comments without '#')
        size_t sp = rhs.find_first_of(" \t");
        std::string valtok = (sp==std::string::npos) ? rhs : rhs.substr(0,sp);
        try{
            int code = std::stoi(valtok,nullptr,0); // base 0 supports 0x..
            keyMap[toLowerStr(name)] = code;
        }catch(...){
            std::cerr << "KeyMap: cannot parse code on line " << lineno << ": " << line << "\n";
            continue;
        }
    }
    return true;
}

// -------- Resolve name -> numeric code (KeyMap first, then built-ins) --------
static int ResolveKeyName(const std::string &name){
    std::string n = toLowerStr(trim(name));
    if(n.empty()) return -1;
    auto it = keyMap.find(n);
    if(it != keyMap.end()) return it->second;

    // built-in shortcuts (fallbacks)
    if(n=="lmb" || n=="mouse1") return 253; // custom left-click code handled separately
    if(n=="rmb" || n=="mouse2") return 252;
    if(n=="mmb" || n=="mouse3" || n=="mouse_middle") return 4;
    if(n=="mouse4") return 5;
    if(n=="mouse5") return 6;
    if(n=="mousewheel_up") return 254;
    if(n=="mousewheel_down") return 255;

    if(n=="space") return VK_SPACE;
    if(n=="shift") return VK_SHIFT;
    if(n=="ctrl")  return VK_CONTROL;
    if(n=="alt")   return VK_MENU;
    if(n=="enter" || n=="return") return VK_RETURN;
    if(n=="esc" || n=="escape") return VK_ESCAPE;
    if(n=="tab") return VK_TAB;
    if(n=="backspace") return VK_BACK;
    if(n=="capslock") return VK_CAPITAL;

    // F1..F24 shorthand
    if(n.size()>=2 && n[0]=='f'){
        try{
            int fn = std::stoi(n.substr(1));
            if(fn >= 1 && fn <= 24) return VK_F1 + (fn-1);
        }catch(...){}
    }

    if(n.size()==1){
        char c = n[0];
        if(c>='a' && c<='z') return (int)std::toupper(c);
        if(c>='0' && c<='9') return (int)c;
    }

    try { return std::stoi(n,nullptr,0); } catch(...) {}
    return -1;
}

// ---- Map config code to VK code used for detection (mouse custom codes -> VKs) ----
static int MapConfigCodeToDetectVK(int cfg){
    switch(cfg){
        case 253: return VK_LBUTTON;
        case 252: return VK_RBUTTON;
        case 4:   return VK_MBUTTON;
        case 5:   return VK_XBUTTON1;
        case 6:   return VK_XBUTTON2;
        // wheel codes cannot be "held" reliably -> can't detect by VK code
        case 254: return -1;
        case 255: return -1;
        default: return cfg; // assume it's a VK code already
    }
}

// ---- Sending helpers (down/up/tap) ----
static void SendDownByConfigCode(int cfg){
    if(cfg == 253){ INPUT in = {}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; SendInput(1,&in,sizeof(INPUT)); return; }
    if(cfg == 252){ INPUT in = {}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_RIGHTDOWN; SendInput(1,&in,sizeof(INPUT)); return; }
    if(cfg == 4){ INPUT in = {}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_MIDDLEDOWN; SendInput(1,&in,sizeof(INPUT)); return; }
    if(cfg == 5 || cfg == 6){ WORD which = (cfg==5)?XBUTTON1:XBUTTON2; INPUT in = {}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_XDOWN; in.mi.mouseData = which; SendInput(1,&in,sizeof(INPUT)); return; }
    if(cfg == 254){ INPUT in = {}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_WHEEL; in.mi.mouseData = WHEEL_DELTA; SendInput(1,&in,sizeof(INPUT)); return; }
    if(cfg == 255){ INPUT in = {}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_WHEEL; in.mi.mouseData = -WHEEL_DELTA; SendInput(1,&in,sizeof(INPUT)); return; }

    INPUT in = {}; in.type = INPUT_KEYBOARD; in.ki.wVk = (WORD)cfg; in.ki.dwFlags = 0; SendInput(1,&in,sizeof(INPUT));
}
static void SendUpByConfigCode(int cfg){
    if(cfg == 253){ INPUT in = {}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_LEFTUP; SendInput(1,&in,sizeof(INPUT)); return; }
    if(cfg == 252){ INPUT in = {}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_RIGHTUP; SendInput(1,&in,sizeof(INPUT)); return; }
    if(cfg == 4){ INPUT in = {}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_MIDDLEUP; SendInput(1,&in,sizeof(INPUT)); return; }
    if(cfg == 5 || cfg == 6){ WORD which = (cfg==5)?XBUTTON1:XBUTTON2; INPUT in = {}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_XUP; in.mi.mouseData = which; SendInput(1,&in,sizeof(INPUT)); return; }
    if(cfg == 254 || cfg == 255){ /* wheel has no up/down */ return; }

    INPUT in = {}; in.type = INPUT_KEYBOARD; in.ki.wVk = (WORD)cfg; in.ki.dwFlags = KEYEVENTF_KEYUP; SendInput(1,&in,sizeof(INPUT));
}
static void SendClickByConfigCode(int cfg){
    if(cfg == 253){ INPUT in[2] = {}; in[0].type=INPUT_MOUSE; in[0].mi.dwFlags=MOUSEEVENTF_LEFTDOWN; in[1].type=INPUT_MOUSE; in[1].mi.dwFlags=MOUSEEVENTF_LEFTUP; SendInput(2,in,sizeof(INPUT)); return; }
    if(cfg == 252){ INPUT in[2] = {}; in[0].type=INPUT_MOUSE; in[0].mi.dwFlags=MOUSEEVENTF_RIGHTDOWN; in[1].type=INPUT_MOUSE; in[1].mi.dwFlags=MOUSEEVENTF_RIGHTUP; SendInput(2,in,sizeof(INPUT)); return; }
    if(cfg == 4){ INPUT in[2] = {}; in[0].type=INPUT_MOUSE; in[0].mi.dwFlags=MOUSEEVENTF_MIDDLEDOWN; in[1].type=INPUT_MOUSE; in[1].mi.dwFlags=MOUSEEVENTF_MIDDLEUP; SendInput(2,in,sizeof(INPUT)); return; }
    if(cfg == 5 || cfg == 6){ WORD which = (cfg==5)?XBUTTON1:XBUTTON2; INPUT in[2] = {}; in[0].type=INPUT_MOUSE; in[0].mi.dwFlags=MOUSEEVENTF_XDOWN; in[0].mi.mouseData=which; in[1].type=INPUT_MOUSE; in[1].mi.dwFlags=MOUSEEVENTF_XUP; in[1].mi.mouseData=which; SendInput(2,in,sizeof(INPUT)); return; }
    if(cfg == 254){ INPUT in = {}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_WHEEL; in.mi.mouseData = WHEEL_DELTA; SendInput(1,&in,sizeof(INPUT)); return; }
    if(cfg == 255){ INPUT in = {}; in.type = INPUT_MOUSE; in.mi.dwFlags = MOUSEEVENTF_WHEEL; in.mi.mouseData = -WHEEL_DELTA; SendInput(1,&in,sizeof(INPUT)); return; }

    INPUT down = {}; down.type = INPUT_KEYBOARD; down.ki.wVk = (WORD)cfg; INPUT up = down; up.ki.dwFlags = KEYEVENTF_KEYUP; SendInput(1,&down,sizeof(INPUT)); Sleep(1); SendInput(1,&up,sizeof(INPUT));
}

// ---- Parse a macro line ----
// Allowed forms:
//  [AutoClick] [HOLD|TOGGLE] [K|D] "trigger" "target" [interval_ms]
//  [Bind] [K|D] "trigger" "target"
static bool ParseMacroLine(const std::string &line,
                           std::string &actionOut, std::string &modeOut,
                           bool &keepOut, bool &dropOut,
                           std::string &triggerOut, std::string &targetOut,
                           DWORD &intervalOut)
{
    size_t i=0, n=line.size();
    auto skip=[&](){ while(i<n && std::isspace((unsigned char)line[i])) ++i; };
    skip();
    // [Action]
    if(!(i<n && line[i]=='[')) return false;
    ++i; size_t start=i;
    while(i<n && line[i]!=']') ++i;
    if(i>=n) return false;
    actionOut = trim(line.substr(start, i-start));
    ++i; skip();
    // [Mode] (optional)
    modeOut = "";
    if(i<n && line[i]=='['){
        ++i; start=i;
        while(i<n && line[i]!=']') ++i;
        if(i>=n) return false;
        modeOut = trim(line.substr(start, i-start));
        ++i; skip();
    }
    // optional [K] or [D]
    keepOut = false; dropOut = false;
    if(i<n && line[i]=='['){
        size_t prev=i;
        ++i; start=i;
        while(i<n && line[i]!=']') ++i;
        if(i>=n) return false;
        std::string tag = trim(line.substr(start, i-start));
        if(toLowerStr(tag)=="k" || toLowerStr(tag)=="keep") keepOut = true;
        if(toLowerStr(tag)=="d" || toLowerStr(tag)=="drop") dropOut = true;
        ++i; skip();
    }
    // "trigger"
    if(!(i<n && line[i]=='\"')) return false;
    ++i; start=i;
    while(i<n && line[i]!='\"') ++i;
    if(i>=n) return false;
    triggerOut = trim(line.substr(start, i-start));
    ++i; skip();
    // "target"
    if(!(i<n && line[i]=='\"')) return false;
    ++i; start=i;
    while(i<n && line[i]!='\"') ++i;
    if(i>=n) return false;
    targetOut = trim(line.substr(start, i-start));
    ++i; skip();
    // optional [interval]
    intervalOut = 0;
    if(i<n && line[i]=='['){
        ++i; start=i;
        while(i<n && line[i]!=']') ++i;
        if(i>=n) return false;
        std::string istr = trim(line.substr(start,i-start));
        ++i; skip();
        try {
            int v = std::stoi(istr,nullptr,0);
            if(v<=0) return false;
            intervalOut = static_cast<DWORD>(v);
        } catch(...) { return false; }
    }
    return true;
}

// ---- Load macros file ----
static bool LoadMacrosFile(const std::string &path){
    std::ifstream in(path);
    if(!in.is_open()) return false;
    std::string line;
    size_t lineno=0;
    while(std::getline(in,line)){
        ++lineno;
        std::string s=line;
        // strip comments starting with # or ;
        size_t cpos = s.find_first_of("#;");
        if(cpos!=std::string::npos) s = s.substr(0,cpos);
        s = trim(s);
        if(s.empty()) continue;
        std::string action, mode, trgName, tgtName;
        bool keep=false, drop=false;
        DWORD interval = 0;
        if(!ParseMacroLine(s, action, mode, keep, drop, trgName, tgtName, interval)){
            std::cerr << "Skipping invalid macro line " << lineno << ": " << s << "\n";
            continue;
        }
        // normalize names
        std::string trgNameN = toLowerStr(trim(trgName));
        std::string tgtNameN = toLowerStr(trim(tgtName));

        int trg = ResolveKeyName(trgNameN);
        int tgt = ResolveKeyName(tgtNameN);
        if(trg == -1){
            std::cerr << "Unknown trigger name '" << trgName << "' on line " << lineno << "\n";
            continue;
        }
        if(tgt == -1){
            std::cerr << "Unknown target name '" << tgtName << "' on line " << lineno << "\n";
            continue;
        }
        Macro *m = new Macro();
        m->triggerCfg = static_cast<DWORD>(trg);
        m->targetCfg  = static_cast<DWORD>(tgt);
        m->clickHold = true;
        if(toLowerStr(action)=="autoclick"){
            // apply K/D flags also to autoclick
            m->keepOriginal = keep;
            m->dropOriginal = drop;
            // keep legacy fields in sync
            m->bindKeepOriginal = keep;
            m->bindDropOriginal = drop;
            m->action = Macro::ACTION_AUTOCLICK;
            if(toLowerStr(mode)=="toggle") m->clickHold = false;
            if(interval==0){
                std::cerr << "AutoClick requires [interval] on line " << lineno << "\n";
                delete m;
                continue;
            }
            m->intervalMs = interval;
            m->active.store(false);
        } else if(toLowerStr(action)=="bind"){
            m->action = Macro::ACTION_BIND;
            m->keepOriginal = keep;
            m->dropOriginal = drop;
            m->bindKeepOriginal = keep;
            m->bindDropOriginal = drop;
            m->intervalMs = 0;
            m->active.store(false);
            m->bindTargetDown.store(false);
        } else {
            std::cerr << "Unknown action '"<<action<<"' on line "<<lineno<<"\n";
            delete m;
            continue;
        }
        macros.push_back(m);
    }
    return true;
}

// ---- Timer callback: dwUser is pointer to Macro ----
void CALLBACK MacroTimerProc(UINT, UINT, DWORD_PTR dwUser, DWORD_PTR, DWORD_PTR){
    Macro *m = reinterpret_cast<Macro*>(dwUser);
    if(!m) return;
    if(m->action != Macro::ACTION_AUTOCLICK) return;
    if(m->active.load()){
        SendClickByConfigCode((int)m->targetCfg);
    }
}

// ---- Low-level hooks: keyboard and mouse ----
static HHOOK gKeyboardHook = nullptr;
static HHOOK gMouseHook = nullptr;

// helper to decide whether to suppress original event based on macro flags
static inline bool ShouldSuppressOriginal(const Macro* m){
    // Respect explicit K/D flags for any macro
    if(m->keepOriginal) return false;
    if(m->dropOriginal) return true;
    // default: suppress original trigger to avoid duplicate behavior
    return true;
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam){
    if(nCode < HC_ACTION) return CallNextHookEx(gKeyboardHook,nCode,wParam,lParam);
    auto info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    if(!info) return CallNextHookEx(gKeyboardHook,nCode,wParam,lParam);
    // ignore injected events (we generated them via SendInput)
    if((info->flags & LLKHF_INJECTED) != 0) return CallNextHookEx(gKeyboardHook,nCode,wParam,lParam);

    bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    int vk = (int)info->vkCode;
    bool handled = false;

    for(auto m : macros){
        int detect = MapConfigCodeToDetectVK((int)m->triggerCfg);
        if(detect <= 0) continue;
        if(detect != vk) continue;

        if(m->action == Macro::ACTION_AUTOCLICK){
            if(m->clickHold){
                m->active.store(isDown);
            } else {
                // TOGGLE: toggle on key down only
                if(isDown){ m->active.store(!m->active.load()); }
            }
            handled = ShouldSuppressOriginal(m);
        } else if(m->action == Macro::ACTION_BIND){
            if(isDown){
                if(!m->bindTargetDown.load()){
                    SendDownByConfigCode((int)m->targetCfg);
                    m->bindTargetDown.store(true);
                }
            } else {
                if(m->bindTargetDown.load()){
                    SendUpByConfigCode((int)m->targetCfg);
                    m->bindTargetDown.store(false);
                }
            }
            handled = ShouldSuppressOriginal(m);
        }
        // if macro matched, we break because a trigger should not match multiple macros identically
        if(detect == vk) break;
    }

    if(handled) return 1; // suppress original
    return CallNextHookEx(gKeyboardHook,nCode,wParam,lParam);
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam){
    if(nCode < HC_ACTION) return CallNextHookEx(gMouseHook,nCode,wParam,lParam);
    auto info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    if(!info) return CallNextHookEx(gMouseHook,nCode,wParam,lParam);
    // ignore injected events
    if((info->flags & LLMHF_INJECTED) != 0) return CallNextHookEx(gMouseHook,nCode,wParam,lParam);

    bool isDown = false;
    int evVK = -1;
    switch(wParam){
        case WM_LBUTTONDOWN: isDown = true; evVK = VK_LBUTTON; break;
        case WM_LBUTTONUP:   isDown = false; evVK = VK_LBUTTON; break;
        case WM_RBUTTONDOWN: isDown = true; evVK = VK_RBUTTON; break;
        case WM_RBUTTONUP:   isDown = false; evVK = VK_RBUTTON; break;
        case WM_MBUTTONDOWN: isDown = true; evVK = VK_MBUTTON; break;
        case WM_MBUTTONUP:   isDown = false; evVK = VK_MBUTTON; break;
        case WM_XBUTTONDOWN:
        case WM_XBUTTONUP: {
            WORD hi = HIWORD(info->mouseData);
            if(hi == XBUTTON1) evVK = VK_XBUTTON1;
            else if(hi == XBUTTON2) evVK = VK_XBUTTON2;
            isDown = (wParam == WM_XBUTTONDOWN);
            break;
        }
        default:
            break;
    }

    if(evVK == -1) return CallNextHookEx(gMouseHook,nCode,wParam,lParam);

    bool handled = false;
    for(auto m : macros){
        int detect = MapConfigCodeToDetectVK((int)m->triggerCfg);
        if(detect <= 0) continue;
        if(detect != evVK) continue;

        if(m->action == Macro::ACTION_AUTOCLICK){
            if(m->clickHold){ m->active.store(isDown); }
            else { if(isDown) m->active.store(!m->active.load()); }
            handled = ShouldSuppressOriginal(m);
        } else if(m->action == Macro::ACTION_BIND){
            if(isDown){ if(!m->bindTargetDown.load()){ SendDownByConfigCode((int)m->targetCfg); m->bindTargetDown.store(true); } }
            else { if(m->bindTargetDown.load()){ SendUpByConfigCode((int)m->targetCfg); m->bindTargetDown.store(false); } }
            handled = ShouldSuppressOriginal(m);
        }
        if(detect == evVK) break;
    }

    if(handled) return 1;
    return CallNextHookEx(gMouseHook,nCode,wParam,lParam);
}

// ---- Cleanup helper ----
static void CleanupAll(){
    for(auto m : macros){ if(m->timerId) timeKillEvent(m->timerId); }
    if(gKeyboardHook) { UnhookWindowsHookEx(gKeyboardHook); gKeyboardHook = nullptr; }
    if(gMouseHook)    { UnhookWindowsHookEx(gMouseHook);    gMouseHook = nullptr; }
    timeEndPeriod(1);
    for(auto m : macros) delete m;
    macros.clear();
}

int main(int argc, char** argv){
    std::cout << "UniMacro engine starting...\n";

    std::string keymapName = "KeyMapping.cfg";
    std::string keymapPath = findFileNearby(keymapName);
    if(!keymapPath.empty()){
        if(LoadKeyMap(keymapPath)){
            std::cout << "Loaded KeyMapping from: " << keymapPath << "\n";
        } else {
            std::cout << "Failed to parse KeyMapping.cfg (will use built-in names)\n";
        }
    } else {
        std::cout << "KeyMapping.cfg not found; using built-in names.\n";
    }

    std::string iniName = "UniMacro.ini";
    if(argc >= 2) iniName = argv[1];
    std::string iniPath = findFileNearby(iniName);
    if(iniPath.empty()){
        std::cerr << "Cannot find " << iniName << " next to exe or in working dir.\n";
        return 1;
    }
    if(!LoadMacrosFile(iniPath)){
        std::cerr << "No macros loaded from " << iniPath << "\n";
    }

    std::cout << "Macros loaded: " << macros.size() << "\n";
    for (size_t i = 0; i < macros.size(); ++i) {
        auto m = macros[i];
        std::cout << "#" << (i + 1)
              << ": action=" << (m->action == Macro::ACTION_AUTOCLICK ? "AutoClick" : "Bind")
              << " triggerCfg=" << m->triggerCfg
              << " targetCfg=" << m->targetCfg
              << " intervalMs=" << m->intervalMs
              << " mode="
              << (m->action == Macro::ACTION_AUTOCLICK
                      ? (m->clickHold ? "HOLD" : "TOGGLE")
                      : (m->keepOriginal ? "K" : "D"))
              << "\n";
    }

    // install hooks (low-level)
    gKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, nullptr, 0);
    if(!gKeyboardHook){
        std::cerr << "Warning: failed to install keyboard hook\n";
    }
    gMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, nullptr, 0);
    if(!gMouseHook){
        std::cerr << "Warning: failed to install mouse hook\n";
    }

    //  start timers for autoclick macros
    timeBeginPeriod(1);
    for(auto m : macros){
        if(m->action == Macro::ACTION_AUTOCLICK){
            if(m->intervalMs > 0){
                m->timerId = timeSetEvent(m->intervalMs, 1, MacroTimerProc, (DWORD_PTR)m, TIME_PERIODIC | TIME_CALLBACK_FUNCTION);
                if(!m->timerId) std::cerr << "Warning: timeSetEvent failed for interval " << m->intervalMs << "\n";
            }
        }
    }

    std::cout << "Macro engine running. Close console window to stop.\n";

    // message loop to keep hooks alive
    MSG msg;
    while(GetMessageW(&msg, nullptr, 0, 0)){
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CleanupAll();
    return 0;
}
