// iPodStats - tiny zero-dependency PC stats server for an old iPod touch.
// Serves index.html at "/" and live stats as JSON at "/stats".
// Build: g++ -std=c++17 -O2 server.cpp -o iPodStats.exe -lws2_32 -liphlpapi
#define _WIN32_WINNT 0x0601
#include <winsock2.h>
#include <windows.h>
#include <iphlpapi.h>
#include <wincrypt.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

static const int PORT = 8099;

// ---------- GPU via pdh.dll (loaded at runtime; MinGW has no pdh headers) ----------
typedef LONG PDH_STATUS;
typedef HANDLE PDH_HQUERY;
typedef HANDLE PDH_HCOUNTER;
struct PdhFmtValue { DWORD CStatus; union { LONG l; double d; LONGLONG ll; LPCSTR s; }; };
struct PdhFmtItem { LPSTR szName; PdhFmtValue FmtValue; };
static const DWORD PDH_FMT_DOUBLE = 0x00000200;
static const DWORD PDH_FMT_NOCAP100 = 0x00008000;
static const PDH_STATUS PDH_MORE_DATA = (PDH_STATUS)0x800007D2L;

typedef PDH_STATUS(WINAPI *OpenQueryFn)(LPCSTR, DWORD_PTR, PDH_HQUERY *);
typedef PDH_STATUS(WINAPI *AddCounterFn)(PDH_HQUERY, LPCSTR, DWORD_PTR, PDH_HCOUNTER *);
typedef PDH_STATUS(WINAPI *CollectFn)(PDH_HQUERY);
typedef PDH_STATUS(WINAPI *GetArrayFn)(PDH_HCOUNTER, DWORD, LPDWORD, LPDWORD, PdhFmtItem *);
typedef PDH_STATUS(WINAPI *RemoveCounterFn)(PDH_HCOUNTER);

struct Gpu {
    bool ok = false;
    CollectFn collect = nullptr;
    GetArrayFn getArray = nullptr;
    RemoveCounterFn remove = nullptr;
    PDH_HQUERY q = nullptr;
    PDH_HCOUNTER engine = nullptr, vram = nullptr, cpuPerf = nullptr, cpuBase = nullptr;

    void init() {
        HMODULE h = LoadLibraryA("pdh.dll");
        if (!h) return;
        OpenQueryFn open = (OpenQueryFn)GetProcAddress(h, "PdhOpenQueryA");
        AddCounterFn add = (AddCounterFn)GetProcAddress(h, "PdhAddEnglishCounterA");
        collect = (CollectFn)GetProcAddress(h, "PdhCollectQueryData");
        getArray = (GetArrayFn)GetProcAddress(h, "PdhGetFormattedCounterArrayA");
        remove = (RemoveCounterFn)GetProcAddress(h, "PdhRemoveCounter");
        if (!open || !add || !collect || !getArray) return;
        if (open(nullptr, 0, &q) != 0) return;
        if (add(q, "\\GPU Engine(*)\\Utilization Percentage", 0, &engine) != 0) return;
        add(q, "\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &vram);
        // CPU speed the way Task Manager estimates it: base clock x "% of base" it is running at.
        add(q, "\\Processor Information(_Total)\\% Processor Performance", 0, &cpuPerf);
        add(q, "\\Processor Information(_Total)\\Processor Frequency", 0, &cpuBase);
        collect(q);
        ok = true;
    }

    // Sum of counter instances; if filter is set, only instances whose name contains it.
    double sum(PDH_HCOUNTER c, const char *filter, bool maxOnly) {
        if (!c) return -1;
        DWORD bytes = 0, count = 0;
        if (getArray(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, nullptr) != PDH_MORE_DATA) return -1;
        std::vector<char> buf(bytes);
        PdhFmtItem *items = (PdhFmtItem *)buf.data();
        if (getArray(c, PDH_FMT_DOUBLE | PDH_FMT_NOCAP100, &bytes, &count, items) != 0) return -1;
        double total = 0;
        for (DWORD i = 0; i < count; i++) {
            if (filter && !strstr(items[i].szName, filter)) continue;
            double v = items[i].FmtValue.d;
            if (maxOnly) { if (v > total) total = v; } else total += v;
        }
        return total;
    }

    // "GPU Engine(*)" has an instance per app per engine and is the costly part of every collect.
    // Once AMD's library delivers GPU usage it is never needed again, so stop collecting it.
    void dropEngine() {
        if (engine && remove) { remove(engine); engine = nullptr; }
    }

    void sample(double &load, double &vramBytes, double &cpuMhz) {
        load = vramBytes = cpuMhz = -1;
        if (!ok || collect(q) != 0) return;
        double perf = sum(cpuPerf, nullptr, false), base = sum(cpuBase, nullptr, false);
        if (perf > 0 && base > 0) cpuMhz = base * perf / 100;
        // Backup when LibreHardwareMonitor is off. Like Task Manager: the busiest engine type (3D, compute,
        // video...), each summed over apps. 3D alone missed compute and video work.
        static const char *types[] = {"engtype_3D", "engtype_Compute", "engtype_VideoDecode", "engtype_VideoEncode", "engtype_Copy"};
        load = -1;
        for (int i = 0; i < 5; i++) {
            double v = sum(engine, types[i], false);
            if (v > load) load = v;
        }
        if (load > 100) load = 100;
        vramBytes = sum(vram, nullptr, true);
    }
};

// ---------- AMD GPU via the driver's own library (ADL) ----------
// atiadlxy.dll is the 32-bit ADL that every AMD driver installs (this exe is 32-bit). Same source
// LibreHardwareMonitor uses for AMD cards, so same numbers, but read directly every second.
// PMLog sensor numbers were matched against LibreHardwareMonitor on this PC (RX 6750 XT).
struct AmdGpu {
    struct Sensor { int supported; int value; };
    struct PMLog { int size; Sensor sensors[256]; };
    typedef void *(__stdcall *MallocCb)(int);
    typedef int (*CreateFn)(MallocCb, int, void **);
    typedef int (*CountFn)(void *, int *);
    typedef int (*PmLogFn)(void *, int, PMLog *);
    enum { GFX_CLOCK = 1, TEMP_EDGE = 8, FAN_RPM = 14, ACTIVITY_GFX = 19, TEMP_HOTSPOT = 27 };
    static void *__stdcall adlMalloc(int n) { return malloc(n); }

    bool ok = false;
    void *ctx = nullptr;
    int adapter = -1;
    PmLogFn pmlog = nullptr;

    void init() {
        HMODULE h = LoadLibraryA("atiadlxy.dll");
        if (!h) return; // not an AMD card: LibreHardwareMonitor / Windows counters take over
        CreateFn create = (CreateFn)GetProcAddress(h, "ADL2_Main_Control_Create");
        CountFn count = (CountFn)GetProcAddress(h, "ADL2_Adapter_NumberOfAdapters_Get");
        pmlog = (PmLogFn)GetProcAddress(h, "ADL2_New_QueryPMLogData_Get");
        if (!create || !count || !pmlog || create(adlMalloc, 1, &ctx) != 0) return;
        int n = 0;
        count(ctx, &n);
        static PMLog out; // 2 KB, keep it off the stack
        for (int i = 0; i < n && adapter < 0; i++) { // ADL lists one GPU several times: first one that answers
            memset(&out, 0, sizeof(out));
            out.size = sizeof(out);
            if (pmlog(ctx, i, &out) == 0 && out.sensors[ACTIVITY_GFX].supported) adapter = i;
        }
        ok = adapter >= 0;
    }

    // -1 for anything not available.
    void sample(double &load, double &temp, double &hot, double &mhz, double &fan) {
        load = temp = hot = mhz = fan = -1;
        if (!ok) return;
        static PMLog out;
        memset(&out, 0, sizeof(out));
        out.size = sizeof(out);
        if (pmlog(ctx, adapter, &out) != 0) return;
        if (out.sensors[ACTIVITY_GFX].supported) load = out.sensors[ACTIVITY_GFX].value;
        if (out.sensors[TEMP_EDGE].supported) temp = out.sensors[TEMP_EDGE].value;
        if (out.sensors[TEMP_HOTSPOT].supported) hot = out.sensors[TEMP_HOTSPOT].value;
        if (out.sensors[GFX_CLOCK].supported) mhz = out.sensors[GFX_CLOCK].value;
        if (out.sensors[FAN_RPM].supported) fan = out.sensors[FAN_RPM].value;
    }

    // Frame rate the driver counts on the main display (only while a game runs; costs nothing otherwise).
    typedef int (*FmStartFn)(void *, int, int);
    typedef int (*FmGetFn)(void *, int, int, float *);
    FmStartFn fmStart = nullptr, fmStop = nullptr;
    FmGetFn fmGet = nullptr;
    bool fmOn = false;
    void fps(bool want, double &out) {
        out = -1;
        if (!ok) return;
        if (!fmGet) {
            HMODULE h = GetModuleHandleA("atiadlxy.dll");
            fmStart = (FmStartFn)GetProcAddress(h, "ADL2_Adapter_FrameMetrics_Start");
            fmStop = (FmStartFn)GetProcAddress(h, "ADL2_Adapter_FrameMetrics_Stop");
            fmGet = (FmGetFn)GetProcAddress(h, "ADL2_Adapter_FrameMetrics_Get");
            if (!fmStart || !fmStop || !fmGet) { ok = false; return; }
        }
        if (want != fmOn) {
            if (want) fmOn = fmStart(ctx, adapter, 0) == 0;
            else { fmStop(ctx, adapter, 0); fmOn = false; }
        }
        float f = 0;
        if (fmOn && fmGet(ctx, adapter, 0, &f) == 0 && f > 0) out = f;
    }
};

// ---------- Game detection ----------
// A game = Windows says a fullscreen app runs (or the cursor is locked), and it isn't a browser or video
// player showing fullscreen video. Stays on 10 s after, so a quick Alt+Tab doesn't flip the iPod back.
struct Game {
    std::string name; // "" = no game
    DWORD start = 0, lastSeen = 0;

