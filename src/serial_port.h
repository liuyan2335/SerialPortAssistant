// 串口通信封装：枚举、打开、写入
#pragma once

#include <windows.h>
#include <string>
#include <vector>

// 枚举系统当前可用的串口号（读取注册表 HARDWARE\DEVICEMAP\SERIALCOMM）
std::vector<std::wstring> EnumSerialPorts();

// 打开串口并配置参数
// 参数：端口名（如 COM3）、波特率、数据位、校验位、停止位（取值同 DCB 结构）
// 返回：成功返回句柄；失败返回 INVALID_HANDLE_VALUE（可用 GetLastError 取错误码）
HANDLE OpenSerialPort(const std::wstring& port, DWORD baud,
                      BYTE dataBits, BYTE parity, BYTE stopBits);

// 向串口写入数据（阻塞直到全部写完或出错）
bool WriteSerialData(HANDLE hPort, const BYTE* data, DWORD len);
