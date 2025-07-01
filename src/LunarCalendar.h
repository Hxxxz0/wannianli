#ifndef LUNAR_CALENDAR_H
#define LUNAR_CALENDAR_H

#include <TFT_eSPI.h>     // 引入库头，让编译器认识 TFT_eSPI

// 这里只做“声明”，真正的对象在 main.cpp 中定义
extern TFT_eSPI mylcd;

// 供外部调用的接口
void displayLunarDate(int year, int month, int day);
void displayLunarYear(int lunarYear);   // 如果你需要

#endif
