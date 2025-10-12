#define UNICODE
#include <windows.h>
#include <mmsystem.h>
#include <shellapi.h>
#pragma comment(lib, "winmm.lib")

#include <string>
#include <vector>
#include <fstream>
#include <iostream>
#include <unordered_map>
#include <algorithm>
#include <cctype>
#include <atomic>
#include <cmath>

#ifndef LLKHF_INJECTED
#define LLKHF_INJECTED 0x10
#endif
#ifndef LLMHF_INJECTED
#define LLMHF_INJECTED 0x00000001
#endif

#define WM_TRAYICON (WM_USER + 1)
#define ID_TRAY_REFRESH 1001
#define ID_TRAY_EDIT    1002
#define ID_TRAY_EXIT    1003
#define ID_TRAY_INI_BASE 2000
#define ID_TRAY_TOGGLE 1004

struct Macro {
    enum ActionType { ACTION_AUTOCLICK = 0, ACTION_BIND = 1 } action = ACTION_AUTOCLICK;
    bool clickHold = true;
    bool bindKeepOriginal = false;
    bool bindDropOriginal = false;
    bool keepOriginal = false;
    bool dropOriginal = false;
    DWORD triggerCfg = 0;
    DWORD targetCfg = 0;
    float originalIntervalMs = 0.0f;
    float effectiveIntervalMs = 0.0f;
    std::atomic_bool active{false};
    std::atomic_bool bindTargetDown{false};
    MMRESULT timerId{0};
    int clicksPerTick = 1;
};

static std::vector<Macro*> macros;
static std::unordered_map<std::string,int> keyMap;
static std::atomic_bool isPaused{false};
static bool key8Down = false;
static bool key9Down = false;
static bool key0Down = false;
static ULONGLONG lastPauseToggle = 0;
static const DWORD DEBOUNCE_MS = 500;
static std::string currentIniPath;
static std::vector<std::string> iniFiles;

// Tray globals
static NOTIFYICONDATA nid;
static HMENU hMenu;
static HWND hwndConsole;
static HWND hwndTray;
static bool consoleVisible = true;
static WNDPROC originalConsoleProc = NULL;

