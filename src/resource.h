// 控件 ID 与自定义消息定义
#pragma once

// 串口设置区
#define IDC_PORT         1001   // 串口号下拉框
#define IDC_BAUD         1002   // 波特率下拉框（可编辑）
#define IDC_DATABITS     1003   // 数据位下拉框
#define IDC_PARITY       1004   // 校验位下拉框
#define IDC_STOPBITS     1005   // 停止位下拉框
#define IDC_OPEN         1006   // 打开/关闭串口按钮
#define IDC_REFRESH      1007   // 刷新串口列表按钮

// 接收区
#define IDC_RECV         1008   // 接收显示编辑框（只读）
#define IDC_CLEAR_RECV   1009   // 清空接收按钮
#define IDC_HEX_RECV     1010   // HEX 显示复选框
#define IDC_TIMESTAMP    1011   // 时间戳复选框
#define IDC_AUTOSCROLL   1012   // 自动滚动复选框

// 发送区
#define IDC_SEND         1013   // 发送内容编辑框
#define IDC_SEND_BTN     1014   // 发送按钮
#define IDC_HEX_SEND     1015   // HEX 发送复选框
#define IDC_AUTO_SEND    1016   // 定时发送复选框
#define IDC_INTERVAL     1017   // 定时发送间隔（毫秒）

// 状态栏
#define IDC_STATUS       1018

// 自定义消息：读线程收到数据后投递给主窗口
// wParam = 数据长度，lParam = 堆上分配的 BYTE* 缓冲区（主线程负责释放）
#define WM_SERIAL_DATA   (WM_USER + 100)
