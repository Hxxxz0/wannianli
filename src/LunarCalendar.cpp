/******************************************************
 *  LunarCalendar.cpp
 *  公历 → 农历计算（1900-01-31 为农历 1900-正月-初一）并在 TFT_eSPI 上显示
 ******************************************************/

#include "LunarCalendar.h"
#include <stdint.h>
#include "font.cpp"

/*============= ①  农历年数据表 1900-2099 =============*/
static const uint32_t LUNAR_INFO[200] = {
    /* 1900-1999 */
    0x04BD8,0x04AE0,0x0A570,0x054D5,0x0D260,0x0D950,0x16554,0x056A0,0x09AD0,0x055D2,
    0x04AE0,0x0A5B6,0x0A4D0,0x0D250,0x1D255,0x0B540,0x0D6A0,0x0ADA2,0x095B0,0x14977,
    0x04970,0x0A4B0,0x0B4B5,0x06A50,0x06D40,0x1AB54,0x02B60,0x09570,0x052F2,0x04970,
    0x06566,0x0D4A0,0x0EA50,0x06E95,0x05AD0,0x02B60,0x186E3,0x092E0,0x1C8D7,0x0C950,
    0x0D4A0,0x1D8A6,0x0B550,0x056A0,0x1A5B4,0x025D0,0x092D0,0x0D2B2,0x0A950,0x0B557,
    0x06CA0,0x0B550,0x15355,0x04DA0,0x0A5D0,0x14573,0x052D0,0x0A9A8,0x0E950,0x06AA0,
    0x0AEA6,0x0AB50,0x04B60,0x0AAE4,0x0A570,0x05260,0x0F263,0x0D950,0x05B57,0x056A0,
    0x096D0,0x04DD5,0x04AD0,0x0A4D0,0x0D4D4,0x0D250,0x0D558,0x0B540,0x0B5A0,0x195A6,
    0x095B0,0x049B0,0x0A974,0x0A4B0,0x0B27A,0x06A50,0x06D40,0x0AF46,0x0AB60,0x09570,
    0x04AF5,0x04970,0x064B0,0x074A3,0x0EA50,0x06B58,0x05AC0,0x0AB60,0x096D5,0x092E0,
    /* 2000-2099 */
    0x0C960,0x0D954,0x0D4A0,0x0DA50,0x07552,0x056A0,0x0ABB7,0x025D0,0x092D0,0x0CAB5,
    0x0A950,0x0B4A0,0x0BAA4,0x0AD50,0x055D9,0x04BA0,0x0A5B0,0x15176,0x052B0,0x0A930,
    0x07954,0x06AA0,0x0AD50,0x05B52,0x04B60,0x0A6E6,0x0A4E0,0x0D260,0x0EA65,0x0D530,
    0x05AA0,0x076A3,0x096D0,0x04AFB,0x04AD0,0x0A4D0,0x1D0B6,0x0D250,0x0D520,0x0DD45,
    0x0B5A0,0x056D0,0x055B2,0x049B0,0x0A577,0x0A4B0,0x0AA50,0x1B255,0x06D20,0x0ADA0
};

/*============= ②  辅助函数 =============*/
static inline bool isSolarLeap(int y) {
    return ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0));
}

static int solarDaysInMonth(int y, int m) {
    static const uint8_t solarMonthLen[12] =
        {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2 && isSolarLeap(y)) return 29;
    return solarMonthLen[m - 1];
}

// 避免与宏冲突，把 bit() 重命名为 getBit()
static inline uint8_t getBit(uint32_t data, int pos) {
    return (data >> pos) & 0x1;
}





// ① 闰月序号：低 4 位
static int leapMonth(int y) {
    return LUNAR_INFO[y - 1900] & 0xF;
}

// ② 闰月天数：bit16=1 ⇒ 30 天，否则 29 天
static int leapDays(int y) {
    if (leapMonth(y))
        return (LUNAR_INFO[y - 1900] & 0x10000) ? 30 : 29;
    return 0;
}

// ③ 指定月天数（m=1..12）
//    月大小标志位从 bit15 开始，依次右移
static int lunarMonthDays(int y, int m) {
    return (LUNAR_INFO[y - 1900] & (0x10000 >> m)) ? 30 : 29;
}