    static std::string utf8(const wchar_t *w) {
        char b[256];
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof(b), nullptr, nullptr);
        return n > 1 ? std::string(b, n - 1) : std::string();
    }
    static std::string lower(std::string s) { for (auto &c : s) c = (char)tolower((unsigned char)c); return s; }

    // Name for the iPod: the window title if it's short, else the exe name.
    static bool foreground(std::string &title, std::string &exe) {
        HWND w = GetForegroundWindow();
        if (!w) return false;
        wchar_t t[128] = {0};
        GetWindowTextW(w, t, 128);
        title = utf8(t);
        DWORD pid = 0;
        GetWindowThreadProcessId(w, &pid);
        exe.clear();
        typedef BOOL(WINAPI *QFn)(HANDLE, DWORD, LPWSTR, PDWORD);
        static QFn q = (QFn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "QueryFullProcessImageNameW");
        HANDLE p = OpenProcess(0x1000 /* PROCESS_QUERY_LIMITED_INFORMATION */, FALSE, pid);
        if (p && q) {
            wchar_t path[MAX_PATH];
            DWORD len = MAX_PATH;
            if (q(p, 0, path, &len)) {
                std::string full = utf8(path);
                size_t sl = full.find_last_of('\\');
                exe = sl == std::string::npos ? full : full.substr(sl + 1);
            }
        }
        if (p) CloseHandle(p);
        return true;
    }

    void update() {
        typedef HRESULT(WINAPI *Fn)(int *);
        static Fn f = (Fn)GetProcAddress(LoadLibraryA("shell32.dll"), "SHQueryUserNotificationState");
        int state = 0;
        bool full = f && f(&state) >= 0 && state >= 2 && state <= 4;
        RECT clip;
        int W = GetSystemMetrics(SM_CXSCREEN), H = GetSystemMetrics(SM_CYSCREEN);
        if (GetClipCursor(&clip) && (clip.right - clip.left < W || clip.bottom - clip.top < H)) full = true;
        DWORD now = GetTickCount();
        std::string title, exe;
        if (full && foreground(title, exe)) {
            static const char *notGames[] = {"chrome.exe", "firefox.exe", "msedge.exe", "opera.exe", "brave.exe",
                "vivaldi.exe", "vlc.exe", "mpc-hc64.exe", "mpc-be64.exe", "potplayermini64.exe", "mpv.exe",
                "explorer.exe", "applicationframehost.exe", "powerpnt.exe", "iPodStats.exe"};
            std::string e = lower(exe);
            for (const char *n : notGames) if (e == lower(n)) full = false;
        }
        if (full) {
            std::string n = !title.empty() && title.size() <= 32 ? title : exe;
            if (n.size() > 4 && lower(n.substr(n.size() - 4)) == ".exe") n.resize(n.size() - 4);
            if (n.empty()) n = "Game";
            if (name.empty()) start = now;  // new session (a return within the 10 s grace keeps the timer)
            name = n;
            lastSeen = now;
        } else if (!name.empty() && now - lastSeen > 10000) {
            name.clear();
        }
    }

    std::string jsonName() const {
        std::string o;
        for (unsigned char c : name) {
            if (c < 0x20) continue;
            if (c == '"' || c == '\\') o += '\\';
            o += (char)c;
        }
        return o;
    }
};

// ---------- Temps from LibreHardwareMonitor (web server on localhost:8085) ----------
// -1 means "not available" (LHM not running or sensor not found).
struct Temps {
    double cpu = -1, gpu = -1, hot = -1, gpuLoad = -1, cpuMhz = -1, gpuMhz = -1, gpuFan = -1, cpuFan = -1;

    static bool fetch(std::string &body) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return false;
        DWORD to = 500;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof(to));
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        a.sin_port = htons(8085);
        bool ok = false;
        if (connect(s, (sockaddr *)&a, sizeof(a)) == 0) {
            const char *req = "GET /data.json HTTP/1.0\r\nHost: 127.0.0.1:8085\r\n\r\n";
            send(s, req, (int)strlen(req), 0);
            std::string all;
            char buf[8192];
            int n;
            while ((n = recv(s, buf, sizeof(buf), 0)) > 0) all.append(buf, n);
            size_t h = all.find("\r\n\r\n");
            if (h != std::string::npos) { body = all.substr(h + 4); ok = true; }
        }
        closesocket(s);
        return ok;
    }

    // Value of a "key":"..." string field inside [from, to).
    static std::string field(const std::string &j, const char *key, size_t from, size_t to) {
        std::string k = std::string("\"") + key + "\":\"";
        size_t p = j.find(k, from);
        if (p == std::string::npos || p >= to) return "";
        p += k.size();
        size_t e = j.find('"', p);
        return e == std::string::npos ? "" : j.substr(p, e - p);
    }

    void update() {
        cpu = gpu = hot = gpuLoad = cpuMhz = gpuMhz = gpuFan = cpuFan = -1;
        std::string j;
        if (!fetch(j)) return;
        // Each sensor object has "SensorId":"/amdcpu/0/temperature/2" etc. Look at temperature ones only.
        size_t p = 0;
        while ((p = j.find("\"SensorId\":\"", p)) != std::string::npos) {
            size_t start = j.rfind('{', p);
            size_t end = j.find('}', p);
            std::string id = field(j, "SensorId", p, end);
            p = end;
            if (start == std::string::npos) continue;
            bool isTemp = id.find("/temperature/") != std::string::npos;
            bool isLoad = id.find("/load/") != std::string::npos;
            bool isClock = id.find("/clock/") != std::string::npos;
            bool isFan = id.find("/fan/") != std::string::npos;
            if (!isTemp && !isLoad && !isClock && !isFan) continue;
            std::string text = field(j, "Text", start, end);
            double v = atof(field(j, "Value", start, end).c_str()); // "56.5 °C" -> 56.5, "4050.0 MHz" -> 4050
            bool isCpu = id.compare(0, 8, "/amdcpu/") == 0 || id.compare(0, 10, "/intelcpu/") == 0;
            bool isGpu = id.compare(0, 4, "/gpu") == 0;
            // GPU usage as the graphics driver itself reports it (what AMD's/NVIDIA's overlay shows).
            if (isFan) {
                if (isGpu && gpuFan < 0) gpuFan = v;
                else if (!isGpu && cpuFan < 0 && id.find("/lpc/") == 0 && id.find("/fan/0") != std::string::npos) cpuFan = v; // CPU_FAN header
                continue;
            }
            if (isLoad) { if (isGpu && gpuLoad < 0 && text == "GPU Core") gpuLoad = v; continue; }
            // CPU speed: average clock of all cores.
            if (isClock) {
                if (isCpu && cpuMhz < 0 && text == "Cores (Average)") cpuMhz = v;
                else if (isGpu && gpuMhz < 0 && text == "GPU Core") gpuMhz = v; // GPU core clock
                continue;
            }
            if (isCpu && cpu < 0 && (text.find("Tctl") != std::string::npos || text.find("Package") != std::string::npos)) cpu = v;
            else if (isGpu && gpu < 0 && text == "GPU Core") gpu = v;
            else if (isGpu && hot < 0 && text == "GPU Hot Spot") hot = v;
        }
    }
};

