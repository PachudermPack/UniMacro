#include <windows.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <unordered_map>
#include <regex>
#include <thread>
#include <atomic>
#include <algorithm>

struct Macro {
    enum Type { AutoClick, Bind } type;
    enum Mode { Hold, Toggle } mode = Hold;
    bool keepOriginal = false; // [K] если true, [D] если false
    DWORD triggerKey;
    DWORD targetKey;
    int intervalMs = 10;
    std::atomic<bool> active{ false };
    std::string triggerName;
    std::string targetName;
};

std::unordered_map<std::string, DWORD> keymap;

void toLower(std::string &s) {
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
}

void LoadKeyMapping(const std::string& filename) {
    keymap.clear();
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Could not open " << filename << "\n";
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        size_t hashPos = line.find('#');
        if (hashPos != std::string::npos) line = line.substr(0, hashPos);

        // trim
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        if (line.empty()) continue;
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        size_t eqPos = line.find('=');
        if (eqPos == std::string::npos) continue;

        std::string key = line.substr(0, eqPos);
        std::string val = line.substr(eqPos + 1);

        // trim key/val
        key.erase(0, key.find_first_not_of(" \t\r\n"));
        key.erase(key.find_last_not_of(" \t\r\n") + 1);
        val.erase(0, val.find_first_not_of(" \t\r\n"));
        val.erase(val.find_last_not_of(" \t\r\n") + 1);

        toLower(key);
        try {
            DWORD code = (DWORD)std::stoul(val);
            keymap[key] = code;
        } catch (...) {
            // ignore
        }
    }
}

WORD getKeyCode(const std::string &name) {
    std::string lower = name;
    toLower(lower);
    if (keymap.count(lower)) return (WORD)keymap[lower];
    return 0;
}

void AutoClickThread(Macro &m) {
    while (true) {
        if (m.mode == Macro::Hold) {
            if (GetAsyncKeyState(m.triggerKey) & 0x8000) {
                if (!m.keepOriginal) keybd_event(m.triggerKey, 0, KEYEVENTF_KEYUP, 0);
                keybd_event(m.targetKey, 0, 0, 0);
                keybd_event(m.targetKey, 0, KEYEVENTF_KEYUP, 0);
                Sleep(m.intervalMs);
            } else {
                Sleep(1);
            }
        } else { // Toggle
            if (GetAsyncKeyState(m.triggerKey) & 1) { // pressed
                m.active = !m.active;
            }
            if (m.active) {
                if (!m.keepOriginal) keybd_event(m.triggerKey, 0, KEYEVENTF_KEYUP, 0);
                keybd_event(m.targetKey, 0, 0, 0);
                keybd_event(m.targetKey, 0, KEYEVENTF_KEYUP, 0);
                Sleep(m.intervalMs);
            } else {
                Sleep(1);
            }
        }
    }
}

void BindThread(Macro &m) {
    while (true) {
        if (GetAsyncKeyState(m.triggerKey) & 1) {
            if (!m.keepOriginal) keybd_event(m.triggerKey, 0, KEYEVENTF_KEYUP, 0);
            keybd_event(m.targetKey, 0, 0, 0);
            keybd_event(m.targetKey, 0, KEYEVENTF_KEYUP, 0);
        }
        Sleep(1);
    }
}

std::vector<Macro> LoadMacros(const std::string& filename) {
    std::vector<Macro> macros;
    std::ifstream file(filename);
    if (!file.is_open()) {
        std::cerr << "Could not open " << filename << "\n";
        return macros;
    }

std::regex re(
    "\\[(AutoClick|Bind)\\]\\s*"
    "(?:\\[(HOLD|TOGGLE)\\])?\\s*"
    "(?:\\[(K|D)\\])?\\s*"
    "\"([^\"]+)\"\\s*\"([^\"]+)\""
    "(?:\\s*\\[(\\d+)\\])?",
    std::regex::icase);

    int lineNum = 0;
    while (std::getline(file, line)) {
        ++lineNum;
        if (line.empty() || line[0] == '#') continue;
        std::smatch m;
        if (std::regex_search(line, m, re)) {
            Macro macro;
            std::string type = m[1].str();
            std::string mode = m[2].str();
            std::string kd = m[3].str();
            std::string trigger = m[4].str();
            std::string target = m[5].str();
            std::string interval = m[6].str();

            toLower(type);
            if (type == "autoclick") macro.type = Macro::AutoClick;
            else macro.type = Macro::Bind;

            toLower(mode);
            if (mode == "toggle") macro.mode = Macro::Toggle;
            else macro.mode = Macro::Hold;

            toLower(kd);
            if (kd == "k") macro.keepOriginal = true;
            else macro.keepOriginal = false;

            macro.triggerName = trigger;
            macro.targetName = target;

            WORD trigCode = getKeyCode(trigger);
            WORD targCode = getKeyCode(target);
            if (!trigCode || !targCode) {
                std::cerr << "Unknown key on line " << lineNum << ": " << trigger << " or " << target << "\n";
                continue;
            }
            macro.triggerKey = trigCode;
            macro.targetKey = targCode;

            if (!interval.empty()) macro.intervalMs = std::stoi(interval);

            macros.push_back(std::move(macro));
        } else {
            std::cerr << "Skipping invalid macro line " << lineNum << ": " << line << "\n";
        }
    }
    return macros;
}

int main() {
    SetConsoleTitleA("UniMacro");
    LoadKeyMapping("KeyMapping.cfg");

    auto macros = LoadMacros("UniMacro.ini");
    std::cout << "Loaded " << macros.size() << " macros:\n";
    int idx = 1;
    for (auto &m : macros) {
        if (m.type == Macro::AutoClick) {
            std::cout << "#" << idx++ << ": AutoClick(" << (m.mode == Macro::Hold ? "HOLD" : "TOGGLE")
                      << ") TriggerKey=" << m.triggerName
                      << " TargetKey=" << m.targetName
                      << " intervalMs=" << m.intervalMs << "\n";
        } else {
            std::cout << "#" << idx++ << ": Bind TriggerKey=" << m.triggerName
                      << " TargetKey=" << m.targetName << "\n";
        }
    }

    for (auto &m : macros) {
        if (m.type == Macro::AutoClick)
            std::thread(AutoClickThread, std::ref(m)).detach();
        else
            std::thread(BindThread, std::ref(m)).detach();
    }

    std::cout << "UniMacro running. Press Ctrl+C in console to quit.\n";
    while (true) {
        Sleep(100);
    }
    return 0;
}