// -------- utilities --------
static inline std::string trim(const std::string &s){
    size_t a=0, b=s.size();
    while(a<b && std::isspace((unsigned char)s[a])) ++a;
    while(b>a && std::isspace((unsigned char)s[b-1])) --b;
    return s.substr(a,b-a);
}
static inline std::string toLowerStr(std::string s){
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

static std::string findFileNearby(const std::string &name){
    std::ifstream f(name);
    if(f.good()){ f.close(); return name; }
    TCHAR path[MAX_PATH];
    if(GetModuleFileName(NULL,path,MAX_PATH) > 0){
        std::wstring full(path);
        size_t p = full.find_last_of(L"\\/");
        if(p != std::wstring::npos){
            std::wstring folder = full.substr(0,p+1);
            // Check CFG folder first
            std::wstring cfgCand = folder + L"CFG\\" + std::wstring(name.begin(), name.end());
            std::ifstream f2;
            f2.open(cfgCand);
            if(f2.good()){ f2.close(); return std::string(cfgCand.begin(), cfgCand.end()); }
            // Fallback to executable directory
            std::wstring rootCand = folder + std::wstring(name.begin(), name.end());
            f2.open(rootCand);
            if(f2.good()){ f2.close(); return std::string(rootCand.begin(), rootCand.end()); }
        }
    }
    return "";
}

// -------- Find all .ini files in the CFG directory --------
static void FindIniFiles(std::vector<std::string> &outFiles){
    outFiles.clear();
    TCHAR path[MAX_PATH];
    if(GetModuleFileName(NULL, path, MAX_PATH) > 0){
        std::wstring full(path);
        size_t p = full.find_last_of(L"\\/");
        if(p != std::wstring::npos){
            std::wstring folder = full.substr(0, p+1);
            std::wstring searchPath = folder + L"CFG\\*.ini";
            WIN32_FIND_DATAW findData;
            HANDLE hFind = FindFirstFileW(searchPath.c_str(), &findData);
            if(hFind != INVALID_HANDLE_VALUE){
                do {
                    std::wstring fileName = findData.cFileName;
                    outFiles.push_back(std::string(fileName.begin(), fileName.end()));
                } while(FindNextFileW(hFind, &findData));
                FindClose(hFind);
            }
        }
    }
}

// -------- Save last used .ini file --------
static void SaveLastConfig(const std::string &iniFile){
    TCHAR path[MAX_PATH];
    if(GetModuleFileName(NULL, path, MAX_PATH) > 0){
        std::wstring full(path);
        size_t p = full.find_last_of(L"\\/");
        if(p != std::wstring::npos){
            std::wstring folder = full.substr(0, p+1);
            std::string lastConfigPath = std::string(folder.begin(), folder.end()) + "CFG\\last_config.txt";
            std::ofstream out(lastConfigPath);
            if(out.is_open()){
                out << iniFile;
                out.close();
            }
        }
    }
}

// -------- Load last used .ini file --------
static std::string LoadLastConfig(){
    TCHAR path[MAX_PATH];
    if(GetModuleFileName(NULL, path, MAX_PATH) > 0){
        std::wstring full(path);
        size_t p = full.find_last_of(L"\\/");
        if(p != std::wstring::npos){
            std::wstring folder = full.substr(0, p+1);
            std::string lastConfigPath = std::string(folder.begin(), folder.end()) + "CFG\\last_config.txt";
            std::ifstream in(lastConfigPath);
            if(in.is_open()){
                std::string lastIni;
                std::getline(in, lastIni);
                in.close();
                // Verify the file still exists
                std::string fullPath = findFileNearby(lastIni);
                if(!fullPath.empty()) return lastIni;
            }
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
        size_t pos = line.find_first_of("#;");
        std::string s = (pos==std::string::npos) ? line : line.substr(0,pos);
        s = trim(s);
        if(s.empty()) continue;
        size_t eq = s.find('=');
        if(eq==std::string::npos) continue;
        std::string name = trim(s.substr(0,eq));
        std::string rhs  = trim(s.substr(eq+1));
        if(name.empty() || rhs.empty()) continue;
        size_t sp = rhs.find_first_of(" \t");
        std::string valtok = (sp==std::string::npos) ? rhs : rhs.substr(0,sp);
        try{
            int code = std::stoi(valtok,nullptr,0);
            keyMap[toLowerStr(name)] = code;
        }catch(...){
            continue;
        }
    }
    return true;
}

// -------- Resolve name -> numeric code --------
static int ResolveKeyName(const std::string &name){
    std::string n = toLowerStr(trim(name));
    if(n.empty()) return -1;
    auto it = keyMap.find(n);
    if(it != keyMap.end()) return it->second;

    if(n=="lmb" || n=="mouse1") return 253;
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

// ---- Map config code to VK code ----
static int MapConfigCodeToDetectVK(int cfg){
    switch(cfg){
        case 253: return VK_LBUTTON;
        case 252: return VK_RBUTTON;
        case 4:   return VK_MBUTTON;
        case 5:   return VK_XBUTTON1;
        case 6:   return VK_XBUTTON2;
        case 254: return -1;
        case 255: return -1;
        default: return cfg;
    }
}

// ---- Sending helpers ----
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
    if(cfg == 254 || cfg == 255){ return; }

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
static bool ParseMacroLine(const std::string &line,
                           std::string &actionOut, std::string &modeOut,
                           bool &keepOut, bool &dropOut,
                           std::string &triggerOut, std::string &targetOut,
                           float &intervalOut)
{
    size_t i=0, n=line.size();
    auto skip=[&](){ while(i<n && std::isspace((unsigned char)line[i])) ++i; };
    skip();
    if(!(i<n && line[i]=='[')){
        return false;
    }
    ++i; size_t actionStart=i;
    while(i<n && line[i]!=']') ++i;
    if(i>=n){
        return false;
    }
    actionOut = trim(line.substr(actionStart, i-actionStart));
    ++i; skip();

    modeOut = "";
    bool isBind = (toLowerStr(actionOut) == "bind");
    if(!isBind && i<n && line[i]=='['){
        ++i; size_t modeStart=i;
        while(i<n && line[i]!=']') ++i;
        if(i>=n){
            return false;
        }
        modeOut = trim(line.substr(modeStart, i-modeStart));
        ++i; skip();
    }

    keepOut = false; dropOut = false;
    if(i<n && line[i]=='['){
        ++i; size_t flagStart=i;
        while(i<n && line[i]!=']') ++i;
        if(i>=n){
            return false;
        }
        std::string tag = trim(line.substr(flagStart, i-flagStart));
        std::string tagLower = toLowerStr(tag);
        if(tagLower == "k" || tagLower == "keep"){
            keepOut = true;
        } else if(tagLower == "d" || tagLower == "drop"){
            dropOut = true;
        }
        ++i; skip();
    }

    if(!(i<n && line[i]=='\"')){
        return false;
    }
    ++i; size_t triggerStart=i;
    while(i<n && line[i]!='\"') ++i;
    if(i>=n){
        return false;
    }
    triggerOut = trim(line.substr(triggerStart, i-triggerStart));
    ++i; skip();
    if(!(i<n && line[i]=='\"')){
        return false;
    }
    ++i; size_t targetStart=i;
    while(i<n && line[i]!='\"') ++i;
    if(i>=n){
        return false;
    }
    targetOut = trim(line.substr(targetStart, i-targetStart));
    ++i; skip();
    intervalOut = 0.0f;
    if(i<n && line[i]=='['){
        ++i; size_t intervalStart=i;
        while(i<n && line[i]!=']') ++i;
        if(i>=n){
            return false;
        }
        std::string istr = trim(line.substr(intervalStart,i-intervalStart));
        try {
            float v = std::stof(istr);
            if(v<=0){
                return false;
            }
            intervalOut = v;
        } catch(...) {
            return false;
        }
    }
    return true;
}

// ---- Function prototype for MacroTimerProc ----
void CALLBACK MacroTimerProc(UINT, UINT, DWORD_PTR, DWORD_PTR, DWORD_PTR);

// ---- Load macros file ----
static bool LoadMacrosFile(const std::string &path){
    std::ifstream in(path);
    if(!in.is_open()){
        return false;
    }
    std::string line;
    size_t lineno=0;
    std::vector<Macro*> newMacros;
    while(std::getline(in,line)){
        ++lineno;
        std::string s=line;
        size_t cpos = s.find_first_of("#;");
        if(cpos!=std::string::npos) s = s.substr(0,cpos);
        s = trim(s);
        if(s.empty()) continue;
        std::string action, mode, trgName, tgtName;
        bool keep=false, drop=false;
        float interval = 0.0f;
        if(!ParseMacroLine(s, action, mode, keep, drop, trgName, tgtName, interval)){
            continue;
        }
        std::string trgNameN = toLowerStr(trim(trgName));
        std::string tgtNameN = toLowerStr(trim(tgtName));

        int trg = ResolveKeyName(trgNameN);
        int tgt = ResolveKeyName(tgtNameN);
        if(trg == -1 || tgt == -1){
            continue;
        }
        Macro *m = new Macro();
        m->triggerCfg = static_cast<DWORD>(trg);
        m->targetCfg  = static_cast<DWORD>(tgt);
        m->clickHold = true;
        if(toLowerStr(action)=="autoclick"){
            m->keepOriginal = keep;
            m->dropOriginal = drop;
            m->bindKeepOriginal = keep;
            m->bindDropOriginal = drop;
            m->action = Macro::ACTION_AUTOCLICK;
            if(toLowerStr(mode)=="toggle") m->clickHold = false;
            if(interval==0){
                delete m;
                continue;
            }
            m->originalIntervalMs = interval;
            if(interval < 1.0f){
                m->clicksPerTick = static_cast<int>(std::ceil(1.0f / interval));
                m->effectiveIntervalMs = 1.0f;
            } else {
                m->clicksPerTick = 1;
                m->effectiveIntervalMs = interval;
            }
            m->active.store(false);
        } else if(toLowerStr(action)=="bind"){
            m->action = Macro::ACTION_BIND;
            m->keepOriginal = keep;
            m->dropOriginal = drop;
            m->bindKeepOriginal = keep;
            m->bindDropOriginal = drop;
            m->originalIntervalMs = 0.0f;
            m->effectiveIntervalMs = 0.0f;
            m->clicksPerTick = 0;
            m->active.store(false);
            m->bindTargetDown.store(false);
        } else {
            delete m;
            continue;
        }
        newMacros.push_back(m);
    }
    in.close();
    
    // Cleanup old macros
    for(auto m : macros){
        if(m->timerId) timeKillEvent(m->timerId);
        delete m;
    }
    macros.clear();
    
    // Assign new macros and set up timers
    macros = newMacros;
    for(auto m : macros){
        if(m->action == Macro::ACTION_AUTOCLICK && m->effectiveIntervalMs > 0){
            m->timerId = timeSetEvent(static_cast<UINT>(m->effectiveIntervalMs), 1, MacroTimerProc, (DWORD_PTR)m, TIME_PERIODIC | TIME_CALLBACK_FUNCTION);
        }
    }
    
    return true;
}

// ---- Timer callback ----
void CALLBACK MacroTimerProc(UINT, UINT, DWORD_PTR dwUser, DWORD_PTR, DWORD_PTR){
    Macro *m = reinterpret_cast<Macro*>(dwUser);
    if(!m) return;
    if(isPaused.load()) return;
    if(m->action != Macro::ACTION_AUTOCLICK) return;
    if(m->active.load()){
        for(int i = 0; i < m->clicksPerTick; ++i){
            SendClickByConfigCode((int)m->targetCfg);
        }
    }
}

// ---- Low-level hooks ----
static HHOOK gKeyboardHook = nullptr;
static HHOOK gMouseHook = nullptr;

static inline bool ShouldSuppressOriginal(const Macro* m){
    if(m->keepOriginal) return false;
    if(m->dropOriginal) return true;
    return true;
}

LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam){
    if(nCode < HC_ACTION) return CallNextHookEx(gKeyboardHook,nCode,wParam,lParam);
    auto info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
    if(!info) return CallNextHookEx(gKeyboardHook,nCode,wParam,lParam);
    if((info->flags & LLKHF_INJECTED) != 0) return CallNextHookEx(gKeyboardHook,nCode,wParam,lParam);

    bool isDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
    int vk = (int)info->vkCode;

    if(vk == '8') key8Down = isDown;
    if(vk == '9') key9Down = isDown;
    if(vk == '0') key0Down = isDown;

    if(key8Down && key9Down && key0Down){
        ULONGLONG now = GetTickCount64();
        if(now - lastPauseToggle > DEBOUNCE_MS){
            bool wasPaused = isPaused.load();
            isPaused.store(!wasPaused);
            lastPauseToggle = now;
            if(isPaused.load()){
                for(auto m : macros){
                    if(m->action == Macro::ACTION_AUTOCLICK){
                        m->active.store(false);
                    }
                }
                std::wcout << L"Macros paused\n";
            } else {
                std::wcout << L"Macros resumed\n";
            }
            return 1;
        }
    }

    if(isPaused.load()) return CallNextHookEx(gKeyboardHook,nCode,wParam,lParam);

    bool handled = false;
    for(auto m : macros){
        int detect = MapConfigCodeToDetectVK((int)m->triggerCfg);
        if(detect <= 0) continue;
        if(detect != vk) continue;

        bool suppress = ShouldSuppressOriginal(m);
        if(m->action == Macro::ACTION_AUTOCLICK){
            if(m->clickHold){
                m->active.store(isDown);
            } else {
                if(isDown){ m->active.store(!m->active.load()); }
            }
            handled = handled || suppress;
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
            handled = handled || suppress;
        }
    }

    if(handled) return 1;
    return CallNextHookEx(gKeyboardHook,nCode,wParam,lParam);
}

LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam){
    if(nCode < HC_ACTION) return CallNextHookEx(gMouseHook,nCode,wParam,lParam);
    auto info = reinterpret_cast<MSLLHOOKSTRUCT*>(lParam);
    if(!info) return CallNextHookEx(gMouseHook,nCode,wParam,lParam);
    if((info->flags & LLMHF_INJECTED) != 0) return CallNextHookEx(gMouseHook,nCode,wParam,lParam);

    if(isPaused.load()) return CallNextHookEx(gMouseHook,nCode,wParam,lParam);

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

        bool suppress = ShouldSuppressOriginal(m);
        if(m->action == Macro::ACTION_AUTOCLICK){
            if(m->clickHold){ m->active.store(isDown); }
            else { if(isDown) m->active.store(!m->active.load()); }
            handled = handled || suppress;
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
            handled = handled || suppress;
        }
    }

    if(handled) return 1;
    return CallNextHookEx(gMouseHook,nCode,wParam,lParam);
}