// ---------- Stats ----------
// GetTickCount64 is missing from old MinGW headers, so look it up at runtime.
static unsigned long uptimeSecs() {
    typedef ULONGLONG(WINAPI *Fn)();
    static Fn f = (Fn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetTickCount64");
    return (unsigned long)((f ? f() : GetTickCount()) / 1000);
}
static ULONGLONG ft(const FILETIME &f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; }

// ---------- Agent approval prompt ----------
// The coder agent runs in a console and stops at "Run this tool? [y/N, Esc to halt]:". Its console can only be read
// from another process that attaches to it, which would cut this server loose from its own console, so the server
// starts a short-lived copy of itself ("iPodStats.exe --readcon") that looks and prints one line:
//   0                      nothing waiting
//   1|<tool>|<command>     an agent waits for y/N
static int readConMain() {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    typedef BOOL(WINAPI * AttachFn)(DWORD);
    AttachFn attach = (AttachFn)GetProcAddress(GetModuleHandleA("kernel32.dll"), "AttachConsole");
    std::string result = "0";
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32 e;
    e.dwSize = sizeof(e);
    if (snap != INVALID_HANDLE_VALUE && Process32First(snap, &e))
        do { if (lstrcmpiA(e.szExeFile, "koboldcpp-nocuda.exe") == 0) pids.push_back(e.th32ProcessID); } while (Process32Next(snap, &e));
    if (snap != INVALID_HANDLE_VALUE) CloseHandle(snap);
    for (size_t k = 0; k < pids.size() && attach && result == "0"; k++) {
        FreeConsole();
        if (!attach(pids[k])) continue;
        HANDLE h = CreateFileA("CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        CONSOLE_SCREEN_BUFFER_INFO i;
        if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &i)) {
            std::vector<std::string> rows;
            for (int y = i.srWindow.Top; y <= i.srWindow.Bottom; y++) {
                std::string line(i.dwSize.X, ' ');
                DWORD got = 0;
                ReadConsoleOutputCharacterA(h, &line[0], i.dwSize.X, COORD{0, (SHORT)y}, &got);
                line.resize(got);
                while (!line.empty() && line.back() == ' ') line.pop_back();
                rows.push_back(line);
            }
            int last = (int)rows.size() - 1;
            while (last >= 0 && rows[last].empty()) last--;
            if (last >= 0 && (rows[last].find("[y/N") != std::string::npos || rows[last].find("[Y/n") != std::string::npos)) {
                std::string tool, cmd;
                for (int r = last; r >= 0 && cmd.empty(); r--) {
                    size_t c = rows[r].find("\"command\": \"");
                    if (c != std::string::npos) {
                        for (size_t p = c + 12; p < rows[r].size() && rows[r][p] != '"'; p++) cmd += rows[r][p];
                    }
                    if (tool.empty() && rows[r].compare(0, 6, "Tool: ") == 0) tool = rows[r].substr(6);
                }
                for (int r = last; r >= 0 && tool.empty(); r--) if (rows[r].compare(0, 6, "Tool: ") == 0) tool = rows[r].substr(6);
                if (cmd.size() > 90) cmd.resize(90);
                for (auto &ch : cmd) if (ch == '|' || (unsigned char)ch < 0x20) ch = ' ';
                for (auto &ch : tool) if (ch == '|' || (unsigned char)ch < 0x20) ch = ' ';
                result = "1|" + tool + "|" + cmd;
            }
        }
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
    DWORD w;
    WriteFile(out, result.c_str(), (DWORD)result.size(), &w, nullptr);
    return 0;
}

// ---------- Local AI models (KoboldCpp) ----------
// Each model listens on its own port (change the list below to match yours). The first one that answers wins.
// Speed numbers come from KoboldCpp's own /api/extra/perf report about its last reply.
struct Ai {
    struct Known { int port; const char *label; };
    static const Known *known(int &n) {
        static const Known k[] = {{5004, "Coder"}, {5005, "Coder 2"}, {5001, "Chat"}, {5003, "Assistant"}};
        n = 4;
        return k;
    }
    int port = 0;
    std::string label, model;
    double tps = -1, tokens = -1, proc = -1, gens = -1, up = -1;
    bool busy = false;
    DWORD lastProbe = 0, startTick = 0;

    // Ports something is listening on, from Windows' own table. Never "try to connect" to find out: a connect to
    // a closed localhost port takes ~2 s to fail on Windows, which froze the whole server (8 s for 4 ports).
    static std::vector<int> listeningPorts() {
        std::vector<int> ports;
        ULONG size = 0;
        GetTcpTable(nullptr, &size, FALSE);
        std::vector<char> buf(size + 1024);
        size = (ULONG)buf.size();
        MIB_TCPTABLE *t = (MIB_TCPTABLE *)buf.data();
        if (GetTcpTable(t, &size, FALSE) != 0) return ports;
        for (DWORD i = 0; i < t->dwNumEntries; i++)
            if (t->table[i].dwState == MIB_TCP_STATE_LISTEN) ports.push_back(ntohs((u_short)t->table[i].dwLocalPort));
        return ports;
    }
    static bool has(const std::vector<int> &v, int p) { return std::find(v.begin(), v.end(), p) != v.end(); }
    static bool get(int p, const char *path, std::string &body) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) return false;
        DWORD to = 300;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&to, sizeof(to));
        sockaddr_in a = {};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = inet_addr("127.0.0.1");
        a.sin_port = htons((u_short)p);
        bool ok = false;
        if (connect(s, (sockaddr *)&a, sizeof(a)) == 0) {
            char req[160];
            snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: 127.0.0.1:%d\r\n\r\n", path, p);
            send(s, req, (int)strlen(req), 0);
            std::string all;
            char buf[4096];
            int n;
            while ((n = recv(s, buf, sizeof(buf), 0)) > 0) all.append(buf, n);
            size_t h = all.find("\r\n\r\n");
            if (h != std::string::npos) { body = all.substr(h + 4); ok = true; }
        }
        closesocket(s);
        return ok;
    }
    static double num(const std::string &b, const char *key, double def) {
        std::string k = std::string("\"") + key + "\"";
        size_t p = b.find(k);
        if (p == std::string::npos) return def;
        p = b.find(':', p);
        if (p == std::string::npos) return def;
        const char *c = b.c_str() + p + 1;
        char *end;
        double v = strtod(c, &end);
        return end == c ? def : v;
    }
    void reset() { port = 0; label.clear(); model.clear(); tps = tokens = proc = gens = up = -1; busy = false; wait = false; ask.clear(); }

    // Is an agent stopped at a y/N question? Checked (by the --readcon child) only while the model is idle.
    bool wait = false;
    std::string ask;
    HANDLE child = nullptr, rd = nullptr;
    DWORD lastSpawn = 0;
    void endChild() {
        if (child) { TerminateProcess(child, 1); CloseHandle(child); child = nullptr; }
        if (rd) { CloseHandle(rd); rd = nullptr; }
    }
    void pollWait(bool want) {
        DWORD now = GetTickCount();
        if (!want) { endChild(); wait = false; ask.clear(); return; }
        if (child) {
            if (WaitForSingleObject(child, 0) != WAIT_OBJECT_0) {
                if (now - lastSpawn > 5000) endChild(); // stuck
                return;
            }
            char buf[400];
            DWORD n = 0;
            ReadFile(rd, buf, sizeof(buf) - 1, &n, nullptr);
            buf[n] = 0;
            endChild();
            std::string r(buf, n);
            if (r.compare(0, 2, "1|") == 0) {
                size_t bar = r.find('|', 2);
                std::string tool = r.substr(2, bar == std::string::npos ? std::string::npos : bar - 2);
                std::string cmd = bar == std::string::npos ? "" : r.substr(bar + 1);
                std::string a = cmd.empty() ? tool : cmd;
                if (a.empty()) a = "approval";
                ask.clear();
                for (char c : a) { if (c == '"' || c == '\\') ask += '\\'; ask += c; }
                wait = true;
            } else { wait = false; ask.clear(); }
            return;
        }
        if (now - lastSpawn < 1500) return;
        lastSpawn = now;
        SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
        HANDLE r = nullptr, w = nullptr;
        if (!CreatePipe(&r, &w, &sa, 0)) return;
        SetHandleInformation(r, HANDLE_FLAG_INHERIT, 0);
        char self[MAX_PATH];
        GetModuleFileNameA(nullptr, self, MAX_PATH);
        std::string cmd = std::string("\"") + self + "\" --readcon";
        std::vector<char> line(cmd.begin(), cmd.end());
        line.push_back(0);
        STARTUPINFOA si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = w;
        si.hStdError = w;
        si.hStdInput = INVALID_HANDLE_VALUE;
        PROCESS_INFORMATION pi = {};
        BOOL ok = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(w);
        if (!ok) { CloseHandle(r); return; }
        CloseHandle(pi.hThread);
        child = pi.hProcess;
        rd = r;
    }

    void update() {
        DWORD now = GetTickCount();
        if (!port || now - lastProbe > 3000) { // look for a model every 3 s, and check the known one is still there
            lastProbe = now;
            int n;
            const Known *k = known(n);
            int found = 0;
            const char *lab = "";
            std::vector<int> open = listeningPorts();
            if (port && has(open, port)) { found = port; for (int i = 0; i < n; i++) if (k[i].port == port) lab = k[i].label; }
            for (int i = 0; !found && i < n; i++) if (has(open, k[i].port)) { found = k[i].port; lab = k[i].label; }
            if (found != port) { reset(); port = found; if (found) { label = lab; startTick = now; } }
        }
        if (!port) return;
        if (model.empty()) {
            std::string b;
            if (get(port, "/api/v1/model", b)) {
                size_t p = b.find("\"result\"");
                size_t q1 = p == std::string::npos ? p : b.find('"', b.find(':', p) + 1);
                size_t q2 = q1 == std::string::npos ? q1 : b.find('"', q1 + 1);
                if (q2 != std::string::npos) {
                    model = b.substr(q1 + 1, q2 - q1 - 1);
                    size_t sl = model.find_last_of("/\\");
                    if (sl != std::string::npos) model = model.substr(sl + 1);
                    if (model.size() > 4 && model.substr(model.size() - 5) == ".gguf") model.resize(model.size() - 5);
                    if (model.size() > 24) model.resize(24);
                    for (auto &c : model) if (c == '"' || c == '\\' || (unsigned char)c < 0x20) c = ' ';
                }
            }
        }
        std::string b;
        if (!get(port, "/api/extra/perf", b)) return;
        double ev = num(b, "last_eval", -1), tc = num(b, "last_token_count", -1);
        tps = ev > 0 && tc > 0 ? tc / ev : -1;
        tokens = tc;
        proc = num(b, "last_process", -1);
        gens = num(b, "total_gens", -1);
        up = num(b, "uptime", -1);
        busy = num(b, "idle", 1) == 0;
    }
    std::string title() const { return label.empty() ? "" : label + (model.empty() ? "" : " \xC2\xB7 " + model); }
};

// ---------- NET page: ping to the router and the iPod, devices on the home network ----------
// Pings run in their own thread (a ping that gets no answer takes its whole timeout) and only while the
// iPod page has the NET tab open: the page adds net=1 to its /stats request.
static volatile DWORD g_netWant = 0; // last time a screen asked for the NET tab (GetTickCount), 0 = never
static volatile DWORD g_ipodIp = 0;  // address (network order) of the last other device that polled /stats

struct PingStat {
    volatile int ms = -2;   // last round trip in ms; -1 = no answer, -2 = not measured
    volatile int loss = 0;  // % of the last 30 pings that got no answer
    bool ring[30] = {};     // true = lost
    int pos = 0, count = 0;
    void add(int r) {
        ms = r;
        ring[pos] = r < 0;
        pos = (pos + 1) % 30;
        if (count < 30) count++;
        int lost = 0;
        for (int i = 0; i < count; i++) if (ring[i]) lost++;
        loss = lost * 100 / count;
    }
    void reset() { ms = -2; loss = 0; pos = count = 0; }
};
static PingStat g_pingRouter, g_pingIpod;
static volatile DWORD g_gateway = 0;

// Interface the internet traffic uses, its gateway and this PC's address there (route lookups only, nothing is sent).
static void netRoute(DWORD &idx, DWORD &gw, DWORD &own) {
    idx = gw = own = 0;
    DWORD dst = inet_addr("1.1.1.1");
    if (GetBestInterface(dst, &idx) != 0) return;
    MIB_IPFORWARDROW fr;
    if (GetBestRoute(dst, 0, &fr) == 0) gw = fr.dwForwardNextHop;
    ULONG size = 0;
    GetIpAddrTable(nullptr, &size, FALSE);
    if (!size) return;
    std::vector<char> buf(size);
    MIB_IPADDRTABLE *t = (MIB_IPADDRTABLE *)buf.data();
    if (GetIpAddrTable(t, &size, FALSE) != 0) return;
    for (DWORD i = 0; i < t->dwNumEntries; i++)
        if (t->table[i].dwIndex == idx) { own = t->table[i].dwAddr; break; }
}

