// 串口通信封装实现
#include "serial_port.h"

#include <vector>

// 枚举串口号：注册表 HARDWARE\DEVICEMAP\SERIALCOMM 下保存了系统识别到的所有串口
std::vector<std::wstring> EnumSerialPorts() {
    std::vector<std::wstring> ports;
    HKEY hKey = nullptr;
    LONG ret = RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                             L"HARDWARE\\DEVICEMAP\\SERIALCOMM",
                             0, KEY_READ, &hKey);
    if (ret != ERROR_SUCCESS) {
        return ports;  // 注册表项不存在时返回空列表
    }

    wchar_t name[256];
    wchar_t value[256];
    for (DWORD i = 0;; ++i) {
        DWORD nameLen = 256;
        DWORD valueLen = sizeof(value);  // RegEnumValue 的长度参数按字节计
        DWORD type = 0;
        ret = RegEnumValueW(hKey, i, name, &nameLen, nullptr, &type,
                            reinterpret_cast<BYTE*>(value), &valueLen);
        if (ret != ERROR_SUCCESS) {
            break;  // 枚举结束
        }
        if (type == REG_SZ) {
            ports.push_back(value);
        }
    }
    RegCloseKey(hKey);
    return ports;
}

// 打开串口：CreateFile + 配置 DCB + 设置超时
HANDLE OpenSerialPort(const std::wstring& port, DWORD baud,
                      BYTE dataBits, BYTE parity, BYTE stopBits) {
    // 串口在 Windows 下的设备路径格式为 \\.\COMx
    std::wstring path = L"\\\\.\\" + port;
    HANDLE hPort = CreateFileW(path.c_str(),
                               GENERIC_READ | GENERIC_WRITE,
                               0,                     // 独占访问
                               nullptr,               // 默认安全属性
                               OPEN_EXISTING,         // 串口必须已存在
                               FILE_ATTRIBUTE_NORMAL,
                               nullptr);
    if (hPort == INVALID_HANDLE_VALUE) {
        return hPort;
    }

    // 扩大收发缓冲区，降低高波特率下丢数据的概率
    SetupComm(hPort, 8192, 8192);

    // 配置串口参数（波特率、数据位、校验位、停止位）
    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(hPort, &dcb)) {
        CloseHandle(hPort);
        return INVALID_HANDLE_VALUE;
    }
    dcb.BaudRate = baud;
    dcb.ByteSize = dataBits;
    dcb.Parity = parity;
    dcb.StopBits = stopBits;
    dcb.fBinary = TRUE;
    dcb.fParity = FALSE;
    if (!SetCommState(hPort, &dcb)) {
        CloseHandle(hPort);
        return INVALID_HANDLE_VALUE;
    }

    // 读超时设置：总超时 100ms，使读线程即使无数据也能周期性返回、
    // 及时响应"关闭串口"的退出标志，而不是永远阻塞在 ReadFile 上
    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutMultiplier = 5;
    timeouts.ReadTotalTimeoutConstant = 100;
    timeouts.WriteTotalTimeoutMultiplier = 5;
    timeouts.WriteTotalTimeoutConstant = 1000;
    SetCommTimeouts(hPort, &timeouts);

    PurgeComm(hPort, PURGE_RXCLEAR | PURGE_TXCLEAR);
    return hPort;
}

// 写入数据：WriteFile 一次调用不保证全部写完，需要循环
bool WriteSerialData(HANDLE hPort, const BYTE* data, DWORD len) {
    DWORD written = 0;
    DWORD total = 0;
    while (total < len) {
        if (!WriteFile(hPort, data + total, len - total, &written, nullptr)) {
            return false;
        }
        total += written;
    }
    return true;
}