// ---- Tray functions ----
static void ShowConsole(bool show) {
    if (show) {
        ShowWindow(hwndConsole, SW_RESTORE);
        SetForegroundWindow(hwndConsole);
    } else {
        ShowWindow(hwndConsole, SW_MINIMIZE);
    }
    consoleVisible = show;
}

// ---- Build tray menu with dynamic .ini files submenu ----
static void BuildTrayMenu() {
    if(hMenu) DestroyMenu(hMenu);
    hMenu = CreatePopupMenu();
    
    // Add "Switch Config" submenu
    HMENU hSubMenu = CreatePopupMenu();
    FindIniFiles(iniFiles);
    for(size_t i = 0; i < iniFiles.size(); ++i) {
        std::wstring wFileName(iniFiles[i].begin(), iniFiles[i].end());
        AppendMenuW(hSubMenu, MF_STRING, ID_TRAY_INI_BASE + i, wFileName.c_str());
    }
    AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hSubMenu, L"\u0412\u044b\u0431\u0440\u0430\u0442\u044c \u043a\u043e\u043d\u0444\u0438\u0433");

    // Add Start/Stop toggle
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_TOGGLE, isPaused.load() ? L"\u0421\u0442\u0430\u0440\u0442" : L"\u0421\u0442\u043e\u043f");

    // Add other menu items
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_REFRESH, L"\u041e\u0431\u043d\u043e\u0432\u0438\u0442\u044c \u043c\u0430\u043a\u0440\u043e\u0441");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EDIT, L"\u0418\u0437\u043c\u0435\u043d\u0438\u0442\u044c \u043c\u0430\u043a\u0440\u043e\u0441");
    AppendMenuW(hMenu, MF_STRING, ID_TRAY_EXIT, L"\u0412\u044b\u0445\u043e\u0434");
}