struct IcmpReply { DWORD Address, Status, RoundTripTime; WORD DataSize, Reserved; void *Data; BYTE Ttl, Tos, Flags, OptionsSize; BYTE *OptionsData; };

static DWORD WINAPI pingThread(LPVOID) {
    typedef HANDLE (WINAPI *CreateFn)();
    typedef DWORD (WINAPI *SendFn)(HANDLE, DWORD, LPVOID, WORD, LPVOID, LPVOID, DWORD, DWORD);
    HMODULE m = LoadLibraryA("iphlpapi.dll");
    CreateFn create = m ? (CreateFn)GetProcAddress(m, "IcmpCreateFile") : nullptr;
    SendFn send = m ? (SendFn)GetProcAddress(m, "IcmpSendEcho") : nullptr;
    HANDLE h = create ? create() : INVALID_HANDLE_VALUE;
    if (!send || h == INVALID_HANDLE_VALUE) return 0;
    bool wasOn = false;
    for (;;) {
        DWORD want = g_netWant, t0 = GetTickCount();
        if (!want || t0 - want > 6000) {
            if (wasOn) { g_pingRouter.reset(); g_pingIpod.reset(); wasOn = false; }
            Sleep(500);
            continue;
        }
        wasOn = true;
        DWORD idx, gw, own;
        netRoute(idx, gw, own);
        g_gateway = gw;
        struct { DWORD addr; PingStat *ps; } targets[2] = { {gw, &g_pingRouter}, {g_ipodIp, &g_pingIpod} };
        for (int i = 0; i < 2; i++) {
            if (!targets[i].addr) { targets[i].ps->ms = -2; continue; }
            char data[8] = {'i', 'P', 'o', 'd', 'S', 't', 'a', 't'}, reply[128];
            DWORD n = send(h, targets[i].addr, data, 8, nullptr, reply, sizeof(reply), 500);
            IcmpReply *r = (IcmpReply *)reply;
            int ms = (n > 0 && r->Status == 0) ? (int)r->RoundTripTime : -1;
            targets[i].ps->add(ms);
        }
        DWORD spent = GetTickCount() - t0;
        if (spent < 1000) Sleep(1000 - spent);
    }
}

// Devices Windows has recently talked to on the home network: router, this PC, the iPod, then the rest.
static std::string netDevices(int &count) {
    count = 0;
    DWORD idx, gw, own;
    netRoute(idx, gw, own);
    ULONG size = 0;
    GetIpNetTable(nullptr, &size, FALSE);
    if (!size || !idx) return "[]";
    std::vector<char> buf(size);
    MIB_IPNETTABLE *t = (MIB_IPNETTABLE *)buf.data();
    if (GetIpNetTable(t, &size, FALSE) != 0) return "[]";
    std::vector<DWORD> others;
    bool haveGw = false, haveIpod = false;
    DWORD ipod = g_ipodIp;
    for (DWORD i = 0; i < t->dwNumEntries; i++) {
        MIB_IPNETROW &r = t->table[i];
        if (r.dwIndex != idx || r.dwPhysAddrLen != 6 || (r.dwType != 3 && r.dwType != 4)) continue;
        BYTE first = (BYTE)(r.dwAddr & 0xFF), last = (BYTE)(r.dwAddr >> 24);
        if (last == 255 || first >= 224 || r.dwAddr == own) continue;
        if (r.dwAddr == gw) haveGw = true;
        else if (r.dwAddr == ipod) haveIpod = true;
        else if (std::find(others.begin(), others.end(), r.dwAddr) == others.end()) others.push_back(r.dwAddr);
    }
    std::sort(others.begin(), others.end(), [](DWORD a, DWORD b) { return (a >> 24) < (b >> 24); });
    std::string s = "[";
    auto add = [&](DWORD a, const char *tag) {
        in_addr ia; ia.s_addr = a;
        if (s.size() > 1) s += ",";
        s += std::string("\"") + inet_ntoa(ia) + "|" + tag + "\"";
    };
    if (haveGw) add(gw, "router");
    if (own) add(own, "this PC");
    if (haveIpod) add(ipod, "iPod");
    for (size_t i = 0; i < others.size() && i < 12; i++) add(others[i], "");
    count = (haveGw ? 1 : 0) + (own ? 1 : 0) + (haveIpod ? 1 : 0) + (int)others.size();
    return s + "]";
}

struct Stats {
    double totDown = 0, totUp = 0, linkBps = 0; // bytes since the server started, and the adapter's link speed
    ULONGLONG lastIdle = 0, lastTotal = 0;
    ULONGLONG lastIn = 0, lastOut = 0;
    DWORD lastTick = 0;
    Gpu gpu;
    AmdGpu amd;
    Temps temps;
    Game game;
    Ai ai;
    unsigned tempTick = 0;
    std::string json = "{}";

    double vramTotal = -1;

    void init() {
        gpu.init();
        amd.init();
        // Graphics card memory size, as the driver records it (largest display adapter wins).
        for (int i = 0; i < 10; i++) {
            char key[128];
            snprintf(key, sizeof(key), "SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e968-e325-11ce-bfc1-08002be10318}\\%04d", i);
            HKEY k;
            if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, key, 0, KEY_READ, &k) != 0) continue;
            ULONGLONG v = 0;
            DWORD len = sizeof(v);
            if (RegQueryValueExA(k, "HardwareInformation.qwMemorySize", nullptr, nullptr, (BYTE *)&v, &len) == 0 && (double)v > vramTotal)
                vramTotal = (double)v;
            RegCloseKey(k);
        }
        sample();
    }

    // Busiest ethernet/wifi adapter. Skips WFP/QoS filter copies, which repeat the same traffic.
    void netBytes(ULONGLONG &in, ULONGLONG &out) {
        in = out = 0;
        ULONG size = 0;
        GetIfTable(nullptr, &size, FALSE);
        std::vector<char> buf(size);
        MIB_IFTABLE *t = (MIB_IFTABLE *)buf.data();
        if (GetIfTable(t, &size, FALSE) != 0) return;
        ULONGLONG best = 0;
        for (DWORD i = 0; i < t->dwNumEntries; i++) {
            MIB_IFROW &r = t->table[i];
            if (r.dwType != 6 && r.dwType != 71) continue;
            ULONGLONG s = (ULONGLONG)r.dwInOctets + r.dwOutOctets;
            if (s > best) { best = s; in = r.dwInOctets; out = r.dwOutOctets; linkBps = r.dwSpeed; }
        }
    }

    void sample() {
        DWORD now = GetTickCount();
        double secs = lastTick ? (now - lastTick) / 1000.0 : 1.0;
        if (secs <= 0) secs = 1;
        lastTick = now;

        FILETIME fi, fk, fu;
        GetSystemTimes(&fi, &fk, &fu);
        ULONGLONG idle = ft(fi), total = ft(fk) + ft(fu);
        double cpu = 0;
        if (lastTotal && total > lastTotal) cpu = 100.0 * (1.0 - (double)(idle - lastIdle) / (total - lastTotal));
        lastIdle = idle; lastTotal = total;
        if (cpu < 0) cpu = 0;

        MEMORYSTATUSEX m; m.dwLength = sizeof(m);
        GlobalMemoryStatusEx(&m);

        ULONGLONG in, out;
        netBytes(in, out);
        // 32-bit counters wrap; unsigned 32-bit subtraction handles one wrap.
        double down = lastIn ? (DWORD)((DWORD)in - (DWORD)lastIn) / secs : 0;
        double up = lastOut ? (DWORD)((DWORD)out - (DWORD)lastOut) / secs : 0;
        lastIn = in; lastOut = out;
        totDown += down * secs; totUp += up * secs;

        // LibreHardwareMonitor every 2 s (its feed is ~23 KB). Only CPU temperature truly needs it.
        if (++tempTick % 2 == 1) temps.update();

        // Best source first for each number:
        //   GPU usage/temps/clock: AMD's library > LibreHardwareMonitor > (usage only) Windows GPU counters
        //   CPU speed: LibreHardwareMonitor (real core clocks) > Windows' estimate
        //   CPU temperature: LibreHardwareMonitor only (Ryzen needs a kernel driver for it)
        double pdhLoad, vram, pdhMhz;
        gpu.sample(pdhLoad, vram, pdhMhz);
        double gpuLoad, gpuT, hotT, gpuMhz, gpuFan;
        amd.sample(gpuLoad, gpuT, hotT, gpuMhz, gpuFan);
        if (gpuLoad >= 0) gpu.dropEngine();
        if (gpuFan < 0) gpuFan = temps.gpuFan;
        if (gpuLoad < 0) gpuLoad = temps.gpuLoad >= 0 ? temps.gpuLoad : pdhLoad;
        if (gpuT < 0) gpuT = temps.gpu;
        if (hotT < 0) hotT = temps.hot;
        if (gpuMhz < 0) gpuMhz = temps.gpuMhz;
        double cpuMhz = temps.cpuMhz >= 0 ? temps.cpuMhz : pdhMhz;

        char host[64] = "PC";
        DWORD hl = sizeof(host);
        GetComputerNameA(host, &hl);

        char b[1024];
        int n = snprintf(b, sizeof(b),
            "{\"host\":\"%s\",\"cpu\":%.1f,\"gpu\":%.1f,\"vram\":%.0f,"
            "\"cpuT\":%.1f,\"gpuT\":%.1f,\"hotT\":%.1f,\"cpuMhz\":%.0f,\"gpuMhz\":%.0f,\"gpuFan\":%.0f,\"cpuFan\":%.0f,"
            "\"ramUsed\":%.0f,\"ramTotal\":%.0f,\"down\":%.0f,\"up\":%.0f,"
            "\"uptime\":%lu,\"disks\":[",
            host, cpu, gpuLoad, vram,
            temps.cpu, gpuT, hotT, cpuMhz, gpuMhz, gpuFan, temps.cpuFan,
            (double)(m.ullTotalPhys - m.ullAvailPhys), (double)m.ullTotalPhys,
            down, up, uptimeSecs());
        std::string s(b, n);
        bool first = true;
        DWORD drives = GetLogicalDrives();
        for (int d = 0; d < 26; d++) {
            if (!(drives & (1u << d))) continue;
            char root[4] = {(char)('A' + d), ':', '\\', 0};
            if (GetDriveTypeA(root) != DRIVE_FIXED) continue;
            ULARGE_INTEGER freeB, totalB;
            if (!GetDiskFreeSpaceExA(root, &freeB, &totalB, nullptr)) continue;
            n = snprintf(b, sizeof(b), "%s{\"n\":\"%c\",\"free\":%.0f,\"total\":%.0f}",
                first ? "" : ",", root[0],
                (double)freeB.QuadPart, (double)totalB.QuadPart);
            s.append(b, n);
            first = false;
        }
        game.update();
        double fps;
        amd.fps(!game.name.empty(), fps);
        n = snprintf(b, sizeof(b), "],\"vramTotal\":%.0f,\"fps\":%.1f,\"gameSecs\":%lu,\"game\":\"", vramTotal, fps,
            game.name.empty() ? 0UL : (unsigned long)((GetTickCount() - game.start) / 1000));
        s.append(b, n);
        s += game.jsonName() + "\"";
        ai.update();
        ai.pollWait(ai.port && !ai.busy);
        n = snprintf(b, sizeof(b), ",\"ai\":\"%s\",\"aiTps\":%.1f,\"aiTok\":%.0f,\"aiProc\":%.1f,\"aiGens\":%.0f,\"aiBusy\":%d,\"aiWait\":%d,\"aiAsk\":\"%s\",\"aiUp\":%.0f",
            ai.title().c_str(), ai.tps, ai.tokens, ai.proc, ai.gens, ai.busy ? 1 : 0, ai.wait ? 1 : 0, ai.ask.c_str(),
            ai.up >= 0 ? ai.up : (ai.port ? (double)((GetTickCount() - ai.startTick) / 1000) : 0.0));
        s.append(b, n);
        // NET page numbers. The device list is only gathered while a screen has the NET tab open.
        DWORD nw = g_netWant;
        int devCount = 0;
        std::string devs = nw && GetTickCount() - nw < 6000 ? netDevices(devCount) : "[]";
        n = snprintf(b, sizeof(b), ",\"net\":{\"tdown\":%.0f,\"tup\":%.0f,\"link\":%.0f,\"pr\":%d,\"lr\":%d,\"pi\":%d,\"li\":%d,\"devn\":%d,\"dev\":",
            totDown, totUp, linkBps, (int)g_pingRouter.ms, (int)g_pingRouter.loss, (int)g_pingIpod.ms, (int)g_pingIpod.loss, devCount);
        s.append(b, n);
        s += devs + "}}";
        json = s;
    }
};

