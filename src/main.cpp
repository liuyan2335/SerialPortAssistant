// 串口调试助手（C++ Win32 上位机 Demo）
// 技术要点：
//   1. 纯 Win32 API 界面（无第三方库，g++/MinGW 即可编译）
//   2. 串口通信：CreateFile + DCB 参数配置 + COMMTIMEOUTS 超时控制
//   3. 多线程：独立读线程阻塞接收，通过 PostMessage 把数据安全投递回 UI 线程
//   4. 支持文本 / HEX 两种收发模式、时间戳、定时发送、收发字节统计

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WINVER 0x0601        // Vista 以上：使用 SetProcessDPIAware
#define _WIN32_WINNT 0x0601

#include <windows.h>
#include <commctrl.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

#include "resource.h"
#include "serial_port.h"

namespace {

constexpr wchar_t kClassName[] = L"SerialAssistantMainWnd";
constexpr int kMargin = 10;        // 边距
constexpr int kLeftW = 210;        // 左侧设置列宽度
constexpr int kRightX = 230;       // 右侧区域起点 X
constexpr int kSendH = 190;        // 发送区高度
constexpr int kStatusH = 24;       // 状态栏高度
constexpr int kMaxRecvChars = 500 * 1024;  // 接收框字符上限，防止无限增长

HINSTANCE g_hInst = nullptr;
HWND g_mainWnd = nullptr;
HFONT g_hFont = nullptr;

HANDLE g_hPort = INVALID_HANDLE_VALUE;   // 串口句柄
HANDLE g_hReadThread = nullptr;          // 接收线程句柄
std::atomic<bool> g_quitting{false};     // 接收线程退出标志

long long g_rxCount = 0;                 // 累计接收字节数
long long g_txCount = 0;                 // 累计发送字节数
std::wstring g_stateText = L"串口未打开";

// 所有控件句柄
struct Controls {
    HWND port, baud, dataBits, parity, stopBits;
    HWND open, refresh;
    HWND recv, clearRecv;
    HWND hexRecv, timestamp, autoScroll;
    HWND send, sendBtn, hexSend, autoSend, interval;
    HWND recvGroup, sendGroup;   // 分组框（需随窗口缩放）
    HWND status;
} g_ctl;

// ==================== 小工具函数 ====================

HWND CreateCtl(const wchar_t* cls, const wchar_t* text, DWORD style,
               int x, int y, int w, int h, HWND parent, int id) {
    return CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                           x, y, w, h, parent,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                           g_hInst, nullptr);
}

bool IsChecked(HWND h) {
    return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

int ComboSel(HWND h) {
    int i = static_cast<int>(SendMessageW(h, CB_GETCURSEL, 0, 0));
    return i < 0 ? 0 : i;
}

// 字节数组转 "01 02 3F " 形式
std::wstring BytesToHex(const BYTE* data, DWORD len) {
    static const wchar_t kHex[] = L"0123456789ABCDEF";
    std::wstring out;
    out.reserve(len * 3);
    for (DWORD i = 0; i < len; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0F]);
        out.push_back(L' ');
    }
    return out;
}

// 解析 HEX 文本（支持 "01 02"、"0x3F"、逗号分隔），失败返回 false
bool ParseHex(const std::wstring& text, std::vector<BYTE>& out) {
    out.clear();
    std::wstring s;
    for (size_t i = 0; i < text.size(); ++i) {   // 先剔除 0x 前缀
        if (text[i] == L'0' && i + 1 < text.size() &&
            (text[i + 1] == L'x' || text[i + 1] == L'X')) {
            ++i;
            continue;
        }
        s.push_back(text[i]);
    }
    int hi = -1;
    for (wchar_t c : s) {
        if (c == L' ' || c == L',' || c == L'\r' || c == L'\n' || c == L'\t') {
            continue;
        }
        int v;
        if (c >= L'0' && c <= L'9')      v = c - L'0';
        else if (c >= L'a' && c <= L'f') v = c - L'a' + 10;
        else if (c >= L'A' && c <= L'F') v = c - L'A' + 10;
        else return false;
        if (hi == -1) {
            hi = v;
        } else {
            out.push_back(static_cast<BYTE>((hi << 4) | v));
            hi = -1;
        }
    }
    return hi == -1;   // 半字节残留说明输入不完整
}