LRESULT CALLBACK ConsoleWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_SYSCOMMAND:
            if ((wParam & 0xFFF0) == SC_MINIMIZE) {
                consoleVisible = false;
                return 0;
            }
            break;
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                consoleVisible = false;
            } else if (wParam == SIZE_RESTORED || wParam == SIZE_MAXIMIZED) {
                consoleVisible = true;
            }
            break;
        case WM_CLOSE:
            ShowConsole(false);
            return 0;
    }
    return CallWindowProc(originalConsoleProc, hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TRAYICON:
        if (lParam == WM_LBUTTONUP) {
            ShowConsole(!consoleVisible);
        } else if (lParam == WM_RBUTTONUP) {
            BuildTrayMenu();
            POINT pt;
            GetCursorPos(&pt);
            SetForegroundWindow(hwnd);
            TrackPopupMenu(hMenu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
        }
        break;

    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_TOGGLE:
            {
                bool wasPaused = isPaused.load();
                isPaused.store(!wasPaused);
                if(isPaused.load()) {
                    for(auto m : macros){
                        if(m->action == Macro::ACTION_AUTOCLICK){
                            m->active.store(false);
                        }
                    }
                    std::wcout << L"Macros paused\n";
                } else {
                    std::wcout << L"Macros resumed\n";
                }
            }
            break;
        case ID_TRAY_REFRESH:
            if(!currentIniPath.empty()) {
                system("cls"); // Clear console
                if(LoadMacrosFile(currentIniPath)) {
                    SaveLastConfig(currentIniPath.substr(currentIniPath.find_last_of("\\/") + 1));
                    std::wcout << L"Macros reloaded: " << macros.size() << L"\n";
                    for (size_t i = 0; i < macros.size(); ++i) {
                        auto m = macros[i];
                        std::string mode = m->action == Macro::ACTION_AUTOCLICK
                            ? (m->clickHold ? "HOLD" : "TOGGLE")
                            : (m->keepOriginal ? "K" : (m->dropOriginal ? "D" : "Default"));
                        std::wcout << L"#" << (i + 1)
                                  << L": action=" << (m->action == Macro::ACTION_AUTOCLICK ? L"AutoClick" : L"Bind")
                                  << L" mode=" << std::wstring(mode.begin(), mode.end())
                                  << L" triggerCfg=" << m->triggerCfg
                                  << L" targetCfg=" << m->targetCfg
                                  << L" intervalMs=" << m->originalIntervalMs;
                        if(m->action == Macro::ACTION_AUTOCLICK && m->originalIntervalMs > 0){
                            int cps = static_cast<int>(std::round(1000.0f / m->originalIntervalMs));
                            std::wcout << L"(-" << cps << L"CPS)";
                        }
                        std::wcout << L"\n";
                    }
                } else {
                    std::wcout << L"Failed to reload macros from " << std::wstring(currentIniPath.begin(), currentIniPath.end()) << L"\n";
                }
            }
            break;
        case ID_TRAY_EDIT:
            if(!currentIniPath.empty()) {
                ShellExecuteA(NULL, "open", currentIniPath.c_str(), NULL, NULL, SW_SHOWNORMAL);
            }
            break;
        case ID_TRAY_EXIT:
            PostQuitMessage(0);
            break;
        default:
            // Handle .ini file selection
            if(LOWORD(wParam) >= ID_TRAY_INI_BASE && LOWORD(wParam) < ID_TRAY_INI_BASE + iniFiles.size()) {
                size_t index = LOWORD(wParam) - ID_TRAY_INI_BASE;
                if(index < iniFiles.size()) {
                    TCHAR path[MAX_PATH];
                    if(GetModuleFileName(NULL, path, MAX_PATH) > 0) {
                        std::wstring full(path);
                        size_t p = full.find_last_of(L"\\/");
                        if(p != std::wstring::npos) {
                            std::wstring folder = full.substr(0, p+1);
                            currentIniPath = std::string(folder.begin(), folder.end()) + "CFG\\" + iniFiles[index];
                            system("cls"); // Clear console
                            if(LoadMacrosFile(currentIniPath)) {
                                SaveLastConfig(iniFiles[index]);
                                std::wcout << L"Switched to config: " << std::wstring(iniFiles[index].begin(), iniFiles[index].end()) << L"\n";
                                std::wcout << L"Macros loaded: " << macros.size() << L"\n";
                                for (size_t i = 0; i < macros.size(); ++i) {
                                    auto m = macros[i];
                                    std::string mode = m->action == Macro::ACTION_AUTOCLICK
                                        ? (m->clickHold ? "HOLD" : "TOGGLE")
                                        : (m->keepOriginal ? "K" : (m->dropOriginal ? "D" : "Default"));
                                    std::wcout << L"#" << (i + 1)
                                              << L": action=" << (m->action == Macro::ACTION_AUTOCLICK ? L"AutoClick" : L"Bind")
                                              << L" mode=" << std::wstring(mode.begin(), mode.end())
                                              << L" triggerCfg=" << m->triggerCfg
                                              << L" targetCfg=" << m->targetCfg
                                              << L" intervalMs=" << m->originalIntervalMs;
                                    if(m->action == Macro::ACTION_AUTOCLICK && m->originalIntervalMs > 0){
                                        int cps = static_cast<int>(std::round(1000.0f / m->originalIntervalMs));
                                        std::wcout << L"(-" << cps << L"CPS)";
                                    }
                                    std::wcout << L"\n";
                                }
                            } else {
                                std::wcout << L"Failed to load config: " << std::wstring(iniFiles[index].begin(), iniFiles[index].end()) << L"\n";
                            }
                        }
                    }
                }
            }
            break;
        }
        break;

    case WM_DESTROY:
        PostQuitMessage(0);
        break;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ---- Cleanup helper ----
static void CleanupAll(){
    for(auto m : macros){ if(m->timerId) timeKillEvent(m->timerId); }
    if(gKeyboardHook) { UnhookWindowsHookEx(gKeyboardHook); gKeyboardHook = nullptr; }
    if(gMouseHook)    { UnhookWindowsHookEx(gMouseHook);    gMouseHook = nullptr; }
    timeEndPeriod(1);
    for(auto m : macros) delete m;
    macros.clear();
    Shell_NotifyIcon(NIM_DELETE, &nid);
    DestroyMenu(hMenu);
    if (hwndTray) DestroyWindow(hwndTray);
    if (originalConsoleProc) SetWindowLongPtr(hwndConsole, GWLP_WNDPROC, (LONG_PTR)originalConsoleProc);
}

int main(int argc, char** argv){
    AllocConsole();
    hwndConsole = GetConsoleWindow();
    std::wcout << L"UniMacro engine starting...\n";
    std::wcout << L"8+9+0 to pause/resume\n";

    std::string keymapName = "KeyMapping.cfg";
    std::string keymapPath = findFileNearby(keymapName);
    if(!keymapPath.empty()){
        if(LoadKeyMap(keymapPath)){
            std::wcout << L"Loaded KeyMapping from: " << std::wstring(keymapPath.begin(), keymapPath.end()) << L"\n";
        }
    }

    // Try to load last used config
    std::string iniName = LoadLastConfig();
    if(iniName.empty() && argc >= 2) {
        iniName = argv[1]; // Use command-line argument if provided
    }

    // Find all .ini files
    FindIniFiles(iniFiles);
    if(!iniFiles.empty()) {
        std::wcout << L"Available config files: " << iniFiles.size() << L"\n";
        for(const auto& file : iniFiles) {
            std::wcout << L" - " << std::wstring(file.begin(), file.end()) << L"\n";
        }
    } else {
        std::wcout << L"No .ini files found in the CFG directory.\n";
    }

    // If no specific iniName provided or last config doesn't exist, use first available .ini
    if(iniName.empty() && !iniFiles.empty()) {
        iniName = iniFiles[0];
    }

    // Load the selected or first .ini file
    if(!iniName.empty()) {
        TCHAR path[MAX_PATH];
        if(GetModuleFileName(NULL, path, MAX_PATH) > 0) {
            std::wstring full(path);
            size_t p = full.find_last_of(L"\\/");
            if(p != std::wstring::npos) {
                std::wstring folder = full.substr(0, p+1);
                currentIniPath = std::string(folder.begin(), folder.end()) + "CFG\\" + iniName;
                if(LoadMacrosFile(currentIniPath)) {
                    SaveLastConfig(iniName);
                    std::wcout << L"Macros loaded from " << std::wstring(currentIniPath.begin(), currentIniPath.end()) << L": " << macros.size() << L"\n";
                    for (size_t i = 0; i < macros.size(); ++i) {
                        auto m = macros[i];
                        std::string mode = m->action == Macro::ACTION_AUTOCLICK
                            ? (m->clickHold ? "HOLD" : "TOGGLE")
                            : (m->keepOriginal ? "K" : (m->dropOriginal ? "D" : "Default"));
                        std::wcout << L"#" << (i + 1)
                                  << L": action=" << (m->action == Macro::ACTION_AUTOCLICK ? L"AutoClick" : L"Bind")
                                  << L" mode=" << std::wstring(mode.begin(), mode.end())
                                  << L" triggerCfg=" << m->triggerCfg
                                  << L" targetCfg=" << m->targetCfg
                                  << L" intervalMs=" << m->originalIntervalMs;
                        if(m->action == Macro::ACTION_AUTOCLICK && m->originalIntervalMs > 0){
                            int cps = static_cast<int>(std::round(1000.0f / m->originalIntervalMs));
                            std::wcout << L"(-" << cps << L"CPS)";
                        }
                        std::wcout << L"\n";
                    }
                } else {
                    std::wcout << L"Failed to load config: " << std::wstring(iniName.begin(), iniName.end()) << L"\n";
                }
            }
        }
    } else {
        std::wcout << L"No initial .ini file loaded. Use tray menu to select a config.\n";
    }

    HINSTANCE hInstance = GetModuleHandle(NULL);
    WNDCLASS wc = { 0 };
    wc.lpfnWndProc = TrayWndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = L"TrayAppClass";
    RegisterClass(&wc);
    hwndTray = CreateWindow(L"TrayAppClass", L"", 0, 0, 0, 0, 0, NULL, NULL, hInstance, NULL);

    HICON hIcon = LoadIcon(hInstance, MAKEINTRESOURCE(1));
    if (!hIcon)
        hIcon = LoadIcon(NULL, IDI_APPLICATION);

    SendMessage(hwndConsole, WM_SETICON, ICON_BIG, (LPARAM)hIcon);
    SendMessage(hwndConsole, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
    SetClassLongPtr(hwndConsole, GCLP_HICON, (LONG_PTR)hIcon);
    SetClassLongPtr(hwndConsole, GCLP_HICONSM, (LONG_PTR)hIcon);

    nid = { 0 };
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwndTray;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;
    nid.hIcon = hIcon;
    wcscpy_s(nid.szTip, sizeof(nid.szTip)/sizeof(wchar_t), L"UniMacro Console");
    Shell_NotifyIcon(NIM_ADD, &nid);

    // Initial menu creation
    BuildTrayMenu();

    ShowConsole(true);

    originalConsoleProc = (WNDPROC)GetWindowLongPtr(hwndConsole, GWLP_WNDPROC);
    SetWindowLongPtr(hwndConsole, GWLP_WNDPROC, (LONG_PTR)ConsoleWndProc);

    gKeyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, nullptr, 0);
    gMouseHook = SetWindowsHookExW(WH_MOUSE_LL, LowLevelMouseProc, nullptr, 0);

    timeBeginPeriod(1);
    for(auto m : macros){
        if(m->action == Macro::ACTION_AUTOCLICK){
            if(m->effectiveIntervalMs > 0){
                m->timerId = timeSetEvent(static_cast<UINT>(m->effectiveIntervalMs), 1, MacroTimerProc, (DWORD_PTR)m, TIME_PERIODIC | TIME_CALLBACK_FUNCTION);
            }
        }
    }

    MSG msg;
    while(GetMessageW(&msg, nullptr, 0, 0)){
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    CleanupAll();
    return 0;
}