// ---------- HTTP ----------
static std::string exeDir() {
    char p[MAX_PATH];
    GetModuleFileNameA(nullptr, p, MAX_PATH);
    std::string s(p);
    return s.substr(0, s.find_last_of("\\/") + 1);
}

static bool readFile(const std::string &path, std::string &out) {
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return true;
}

static void sendAll(SOCKET c, const std::string &s) {
    size_t sent = 0;
    while (sent < s.size()) {
        int r = send(c, s.data() + sent, (int)(s.size() - sent), 0);
        if (r <= 0) return;
        sent += r;
    }
}

static void reply(SOCKET c, const char *status, const char *type, const std::string &body) {
    char h[256];
    snprintf(h, sizeof(h),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\n"
        "Cache-Control: no-cache, no-store\r\nConnection: close\r\n\r\n",
        status, type, (unsigned)body.size());
    sendAll(c, std::string(h) + body);
}

static void logLine(const char *ip, const char *what) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    printf("%02d:%02d:%02d  %-15s  %s\n", t.wHour, t.wMinute, t.wSecond, ip, what);
    fflush(stdout);
}

// ---------- Media ----------
// nowplaying.json is written every second by media.ps1. Ignore it if it is older than 5 s
// (helper not running) or half-written.
static std::string mediaJson(const std::string &dir) {
    std::string path = dir + "nowplaying.json", s;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa)) return "null";
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    if (ft(now) - ft(fa.ftLastWriteTime) > 5ULL * 10000000ULL) return "null";
    if (!readFile(path, s) || s.size() < 2 || s[0] != '{' || s[s.size() - 1] != '}') return "null";
    return s;
}

// ---------- System volume (Core Audio; interfaces declared by hand, old MinGW has no headers) ----------
#ifndef SUCCEEDED
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#endif
struct IMMDevice_ : IUnknown {
    virtual HRESULT __stdcall Activate(REFIID, DWORD, void *, void **) = 0;
};
struct IMMDeviceEnumerator_ : IUnknown {
    virtual HRESULT __stdcall EnumAudioEndpoints(int, DWORD, void **) = 0;
    virtual HRESULT __stdcall GetDefaultAudioEndpoint(int, int, IMMDevice_ **) = 0;
};
struct IAudioEndpointVolume_ : IUnknown {
    virtual HRESULT __stdcall RegisterControlChangeNotify(void *) = 0;
    virtual HRESULT __stdcall UnregisterControlChangeNotify(void *) = 0;
    virtual HRESULT __stdcall GetChannelCount(UINT *) = 0;
    virtual HRESULT __stdcall SetMasterVolumeLevel(float, LPCGUID) = 0;
    virtual HRESULT __stdcall SetMasterVolumeLevelScalar(float, LPCGUID) = 0;
    virtual HRESULT __stdcall GetMasterVolumeLevel(float *) = 0;
    virtual HRESULT __stdcall GetMasterVolumeLevelScalar(float *) = 0;
    virtual HRESULT __stdcall SetChannelVolumeLevel(UINT, float, LPCGUID) = 0;
    virtual HRESULT __stdcall SetChannelVolumeLevelScalar(UINT, float, LPCGUID) = 0;
    virtual HRESULT __stdcall GetChannelVolumeLevel(UINT, float *) = 0;
    virtual HRESULT __stdcall GetChannelVolumeLevelScalar(UINT, float *) = 0;
    virtual HRESULT __stdcall SetMute(BOOL, LPCGUID) = 0;
    virtual HRESULT __stdcall GetMute(BOOL *) = 0;
};
static const GUID CLSID_MMDevEnum = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const GUID IID_MMDevEnum = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const GUID IID_EndpointVol = {0x5CDF2C82, 0x841E, 0x4546, {0x97, 0x22, 0x0C, 0xF7, 0x40, 0x78, 0x22, 0x9A}};

// Volume control of the current default speakers (looked up each time, so device switches just work).
static IAudioEndpointVolume_ *openVolume() {
    IMMDeviceEnumerator_ *en = nullptr;
    IMMDevice_ *dev = nullptr;
    IAudioEndpointVolume_ *vol = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_MMDevEnum, nullptr, CLSCTX_INPROC_SERVER, IID_MMDevEnum, (void **)&en))) {
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(0 /*render*/, 0 /*console*/, &dev))) {
            dev->Activate(IID_EndpointVol, 0x17 /*CLSCTX_ALL*/, nullptr, (void **)&vol);
            dev->Release();
        }
        en->Release();
    }
    return vol;
}

static std::string volumeJson() {
    int v = -1, m = 0;
    if (IAudioEndpointVolume_ *vol = openVolume()) {
        float f = 0;
        BOOL mute = FALSE;
        if (SUCCEEDED(vol->GetMasterVolumeLevelScalar(&f))) v = (int)(f * 100 + 0.5f);
        if (SUCCEEDED(vol->GetMute(&mute))) m = mute ? 1 : 0;
        vol->Release();
    }
    char b[48];
    snprintf(b, sizeof(b), ",\"vol\":%d,\"mute\":%d", v, m);
    return b;
}

// Value of one "key=value" pair in a query string like "cmd=vol&v=40&t=123".
static std::string param(const std::string &query, const char *key) {
    std::string k = std::string(key) + "=";
    size_t p = 0;
    while (p < query.size()) {
        size_t e = query.find('&', p);
        if (e == std::string::npos) e = query.size();
        if (query.compare(p, k.size(), k) == 0) return query.substr(p + k.size(), e - p - k.size());
        p = e + 1;
    }
    return "";
}

static void pressKey(BYTE vk) {
    keybd_event(vk, 0, KEYEVENTF_EXTENDEDKEY, 0);
    keybd_event(vk, 0, KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP, 0);
}

// Media buttons act like keyboard media keys; volume is set directly.
static void mediaCommand(const std::string &query) {
    std::string cmd = param(query, "cmd");
    if (cmd == "playpause") pressKey(VK_MEDIA_PLAY_PAUSE);
    else if (cmd == "next") pressKey(VK_MEDIA_NEXT_TRACK);
    else if (cmd == "prev") pressKey(VK_MEDIA_PREV_TRACK);
    else if (cmd == "vol" || cmd == "mute") {
        IAudioEndpointVolume_ *vol = openVolume();
        if (!vol) return;
        if (cmd == "vol") {
            int v = atoi(param(query, "v").c_str());
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            vol->SetMasterVolumeLevelScalar(v / 100.0f, nullptr);
            vol->SetMute(FALSE, nullptr);
        } else {
            BOOL mute = FALSE;
            vol->GetMute(&mute);
            vol->SetMute(!mute, nullptr);
        }
        vol->Release();
    }
}