// ==================== 状态栏 ====================

void UpdateStatus() {
    wchar_t buf[256];
    swprintf(buf, 256, L"  %ls", g_stateText.c_str());
    SendMessageW(g_ctl.status, SB_SETTEXT, 0,
                 reinterpret_cast<LPARAM>(buf));
    swprintf(buf, 256, L" RX: %lld 字节    TX: %lld 字节  ",
             g_rxCount, g_txCount);
    SendMessageW(g_ctl.status, SB_SETTEXT, 1,
                 reinterpret_cast<LPARAM>(buf));
}

// ==================== 串口开关 ====================

// 接收线程：阻塞读串口，把数据投递回 UI 线程
DWORD WINAPI ReadThreadProc(LPVOID param) {
    HANDLE hPort = static_cast<HANDLE>(param);
    BYTE buf[4096];
    while (!g_quitting.load()) {
        DWORD n = 0;
        // 设置了 100ms 读超时：无数据时周期性返回，能及时响应退出标志
        if (!ReadFile(hPort, buf, sizeof(buf), &n, nullptr)) {
            break;   // 串口关闭或出错，线程退出
        }
        if (n == 0) {
            continue;
        }
        BYTE* copy = new BYTE[n];          // 堆上分配，UI 线程负责释放
        memcpy(copy, buf, n);
        PostMessageW(g_mainWnd, WM_SERIAL_DATA,
                     static_cast<WPARAM>(n),
                     reinterpret_cast<LPARAM>(copy));
    }
    return 0;
}

void EnableSettings(bool enable) {
    const HWND list[] = {g_ctl.port, g_ctl.baud, g_ctl.dataBits,
                         g_ctl.parity, g_ctl.stopBits, g_ctl.refresh};
    for (HWND h : list) {
        EnableWindow(h, enable);
    }
}

void TogglePort() {
    if (g_hPort == INVALID_HANDLE_VALUE) {
        // ---------- 打开 ----------
        wchar_t buf[64];
        GetWindowTextW(g_ctl.port, buf, 64);
        std::wstring port = buf;
        if (port.empty()) {
            MessageBoxW(g_mainWnd, L"请先选择串口", L"提示",
                        MB_OK | MB_ICONINFORMATION);
            return;
        }
        GetWindowTextW(g_ctl.baud, buf, 64);
        DWORD baud = static_cast<DWORD>(wcstoul(buf, nullptr, 10));

        const BYTE kDataBits[] = {5, 6, 7, 8};
        const BYTE kParity[] = {NOPARITY, ODDPARITY, EVENPARITY,
                                MARKPARITY, SPACEPARITY};
        const BYTE kStopBits[] = {ONESTOPBIT, ONE5STOPBITS, TWOSTOPBITS};

        HANDLE h = OpenSerialPort(port, baud,
                                  kDataBits[ComboSel(g_ctl.dataBits)],
                                  kParity[ComboSel(g_ctl.parity)],
                                  kStopBits[ComboSel(g_ctl.stopBits)]);
        if (h == INVALID_HANDLE_VALUE) {
            swprintf(buf, 64, L"打开 %ls 失败（错误码 %lu）",
                     port.c_str(), GetLastError());
            MessageBoxW(g_mainWnd, buf, L"错误", MB_OK | MB_ICONERROR);
            return;
        }
        g_hPort = h;
        g_quitting.store(false);
        g_hReadThread = CreateThread(nullptr, 0, ReadThreadProc, h, 0, nullptr);
        SetWindowTextW(g_ctl.open, L"关闭串口");
        swprintf(buf, 64, L"%ls 已打开 @%lu", port.c_str(), baud);
        g_stateText = buf;
        EnableSettings(false);
    } else {
        // ---------- 关闭 ----------
        g_quitting.store(true);
        if (g_hReadThread) {
            WaitForSingleObject(g_hReadThread, 2000);  // 等待读线程退出
            CloseHandle(g_hReadThread);
            g_hReadThread = nullptr;
        }
        CloseHandle(g_hPort);
        g_hPort = INVALID_HANDLE_VALUE;
        SetWindowTextW(g_ctl.open, L"打开串口");
        g_stateText = L"串口已关闭";
        EnableSettings(true);
    }
    UpdateStatus();
}