// ④ 全年天数：先算 29*12 基数，再把 12 bit 大月加 1，再加闰月
static int lunarYearDays(int y) {
    uint32_t info = LUNAR_INFO[y - 1900];
    int days = 348;                     // 29 × 12
    for (uint32_t mask = 0x8000; mask > 0x8; mask >>= 1)
        if (info & mask) ++days;        // 大月多一天
    return days + leapDays(y);
}





/*============= ③  公历 → 农历核心转换 =============*/
struct LunarDate {
    int  year;
    int  month;
    int  day;
    bool isLeap;
};

static LunarDate solarToLunar(int y, int m, int d) {
    long offset = 0;
    for (int i = 1900; i < y; ++i)
        offset += isSolarLeap(i) ? 366 : 365;
    for (int i = 1; i < m; ++i)
        offset += solarDaysInMonth(y, i);
    offset += (d - 31);

    int lunarY     = 1900;
    int daysOfYear = lunarYearDays(lunarY);
    while (offset >= daysOfYear) {
        offset -= daysOfYear;
        ++lunarY;
        daysOfYear = lunarYearDays(lunarY);
    }

    int leap        = leapMonth(lunarY);
    bool isLeap     = false;
    int lunarM      = 1;
    int daysOfMonth = lunarMonthDays(lunarY, lunarM);

    while (offset >= daysOfMonth) {
        offset -= daysOfMonth;
        if (leap && lunarM == leap) {
            if (!isLeap) {
                isLeap = true;
            } else {
                isLeap = false;
                ++lunarM;
            }
        } else {
            ++lunarM;
        }
        daysOfMonth = lunarMonthDays(lunarY, isLeap ? 13 : lunarM);
    }

    LunarDate res;
    res.year   = lunarY;
    res.month  = lunarM;
    res.day    = offset + 1;
    res.isLeap = isLeap;
    return res;
}

/******************************************************
 * ④ 对外接口：显示农历日期
 ******************************************************/
void displayLunarDate(int year, int month, int day)
{
    LunarDate lunar = solarToLunar(year, month, day);
    displayLunarYear(lunar.year);

    int x = 110, y = 115;
    if (lunar.isLeap) {
        mylcd.pushImage(x, y, 14, 14, run);
        x += 20;
    }

    switch (lunar.month) {
        case 1:  mylcd.pushImage(x,y,14,14,yi);   break;
        case 2:  mylcd.pushImage(x,y,14,14,er);   break;
        case 3:  mylcd.pushImage(x,y,14,14,san);  break;
        case 4:  mylcd.pushImage(x,y,14,14,si);   break;
        case 5:  mylcd.pushImage(x,y,14,14,wu);   break;
        case 6:  mylcd.pushImage(x,y,14,14,liu);  break;
        case 7:  mylcd.pushImage(x,y,14,14,qi);   break;
        case 8:  mylcd.pushImage(x,y,14,14,ba);   break;
        case 9:  mylcd.pushImage(x,y,14,14,jiu);  break;
        case 10: mylcd.pushImage(x,y,14,14,shii); break;
        case 11: mylcd.pushImage(x,y,14,14,dong); break;
        case 12: mylcd.pushImage(x,y,14,14,la);   break;
    }
    x += 20;
    mylcd.pushImage(x, y, 14, 14, yue);

    x += 30;
    int ld = lunar.day;
    const uint16_t* nums[] =
        {nullptr, yi, er, san, si, wu, liu, qi, ba, jiu, shii};

    if (ld <= 10) {
        mylcd.pushImage(x,y,14,14,chu); x += 20;
        mylcd.pushImage(x,y,14,14,nums[ld]);
    }
    else if (ld < 20) {
        mylcd.pushImage(x,y,14,14,shii); x += 20;
        if (ld % 10) mylcd.pushImage(x,y,14,14,nums[ld % 10]);
    }
    else if (ld == 20) {
        mylcd.pushImage(x,y,14,14,er);   x += 20;
        mylcd.pushImage(x,y,14,14,shii);
    }
    else if (ld < 30) {
        mylcd.pushImage(x,y,14,14,nian); x += 20;
        //mylcd.pushImage(x,y,14,14,shii); x+=20;
        mylcd.pushImage(x,y,14,14,nums[ld % 10]);
    }
    else {
        mylcd.pushImage(x,y,14,14,san);  x += 20;
        mylcd.pushImage(x,y,14,14,shii);
    }
}