// ---------- Wallpapers ----------
// Resized copies made by media.ps1 live in backgrounds\cache. version changes when any file changes,
// so the iPod reloads the picture.
static std::vector<std::string> wallFiles(const std::string &dir, unsigned &version) {
    std::vector<std::string> v;
    version = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "backgrounds\\cache\\*.*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return v;
    do {
        std::string name = fd.cFileName;
        std::string ext = name.size() > 4 ? name.substr(name.size() - 4) : "";
        if (ext != ".jpg" && ext != ".gif") continue; // skips folders and half-written .tmp files
        v.push_back(name);
        version += fd.ftLastWriteTime.dwLowDateTime + fd.nFileSizeLow;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(v.begin(), v.end());
    return v;
}

// Index of cache file `name` in the list, or -1 (also guards against paths like "..\x").
static int wallIndex(const std::vector<std::string> &files, const std::string &name) {
    for (size_t i = 0; i < files.size(); i++)
        if (files[i] == name) return (int)i;
    return -1;
}

static std::string wallJson(const std::string &dir) {
    unsigned ver;
    size_t n = wallFiles(dir, ver).size();
    char b[64];
    snprintf(b, sizeof(b), ",\"walls\":%u,\"wallv\":%u", (unsigned)n, ver % 1000000u);
    return b;
}

// ---------- PC mouse on the iPod ----------
// The iPod sits below the monitor. Keep pushing the mouse down against the bottom screen edge and it
// "crosses over": a low-level mouse hook swallows the PC's mouse events and turns them into a cursor
// position on the iPod's 480x300 page, which the main loop streams to the page (/cursor).
// Ways back: push up past the iPod's top edge, right-click, or the iPod going offline.
static const int IPOD_W = 480, IPOD_H = 300;
static const DWORD PUSH_MS = 250;      // keep pushing against the bottom edge this long...
static const int PUSH_PX = 80;         // ...and this far to cross over, so a quick flick to the taskbar doesn't
static const int EXIT_PX = 60;         // push this far past the iPod's top edge to come back
static const double MOUSE_SCALE = 1.0; // iPod pixels per PC mouse pixel

static struct Mouse {
    CRITICAL_SECTION cs; // guards everything below up to wakePending
    bool captured;       // true while the mouse is "on the iPod"
    double x, y;         // cursor on the iPod page
    unsigned downs, ups; // left-button presses / releases so far (counters, so quick clicks aren't lost)
    unsigned version;    // bumped on every change
    bool listening;      // set by the main loop: a real iPod is connected and alive
    LONG wakePending;
    SOCKET wakeTx;       // 1 byte to wakeAddr wakes the main loop's select()
    sockaddr_in wakeAddr;
    // used by the hook thread only
    DWORD pushStart, lastPush;
    int pushSum;
    double exitSum;
    bool eatRightUp;     // the right-click that brought the mouse back: its release must not reach the PC
} gm;

// Call with gm.cs held.
static void mouseChanged() {
    gm.version++;
    if (InterlockedExchange(&gm.wakePending, 1) == 0)
        sendto(gm.wakeTx, "w", 1, 0, (sockaddr *)&gm.wakeAddr, sizeof(gm.wakeAddr));
}

// Give the mouse back to the PC. Call with gm.cs held; returns where to put the PC cursor (do it after unlocking).
static POINT mouseRelease(int W, int H) {
    gm.captured = false;
    gm.ups = gm.downs; // let go of anything held on the iPod
    gm.pushSum = 0;
    mouseChanged();
    POINT p = {(LONG)(gm.x / IPOD_W * W), (LONG)(H - 12)}; // just above the edge, so an auto-hide taskbar stays down
    return p;
}

// Games and fullscreen video: never steal the mouse from those.
static bool fullscreenApp(int W, int H) {
    RECT clip;
    if (GetClipCursor(&clip) && (clip.right - clip.left < W || clip.bottom - clip.top < H)) return true; // a game locked the cursor
    // Windows' own verdict (the one that mutes notifications): 2 = fullscreen app, 3 = fullscreen Direct3D,
    // 4 = presentation mode. Window size alone is no good: with an auto-hide taskbar every maximized
    // window covers the whole screen. Missing from old MinGW headers, so looked up at runtime.
    typedef HRESULT(WINAPI *Fn)(int *);
    static Fn f = (Fn)GetProcAddress(LoadLibraryA("shell32.dll"), "SHQueryUserNotificationState");
    int state = 0;
    return f && f(&state) >= 0 && state >= 2 && state <= 4;
}

static LRESULT CALLBACK mouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wp, lp);
    MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lp;
    int W = GetSystemMetrics(SM_CXSCREEN), H = GetSystemMetrics(SM_CYSCREEN);
    bool swallow = false;
    POINT warp = {-1, -1};

    EnterCriticalSection(&gm.cs);
    if (!gm.captured && wp == WM_RBUTTONUP && gm.eatRightUp) {
        gm.eatRightUp = false;
        swallow = true;
    } else if (!gm.captured) {
        if (wp == WM_MOUSEMOVE) {
            // The hook sees where the cursor WOULD go, even past the screen edge: over = how hard the edge is pushed.
            int over = m->pt.y - (H - 1);
            DWORD now = GetTickCount();
            if (over > 0 && gm.listening && m->pt.x >= 0 && m->pt.x < W) {
                if (now - gm.lastPush > 150) { gm.pushStart = now; gm.pushSum = 0; }
                gm.lastPush = now;
                gm.pushSum += over;
                bool buttons = (GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON) | GetAsyncKeyState(VK_MBUTTON)) & 0x8000;
                if (now - gm.pushStart >= PUSH_MS && gm.pushSum >= PUSH_PX && !buttons && !fullscreenApp(W, H)) {
                    gm.captured = true;
                    gm.x = (double)m->pt.x * IPOD_W / W; // arrive at the matching spot along the iPod's top edge
                    gm.y = 0;
                    gm.exitSum = 0;
                    gm.pushSum = 0;
                    warp.x = W - 1; warp.y = H / 2; // park the PC cursor on the right edge: nearly invisible, and off an auto-hide taskbar
                    swallow = true;
                    mouseChanged();
                }
            } else if (over < 0) gm.pushSum = 0;
        }
    } else if (!gm.listening || wp == WM_RBUTTONDOWN) {
        warp = mouseRelease(W, H); // iPod went away, or right-click = "give me my mouse back"
        swallow = wp == WM_RBUTTONDOWN;
        gm.eatRightUp = swallow;
    } else {
        swallow = true; // the PC must not see anything the mouse does while it is on the iPod
        if (wp == WM_MOUSEMOVE) {
            POINT cur;
            GetCursorPos(&cur); // swallowed moves leave the real cursor parked, so pt - cur = how far the mouse moved
            double dy = (m->pt.y - cur.y) * MOUSE_SCALE;
            gm.x += (m->pt.x - cur.x) * MOUSE_SCALE;
            gm.y += dy;
            if (gm.x < 0) gm.x = 0;
            if (gm.x > IPOD_W - 1) gm.x = IPOD_W - 1;
            if (gm.y > IPOD_H - 1) gm.y = IPOD_H - 1;
            if (gm.y < 0) {
                gm.exitSum += -gm.y;
                gm.y = 0;
            } else if (dy > 0) gm.exitSum = 0;
            if (gm.exitSum >= EXIT_PX) warp = mouseRelease(W, H);
            else mouseChanged();
        } else if (wp == WM_LBUTTONDOWN) {
            gm.downs++;
            mouseChanged();
        } else if (wp == WM_LBUTTONUP) {
            if (gm.ups < gm.downs) gm.ups++;
            mouseChanged();
        }
    }
    LeaveCriticalSection(&gm.cs);

    if (warp.x >= 0) SetCursorPos(warp.x, warp.y);
    return swallow ? 1 : CallNextHookEx(nullptr, code, wp, lp);
}

// Low-level hooks are called on the thread that installed them, which must pump messages.
static DWORD WINAPI hookThread(LPVOID) {
    SetWindowsHookEx(WH_MOUSE_LL, mouseProc, GetModuleHandle(nullptr), 0);
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) DispatchMessage(&msg);
    return 0;
}

static std::vector<std::string> g_localIps; // this PC's own addresses: a browser tab here is not "the iPod"
static std::vector<std::string> g_events; // last 40 media commands / blocked devices, shown at /debug
static int g_wsStreams = 0, g_xhrStreams = 0;
static DWORD g_lastRemoteStats = 0;         // last /stats poll from another device (proof the iPod page is alive)
static DWORD g_lastStatsPoll = 0;
static bool g_idle = false;           // last /stats poll from anyone: nobody for a while = idle, skip the sensor work
static bool isLocal(const char *ip) {
    for (size_t i = 0; i < g_localIps.size(); i++)
        if (g_localIps[i] == ip) return true;
    return false;
}

static void logEvent(const char *ip, const std::string &what) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[160];
    snprintf(stamp, sizeof(stamp), "%02d:%02d:%02d.%03d  %-15s  %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, ip, what.c_str());
    g_events.push_back(stamp);
    if (g_events.size() > 40) g_events.erase(g_events.begin());
}

// ---------- Who may connect ----------
// Only this PC and the devices listed in allowed.txt (by hardware/MAC address, which unlike the IP
// never changes on the iPod). Everyone else on the network gets "not allowed" and nothing else.
static std::vector<std::string> g_allowed;
static FILETIME g_allowedTime = {0, 0};

static std::string normMac(std::string m) {
    std::string out;
    for (size_t i = 0; i < m.size(); i++) {
        char ch = m[i];
        if (ch >= 'A' && ch <= 'F') ch = ch - 'A' + 'a';
        if ((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f')) out += ch;
    }
    return out.size() == 12 ? out : "";
}

// Re-read allowed.txt when it changes. Lines: a MAC address, anything after # is a comment.
static void loadAllowed(const std::string &dir) {
    std::string path = dir + "allowed.txt", text;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fa)) { g_allowed.clear(); return; }
    if (CompareFileTime(&fa.ftLastWriteTime, &g_allowedTime) == 0) return;
    g_allowedTime = fa.ftLastWriteTime;
    g_allowed.clear();
    if (!readFile(path, text)) return;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        size_t hash = line.find('#');
        if (hash != std::string::npos) line = line.substr(0, hash);
        std::string mac = normMac(line);
        if (!mac.empty()) g_allowed.push_back(mac);
        p = e + 1;
    }
}