// ==================== 收发逻辑 ====================

void AppendRecvText(const std::wstring& text) {
    std::wstring line = text;
    if (IsChecked(g_ctl.timestamp)) {   // 时间戳模式：块首加时间并换行
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t ts[32];
        wsprintfW(ts, L"[%02d:%02d:%02d.%03d] ",
                  st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        line = ts + line;
        if (line.empty() || line.back() != L'\n') {
            line += L"\r\n";
        }
    }
    // 超出上限时清空，防止界面卡顿与内存增长
    if (GetWindowTextLengthW(g_ctl.recv) +
            static_cast<int>(line.size()) > kMaxRecvChars) {
        SetWindowTextW(g_ctl.recv, L"");
    }
    SendMessageW(g_ctl.recv, EM_SETSEL, 0x7FFFFFFF, 0x7FFFFFFF);
    SendMessageW(g_ctl.recv, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(line.c_str()));
    if (IsChecked(g_ctl.autoScroll)) {
        SendMessageW(g_ctl.recv, EM_SCROLLCARET, 0, 0);
    }
}

// 读取发送框内容并发送；silent=true 时（定时发送）失败不弹窗
void SendFromEdit(bool silent) {
    if (g_hPort == INVALID_HANDLE_VALUE) {
        if (!silent) {
            MessageBoxW(g_mainWnd, L"请先打开串口", L"提示",
                        MB_OK | MB_ICONINFORMATION);
        }
        return;
    }
    int len = GetWindowTextLengthW(g_ctl.send);
    std::wstring text(static_cast<size_t>(len), L'\0');
    GetWindowTextW(g_ctl.send, &text[0], len + 1);

    std::vector<BYTE> data;
    if (IsChecked(g_ctl.hexSend)) {
        if (!ParseHex(text, data) || data.empty()) {
            if (!silent) {
                MessageBoxW(g_mainWnd, L"HEX 格式不正确（示例：01 02 0x3F 4A）",
                            L"提示", MB_OK | MB_ICONWARNING);
            }
            return;
        }
    } else {
        if (text.empty()) {
            return;
        }
        int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1,
                                    nullptr, 0, nullptr, nullptr);
        std::vector<char> utf8(static_cast<size_t>(n));
        WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1,
                            utf8.data(), n, nullptr, nullptr);
        data.assign(utf8.begin(), utf8.end() - 1);  // 去掉结尾 '\0'
    }

    if (WriteSerialData(g_hPort, data.data(),
                        static_cast<DWORD>(data.size()))) {
        g_txCount += data.size();
        UpdateStatus();
    } else if (!silent) {
        MessageBoxW(g_mainWnd, L"发送失败，串口可能已被移除", L"错误",
                    MB_OK | MB_ICONERROR);
    }
}

void RefreshPorts() {
    SendMessageW(g_ctl.port, CB_RESETCONTENT, 0, 0);
    for (const auto& p : EnumSerialPorts()) {
        SendMessageW(g_ctl.port, CB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(p.c_str()));
    }
    if (SendMessageW(g_ctl.port, CB_GETCOUNT, 0, 0) > 0) {
        SendMessageW(g_ctl.port, CB_SETCURSEL, 0, 0);
    }
}

// ==================== 界面布局 ====================

void LayoutControls(int cx, int cy) {
    int recvH = cy - kStatusH - kMargin - kSendH - kMargin - kMargin;
    if (recvH < 120) recvH = 120;
    int sendTop = kMargin + recvH + kMargin;

    // 分组框
    MoveWindow(g_ctl.recvGroup, kRightX, kMargin,
               cx - kRightX - kMargin, recvH, TRUE);
    MoveWindow(g_ctl.sendGroup, kRightX, sendTop,
               cx - kRightX - kMargin, kSendH, TRUE);
    // 接收区
    MoveWindow(g_ctl.recv, kRightX + kMargin, kMargin + 20,
               cx - kRightX - 2 * kMargin, recvH - 50, TRUE);
    MoveWindow(g_ctl.clearRecv, kRightX + kMargin, kMargin + recvH - 28,
               90, 24, TRUE);
    // 发送区
    MoveWindow(g_ctl.send, kRightX + kMargin, sendTop + 20,
               cx - kRightX - 2 * kMargin, 92, TRUE);
    MoveWindow(g_ctl.sendBtn, kRightX + kMargin, sendTop + 126, 90, 30, TRUE);
    MoveWindow(g_ctl.hexSend, kRightX + 110, sendTop + 132, 90, 20, TRUE);
    MoveWindow(g_ctl.autoSend, kRightX + 210, sendTop + 132, 90, 20, TRUE);
    MoveWindow(g_ctl.interval, kRightX + 310, sendTop + 128, 70, 24, TRUE);
    // 状态栏
    int parts[2] = {cx - 260, -1};
    SendMessageW(g_ctl.status, SB_SETPARTS, 2,
                 reinterpret_cast<LPARAM>(parts));
    SendMessageW(g_ctl.status, WM_SIZE, 0, 0);
}

// 把所有子控件字体统一为微软雅黑
BOOL CALLBACK SetChildFontProc(HWND hWnd, LPARAM lParam) {
    SendMessageW(hWnd, WM_SETFONT, static_cast<WPARAM>(lParam), TRUE);
    return TRUE;
}