// MAC of a device on the local network, from Windows' address table (filled in by the connection itself).
static std::string macOf(const char *ip) {
    ULONG size = 0;
    GetIpNetTable(nullptr, &size, FALSE);
    if (!size) return "";
    std::vector<char> buf(size);
    MIB_IPNETTABLE *t = (MIB_IPNETTABLE *)buf.data();
    if (GetIpNetTable(t, &size, FALSE) != 0) return "";
    DWORD addr = inet_addr(ip);
    for (DWORD i = 0; i < t->dwNumEntries; i++) {
        MIB_IPNETROW &r = t->table[i];
        if (r.dwAddr != addr || r.dwPhysAddrLen != 6 || r.dwType == 2 /*invalid*/) continue;
        char m[13];
        snprintf(m, sizeof(m), "%02x%02x%02x%02x%02x%02x", r.bPhysAddr[0], r.bPhysAddr[1], r.bPhysAddr[2],
                 r.bPhysAddr[3], r.bPhysAddr[4], r.bPhysAddr[5]);
        return m;
    }
    return "";
}

static bool allowedDevice(const char *ip, const std::string &dir) {
    if (isLocal(ip)) return true;
    loadAllowed(dir);
    std::string mac = macOf(ip);
    for (size_t i = 0; i < g_allowed.size(); i++)
        if (!mac.empty() && g_allowed[i] == mac) return true;
    // Remember who knocked, once a minute per device, so a new device is easy to add to allowed.txt.
    static std::string lastIp;
    static DWORD lastTime = 0;
    if (lastIp != ip || GetTickCount() - lastTime > 60000) {
        std::string pretty = mac.empty() ? "unknown" : mac.substr(0, 2) + "-" + mac.substr(2, 2) + "-" + mac.substr(4, 2) + "-" +
                                                          mac.substr(6, 2) + "-" + mac.substr(8, 2) + "-" + mac.substr(10, 2);
        logEvent(ip, "BLOCKED, not in allowed.txt (MAC " + pretty + ")");
        lastIp = ip;
        lastTime = GetTickCount();
    }
    return false;
}

// ---------- WebSocket, 2010 edition ----------
// iOS 5 Safari speaks the old "hixie-76" WebSocket. Its plain-XHR streaming only reaches the page ~20 times
// a second (WebKit throttles it); WebSocket messages arrive at once, so the arrow is much smoother.
// Handshake: two header keys + 8 body bytes -> MD5 -> 16-byte answer. Frames: 0x00 text 0xFF.
static std::string header(const std::string &req, const char *name) {
    std::string k = std::string("\r\n") + name + ": ";
    size_t p = req.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    size_t e = req.find("\r\n", p);
    return e == std::string::npos ? "" : req.substr(p, e - p);
}

// A key's number = all its digits read as one number, divided by how many spaces it has.
static unsigned wsKeyNumber(const std::string &key) {
    unsigned long long digits = 0;
    unsigned spaces = 0;
    for (size_t i = 0; i < key.size(); i++) {
        if (key[i] >= '0' && key[i] <= '9') digits = digits * 10 + (key[i] - '0');
        else if (key[i] == ' ') spaces++;
    }
    return spaces ? (unsigned)(digits / spaces) : 0;
}

static bool md5(const unsigned char *data, DWORD len, unsigned char out[16]) {
    HCRYPTPROV prov = 0;
    HCRYPTHASH hash = 0;
    DWORD n = 16;
    bool ok = CryptAcquireContextA(&prov, nullptr, nullptr, PROV_RSA_FULL, CRYPT_VERIFYCONTEXT) &&
              CryptCreateHash(prov, CALG_MD5, 0, 0, &hash) && CryptHashData(hash, (PBYTE)data, len, 0) &&
              CryptGetHashParam(hash, HP_HASHVAL, out, &n, 0);
    if (hash) CryptDestroyHash(hash);
    if (prov) CryptReleaseContext(prov, 0);
    return ok;
}