// ==================== 窗口过程 ====================

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        g_mainWnd = hWnd;
        // 左侧：串口设置
        CreateCtl(L"BUTTON", L"串口设置", BS_GROUPBOX, kMargin, kMargin,
                  kLeftW, 192, hWnd, 0);
        CreateCtl(L"STATIC", L"串口", SS_LEFT, kMargin + 12, kMargin + 24,
                  30, 18, hWnd, 0);
        g_ctl.port = CreateCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST,
                               kMargin + 48, kMargin + 20, 150, 200, hWnd, IDC_PORT);
        CreateCtl(L"STATIC", L"波特率", SS_LEFT, kMargin + 12, kMargin + 54,
                  45, 18, hWnd, 0);
        g_ctl.baud = CreateCtl(L"COMBOBOX", L"", CBS_DROPDOWN,
                               kMargin + 48, kMargin + 50, 150, 200, hWnd, IDC_BAUD);
        CreateCtl(L"STATIC", L"数据位", SS_LEFT, kMargin + 12, kMargin + 84,
                  45, 18, hWnd, 0);
        g_ctl.dataBits = CreateCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST,
                                   kMargin + 48, kMargin + 80, 60, 200, hWnd, IDC_DATABITS);
        CreateCtl(L"STATIC", L"校验位", SS_LEFT, kMargin + 116, kMargin + 84,
                  45, 18, hWnd, 0);
        g_ctl.parity = CreateCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST,
                                 kMargin + 150, kMargin + 80, 48, 200, hWnd, IDC_PARITY);
        CreateCtl(L"STATIC", L"停止位", SS_LEFT, kMargin + 12, kMargin + 114,
                  45, 18, hWnd, 0);
        g_ctl.stopBits = CreateCtl(L"COMBOBOX", L"", CBS_DROPDOWNLIST,
                                   kMargin + 48, kMargin + 110, 60, 200, hWnd, IDC_STOPBITS);
        g_ctl.open = CreateCtl(L"BUTTON", L"打开串口", BS_PUSHBUTTON,
                               kMargin + 12, kMargin + 148, 110, 30, hWnd, IDC_OPEN);
        g_ctl.refresh = CreateCtl(L"BUTTON", L"刷新", BS_PUSHBUTTON,
                                  kMargin + 130, kMargin + 148, 68, 30, hWnd, IDC_REFRESH);

        // 左侧：接收设置
        CreateCtl(L"BUTTON", L"接收设置", BS_GROUPBOX, kMargin, kMargin + 202,
                  kLeftW, 100, hWnd, 0);
        g_ctl.hexRecv = CreateCtl(L"BUTTON", L"HEX 显示", BS_AUTOCHECKBOX,
                                  kMargin + 12, kMargin + 226, 90, 20, hWnd, IDC_HEX_RECV);
        g_ctl.timestamp = CreateCtl(L"BUTTON", L"时间戳", BS_AUTOCHECKBOX,
                                    kMargin + 12, kMargin + 252, 90, 20, hWnd, IDC_TIMESTAMP);
        g_ctl.autoScroll = CreateCtl(L"BUTTON", L"自动滚动", BS_AUTOCHECKBOX,
                                     kMargin + 12, kMargin + 278, 90, 20, hWnd, IDC_AUTOSCROLL);
        SendMessageW(g_ctl.autoScroll, BM_SETCHECK, BST_CHECKED, 0);

        // 右侧：接收区
        g_ctl.recvGroup = CreateCtl(L"BUTTON", L"接收区", BS_GROUPBOX,
                                    kRightX, kMargin, 400, 300, hWnd, 0);
        g_ctl.recv = CreateCtl(L"EDIT", L"",
                               ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY |
                               WS_VSCROLL | WS_HSCROLL | ES_LEFT,
                               kRightX + kMargin, kMargin + 20, 380, 250,
                               hWnd, IDC_RECV);
        g_ctl.clearRecv = CreateCtl(L"BUTTON", L"清空接收", BS_PUSHBUTTON,
                                    kRightX + kMargin, kMargin + 272, 90, 24,
                                    hWnd, IDC_CLEAR_RECV);

        // 右侧：发送区
        g_ctl.sendGroup = CreateCtl(L"BUTTON", L"发送区", BS_GROUPBOX,
                                    kRightX, kMargin + 310, 400, kSendH,
                                    hWnd, 0);
        g_ctl.send = CreateCtl(L"EDIT", L"",
                               ES_MULTILINE | ES_AUTOVSCROLL |
                               WS_VSCROLL | ES_LEFT,
                               kRightX + kMargin, kMargin + 330, 380, 92,
                               hWnd, IDC_SEND);
        g_ctl.sendBtn = CreateCtl(L"BUTTON", L"发送", BS_PUSHBUTTON,
                                  kRightX + kMargin, kMargin + 436, 90, 30,
                                  hWnd, IDC_SEND_BTN);
        g_ctl.hexSend = CreateCtl(L"BUTTON", L"HEX 发送", BS_AUTOCHECKBOX,
                                  kRightX + 110, kMargin + 442, 90, 20,
                                  hWnd, IDC_HEX_SEND);
        g_ctl.autoSend = CreateCtl(L"BUTTON", L"定时发送", BS_AUTOCHECKBOX,
                                   kRightX + 210, kMargin + 442, 90, 20,
                                   hWnd, IDC_AUTO_SEND);
        g_ctl.interval = CreateCtl(L"EDIT", L"1000",
                                   ES_NUMBER | ES_CENTER | WS_BORDER,
                                   kRightX + 310, kMargin + 438, 70, 24,
                                   hWnd, IDC_INTERVAL);

        // 状态栏
        g_ctl.status = CreateStatusWindowW(
            WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, L"", hWnd, IDC_STATUS);

        // 填充下拉框
        RefreshPorts();
        const wchar_t* bauds[] = {L"2400", L"4800", L"9600", L"19200",
                                  L"38400", L"57600", L"115200",
                                  L"230400", L"460800", L"921600"};
        for (const wchar_t* b : bauds) {
            SendMessageW(g_ctl.baud, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(b));
        }
        SetWindowTextW(g_ctl.baud, L"9600");
        for (const wchar_t* s : {L"5", L"6", L"7", L"8"}) {
            SendMessageW(g_ctl.dataBits, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(s));
        }
        SendMessageW(g_ctl.dataBits, CB_SETCURSEL, 3, 0);   // 默认 8
        for (const wchar_t* s : {L"无", L"奇", L"偶", L"标记", L"空格"}) {
            SendMessageW(g_ctl.parity, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(s));
        }
        SendMessageW(g_ctl.parity, CB_SETCURSEL, 0, 0);     // 默认无
        for (const wchar_t* s : {L"1", L"1.5", L"2"}) {
            SendMessageW(g_ctl.stopBits, CB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(s));
        }
        SendMessageW(g_ctl.stopBits, CB_SETCURSEL, 0, 0);   // 默认 1

        // 统一字体
        EnumChildWindows(hWnd, SetChildFontProc,
                         reinterpret_cast<LPARAM>(g_hFont));

        RECT rc;
        GetClientRect(hWnd, &rc);
        LayoutControls(rc.right, rc.bottom);
        UpdateStatus();
        return 0;
    }

    case WM_COMMAND: {
        switch (LOWORD(wParam)) {
        case IDC_OPEN:
            TogglePort();
            break;
        case IDC_REFRESH:
            RefreshPorts();
            break;
        case IDC_SEND_BTN:
            SendFromEdit(false);
            break;
        case IDC_CLEAR_RECV:
            SetWindowTextW(g_ctl.recv, L"");
            break;
        case IDC_AUTO_SEND:
            if (IsChecked(g_ctl.autoSend)) {
                wchar_t buf[16];
                GetWindowTextW(g_ctl.interval, buf, 16);
                UINT ms = static_cast<UINT>(wcstoul(buf, nullptr, 10));
                if (ms < 10) ms = 10;
                SetTimer(hWnd, 1, ms, nullptr);
            } else {
                KillTimer(hWnd, 1);
            }
            break;
        }
        return 0;
    }

    case WM_TIMER:   // 定时发送
        if (wParam == 1) {
            SendFromEdit(true);
        }
        return 0;

    case WM_SERIAL_DATA: {
        // 读线程投递的数据：wParam=长度，lParam=缓冲区（用后释放）
        DWORD n = static_cast<DWORD>(wParam);
        BYTE* data = reinterpret_cast<BYTE*>(lParam);
        g_rxCount += n;
        std::wstring text;
        if (IsChecked(g_ctl.hexRecv)) {
            text = BytesToHex(data, n);
        } else {
            int wlen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                           reinterpret_cast<LPCCH>(data),
                                           static_cast<int>(n), nullptr, 0);
            UINT cp = (wlen > 0) ? CP_UTF8 : CP_ACP;   // UTF-8 失败退回 ANSI
            wlen = MultiByteToWideChar(cp, 0, reinterpret_cast<LPCCH>(data),
                                       static_cast<int>(n), nullptr, 0);
            std::wstring w(static_cast<size_t>(wlen), L'\0');
            MultiByteToWideChar(cp, 0, reinterpret_cast<LPCCH>(data),
                                static_cast<int>(n), &w[0], wlen);
            text = w;
        }
        AppendRecvText(text);
        UpdateStatus();
        delete[] data;
        return 0;
    }

    case WM_SIZE: {
        RECT rc;
        GetClientRect(hWnd, &rc);
        LayoutControls(rc.right, rc.bottom);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = 720;
        mmi->ptMinTrackSize.y = 500;
        return 0;
    }

    case WM_DESTROY:
        if (g_hPort != INVALID_HANDLE_VALUE) {
            g_quitting.store(true);
            if (g_hReadThread) {
                WaitForSingleObject(g_hReadThread, 2000);
                CloseHandle(g_hReadThread);
            }
            CloseHandle(g_hPort);
        }
        KillTimer(hWnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

}  // namespace

// ==================== 程序入口 ====================

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int nCmdShow) {
    g_hInst = hInstance;
    SetProcessDPIAware();   // 高分屏下界面不模糊

    INITCOMMONCONTROLSEX icc = {sizeof(icc),
                                ICC_WIN95_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);

    g_hFont = CreateFontW(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                          DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH, L"Microsoft YaHei UI");

    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc)) {
        return 1;
    }

    HWND hWnd = CreateWindowExW(
        0, kClassName, L"串口调试助手（C++ Win32 Demo）",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        720, 560, nullptr, nullptr, hInstance, nullptr);
    if (!hWnd) {
        return 1;
    }

    ShowWindow(hWnd, nCmdShow);
    UpdateWindow(hWnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DeleteObject(g_hFont);
    return static_cast<int>(msg.wParam);
}