// ---------- Song helper watchdog ----------
// media.ps1 holds the mutex "Local\iPodStatsMedia" while it runs. If it is gone, start it again (hidden),
// at most every 30 s, so a crash or a killed process never leaves the MEDIA tab frozen.
static void keepHelperAlive(const std::string &dir) {
    static DWORD lastStart = 0;
    HANDLE m = OpenMutexA(SYNCHRONIZE, FALSE, "Local\\iPodStatsMedia");
    if (m) { CloseHandle(m); return; } // running
    if (lastStart && GetTickCount() - lastStart < 30000) return;
    lastStart = GetTickCount();
    // This exe is 32-bit, so plain "powershell.exe" would be the 32-bit one (app names fail there).
    // Sysnative = the real 64-bit System32 as seen from a 32-bit program.
    char win[MAX_PATH] = "C:\\Windows";
    GetWindowsDirectoryA(win, MAX_PATH);
    std::string cmd = std::string("\"") + win + "\\Sysnative\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + dir + "media.ps1\"";
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::vector<char> line(cmd.begin(), cmd.end());
    line.push_back(0);
    if (CreateProcessA(nullptr, line.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        logEvent("127.0.0.1", "song helper was not running: started it");
    }
}

// Answer one complete request (we only need the first line).
// Returns 0 normally; 1 if the socket became an XHR cursor stream, 2 a WebSocket one (both must stay open).
static int handle(SOCKET c, const std::string &req, const char *ip, Stats &st, const std::string &dir) {
    char method[8] = "", path[256] = "";
    sscanf(req.c_str(), "%7s %255s", method, path);
    std::string p(path), query;
    size_t q = p.find('?');
    if (q != std::string::npos) { query = p.substr(q + 1); p = p.substr(0, q); }

    // Not logged: these come every second / every track.
    if (p == "/stats") {
        if (!isLocal(ip)) { g_lastRemoteStats = GetTickCount(); g_ipodIp = inet_addr(ip); }
        if (query.find("net=1") != std::string::npos) g_netWant = GetTickCount();
        if (g_idle) { st.sample(); DeleteFileA((dir + "idle.flag").c_str()); g_idle = false; }
        g_lastStatsPoll = GetTickCount();
        std::string body = st.json;
        char cs[48];
        snprintf(cs, sizeof(cs), ",\"curws\":%d,\"curxhr\":%d", g_wsStreams, g_xhrStreams);
        body.insert(body.size() - 1, volumeJson() + wallJson(dir) + cs + ",\"media\":" + mediaJson(dir));
        reply(c, "200 OK", "application/json", body);
        return 0;
    }
    if (p == "/cursor") { // endless response: the main loop keeps writing cursor lines to it
        std::string head = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nCache-Control: no-cache, no-store\r\nConnection: close\r\n\r\n";
        head += std::string(2048, ' ') + "\n"; // old WebKit hands nothing to the page until ~2 KB has arrived
        sendAll(c, head);
        return 1;
    }
    if (p == "/cursorws") {
        std::string k1 = header(req, "Sec-WebSocket-Key1"), k2 = header(req, "Sec-WebSocket-Key2");
        size_t end = req.find("\r\n\r\n");
        unsigned char in[16], out[16];
        if (k1.empty() || k2.empty() || end == std::string::npos || req.size() < end + 12) {
            reply(c, "400 Bad Request", "text/plain", "only the old hixie-76 WebSocket is spoken here"); // modern browsers fall back to /cursor
            return 0;
        }
        unsigned n1 = wsKeyNumber(k1), n2 = wsKeyNumber(k2);
        for (int i = 0; i < 4; i++) { in[i] = (unsigned char)(n1 >> (24 - 8 * i)); in[4 + i] = (unsigned char)(n2 >> (24 - 8 * i)); }
        memcpy(in + 8, req.data() + end + 4, 8);
        if (!md5(in, 16, out)) return 0;
        std::string r = "HTTP/1.1 101 WebSocket Protocol Handshake\r\nUpgrade: WebSocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Origin: " + header(req, "Origin") + "\r\nSec-WebSocket-Location: ws://" + header(req, "Host") + "/cursorws\r\n\r\n";
        r.append((const char *)out, 16);
        sendAll(c, r);
        return 2;
    }
    if (p == "/art") {
        std::string img;
        if (readFile(dir + "art.jpg", img)) reply(c, "200 OK", "image/jpeg", img);
        else reply(c, "404 Not Found", "text/plain", "no art");
        return 0;
    }
    // Wallpapers are picked by file name, so adding or removing pictures never changes the chosen one.
    if (p == "/walls") { // ["name.jpg", ...] in the same order as the picker
        unsigned ver;
        std::vector<std::string> files = wallFiles(dir, ver);
        std::string out = "[";
        for (size_t i = 0; i < files.size(); i++) out += (i ? ",\"" : "\"") + files[i] + "\"";
        reply(c, "200 OK", "application/json", out + "]");
        return 0;
    }
    if (p == "/wallthumb") { // small preview, for the picker
        unsigned ver;
        std::vector<std::string> files = wallFiles(dir, ver);
        int i = wallIndex(files, param(query, "n"));
        std::string img;
        if (i >= 0 && readFile(dir + "backgrounds\\cache\\thumbs\\" + files[i].substr(0, files[i].size() - 4) + ".jpg", img))
            reply(c, "200 OK", "image/jpeg", img);
        else reply(c, "404 Not Found", "text/plain", "no preview");
        return 0;
    }
    if (p == "/wall") {
        unsigned ver;
        std::vector<std::string> files = wallFiles(dir, ver);
        int i = wallIndex(files, param(query, "n"));
        std::string img;
        if (i >= 0 && readFile(dir + "backgrounds\\cache\\" + files[i], img))
            reply(c, "200 OK", files[i].substr(files[i].size() - 4) == ".gif" ? "image/gif" : "image/jpeg", img);
        else reply(c, "404 Not Found", "text/plain", "no wallpaper");
        return 0;
    }
    if (p == "/media") {
        logEvent(ip, param(query, "cmd"));
        mediaCommand(query);
        reply(c, "200 OK", "application/json", "{" + volumeJson().substr(1) + "}");
        return 0;
    }
    if (p == "/debug") { // recent media commands, to see what a device really sent
        std::string out = "recent events (media commands, blocked devices):\n";
        for (size_t i = 0; i < g_events.size(); i++) out += g_events[i];
        reply(c, "200 OK", "text/plain", out);
        return 0;
    }
    logLine(ip, (std::string(method) + " " + path).c_str());
    if (p == "/" || p == "/index.html") {
        std::string html;
        if (readFile(dir + "index.html", html)) reply(c, "200 OK", "text/html; charset=utf-8", html);
        else reply(c, "500 Internal Server Error", "text/plain", "index.html missing next to the exe");
    } else if (p == "/log") {
        // The page reports JS errors here so they show in this window.
        logLine(ip, "  ^ page error report");
        reply(c, "204 No Content", "text/plain", "");
    } else {
        reply(c, "404 Not Found", "text/plain", "not found");
    }
    return 0;
}

struct Client {
    SOCKET s;
    DWORD since;
    std::string buf;
    char ip[16];
};

struct Stream {
    SOCKET s;
    bool remote; // from another device (the iPod), not a browser on this PC
    bool ws;     // WebSocket framing instead of plain lines
};

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--readcon") == 0) return readConMain();
    WSADATA w;
    if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return 1;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED); // for the volume control
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(PORT);
    if (bind(ls, (sockaddr *)&a, sizeof(a)) != 0 || listen(ls, 16) != 0) {
        printf("Could not open port %d (already running?)\n", PORT);
        return 1;
    }

    Stats st;
    st.init();
    g_lastStatsPoll = GetTickCount(); // first 15 s awake, then idle until a screen asks
    std::string dir = exeDir();
    DeleteFileA((dir + "idle.flag").c_str()); // left over from a crash or shutdown while idle: wake the song helper

    printf("iPodStats running. On the iPod open one of these in Safari:\n");
    char name[256];
    gethostname(name, sizeof(name));
    hostent *he = gethostbyname(name);
    for (int i = 0; he && he->h_addr_list[i]; i++)
        printf("  http://%s:%d\n", inet_ntoa(*(in_addr *)he->h_addr_list[i]), PORT);
    printf("Press Ctrl+C to stop.\n\n");
    fflush(stdout);

    // Mouse-on-iPod: wake-up socket (hook thread -> this loop) and the hook thread itself.
    g_localIps.push_back("127.0.0.1");
    for (int i = 0; he && he->h_addr_list[i]; i++) g_localIps.push_back(inet_ntoa(*(in_addr *)he->h_addr_list[i]));
    InitializeCriticalSection(&gm.cs);
    SOCKET wakeRx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in wa = {};
    wa.sin_family = AF_INET;
    wa.sin_addr.s_addr = inet_addr("127.0.0.1");
    bind(wakeRx, (sockaddr *)&wa, sizeof(wa)); // port 0 = any free port
    int wl = sizeof(gm.wakeAddr);
    getsockname(wakeRx, (sockaddr *)&gm.wakeAddr, &wl);
    u_long nonBlocking = 1;
    ioctlsocket(wakeRx, FIONBIO, &nonBlocking);
    gm.wakeTx = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    CreateThread(nullptr, 0, hookThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, pingThread, nullptr, 0, nullptr);

    // Several connections at once: browsers open spare idle sockets, which must not block others.
    std::vector<Client> clients;
    std::vector<Stream> streams; // open /cursor connections
    unsigned sentVer = 0;
    DWORD lastCursorSend = 0;
    DWORD next = GetTickCount() + 1000;
    for (;;) {
        DWORD now = GetTickCount();
        long wait = (long)(next - now);
        if (wait <= 0) {
            bool idle = now - g_lastStatsPoll > 15000;
            if (idle != g_idle) {
                g_idle = idle;
                std::string flag = dir + "idle.flag";
                if (idle) { FILE *f = fopen(flag.c_str(), "w"); if (f) fclose(f); }
                else DeleteFileA(flag.c_str());
            }
            if (!idle) st.sample();
            static unsigned helperTick = 0;
            if (++helperTick % 10 == 0) keepHelperAlive(dir); // every 10 s
            next += 1000;
            if ((long)(next - GetTickCount()) < 0) next = GetTickCount() + 1000;
            continue;
        }

        // ---- cursor stream: one short line per change, at most ~60 a second, plus a heartbeat each second ----
        bool remote = false;
        g_wsStreams = g_xhrStreams = 0; // other devices only; reported in /stats so it is easy to see which channel the iPod got
        for (size_t i = 0; i < streams.size(); i++) {
            if (!streams[i].remote) continue;
            remote = true;
            if (streams[i].ws) g_wsStreams++; else g_xhrStreams++;
        }
        EnterCriticalSection(&gm.cs);
        gm.listening = remote && now - g_lastRemoteStats < 3500;
        unsigned ver = gm.version, downs = gm.downs, ups = gm.ups;
        int on = gm.captured ? 1 : 0, cx = (int)gm.x, cy = (int)gm.y;
        LeaveCriticalSection(&gm.cs);
        if (streams.empty()) sentVer = ver;
        else {
            DWORD since = now - lastCursorSend;
            if ((ver != sentVer && since >= 16) || since >= 1000) {
                // Same text either way; a WebSocket frame is 0x00 text 0xFF.
                // The mouse is on the iPod, not on a browser tab of this PC that happens to show the page:
                // a tab here only gets "no cursor" heartbeats. Otherwise it would replay every click too
                // and send each command a second time (play/pause twice = nothing happens).
                char line[64], frame[64], idleLine[] = "0 0 0 0 0\n", idleFrame[12];
                int n = snprintf(line, sizeof(line), "%d %d %d %u %u\n", on, cx, cy, downs, ups);
                frame[0] = 0;
                memcpy(frame + 1, line, n - 1);
                frame[n] = (char)0xFF;
                idleFrame[0] = 0;
                memcpy(idleFrame + 1, idleLine, 9);
                idleFrame[10] = (char)0xFF;
                for (size_t i = 0; i < streams.size();) {
                    bool real = streams[i].remote;
                    const char *data = streams[i].ws ? (real ? frame : idleFrame) : (real ? line : idleLine);
                    int len = streams[i].ws ? (real ? n + 1 : 11) : (real ? n : 10);
                    int r = send(streams[i].s, data, len, 0);
                    bool full = r == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK; // iPod busy: skip this one whole
                    if ((r == SOCKET_ERROR && !full) || (r >= 0 && r < len)) {
                        closesocket(streams[i].s);
                        streams.erase(streams.begin() + i);
                    } else i++;
                }
                sentVer = ver;
                lastCursorSend = now;
                since = 0;
            }
            long cwait = ver != sentVer ? 16 - (long)since : 1000 - (long)since;
            if (cwait < 1) cwait = 1;
            if (cwait < wait) wait = cwait;
        }

        // Drop sockets that sent nothing useful for 10 s.
        for (size_t i = 0; i < clients.size();) {
            if (now - clients[i].since > 10000) {
                closesocket(clients[i].s);
                clients.erase(clients.begin() + i);
            } else i++;
        }

        fd_set fs;
        FD_ZERO(&fs);
        FD_SET(ls, &fs);
        FD_SET(wakeRx, &fs);
        for (size_t i = 0; i < clients.size() && i < 40; i++) FD_SET(clients[i].s, &fs);
        for (size_t i = 0; i < streams.size() && i < 16; i++) FD_SET(streams[i].s, &fs);
        timeval tv = {wait / 1000, (wait % 1000) * 1000};
        if (select(0, &fs, nullptr, nullptr, &tv) <= 0) continue;

        if (FD_ISSET(wakeRx, &fs)) { // the mouse did something: loop round and send it
            char junk[16];
            while (recv(wakeRx, junk, sizeof(junk), 0) > 0) {}
            InterlockedExchange(&gm.wakePending, 0);
        }

        // A stream socket only becomes readable when the page closed it.
        for (size_t i = 0; i < streams.size();) {
            char junk[64];
            if (FD_ISSET(streams[i].s, &fs) && recv(streams[i].s, junk, sizeof(junk), 0) <= 0 && WSAGetLastError() != WSAEWOULDBLOCK) {
                closesocket(streams[i].s);
                streams.erase(streams.begin() + i);
            } else i++;
        }

        if (FD_ISSET(ls, &fs)) {
            sockaddr_in from;
            int fl = sizeof(from);
            SOCKET c = accept(ls, (sockaddr *)&from, &fl);
            if (c != INVALID_SOCKET && !allowedDevice(inet_ntoa(from.sin_addr), dir)) {
                const char *no = "HTTP/1.1 403 Forbidden\r\nContent-Type: text/plain\r\nContent-Length: 25\r\nConnection: close\r\n\r\nThis device isn't allowed";
                send(c, no, (int)strlen(no), 0);
                closesocket(c);
            } else if (c != INVALID_SOCKET) {
                Client cl;
                cl.s = c;
                cl.since = GetTickCount();
                strncpy(cl.ip, inet_ntoa(from.sin_addr), sizeof(cl.ip) - 1);
                cl.ip[sizeof(cl.ip) - 1] = 0;
                clients.push_back(cl);
            }
        }

        for (size_t i = 0; i < clients.size();) {
            Client &cl = clients[i];
            bool done = false;
            int keep = 0;
            if (FD_ISSET(cl.s, &fs)) {
                char b[2048];
                int n = recv(cl.s, b, sizeof(b), 0);
                if (n <= 0) done = true;
                else {
                    cl.buf.append(b, n);
                    size_t end = cl.buf.find("\r\n\r\n");
                    // An old-style WebSocket handshake has 8 more bytes after the headers: wait for them.
                    bool wsWait = end != std::string::npos && cl.buf.find("Sec-WebSocket-Key1") != std::string::npos && cl.buf.size() < end + 12;
                    if ((end != std::string::npos && !wsWait) || cl.buf.size() > 8192) {
                        keep = handle(cl.s, cl.buf, cl.ip, st, dir);
                        done = true;
                    }
                }
            }
            if (keep) { // now a cursor stream: small packets must leave at once, and a slow iPod must never block us
                BOOL noDelay = TRUE;
                setsockopt(cl.s, IPPROTO_TCP, TCP_NODELAY, (const char *)&noDelay, sizeof(noDelay));
                ioctlsocket(cl.s, FIONBIO, &nonBlocking);
                Stream s = {cl.s, !isLocal(cl.ip), keep == 2};
                streams.push_back(s);
                sentVer = ~0u; // send the current state to the newcomer right away
            } else if (done) closesocket(cl.s);
            if (done) clients.erase(clients.begin() + i);
            else i++;
        }
    }
}
