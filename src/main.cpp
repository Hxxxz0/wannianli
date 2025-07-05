#include <Arduino.h>

#include <TFT_eSPI.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <time.h>
#include <SPIFFS.h>
#include <ESPAsyncWebServer.h>
#include <DHT.h>
#include "image.cpp"
#include "font.cpp"
#include "LunarCalendar.h"
#include "driver/i2s.h"
#include <math.h>
#include "AudioFileSourceSPIFFS.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"
#include <WiFiClientSecure.h>   // 新增：用于SMTP邮件发送



/**************** MAX98357 I2S 引脚 ****************/
#define SPK_BCLK 15   // BCLK
#define SPK_LRC  16   // WS / LRC  
#define SPK_DIN  7    // DIN
// SD 引脚请直接接 3.3V（确保放大器启用）

/**************** 火焰传感器引脚与阈值 ****************/
#define PIN_DO 9   // 数字输出 DO
#define PIN_AO 8   // 模拟输出 AO (ADC1)
#define FLAME_THRESHOLD 40   // AO 百分比阈值 (0~100)

// 音频对象 - 单例模式，避免重复创建
AudioGeneratorMP3 *mp3;
AudioFileSourceSPIFFS *musicFile;
AudioOutputI2S *audioOut;

// 统一音频状态管理
enum AudioJob { JOB_NONE, JOB_MUSIC_LOOP, JOB_BEEP_LOOP };
volatile AudioJob audioJob = JOB_NONE;

// DHT11 配置
#define DHTPIN  10      // DHT11 数据引脚
#define DHTTYPE DHT11   // DHT 传感器类型

// DHT11 相关变量
DHT dht(DHTPIN, DHTTYPE);
float dht11_temp = 0;
float dht11_humi = 0;
unsigned long lastDHTRead = 0;
const unsigned long dhtReadInterval = 2000; // 2秒读取一次

// 闹钟相关结构和变量
struct Alarm {
    bool enabled;
    int hour;
    int minute;
    String label;
};

const int MAX_ALARMS = 3;
Alarm alarms[MAX_ALARMS] = {
    {false, 0, 0, "Alarm 1"},
    {false, 0, 0, "Alarm 2"},
    {false, 0, 0, "Alarm 3"}
};

// 闹钟配置文件
const char *alarmsFile = "/alarms.json";

// 事件提醒相关结构和变量
struct Event {
    bool enabled;
    int year;
    int month;
    int day;
    int hour;
    int minute;
    String title;
    String description;
    bool hasTime;    // 是否设置了具体时间
    bool notifyBefore; // 是否提前一天提醒
};

const int MAX_EVENTS = 4;
Event events[MAX_EVENTS] = {
    {false, 2024, 1, 1, 0, 0, "Event 1", "", false, true},
    {false, 2024, 1, 1, 0, 0, "Event 2", "", false, true},
    {false, 2024, 1, 1, 0, 0, "Event 3", "", false, true},
    {false, 2024, 1, 1, 0, 0, "Event 4", "", false, true}
};

// 事件配置文件
const char *eventsFile = "/events.json";

// 当前活动提醒
String currentReminder = "";
unsigned long reminderStartTime = 0;
const unsigned long reminderDuration = 20000; // 提醒显示20秒，让用户有更多时间看到滚动内容

// 定义自定义颜色 (RGB565格式)
#define TFT_GRAY 0x7BEF  // 中等灰色

// 函数声明
void handleWiFiConfig();
void setupWebServer();  // 新增：设置Web服务器
void attemptWiFiConnection(String ssid, String pass);
void startConfigMode();
void loadWiFiConfig();
void fetchWeather();
void checkWiFiConnection();
String getLunarDate(int year, int month, int day);  // 新增：农历计算
void loadDisplaySettings();
void saveDisplaySettings();
void updateDHT11();  // 新增：DHT11更新函数声明
void loadAlarms();   // 新增：加载闹钟配置函数声明
void saveAlarms();   // 新增：保存闹钟配置函数声明
void loadEvents();   // 新增：加载事件配置函数声明
void saveEvents();   // 新增：保存事件配置函数声明
void checkEvents();  // 新增：检查事件提醒函数声明
void ClockInit();    // 新增：时钟初始化函数声明
void showClockPage(); // 新增：时钟显示函数声明
void displayLunarDate(int year, int month, int day);
void displayLunarYear(int year);

// I2S & Alarm function declarations
void initI2S();
void generateTone(int frequency, int durationMs);
void startAlarm();
void stopAlarm();
void checkAlarms();
void startBuzzerTest();
void stopBuzzerTest();
void initMusicPlayer();      // 新增：音乐播放器初始化函数声明
void startMusicLoop();       // 新增：开始音乐循环播放函数声明
void stopMusicLoop();        // 新增：停止音乐播放函数声明
void serviceAudio();         // 新增：统一音频服务函数声明
void audioTask(void *parameter); // 新增：音频任务声明
void checkFlameSensor();     // 新增：火焰传感器检测函数声明
void startFlameAlarm();      // 新增：火焰报警启动函数声明
void stopFlameAlarm();       // 新增：火焰报警停止函数声明
bool sendFlameAlertEmail(const String &subject, const String &body); // 新增：发送火焰警报邮件函数声明

const char *ssidFile = "/ssid.json";
const char *settingsFile = "/settings.json";  // 新增：设置文件
AsyncWebServer server(80);
const char *weatherAPI = "http://api.seniverse.com/v3/weather/daily.json?key=";

// 显示状态变量
bool isLunarDisplay = false;  // false为公历显示，true为农历显示
int weatherpic = 2; // 天气图标选择：0=云，1=雨，2=太阳(默认)
String temperature = "";
String humidity = "";
String weather = "";
int currentState = 3; // 保留原有状态用于内部逻辑
int lastState = 0; // 记录上次的显示状态

// 页面切换相关变量
int currentPage = 0; // 0: 主页面, 1: 详细天气页面, 2: 闹钟页面, 3: 事件提醒页面, 4: 网络信息页面, 5: 模拟时钟页面, 6: 报警页面, 7: 火焰报警页面
int lastPage = -1; // 记录上次页面，用于刷新检测
const int totalPages = 6; // 总页面数（不含报警页面）

String cityname = "qingdao"; // 默认城市设置为青岛
String weatherapi = "SceYi5H8YF5sO9bYA"; // 默认天气API密钥（有效）
static bool initweather = false; // 天气初始化

// 新增：显示设置变量
bool useAmPmFormat = false;  // 是否使用12小时制（AM/PM格式）
bool showLunar = true;       // 是否显示农历
bool autoRotatePages = false; // 是否自动轮转页面
int pageRotateInterval = 10;  // 页面轮转间隔（秒）

// WiFi重连相关变量
unsigned long lastWiFiCheck = 0;
const unsigned long wifiCheckInterval = 30000; // 30秒检查一次WiFi
int wifiFailCount = 0;
const int maxWifiFailCount = 5; // 失败5次后重新开启热点
bool isInConfigMode = false;
bool webServerStarted = false;  // 新增：Web服务器状态

////////////////////////////
// 配置参数
////////////////////////////
const char *ssid = "TV-PRO";
const char *password = "";

////////////////////////////
// 显示任务相关变量
////////////////////////////
TaskHandle_t displayTaskHandle = NULL;

// Alarm ring control
bool alarmRinging = false;
bool flameAlarmActive = false; // 新增：火焰警报状态

// Buzzer test control
bool buzzerTesting = false;

// 新增：音频服务任务句柄
TaskHandle_t audioTaskHandle = NULL;

// 按键引脚
#define BUTTON_MID 21
#define BUTTON_LEFT 39
#define BUTTON_RIGHT 40

TFT_eSPI mylcd = TFT_eSPI();

// API配置参数（简化后主要用于天气）
String apiKey = "";
String apiSecret = "";
String appId = "";

// SMTP 邮件配置
const char* smtpServer = "smtp.163.com";    // SMTP服务器
const int   smtpPort = 465;                 // SSL端口
const char* smtpUser = "15253286380@163.com"; // 发件人邮箱
const char* smtpPassword = "WP7KhS4Y9a5KSnmt"; // 邮箱授权码
const char* emailRecipient = "980228683@qq.com"; // 收件人邮箱
static bool emailSent = false;              // 火警邮件发送状态（避免重复发送）
String emailErrorMsg = "";                 // 邮件发送错误信息

////////////////////////////
// 全局对象及变量
////////////////////////////
WiFiUDP udp;
NTPClient timeClient(udp, "cn.pool.ntp.org", 8*3600, 60000); // 使用中国NTP服务器，时区+8小时
WiFiMulti wifiMulti;

// 时钟页面相关变量
bool initialized = false;
unsigned long total_time = 0;
int hours = 0, mins = 0, secs = 0;
int hours_x = 0, hours_y = 0;
int mins_x = 0, mins_y = 0;
int secs_x = 0, secs_y = 0;
bool flag = true;

// 时间同步相关函数
void initTimeSync()
{
    Serial.println("初始化时间同步...");
    timeClient.begin();
    
    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("尝试同步网络时间...");
        
        int retries = 0;
        while (!timeClient.update() && retries < 10)
        {
            Serial.print("时间同步重试 ");
            Serial.println(retries + 1);
            timeClient.forceUpdate();
            delay(1000);
            retries++;
        }
        
        if (timeClient.isTimeSet())
        {
            Serial.println("时间同步成功！");
            time_t epochTime = timeClient.getEpochTime();
            Serial.print("当前时间戳: ");
            Serial.println(epochTime);
            
            // 显示格式化时间
            struct tm *ptm = gmtime(&epochTime);
            char timeStr[50];
            sprintf(timeStr, "%04d-%02d-%02d %02d:%02d:%02d", 
                ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday,
                (ptm->tm_hour + 8) % 24, ptm->tm_min, ptm->tm_sec);
            Serial.print("当前时间: ");
            Serial.println(timeStr);
        }
        else
        {
            Serial.println("时间同步失败，将使用本地时间");
        }
    }
    else
    {
        Serial.println("WiFi未连接，无法同步时间");
    }
}

void handleWiFiConfig()
{
    server.on("/connect", HTTP_POST, [](AsyncWebServerRequest *request)
    {
        // 获取POST参数
        String ssid = request->getParam("ssid", true)->value();
        String pass = request->getParam("pass", true)->value();
        String city = request->getParam("city", true)->value();
        String api = request->getParam("api", true)->value();

        Serial.print("收到WiFi配置 - SSID: ");
        Serial.print(ssid);
        Serial.print(", Password: ");
        Serial.println(pass);

        // 保存WiFi信息到JSON文件
        DynamicJsonDocument doc(1024);
        doc["ssid"] = ssid;
        doc["pass"] = pass;
        doc["city"] = city;
        doc["api"] = api;
        
        fs::File file = SPIFFS.open(ssidFile, "w");
        if (file) {
            serializeJson(doc, file);
            file.close();
            Serial.println("WiFi配置已保存");
        }

        // 更新全局变量
        cityname = city;
        weatherapi = api;

        // 发送响应
        request->send(200, "text/html; charset=UTF-8", 
            "<!DOCTYPE html>"
            "<html>"
            "<head>"
            "    <meta charset='UTF-8'>"
            "    <title>配置状态</title>"
            "</head>"
            "<body>"
            "    <h1>WiFi配置已保存，设备将尝试连接...</h1>"
            "    <p>如果连接成功，设备将自动重启并开始正常工作</p>"
            "    <p>如果连接失败，设备将重新进入配置模式</p>"
            "</body>"
            "</html>"
        );
        
        // 延迟后尝试连接新的WiFi
        delay(2000);
        attemptWiFiConnection(ssid, pass);
    });
    
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        if (SPIFFS.exists("/index.html")) {
            fs::File file = SPIFFS.open("/index.html", "r");
            if (file) {
                String fileContent;
                while (file.available()) {
                    fileContent += (char)file.read();
                }
                file.close();
                request->send(200, "text/html", fileContent);
                return;
            }
        }
        
        // 如果没有找到index.html文件，使用内置的配置页面
        String defaultHTML = R"(
<!DOCTYPE html>
<html lang='zh-CN'>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>TV-PRO WiFi配置</title>
    <style>
        body { 
            font-family: 'Microsoft YaHei', Arial, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            margin: 0; padding: 20px; min-height: 100vh; color: #333;
        }
        .container { 
            max-width: 600px; margin: 0 auto; 
            background: rgba(255,255,255,0.95); 
            padding: 30px; border-radius: 15px; 
            box-shadow: 0 10px 30px rgba(0,0,0,0.2);
        }
        h1 { color: #4a5568; text-align: center; margin-bottom: 20px; }
        .step { 
            background: #e8f4fd; padding: 15px; margin: 15px 0; 
            border-left: 4px solid #3182ce; border-radius: 5px; 
        }
        .form-group { margin: 15px 0; }
        label { display: block; margin-bottom: 5px; font-weight: bold; color: #4a5568; }
        input[type="text"], input[type="password"] { 
            width: 100%; padding: 12px; border: 2px solid #e2e8f0; 
            border-radius: 8px; font-size: 16px; box-sizing: border-box;
        }
        input[type="text"]:focus, input[type="password"]:focus { 
            outline: none; border-color: #3182ce; 
        }
        .required { color: #e53e3e; }
        .button { 
            width: 100%; padding: 15px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white; border: none; border-radius: 8px; 
            font-size: 16px; font-weight: bold; cursor: pointer; margin-top: 20px;
        }
        .button:hover { transform: translateY(-2px); }
        .warning { 
            background: #fff5f5; border: 1px solid #fed7d7; 
            padding: 12px; margin: 15px 0; border-radius: 6px; color: #c53030; 
        }
    </style>
</head>
<body>
    <div class='container'>
        <h1>📱 TV-PRO 时钟配置</h1>
        
        <div class='step'>
            <h3>🔗 连接步骤指南</h3>
            <p><strong>1.</strong> 确保您的设备已连接到 <strong>TV-PRO</strong> 热点</p>
            <p><strong>2.</strong> 在下方填写您的WiFi信息</p>
            <p><strong>3.</strong> 点击保存，设备将自动连接并重启</p>
        </div>
        
        <form action='/connect' method='POST'>
            <div class='form-group'>
                <label>WiFi名称 <span class='required'>*</span></label>
                <input type='text' name='ssid' placeholder='请输入您的WiFi网络名称' required>
            </div>
            
            <div class='form-group'>
                <label>WiFi密码</label>
                <input type='password' name='pass' placeholder='请输入WiFi密码（无密码可留空）'>
            </div>
            
            <div class='form-group'>
                <label>天气API密钥 <span style='color:#666;'>(可选)</span></label>
                <input type='text' name='api' placeholder='心知天气API密钥'>
            </div>
            
            <div class='form-group'>
                <label>城市名称 <span style='color:#666;'>(可选)</span></label>
                <input type='text' name='city' placeholder='城市拼音小写，如：beijing'>
            </div>
            
            <div class='warning'>
                ⚠️ 温馨提示：只有WiFi配置是必填的，天气配置可以稍后重新配置
            </div>
            
            <input type='submit' class='button' value='💾 保存配置'>
        </form>
        
        <div style='margin-top: 30px; padding: 15px; background: #f7fafc; border-radius: 8px;'>
            <p><strong>设备信息：</strong></p>
            <p>设备：TV-PRO 智能时钟 (ESP32-S3) | 配置地址：http://192.168.4.1</p>
        </div>
    </div>
</body>
</html>
        )";
        
        request->send(200, "text/html", defaultHTML);
    });
    
    server.begin();
    Serial.println("Web服务器已启动");
}

void attemptWiFiConnection(String ssid, String pass)
{
    Serial.print("尝试连接WiFi: ");
    Serial.println(ssid);
    
    WiFi.begin(ssid.c_str(), pass.c_str());
    
    // 连接超时设置 (20秒)
    int timeout = 20;
    String progress = "Connecting to " + ssid + "...";
    
    mylcd.fillScreen(TFT_BLACK);
    mylcd.setTextColor(TFT_WHITE);
    mylcd.drawString(progress, 10, 20, 2); // Y=20，顶部标题
    
    // 显示进度条区域 - 适配240*240像素
    int progressY = 60; // 进度显示的Y坐标
    
    while (WiFi.status() != WL_CONNECTED && timeout > 0)
    {
        delay(1000);
        timeout--;
        
        // 清除之前的进度显示
        mylcd.fillRect(0, progressY, 240, 60, TFT_BLACK); // 清除60像素高度的区域
        
        // 显示倒计时和进度
        String timeoutStr = "Timeout: " + String(timeout) + "s";
        mylcd.setTextColor(TFT_YELLOW);
        mylcd.drawString(timeoutStr, 10, progressY, 2); // Y=60
        
        // 显示WiFi状态
        mylcd.setTextColor(TFT_CYAN);
        mylcd.drawString("Status: Connecting...", 10, progressY + 30, 1); // Y=90
        
        // 简单的进度指示
        int dots = (20 - timeout) % 4;
        String dotStr = "";
        for (int i = 0; i < dots; i++) {
            dotStr += ".";
        }
        mylcd.setTextColor(TFT_GREEN);
        mylcd.drawString("Progress" + dotStr, 10, progressY + 50, 1); // Y=110
        
        Serial.print(".");
    }
    
    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("\nWiFi连接成功！");
        Serial.print("IP地址: ");
        Serial.println(WiFi.localIP());
        
        mylcd.fillScreen(TFT_BLACK);
        mylcd.setTextColor(TFT_GREEN);
        mylcd.drawString("WiFi Connected!", 10, 80, 2); // Y=80，居中显示
        mylcd.setTextColor(TFT_WHITE);
        mylcd.drawString("IP: " + WiFi.localIP().toString(), 10, 120, 1); // Y=120
        mylcd.drawString("Restarting...", 10, 150, 2); // Y=150
        
        delay(3000);
        ESP.restart(); // 重启以进入正常模式
    }
    else
    {
        Serial.println("\nWiFi连接失败，返回配置模式");
        mylcd.fillScreen(TFT_BLACK);
        mylcd.setTextColor(TFT_RED);
        mylcd.drawString("Connection Failed!", 10, 80, 2); // Y=80
        mylcd.setTextColor(TFT_WHITE);
        mylcd.drawString("Back to config mode", 10, 120, 1); // Y=120
        delay(2000);
        startConfigMode();
    }
}

void startConfigMode()
{
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ssid, password);
    
    Serial.println("启动配置模式 - AP热点");
    Serial.print("SSID: ");
    Serial.println(ssid);
    Serial.print("IP地址: ");
    Serial.println(WiFi.softAPIP());
    
    // 显示配置信息 - 适配240*240像素
    mylcd.fillScreen(TFT_BLACK);
    mylcd.setTextColor(TFT_CYAN);
    mylcd.drawString("Config Mode", 10, 10, 2); // Y=10，顶部标题
    
    mylcd.setTextColor(TFT_WHITE);
    mylcd.drawString("Connect to WiFi:", 5, 40, 1); // Y=40
    mylcd.drawString("   Name: TV-PRO", 5, 60, 1); // Y=60
    mylcd.drawString("   Password: (none)", 5, 80, 1); // Y=80
    
    mylcd.setTextColor(TFT_YELLOW);
    mylcd.drawString("Then open browser:", 5, 110, 1); // Y=110
    mylcd.setTextColor(TFT_GREEN);
    mylcd.drawString("http://192.168.4.1", 5, 130, 1); // Y=130
    
    mylcd.setTextColor(TFT_ORANGE);
    mylcd.drawString("Setup WiFi & API", 5, 160, 1); // Y=160
    
    mylcd.setTextColor(TFT_RED);
    mylcd.drawString("Waiting for config...", 5, 180, 1); // Y=180
    
    mylcd.setTextColor(TFT_GRAY);
    mylcd.drawLine(0, 200, 240, 200, TFT_GRAY); // Y=200，分割线
    mylcd.drawString("Press button to exit", 0, 210, 1); // Y=210，底部提示
    
    // 启动Web服务器
    handleWiFiConfig();
    server.begin();
    Serial.println("HTTP服务器已启动");
    
    isInConfigMode = true;
}

void loadWiFiConfig()
{
    if (SPIFFS.begin())
    {
        fs::File file = SPIFFS.open(ssidFile, "r");
        if (file)
        {
            DynamicJsonDocument doc(1024);
            DeserializationError error = deserializeJson(doc, file);
            if (!error)
            {
                String ssid = doc["ssid"];
                String pass = doc["pass"];
                String city = doc["city"];
                String api = doc["api"];
                
                // 更新全局变量，只有当配置文件中有有效值时才更新
                if (city.length() > 0) {
                    cityname = city;
                    Serial.println("从配置文件加载城市: " + city);
                } else {
                    Serial.println("配置文件中无城市信息，保持默认城市: " + cityname);
                }
                
                if (api.length() > 0) {
                    weatherapi = api;
                    Serial.println("从配置文件加载天气API: " + api);
                } else {
                    Serial.println("配置文件中无天气API信息");
                }
                
                // 尝试连接WiFi
                Serial.print("尝试连接到WiFi: ");
                Serial.println(ssid);
                mylcd.drawString("Connecting WiFi...", 0, 180, 2); // Y=180
                
                WiFi.begin(ssid.c_str(), pass.c_str());
                
                // 尝试连接WiFi，最多等待15秒
                unsigned long startAttemptTime = millis();
                while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000)
                {
                    delay(500);
                    Serial.print(".");
                }
                
                if (WiFi.status() == WL_CONNECTED)
                {
                    Serial.println("");
                    Serial.println("WiFi连接成功!");
                    Serial.print("IP地址: ");
                    Serial.println(WiFi.localIP());
                    mylcd.fillScreen(TFT_BLACK);
                    mylcd.setTextSize(2);
                    mylcd.drawString("WiFi Connected!", 0, 100, 2); // Y=100，居中显示
                    mylcd.drawString(WiFi.localIP().toString(), 0, 140, 2); // Y=140
                    delay(2000);
                    return; // 连接成功，直接返回
                }
                else
                {
                    Serial.println("");
                    Serial.println("WiFi连接失败");
                }
            }
            file.close();
        }
        else
        {
            Serial.println("未找到WiFi配置文件");
        }
    }
    else
    {
        Serial.println("SPIFFS初始化失败");
    }
    
    // 如果没有配置文件或连接失败，启动热点配置模式
    Serial.println("启动WiFi配置模式...");
    startConfigMode();
}

void fetchWeather()
{ // 天气捕捉
    Serial.println("=== 天气获取函数调用 ===");
    Serial.print("initweather: "); Serial.println(initweather ? "true" : "false");
    Serial.print("WiFi状态: "); Serial.println(WiFi.status() == WL_CONNECTED ? "已连接" : "未连接");
    Serial.print("weatherapi长度: "); Serial.println(weatherapi.length());
    Serial.print("cityname: "); Serial.println(cityname);
    Serial.print("weatherapi: "); Serial.println(weatherapi);
    
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("WiFi未连接，无法获取天气");
        return;
    }
    
    if (weatherapi.length() == 0)
    {
        Serial.println("天气API密钥未配置，请通过配置页面设置");
        // 设置默认显示信息
        temperature = "N/A";
        humidity = "N/A";
        weather = "Need API Key";
        return;
    }
    
    if (cityname.length() == 0)
    {
        Serial.println("城市名称未设置，使用默认城市：qingdao");
        cityname = "qingdao";
    }
    
    if (initweather == false)
    {
        Serial.println("开始获取天气信息...");
        WiFiClient client3;
        HTTPClient http3;
        
        String fullURL = weatherAPI + weatherapi + "&location=" + cityname + "&language=zh-Hans&unit=c&start=0&days=1";
        Serial.print("请求URL: ");
        Serial.println(fullURL);
        
        if (http3.begin(client3, fullURL))
        {
            Serial.println("HTTP连接建立成功");
            http3.setTimeout(10000); // 设置10秒超时
            int httpCode = http3.GET();
            
            Serial.print("HTTP响应码: ");
            Serial.println(httpCode);
            
            if (httpCode > 0)
            {
                String payload = http3.getString();
                Serial.println("=== 天气API响应开始 ===");
                Serial.println(payload);
                Serial.println("=== 天气API响应结束 ===");
                
                DynamicJsonDocument doc(2048);
                DeserializationError error = deserializeJson(doc, payload);
                
                if (error)
                {
                    Serial.print("JSON解析失败: ");
                    Serial.println(error.c_str());
                    temperature = "Parse Error";
                    humidity = "Parse Error";
                    weather = "JSON Error";
                }
                else
                {
                    Serial.println("JSON解析成功");
                    
                    // 检查API响应是否包含错误
                    if (doc.containsKey("status_code"))
                    {
                        String statusCode = doc["status_code"];
                        String statusMsg = doc["status"];
                        Serial.print("API返回错误: ");
                        Serial.print(statusCode);
                        Serial.print(" - ");
                        Serial.println(statusMsg);
                        temperature = "API Error";
                        humidity = "API Error";
                        weather = statusMsg;
                    }
                    else if (doc.containsKey("results") && doc["results"].size() > 0)
                    {
                        // 成功获取数据
                        String temperature2 = doc["results"][0]["daily"][0]["high"];
                        String humidity2 = doc["results"][0]["daily"][0]["humidity"];
                        String weather2 = doc["results"][0]["daily"][0]["text_day"];
                        
                        temperature = temperature2;
                        humidity = humidity2;
                        weather = weather2;
                        Serial.print("天气: "); Serial.println(weather);
                        Serial.print("温度: "); Serial.println(temperature);
                        Serial.print("湿度: "); Serial.println(humidity);
                        
                        // 根据天气状况选择图标
                        if (weather == "阴" || weather == "多云" || weather.indexOf("云") >= 0)
                        {
                            weatherpic = 0; // 云图标
                        }
                        else if (weather == "小雨" || weather == "大雨" || weather == "暴雨" || weather == "雨" || weather.indexOf("雨") >= 0)
                        {
                            weatherpic = 1; // 雨图标
                        }
                        else 
                        {
                            weatherpic = 2; // 太阳图标 (晴天或其他)
                        }
                        
                        initweather = true;
                        
                        Serial.println("=== 天气数据获取成功 ===");
                        Serial.print("温度: "); Serial.println(temperature);
                        Serial.print("湿度: "); Serial.println(humidity);
                        Serial.print("天气: "); Serial.println(weather);
                    }
                    else
                    {
                        Serial.println("API响应格式异常");
                        temperature = "No Data";
                        humidity = "No Data";
                        weather = "No Results";
                    }
                }
            }
            else
            {
                Serial.printf("HTTP请求失败，错误码: %d (%s)\n", httpCode, http3.errorToString(httpCode).c_str());
                temperature = "HTTP Error";
                humidity = "HTTP Error";
                weather = "Request Failed";
            }
            http3.end();
        }
        else
        {
            Serial.println("HTTP连接建立失败");
            temperature = "Connect Error";
            humidity = "Connect Error";
            weather = "Connection Failed";
        }
    }
    
    // 设置天气图标
    if (weather == "阴" || weather == "多云")
    {
        weatherpic = 0;
    }
    else if (weather == "小雨" || weather == "大雨" || weather == "暴雨" || weather == "雨")
    {
        weatherpic = 1;
    }
    else 
    {
        weatherpic = 2;
    }
}

void checkWiFiConnection()
{
    // 只在非配置模式下检查WiFi连接
    if (isInConfigMode) return;
    
    unsigned long currentTime = millis();
    if (currentTime - lastWiFiCheck >= wifiCheckInterval)
    {
        lastWiFiCheck = currentTime;
        
        if (WiFi.status() != WL_CONNECTED)
        {
            wifiFailCount++;
            Serial.printf("WiFi连接丢失，失败次数: %d/%d\n", wifiFailCount, maxWifiFailCount);
            
            if (wifiFailCount >= maxWifiFailCount)
            {
                Serial.println("WiFi多次连接失败，启动配置模式");
                wifiFailCount = 0;
                startConfigMode();
                return;
            }
            
            // 尝试重新连接
            Serial.println("尝试重新连接WiFi...");
            WiFi.reconnect();
        }
        else
        {
            // 连接正常，重置失败计数
            if (wifiFailCount > 0)
            {
                Serial.println("WiFi重新连接成功");
                wifiFailCount = 0;
            }
        }
    }
}

void displayTask(void *parameter)
{
    currentPage = 0; // 确保任务启动时从主页面开始
    int lastDisplayedPage = -1; // 用于检测页面切换
    
    for (;;)
    {
        // 检测页面切换
        bool pageChanged = (currentPage != lastDisplayedPage);
        
        // 根据页面类型决定清屏策略
        if (pageChanged) {
            if (currentPage == 5) { // 时钟页面需要特殊处理
                // 时钟页面切换时不立即清屏，让ClockInit来处理
                initialized = false; // 重置时钟初始化标志
            } else {
                // 其他页面切换时清屏
                mylcd.fillScreen(TFT_BLACK);
                if (lastDisplayedPage == 5) {
                    // 从时钟页面切换出来，重置时钟初始化
                    initialized = false;
                }
            }
            lastDisplayedPage = currentPage;
        }

        switch (currentPage)
        {
            case 0: // 主页面
            {
                // 页面切换时绘制静态内容
                if (pageChanged) {
                    // 顶部logo - edalogo (158*30)
                    mylcd.pushImage(0, 0, 150, 42, edalogo);
                    
                    // 分割线
                    mylcd.drawLine(0, 148, 240, 148, TFT_YELLOW);
                    
                    // 城市名显示（相对静态）
                    mylcd.setTextSize(2);
                    mylcd.setTextColor(TFT_WHITE);
                    mylcd.drawString(cityname, 108, 150, 1);
                }
                
                // 动态内容更新
                if (WiFi.status() == WL_CONNECTED)
                {
                    timeClient.update();
                }
                
                // 获取时间信息
                int currentHour = timeClient.getHours();
                int currentMinute = timeClient.getMinutes();
                time_t rawTime = (time_t)timeClient.getEpochTime();
                struct tm *ptm = localtime(&rawTime);

                // 清除并更新右上角星期显示
                mylcd.fillRect(170, 0, 70, 25, TFT_BLACK);
                const char* weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
                int weekday = (ptm->tm_wday >= 0 && ptm->tm_wday < 7) ? ptm->tm_wday : 0;
                mylcd.setTextSize(2);
                mylcd.setTextColor(TFT_WHITE);
                mylcd.drawString(weekdays[weekday], 170, 0, 2);

                // 清除并更新大时钟显示区域
                mylcd.fillRect(15, 35, 230, 65, TFT_BLACK);
                char timeBuffer[10];
                if (useAmPmFormat) {
                    int displayHour = currentHour;
                    String ampm = "AM";
                    if (displayHour == 0) {
                        displayHour = 12;
                    } else if (displayHour > 12) {
                        displayHour -= 12;
                        ampm = "PM";
                    } else if (displayHour == 12) {
                        ampm = "PM";
                    }
                    snprintf(timeBuffer, sizeof(timeBuffer), "%2d:%02d", displayHour, currentMinute);
                    mylcd.setTextSize(5);
                    mylcd.setTextColor(TFT_WHITE);
                    mylcd.drawString(timeBuffer, 15, 30, 2);
                    
                    mylcd.setTextSize(2);
                    mylcd.setTextColor(TFT_YELLOW);
                    mylcd.drawString(ampm, 190, 55, 2);
                } else {
                    snprintf(timeBuffer, sizeof(timeBuffer), "%02d:%02d", currentHour, currentMinute);
                    mylcd.setTextSize(6);
                    mylcd.setTextColor(TFT_WHITE);
                    mylcd.drawString(timeBuffer, 15, 25, 2);
                }

                // 清除并更新日期显示区域
                mylcd.fillRect(40, 115, 200, 30, TFT_BLACK);
                if (!isLunarDisplay) {
                    char dateStr[11];
                    snprintf(dateStr, sizeof(dateStr), "%04d.%02d.%02d", ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);
                    mylcd.setTextSize(2);
                    mylcd.setTextColor(TFT_WHITE);
                    mylcd.drawString(dateStr, 40, 110, 2);
                } else {
                    displayLunarDate(ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);
                }

                // 清除并更新农历显示区域
                if (showLunar) {
                    mylcd.fillRect(40, 135, 200, 40, TFT_BLACK);
                    String lunarDate = getLunarDate(ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);
                    mylcd.setTextSize(1);
                    mylcd.setTextColor(TFT_CYAN);
                    mylcd.drawString(lunarDate, 40, 135, 2);
                }

                // 重新绘制城市名称，防止被清屏覆盖
                mylcd.setTextSize(2);
                mylcd.setTextColor(TFT_WHITE);
                mylcd.drawString(cityname, 108, 150, 1);

                // 重新绘制分割线，防止被覆盖
                mylcd.drawLine(0, 148, 240, 148, TFT_YELLOW);

                // 重新绘制天气图标，防止被覆盖
                if (weatherpic == 0) {
                    mylcd.pushImage(13, 155, 75, 75, cloud);
                } else if (weatherpic == 1) {
                    mylcd.pushImage(13, 155, 75, 75, rain);
                } else {
                    mylcd.pushImage(13, 155, 75, 75, sun);
                }

                // 清除并更新温湿度显示区域
                mylcd.fillRect(108, 178, 132, 50, TFT_BLACK);
                mylcd.setTextSize(2);
                mylcd.setTextColor(TFT_WHITE);
                mylcd.drawString("H: " + humidity + " %", 108, 178, 1);
                mylcd.drawString("T: " + temperature + " C", 108, 205, 1);

                // 清除并更新事件提醒显示
                mylcd.fillRect(0, 225, 240, 15, TFT_BLACK);

                /* ---------- LED 跑马灯首尾相接滚动算法 ---------- */
                static uint16_t scrollOffset = 0;           // 当前偏移（像素）
                static unsigned long lastTick = 0;          // 上次刷新时间
                static String lastText = "";               // 上次滚动的文本

                if (currentReminder.length() > 0 && millis() - reminderStartTime < reminderDuration)
                {
                    // 如果文本变化，则重置偏移
                    if (currentReminder != lastText)
                    {
                        scrollOffset = 0;
                        lastText = currentReminder;
                    }

                    uint16_t textW = mylcd.textWidth(currentReminder, 1);   // 精确计算文本像素宽
                    textW += 2;  // 安全余量，防止边缘裁剪
                    const uint16_t gapPix = 40;                       // 文本间隔空白
                    const uint16_t cycle  = textW + gapPix;           // 一完整循环长度 L

                    const uint8_t  pixelStep   = 10;                 // 每帧移动 px
                    const uint8_t  scrollSpeed = 5;                  // 帧间隔 ms

                    // 更新偏移
                    if (millis() - lastTick >= scrollSpeed)
                    {
                        scrollOffset = (scrollOffset + pixelStep) % cycle;
                        lastTick = millis();
                    }

                    // 计算各段文本 X 坐标
                    int16_t x1 = 240 - scrollOffset;      // 第一段
                    int16_t x2 = x1 + cycle;              // 第二段（必在第一段右侧）
                    int16_t x3 = x1 - cycle;              // 第三段（处理极短文本）

                    mylcd.setTextColor(TFT_YELLOW);
                    // 在可视区才绘制，避免越界闪烁
                    if (x1 > -textW && x1 < 240)
                        mylcd.drawString(currentReminder, x1, 225, 1);
                    if (x2 > -textW && x2 < 240)
                        mylcd.drawString(currentReminder, x2, 225, 1);
                    if (x3 > -textW && x3 < 240)
                        mylcd.drawString(currentReminder, x3, 225, 1);
                }
                /* ---------- 滚动算法结束 ---------- */

                break;
            }
            
            case 1: // 天气页面 - 显示实时温湿度
            {
                // 更新DHT11数据
                updateDHT11();
                
                // 页面切换时或舒适度改变时更新表情图标（相对静态）
                static int lastComfortLevel = -1;
                int currentComfortLevel = 0;  // 0=不舒适, 1=一般, 2=舒适
                
                if (dht11_temp >= 20 && dht11_temp <= 26 && dht11_humi >= 40 && dht11_humi <= 60) {
                    currentComfortLevel = 2; // 最舒适
                } else if ((dht11_temp >= 15 && dht11_temp < 20 || dht11_temp > 26 && dht11_temp <= 30) &&
                         (dht11_humi >= 30 && dht11_humi < 40 || dht11_humi > 60 && dht11_humi <= 70)) {
                    currentComfortLevel = 1; // 一般舒适
                } else {
                    currentComfortLevel = 0; // 不舒适
                }
                
                if (pageChanged || currentComfortLevel != lastComfortLevel) {
                    // 清除并更新表情图标
                    mylcd.fillRect(20, 0, 200, 100, TFT_BLACK);
                    
                    if (currentComfortLevel == 2) {
                        mylcd.pushImage(mylcd.width() / 2-(200/2), 0, 200, 100, happy);
                    } else if (currentComfortLevel == 1) {
                        mylcd.pushImage(mylcd.width() / 2-(200/2), 0, 200, 100, mid);
                    } else {
                        mylcd.pushImage(mylcd.width() / 2-(200/2), 0, 200, 100, unhappy);
                    }
                    lastComfortLevel = currentComfortLevel;
                }
                
                // 清除并更新温度显示区域
                mylcd.fillRect(100, 120, 140, 25, TFT_BLACK);
                mylcd.setTextColor(TFT_RED);
                mylcd.pushImage(10, 120, 25, 25, wen);
                mylcd.pushImage(40, 120, 25, 25, du);
                mylcd.pushImage(70, 120, 25, 25, maohao);
                mylcd.setTextSize(3);
                mylcd.drawString(String(dht11_temp, 1) + "C", 100, 120, 1);
                mylcd.setTextSize(2);
                
                // 清除并更新湿度显示区域
                mylcd.fillRect(100, 190, 140, 25, TFT_BLACK);
                mylcd.setTextColor(TFT_BLUE);
                mylcd.pushImage(10, 190, 25, 25, shi);
                mylcd.pushImage(40, 190, 25, 25, du);
                mylcd.pushImage(70, 190, 25, 25, maohao);
                mylcd.setTextSize(3);
                mylcd.drawString(String(dht11_humi, 1) + "%", 100, 190, 1);
                mylcd.setTextSize(2);
                
                break;
            }
            
            case 2: // 闹钟页面
            {
                // 页面切换时绘制静态内容
                if (pageChanged) {
                    mylcd.setTextColor(TFT_GREEN);
                    mylcd.drawString("Alarm Clock", 10, 10, 2);
                    mylcd.drawLine(0, 35, 240, 35, TFT_GREEN);
                    
                    // 显示分隔线
                    mylcd.drawLine(0, 105, 240, 105, TFT_GRAY);
                    
                    // 显示闹钟列表
                    mylcd.setTextColor(TFT_CYAN);
                    mylcd.drawString("Alarms:", 10, 110, 2);
                    
                    int yPos = 140;
                    bool hasEnabledAlarms = false;
                    
                    for (int i = 0; i < MAX_ALARMS; i++) {
                        if (alarms[i].enabled) {
                            hasEnabledAlarms = true;
                            // 闹钟状态图标
                            mylcd.setTextColor(TFT_YELLOW);
                            mylcd.drawString("[A]", 10, yPos, 2);
                            
                            // 闹钟时间
                            char alarmTime[6];
                            snprintf(alarmTime, sizeof(alarmTime), "%02d:%02d", alarms[i].hour, alarms[i].minute);
                            mylcd.setTextColor(TFT_WHITE);
                            mylcd.drawString(alarmTime, 40, yPos, 2);
                            
                            // 闹钟标签
                            mylcd.setTextColor(TFT_GRAY);
                            mylcd.drawString(alarms[i].label, 120, yPos, 2);
                            
                            yPos += 30;
                        }
                    }
                    
                    if (!hasEnabledAlarms) {
                        mylcd.setTextColor(TFT_GRAY);
                        mylcd.drawString("No alarms set", 10, 140, 2);
                    }
                }
                
                // 总是更新时间显示
                if (WiFi.status() == WL_CONNECTED)
                {
                    timeClient.update();
                }
                
                // 获取当前时间信息
                int currentHour = timeClient.getHours();
                int currentMinute = timeClient.getMinutes();
                int currentSecond = timeClient.getSeconds();
                
                // 只清除和重绘时间显示区域
                mylcd.fillRect(10, 50, 220, 50, TFT_BLACK);
                
                // 大字体显示当前时间
                char timeBuffer[10];
                snprintf(timeBuffer, sizeof(timeBuffer), "%02d:%02d:%02d", currentHour, currentMinute, currentSecond);
                mylcd.setTextColor(TFT_WHITE);
                mylcd.setTextSize(4);
                mylcd.drawString(timeBuffer, 10, 45, 2);
                mylcd.setTextSize(2); // 恢复默认大小
                
                break;
            }
            
            case 3: // 事件提醒页面
            {
                // 页面切换时绘制静态内容
                if (pageChanged) {
                    mylcd.setTextColor(TFT_BLUE);
                    mylcd.drawString("Event Reminder", 10, 10, 2);
                    mylcd.drawLine(0, 35, 240, 35, TFT_BLUE);
                    
                    // 显示分隔线
                    mylcd.drawLine(0, 105, 240, 105, TFT_GRAY);
                    
                    // 显示事件列表
                    mylcd.setTextColor(TFT_CYAN);
                    mylcd.drawString("Events:", 10, 110, 2);
                    
                    int yPos = 140;
                    bool hasEnabledEvents = false;
                    
                    for (int i = 0; i < MAX_EVENTS; i++) {
                        if (events[i].enabled && yPos < 220) { // 确保不超出屏幕范围
                            hasEnabledEvents = true;
                            // 事件状态图标
                            mylcd.setTextColor(TFT_YELLOW);
                            
                            
                            mylcd.drawString("*", 10, yPos, 2);
                            
                            // 事件标题（缩短以适应屏幕）
                            String eventTitle = events[i].title;
                            if (eventTitle.length() > 12) {
                                eventTitle = eventTitle.substring(0, 12) + "..";
                            }
                            mylcd.setTextColor(TFT_WHITE);
                            mylcd.drawString(eventTitle, 25, yPos, 1);
                            
                            // 事件日期和时间信息
                            mylcd.setTextColor(TFT_GRAY);
                            char eventInfo[20];
                            if (events[i].hasTime) {
                                snprintf(eventInfo, sizeof(eventInfo), "%02d/%02d %02d:%02d", 
                                       events[i].month, events[i].day, events[i].hour, events[i].minute);
                            } else {
                                snprintf(eventInfo, sizeof(eventInfo), "%02d/%02d", 
                                       events[i].month, events[i].day);
                            }
                            mylcd.drawString(eventInfo, 25, yPos + 15, 1);
                            
                            yPos += 35; // 每个事件占用更多空间以显示完整信息
                        }
                    }
                    
                    if (!hasEnabledEvents) {
                        mylcd.setTextColor(TFT_GRAY);
                        mylcd.drawString("No events set", 10, 140, 2);
                        mylcd.setTextColor(TFT_ORANGE);
                        mylcd.drawString("Use web interface", 10, 170, 1);
                        mylcd.drawString("to add events", 10, 185, 1);
                    }
                }
                
                // 总是更新时间显示
                if (WiFi.status() == WL_CONNECTED)
                {
                    timeClient.update();
                }
                
                // 获取当前时间信息
                int currentHour = timeClient.getHours();
                int currentMinute = timeClient.getMinutes();
                int currentSecond = timeClient.getSeconds();
                
                // 只清除和重绘时间显示区域
                mylcd.fillRect(10, 50, 220, 50, TFT_BLACK);
                
                // 大字体显示当前时间
                char timeBuffer[10];
                snprintf(timeBuffer, sizeof(timeBuffer), "%02d:%02d:%02d", currentHour, currentMinute, currentSecond);
                mylcd.setTextColor(TFT_WHITE);
                mylcd.setTextSize(4);
                mylcd.drawString(timeBuffer, 10, 45, 2);
                mylcd.setTextSize(2); // 恢复默认大小
                
                break;
            }
            
            case 4: // 网络信息页面 - 专门显示IP和网络信息
            {
                // 页面切换时绘制静态内容
                if (pageChanged) {
                    mylcd.setTextColor(TFT_BLUE);
                    mylcd.drawString("Network Info", 10, 10, 2);
                    mylcd.drawLine(0, 35, 240, 35, TFT_BLUE);
                }
                
                // 动态内容更新
                // WiFi连接状态
                if (WiFi.status() == WL_CONNECTED)
                {
                    // 清除动态信息显示区域
                    mylcd.fillRect(10, 50, 230, 190, TFT_BLACK);
                    
                    mylcd.setTextColor(TFT_GREEN);
                    mylcd.drawString("WiFi: Connected", 10, 50, 2);
                    
                    // 显示IP地址 - 大字体突出显示
                    mylcd.setTextColor(TFT_WHITE);
                    mylcd.drawString("Device IP:", 10, 80, 2);
                    mylcd.setTextSize(2);
                    mylcd.setTextColor(TFT_YELLOW);
                    String ipStr = WiFi.localIP().toString();
                    mylcd.drawString(ipStr, 10, 105, 2);
                    mylcd.setTextSize(2); // 恢复默认大小
                    
                    // WiFi网络名称
                    mylcd.setTextColor(TFT_CYAN);
                    mylcd.drawString("Network:", 10, 140, 1);
                    mylcd.setTextColor(TFT_WHITE);
                    String wifiName = WiFi.SSID();
                    if (wifiName.length() > 20) {
                        wifiName = wifiName.substring(0, 20) + "...";
                    }
                    mylcd.drawString(wifiName, 120, 140, 1);
                    
                    // 信号强度
                    mylcd.setTextColor(TFT_CYAN);
                    mylcd.drawString("Signal:", 10, 165, 1);
                    mylcd.setTextColor(TFT_WHITE);
                    int rssi = WiFi.RSSI();
                    String signalStr = String(rssi) + " dBm";
                    if (rssi > -50) {
                        mylcd.setTextColor(TFT_GREEN);
                        signalStr += " (强)";
                    } else if (rssi > -70) {
                        mylcd.setTextColor(TFT_YELLOW);
                        signalStr += " (中)";
                    } else {
                        mylcd.setTextColor(TFT_RED);
                        signalStr += " (弱)";
                    }
                    mylcd.drawString(signalStr, 10, 190, 1);
                    
                    // Web设置提示
                    mylcd.setTextColor(TFT_ORANGE);
                    mylcd.drawString("Web Settings:", 10, 210, 1);
                    mylcd.setTextColor(TFT_GREEN);
                    mylcd.drawString("http://" + ipStr, 10, 225, 1);
                }
                else
                {
                    // 清除动态信息显示区域
                    mylcd.fillRect(10, 50, 230, 190, TFT_BLACK);
                    
                    mylcd.setTextColor(TFT_RED);
                    mylcd.drawString("WiFi: Disconnected", 10, 50, 2);
                    mylcd.setTextColor(TFT_YELLOW);
                    mylcd.drawString("Please check WiFi", 10, 80, 1);
                    mylcd.drawString("connection and retry", 10, 95, 1);
                    
                    mylcd.setTextColor(TFT_CYAN);
                    mylcd.drawString("To reconfigure:", 10, 120, 1);
                    mylcd.setTextColor(TFT_WHITE);
                    mylcd.drawString("1. Long press center", 10, 135, 1);
                    mylcd.drawString("   button (2 sec)", 10, 150, 1);
                    mylcd.drawString("2. Connect to TV-PRO", 10, 165, 1);
                    mylcd.drawString("3. Visit 192.168.4.1", 10, 180, 1);
                }

                break;
            }
            
            case 5: // 新增：模拟时钟页面
            {
                showClockPage();
                break;
            }
            
            case 6: // Alarm ringing page
            {
                if (pageChanged) {
                    mylcd.fillScreen(TFT_BLACK);
                    mylcd.setTextColor(TFT_RED);
                    mylcd.setTextSize(4);
                    mylcd.drawString("ALARM!", 50, 80, 2);
                    mylcd.setTextSize(2);
                    mylcd.setTextColor(TFT_WHITE);
                    mylcd.drawString("Press any button", 20, 150, 1);
                    mylcd.drawString("to stop", 70, 170, 1);
                }
                break;
            }

            case 7: // 新增：火焰报警页面
            {
                if (pageChanged) {
                    mylcd.fillScreen(TFT_RED); // 使用红色背景以示紧急
                    mylcd.setTextColor(TFT_WHITE);
                    mylcd.setTextSize(3);
                    mylcd.drawString("FIRE ALARM!", 15, 80, 2);
                    mylcd.setTextSize(2);
                    mylcd.drawString("Press any button", 20, 150, 1);
                    mylcd.drawString("to dismiss", 50, 170, 1);
                }
                
                // 清除并更新邮件状态显示区域
                mylcd.fillRect(10, 200, 220, 40, TFT_RED);
                mylcd.setTextSize(1);
                if (emailSent) {
                    mylcd.setTextColor(TFT_GREEN);
                    mylcd.drawString("Email: Success" , 10, 200, 1);
                } else {
                    mylcd.setTextColor(TFT_YELLOW);
                    mylcd.drawString("Email: " + emailErrorMsg, 10, 200, 1);
                }
                
                
                break;
            }
        }

        // 根据页面类型调整刷新率和策略
        if (currentPage == 5) { 
            // 时钟页面使用快速刷新
            vTaskDelay(50 / portTICK_PERIOD_MS);
        } else if (currentPage == 2 || currentPage == 3) {
            // 闹钟页面和事件页面有时间显示，需要每秒刷新
            vTaskDelay(100 / portTICK_PERIOD_MS);
        } else {
            // 其他页面使用正常刷新
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        
        // 改进的页面刷新控制
        static unsigned long lastPageRefresh = 0;
        unsigned long currentTime = millis();
        
        // 页面刷新策略
        bool shouldRefresh = false;
        
        if (pageChanged) {
            // 页面切换时一定要刷新
            shouldRefresh = true;
            lastPageRefresh = currentTime;
        } else if (currentPage == 5) {
            // 时钟页面总是刷新（由showClockPage内部控制）
            shouldRefresh = true;
        } else if (currentPage == 2 || currentPage == 3) {
            // 有时间显示的页面每秒刷新一次
            if (currentTime - lastPageRefresh >= 1000) {
                shouldRefresh = true;
                lastPageRefresh = currentTime;
            }
        } else {
            // 其他页面每5秒刷新一次以节省资源
            if (currentTime - lastPageRefresh >= 5000) {
                shouldRefresh = true;
                lastPageRefresh = currentTime;
            }
        }
        
        if (!shouldRefresh) {
            vTaskDelay(100 / portTICK_PERIOD_MS);
            continue;
        }
    }
}

////////////////////////////
// setup()函数：初始化
////////////////////////////
void setup()
{
    mylcd.init();
    mylcd.setRotation(0);
    mylcd.fillScreen(TFT_BLACK);
    mylcd.setTextSize(2);
    // 调整Logo位置，适配240*240像素
    mylcd.pushImage(60,0,120,106,openlogo); // Logo保持原位置
    mylcd.drawString("Starting...", 0, 120, 2); // Y=120，Logo下方有足够空间
    
    Serial.begin(115200);
    Serial.println("TV-PRO 智能时钟启动中...");
    
    // 初始化DHT11
    dht.begin();
    Serial.println("DHT11传感器初始化完成");
    
    // 初始化按键
    pinMode(BUTTON_MID, INPUT_PULLUP);
    pinMode(BUTTON_LEFT, INPUT_PULLUP);
    pinMode(BUTTON_RIGHT, INPUT_PULLUP);
    
    // 初始化SPIFFS
    if (!SPIFFS.begin()) {
        Serial.println("SPIFFS初始化失败");
        mylcd.drawString("SPIFFS Init Failed!", 0, 150, 2); // Y=150
        delay(3000);
        ESP.restart();
    }
    
    // 尝试加载和连接WiFi配置
    mylcd.drawString("Loading WiFi config...", 0, 150, 2); // Y=150
    loadWiFiConfig();
    
    // 如果WiFi连接成功，继续初始化其他组件
    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("WiFi已连接，继续初始化...");
        mylcd.drawString("Initializing NTP...", 0, 180, 2); // Y=180
        
        // 初始化NTP客户端
        initTimeSync();
        
        mylcd.drawString("Loading weather...", 0, 210, 2); // Y=210
        fetchWeather();
        
        // 加载显示设置
        loadDisplaySettings();
        
        // 加载闹钟配置
        loadAlarms();
        
        // 加载事件配置
        loadEvents();
        
        // 启动Web设置服务器
        if (!webServerStarted) {
            setupWebServer();
        }
        
        mylcd.fillScreen(TFT_BLACK);
        mylcd.setTextSize(2);
        mylcd.setTextColor(TFT_GREEN);
        mylcd.drawString("System Ready!", 10, 40, 2);
        
        // 显示IP地址 - 突出显示
        mylcd.setTextColor(TFT_WHITE);
        mylcd.drawString("Device IP:", 10, 80, 2);
        mylcd.setTextSize(3);
        mylcd.setTextColor(TFT_YELLOW);
        String deviceIP = WiFi.localIP().toString();
        mylcd.drawString(deviceIP, 10, 110, 2);
        mylcd.setTextSize(2);
        
        // 访问提示
        mylcd.setTextColor(TFT_CYAN);
        mylcd.drawString("Web Settings:", 10, 160, 2);
        mylcd.setTextColor(TFT_WHITE);
        mylcd.drawString("http://" + deviceIP, 10, 185, 1);
        
        // 按键提示
        mylcd.setTextColor(TFT_ORANGE);
        mylcd.drawString("Press buttons to", 10, 210, 1);
        mylcd.drawString("navigate pages", 10, 225, 1);
        
        delay(5000); // 延长显示时间让用户看清IP
        
        Serial.println("系统初始化完成");
        Serial.println("配置状态:");
        Serial.println("城市: " + cityname);
        Serial.println(String("天气API: ") + (weatherapi.length() > 0 ? "已配置" : "未配置"));
        Serial.println("Web设置页面: http://" + WiFi.localIP().toString());
        
        // 确保调用一次天气获取以显示调试信息
        Serial.println("初始化时尝试获取天气...");
        fetchWeather();
        
        // 启动显示任务
        xTaskCreatePinnedToCore(
            displayTask,
            "DisplayTask",
            10000,
            NULL,
            1,
            &displayTaskHandle,
            1
        );
        
        Serial.println("显示任务已启动");
        currentPage = 0; // 确保从主页面开始显示
    } else {
        Serial.println("WiFi连接失败，进入配置模式");
        startConfigMode();
    }

    // 初始化I2S蜂鸣器
    initI2S();
    Serial.println("I2S buzzer ready");
    
    // 初始化音乐播放器
    initMusicPlayer();
    Serial.println("Music player ready");

    // 启动音频服务任务（高优先级，核心0）
    xTaskCreatePinnedToCore(
        audioTask,
        "AudioTask",
        4096,
        NULL,
        2,
        &audioTaskHandle,
        0
    );
    Serial.println("Audio task started");

    // 设置音频输出
    // audioOut->SetPinout(SPK_BCLK, SPK_LRC, SPK_DIN);
    // audioOut->SetGain(0.8); // 设置音量 (0.0 到 4.0)

    // 初始化火焰传感器引脚
    pinMode(PIN_DO, INPUT);
    pinMode(PIN_AO, INPUT);
    analogReadResolution(12);       // 默认 12 位
    analogSetAttenuation(ADC_11db); // 量程 0-3.3V

    // 初始化音乐播放器
    // initMusicPlayer();
}

// 按键处理函数（页面切换版）
void handleButtonPress()
{
    static bool lastButtonMIDState = HIGH;
    static bool lastButtonLEFTState = HIGH;
    static bool lastButtonRIGHTState = HIGH;
    static unsigned long buttonMIDPressTime = 0;

    bool currentButtonMIDState = digitalRead(BUTTON_MID);
    bool currentButtonLEFTState = digitalRead(BUTTON_LEFT);
    bool currentButtonRIGHTState = digitalRead(BUTTON_RIGHT);

    // If alarm is ringing, any button press stops it
    if (alarmRinging && (currentButtonMIDState == LOW || currentButtonLEFTState == LOW || currentButtonRIGHTState == LOW)) {
        stopAlarm();
        // Debounce: wait until buttons released
        while (digitalRead(BUTTON_MID) == LOW || digitalRead(BUTTON_LEFT) == LOW || digitalRead(BUTTON_RIGHT) == LOW) {
            delay(10);
        }
        return;
    }

    // 新增：如果火焰警报激活，任何按键按下都会停止它
    if (flameAlarmActive && (currentButtonMIDState == LOW || currentButtonLEFTState == LOW || currentButtonRIGHTState == LOW)) {
        stopFlameAlarm();
        // Debounce: wait until buttons released
        while (digitalRead(BUTTON_MID) == LOW || digitalRead(BUTTON_LEFT) == LOW || digitalRead(BUTTON_RIGHT) == LOW) {
            delay(10);
        }
        return;
    }

    // 中键：回到主页面或切换显示模式
    if (lastButtonMIDState == HIGH && currentButtonMIDState == LOW)
    {
        buttonMIDPressTime = millis(); // 记录按下时间
    }
    else if (lastButtonMIDState == LOW && currentButtonMIDState == HIGH)
    {
        unsigned long pressDuration = millis() - buttonMIDPressTime;
        
        if (pressDuration > 2000) // 长按超过2秒
        {
            Serial.println("中键长按 - 进入配置模式");
            mylcd.fillScreen(TFT_BLACK);
            mylcd.setTextSize(2);
            mylcd.drawString("Entering Config Mode...", 0, 100, 2);
            mylcd.drawString("Please wait...", 0, 140, 2);
            delay(1000);
            startConfigMode();
        }
        else // 短按
        {
            if (currentPage == 0) { // 如果当前在主页面
                isLunarDisplay = !isLunarDisplay; // 切换显示模式
                Serial.println(isLunarDisplay ? "切换到农历显示" : "切换到公历显示");
            } else {
                Serial.println("中键短按 - 回到主页面");
                currentPage = 0; // 回到主页面
            }
        }
    }
    
    // 左键：向前切换页面（上一页）
    if (lastButtonLEFTState == HIGH && currentButtonLEFTState == LOW)
    {
        Serial.println("左键按下 - 向前切换页面");
        currentPage = (currentPage - 1 + totalPages) % totalPages;
        Serial.printf("切换到页面: %d\n", currentPage);
    }
    
    // 右键：向后切换页面（下一页）
    if (lastButtonRIGHTState == HIGH && currentButtonRIGHTState == LOW)
    {
        Serial.println("右键按下 - 向后切换页面");
        currentPage = (currentPage + 1) % totalPages;
        Serial.printf("切换到页面: %d\n", currentPage);
    }

    lastButtonMIDState = currentButtonMIDState;
    lastButtonLEFTState = currentButtonLEFTState;
    lastButtonRIGHTState = currentButtonRIGHTState;
}

void loop()
{
    // 处理按键
    handleButtonPress();
    
    // 检查WiFi连接状态
    checkWiFiConnection();
    
    // 更新DHT11数据
    updateDHT11();
    
    // 自动页面轮转功能
    static unsigned long lastPageRotate = 0;
    if (autoRotatePages && !isInConfigMode && millis() - lastPageRotate > (pageRotateInterval * 1000))
    {
        currentPage = (currentPage + 1) % totalPages;
        lastPageRotate = millis();
        Serial.printf("自动轮转到页面: %d\n", currentPage);
    }
    
    // 定期重新获取天气（每小时一次）
    static unsigned long lastWeatherUpdate = 0;
    if (millis() - lastWeatherUpdate > 3600000) // 1小时
    {
        if (WiFi.status() == WL_CONNECTED)
        {
            initweather = false;
            fetchWeather();
            lastWeatherUpdate = millis();
        }
    }
    
    // 定期检查时间同步（每10分钟检查一次）
    static unsigned long lastTimeSync = 0;
    if (millis() - lastTimeSync > 600000) // 10分钟
    {
        if (WiFi.status() == WL_CONNECTED)
        {
            if (!timeClient.isTimeSet() || timeClient.getEpochTime() < 1000000000) // 如果时间未设置或异常
            {
                Serial.println("检测到时间异常，重新同步...");
                initTimeSync();
            }
            else
            {
                timeClient.update(); // 定期更新时间
            }
            lastTimeSync = millis();
        }
    }
    
    // 定期输出调试信息（每30秒一次）
    static unsigned long lastDebugOutput = 0;
    if (millis() - lastDebugOutput > 30000) // 30秒
    {
        Serial.println("=== 定期调试信息 ===");
        Serial.print("当前页面: "); Serial.println(currentPage);
        Serial.print("城市名称: "); Serial.println(cityname);
        Serial.print("城市名称长度: "); Serial.println(cityname.length());
        Serial.print("温度: "); Serial.println(temperature);
        Serial.print("湿度: "); Serial.println(humidity);
        Serial.print("天气: "); Serial.println(weather);
        Serial.print("WiFi状态: "); Serial.println(WiFi.status() == WL_CONNECTED ? "已连接" : "未连接");
        Serial.print("AM/PM格式: "); Serial.println(useAmPmFormat ? "是" : "否");
        Serial.print("显示农历: "); Serial.println(showLunar ? "是" : "否");
        Serial.print("自动轮转: "); Serial.println(autoRotatePages ? "是" : "否");
        
        if (WiFi.status() == WL_CONNECTED && !isInConfigMode) {
            Serial.println("Web设置页面: http://" + WiFi.localIP().toString());
        }
        
        // 事件提醒状态调试
        Serial.print("当前提醒: ");
        if (currentReminder.length() > 0) {
            Serial.println("\"" + currentReminder + "\"");
            unsigned long remainingTime = reminderDuration - (millis() - reminderStartTime);
            Serial.print("剩余显示时间: "); Serial.print(remainingTime / 1000); Serial.println(" 秒");
        } else {
            Serial.println("无");
        }
        
        // 时间调试信息
        if (timeClient.isTimeSet())
        {
            time_t epochTime = timeClient.getEpochTime();
            Serial.print("时间戳: "); Serial.println(epochTime);
            Serial.print("当前时间: "); 
            Serial.print(timeClient.getHours()); Serial.print(":");
            Serial.print(timeClient.getMinutes()); Serial.print(":");
            Serial.println(timeClient.getSeconds());
            
            struct tm *ptm = localtime(&epochTime);
            Serial.printf("日期: %04d-%02d-%02d\n", ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);
        }
        else
        {
            Serial.println("时间: 未同步");
        }
        
        lastDebugOutput = millis();
    }
    
    // 检查事件提醒（每10秒检查一次）
    static unsigned long lastEventCheck = 0;
    if (millis() - lastEventCheck > 10000) // 10秒检查一次
    {
        checkEvents();
        lastEventCheck = millis();
    }
    
    // Alarm check every second
    static unsigned long lastAlarmCheck = 0;
    if (millis() - lastAlarmCheck > 1000) {
        checkAlarms();
        lastAlarmCheck = millis();
    }
    
    // 新增：火焰传感器检测（每200ms）
    static unsigned long lastFlameCheck = 0;
    if (millis() - lastFlameCheck > 200) {
        checkFlameSensor();
        lastFlameCheck = millis();
    }

    // Handle unified audio service
    // 音频播放已由 audioTask 独立处理，这里无需再调用
     
    delay(20);
}

String getLunarDate(int year, int month, int day)
{
    // 改进的农历计算算法
    // 农历月份名称
    const char* lunarMonths[] = {"正月", "二月", "三月", "四月", "五月", "六月", 
                                 "七月", "八月", "九月", "十月", "冬月", "腊月"};
    
    // 农历日期名称
    const char* lunarDays[] = {"初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
                               "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
                               "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"};
    
    // 简化的农历数据表（仅2024年部分月份示例）
    // 实际应用中需要完整的农历数据表
    struct LunarData {
        int year;
        int month; 
        int day;
        int lunarMonth;
        int lunarDay;
    };
    
    // 2024年部分农历对照表（示例数据）
    LunarData lunarTable[] = {
        {2024, 1, 1, 11, 20},   // 2024年1月1日 = 农历冬月二十
        {2024, 1, 10, 11, 29},  // 2024年1月10日 = 农历冬月廿九
        {2024, 1, 11, 12, 1},   // 2024年1月11日 = 农历腊月初一
        {2024, 2, 9, 12, 30},   // 2024年2月9日 = 农历腊月三十
        {2024, 2, 10, 1, 1},    // 2024年2月10日 = 农历正月初一（春节）
        {2024, 3, 11, 2, 1},    // 2024年3月11日 = 农历二月初一
        {2024, 4, 9, 3, 1},     // 2024年4月9日 = 农历三月初一
        {2024, 5, 8, 4, 1},     // 2024年5月8日 = 农历四月初一
        {2024, 6, 6, 5, 1},     // 2024年6月6日 = 农历五月初一
        {2024, 7, 6, 6, 1},     // 2024年7月6日 = 农历六月初一
        {2024, 8, 4, 7, 1},     // 2024年8月4日 = 农历七月初一
        {2024, 9, 3, 8, 1},     // 2024年9月3日 = 农历八月初一
        {2024, 10, 2, 9, 1},    // 2024年10月2日 = 农历九月初一
        {2024, 11, 1, 10, 1},   // 2024年11月1日 = 农历十月初一
        {2024, 12, 1, 11, 1},   // 2024年12月1日 = 农历冬月初一
        {2024, 12, 30, 11, 30}, // 2024年12月30日 = 农历冬月三十
    };
    
    int tableSize = sizeof(lunarTable) / sizeof(LunarData);
    
    // 计算目标日期的儒略日数
    int targetJulian = 0;
    if (month > 2) {
        targetJulian = 365 * year + year / 4 - year / 100 + year / 400 + (month - 3) * 30 + (month - 3) / 2 + day;
    } else {
        year--;
        targetJulian = 365 * year + year / 4 - year / 100 + year / 400 + (month + 9) * 30 + (month + 9) / 2 + day;
    }
    
    // 寻找最接近的农历数据
    int bestMatch = 0;
    int minDiff = 99999;
    
    for (int i = 0; i < tableSize; i++) {
        int refJulian = 0;
        int refYear = lunarTable[i].year;
        int refMonth = lunarTable[i].month;
        int refDay = lunarTable[i].day;
        
        if (refMonth > 2) {
            refJulian = 365 * refYear + refYear / 4 - refYear / 100 + refYear / 400 + (refMonth - 3) * 30 + (refMonth - 3) / 2 + refDay;
        } else {
            refYear--;
            refJulian = 365 * refYear + refYear / 4 - refYear / 100 + refYear / 400 + (refMonth + 9) * 30 + (refMonth + 9) / 2 + refDay;
        }
        
        int diff = abs(targetJulian - refJulian);
        if (diff < minDiff) {
            minDiff = diff;
            bestMatch = i;
        }
    }
    
    // 根据最接近的数据计算农历日期
    int dayDiff = targetJulian - (lunarTable[bestMatch].year * 365 + lunarTable[bestMatch].month * 30 + lunarTable[bestMatch].day);
    int lunarMonth = lunarTable[bestMatch].lunarMonth;
    int lunarDay = lunarTable[bestMatch].lunarDay + (dayDiff % 30);
    
    // 处理月份和日期的进位
    while (lunarDay > 30) {
        lunarDay -= 30;
        lunarMonth++;
        if (lunarMonth > 12) {
            lunarMonth = 1;
        }
    }
    while (lunarDay < 1) {
        lunarDay += 30;
        lunarMonth--;
        if (lunarMonth < 1) {
            lunarMonth = 12;
        }
    }
    
    // 确保索引在有效范围内
    if (lunarMonth < 1 || lunarMonth > 12) lunarMonth = 1;
    if (lunarDay < 1 || lunarDay > 30) lunarDay = 1;
    
    String result = String(lunarMonths[lunarMonth - 1]) + String(lunarDays[lunarDay - 1]);
    return result;
}

// 新增：加载显示设置
void loadDisplaySettings()
{
    if (SPIFFS.exists(settingsFile))
    {
        fs::File file = SPIFFS.open(settingsFile, "r");
        if (file)
        {
            DynamicJsonDocument doc(1024);
            DeserializationError error = deserializeJson(doc, file);
            if (!error)
            {
                useAmPmFormat = doc["ampm"] | false;
                showLunar = doc["lunar"] | true;
                autoRotatePages = doc["autorotate"] | false;
                pageRotateInterval = doc["interval"] | 10;
                
                Serial.println("显示设置已加载：");
                Serial.printf("AM/PM格式: %s\n", useAmPmFormat ? "是" : "否");
                Serial.printf("显示农历: %s\n", showLunar ? "是" : "否");
                Serial.printf("自动轮转: %s\n", autoRotatePages ? "是" : "否");
                Serial.printf("轮转间隔: %d秒\n", pageRotateInterval);
            }
            file.close();
        }
    }
}

// 新增：保存显示设置
void saveDisplaySettings()
{
    DynamicJsonDocument doc(1024);
    doc["ampm"] = useAmPmFormat;
    doc["lunar"] = showLunar;
    doc["autorotate"] = autoRotatePages;
    doc["interval"] = pageRotateInterval;
    
    fs::File file = SPIFFS.open(settingsFile, "w");
    if (file)
    {
        serializeJson(doc, file);
        file.close();
        Serial.println("显示设置已保存");
    }
}

// 新增：设置Web服务器（连接WiFi后使用）
void setupWebServer()
{
    // 处理设置页面请求
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        String html = R"(
<!DOCTYPE html>
<html lang='zh-CN'>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>TV-PRO 智能时钟 - 设置面板</title>
    <style>
        body { 
            font-family: 'Microsoft YaHei', Arial, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            margin: 0; padding: 20px; min-height: 100vh; color: #333;
        }
        .container { 
            max-width: 800px; margin: 0 auto; 
            background: rgba(255,255,255,0.95); 
            padding: 30px; border-radius: 15px; 
            box-shadow: 0 10px 30px rgba(0,0,0,0.2);
        }
        h1 { color: #4a5568; text-align: center; margin-bottom: 20px; }
        .section { 
            background: #f7fafc; padding: 20px; margin: 20px 0; 
            border-left: 4px solid #3182ce; border-radius: 8px; 
        }
        .form-group { margin: 15px 0; }
        label { display: block; margin-bottom: 5px; font-weight: bold; color: #4a5568; }
        input[type="text"], input[type="password"], select { 
            width: 100%; padding: 12px; border: 2px solid #e2e8f0; 
            border-radius: 8px; font-size: 16px; box-sizing: border-box;
        }
        input[type="checkbox"] { transform: scale(1.5); margin-right: 10px; }
        .button { 
            padding: 12px 24px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white; border: none; border-radius: 8px; 
            font-size: 16px; font-weight: bold; cursor: pointer; margin: 10px;
        }
        .button:hover { transform: translateY(-2px); }
        .status { padding: 15px; margin: 15px 0; border-radius: 8px; }
        .success { background: #f0fff4; border: 1px solid #9ae6b4; color: #2d5016; }
        .info { background: #ebf8ff; border: 1px solid #90cdf4; color: #1a365d; }
    </style>
</head>
<body>
    <div class='container'>
        <h1>🕐 TV-PRO 智能时钟设置</h1>
        
        <div class='status success'>
            <strong>✅ 设备状态：</strong>已连接WiFi，功能正常运行
        </div>
        
        <div class='section'>
            <h3>🌐 网络配置</h3>
            <form action='/wifi' method='POST'>
                <div class='form-group'>
                    <label>WiFi名称</label>
                    <input type='text' name='ssid' placeholder='当前WiFi名称'>
                </div>
                <div class='form-group'>
                    <label>WiFi密码</label>
                    <input type='password' name='pass' placeholder='WiFi密码'>
                </div>
                <input type='submit' class='button' value='更新WiFi设置'>
            </form>
        </div>
        
        <div class='section'>
            <h3>🌤️ 天气配置</h3>
            <form action='/weather' method='POST'>
                <div class='form-group'>
                    <label>城市名称</label>
                    <input type='text' name='city' placeholder='如：beijing、shanghai' value=')" + cityname + R"('>
                </div>
                <div class='form-group'>
                    <label>天气API密钥</label>
                    <input type='text' name='api' placeholder='心知天气API密钥' value=')" + weatherapi + R"('>
                </div>
                <input type='submit' class='button' value='更新天气设置'>
            </form>
        </div>
        
        <div class='section'>
            <h3>🎨 显示设置</h3>
            <form action='/display' method='POST'>
                <div class='form-group'>
                    <label>
                        <input type='checkbox' name='ampm' )" + String(useAmPmFormat ? "checked" : "") + R"(>
                        使用12小时制（AM/PM格式）
                    </label>
                </div>
                <div class='form-group'>
                    <label>
                        <input type='checkbox' name='lunar' )" + String(showLunar ? "checked" : "") + R"(>
                        显示农历日期
                    </label>
                </div>
                <div class='form-group'>
                    <label>
                        <input type='checkbox' name='autorotate' )" + String(autoRotatePages ? "checked" : "") + R"(>
                        自动轮转页面
                    </label>
                </div>
                <div class='form-group'>
                    <label>轮转间隔（秒）</label>
                    <select name='interval'>
                        <option value='5' )" + String(pageRotateInterval == 5 ? "selected" : "") + R"(>5秒</option>
                        <option value='10' )" + String(pageRotateInterval == 10 ? "selected" : "") + R"(>10秒</option>
                        <option value='15' )" + String(pageRotateInterval == 15 ? "selected" : "") + R"(>15秒</option>
                        <option value='30' )" + String(pageRotateInterval == 30 ? "selected" : "") + R"(>30秒</option>
                    </select>
                </div>
                <input type='submit' class='button' value='更新显示设置'>
            </form>
        </div>
        
        <div class='section'>
            <h3>⏰ 闹钟设置</h3>
            <p>设置最多3个闹钟，可以分别设置时间和标签。</p>
            <a href='/alarms' style='text-decoration: none;'>
                <button class='button'>管理闹钟</button>
            </a>
        </div>
        
        <div class='section'>
            <h3>📅 事件提醒设置</h3>
            <p>设置最多4个重要事件提醒，支持前一天和当天提醒。</p>
            <a href='/events' style='text-decoration: none;'>
                <button class='button'>管理事件</button>
            </a>
        </div>
        
        <div class='section'>
            <h3>🔊 蜂鸣器测试</h3>
            <p>测试设备蜂鸣器功能，可以启动持续蜂鸣或停止蜂鸣。</p>
            <button class='button' onclick='startBuzzerTest()' style='background: linear-gradient(135deg, #48bb78 0%, #38a169 100%);'>▶️ 开始蜂鸣测试</button>
            <button class='button' onclick='stopBuzzerTest()' style='background: linear-gradient(135deg, #f56565 0%, #e53e3e 100%);'>⏹️ 停止蜂鸣测试</button>
        </div>
        
        <div class='section'>
            <h3>🎵 音乐播放控制</h3>
            <p>播放存储在设备中的MP3音乐文件，支持自动循环播放。</p>
            <div style='margin: 15px 0;'>
                <button class='button' onclick='playMusic()' style='width: 48%; margin-right: 4%; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);'>🎵 播放音乐</button>
                <button class='button' onclick='stopMusic()' style='width: 48%; background: linear-gradient(135deg, #f56565 0%, #e53e3e 100%);'>⏹️ 停止播放</button>
            </div>
            <div id='musicStatus' style='margin: 10px 0; padding: 10px; background: #f7fafc; border-radius: 6px; color: #4a5568; font-size: 0.9em; border: 1px solid #e2e8f0;'>
                🎶 点击播放按钮开始播放音乐
            </div>
            <div id='alarmStatus' style='margin: 10px 0; padding: 10px; background: #f7fafc; border-radius: 6px; color: #e53e3e; font-size: 0.9em; border: 1px solid #e2e8f0; display: none;'>
                <!-- 闹钟状态 -->
            </div>
        </div>
        
        <div class='section'>
            <h3>📊 设备信息</h3>
            <div class='info'>
                <p><strong>设备IP：</strong>)" + WiFi.localIP().toString() + R"(</p>
                <p><strong>WiFi强度：</strong>)" + String(WiFi.RSSI()) + R"( dBm</p>
                <p><strong>运行时间：</strong>)" + String(millis() / 1000) + R"( 秒</p>
                <p><strong>城市：</strong>)" + cityname + R"(</p>
                <p><strong>温度：</strong>)" + temperature + R"(°C</p>
                <p><strong>湿度：</strong>)" + humidity + R"(%</p>
            </div>
        </div>
        
        <div style='text-align: center; margin-top: 30px;'>
            <button class='button' onclick='location.reload()'>🔄 刷新页面</button>
            <button class='button' onclick='fetch("/restart")'>🔄 重启设备</button>
        </div>
    </div>
    
    <script>
        function testBuzzer() {
            fetch('/beep')
                .then(response => response.text())
                .then(data => {
                    alert('蜂鸣器测试完成！');
                })
                .catch(error => {
                    alert('蜂鸣器测试失败：' + error);
                });
        }
        
        function startBuzzerTest() {
            fetch('/beep-start')
                .then(response => response.text())
                .then(data => {
                    alert('蜂鸣器测试已开始，点击停止按钮结束测试');
                })
                .catch(error => {
                    alert('启动蜂鸣器测试失败：' + error);
                });
        }
        
        function stopBuzzerTest() {
            fetch('/beep-stop')
                .then(response => response.text())
                .then(data => {
                    alert('蜂鸣器测试已停止');
                })
                .catch(error => {
                    alert('停止蜂鸣器测试失败：' + error);
                });
        }
        
        // 音乐播放控制函数
        function playMusic() {
            fetch('/play-music', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                }
            })
            .then(response => response.json())
            .then(data => {
                const statusDiv = document.getElementById('musicStatus');
                if (data.success) {
                    statusDiv.innerHTML = '🎵 ' + data.message;
                    statusDiv.style.background = '#f0fff4';
                    statusDiv.style.color = '#2d5016';
                    statusDiv.style.borderColor = '#9ae6b4';
                } else {
                    statusDiv.innerHTML = '❌ ' + data.message;
                    statusDiv.style.background = '#fff5f5';
                    statusDiv.style.color = '#c53030';
                    statusDiv.style.borderColor = '#fed7d7';
                }
            })
            .catch(error => {
                console.error('播放音乐失败:', error);
                const statusDiv = document.getElementById('musicStatus');
                statusDiv.innerHTML = '❌ 播放请求失败，请检查网络连接';
                statusDiv.style.background = '#fff5f5';
                statusDiv.style.color = '#c53030';
                statusDiv.style.borderColor = '#fed7d7';
            });
        }
        
        function stopMusic() {
            fetch('/stop-music', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                }
            })
            .then(response => response.json())
            .then(data => {
                const statusDiv = document.getElementById('musicStatus');
                if (data.success) {
                    statusDiv.innerHTML = '⏹️ ' + data.message;
                    statusDiv.style.background = '#f7fafc';
                    statusDiv.style.color = '#4a5568';
                    statusDiv.style.borderColor = '#e2e8f0';
                } else {
                    statusDiv.innerHTML = '❌ ' + data.message;
                    statusDiv.style.background = '#fff5f5';
                    statusDiv.style.color = '#c53030';
                    statusDiv.style.borderColor = '#fed7d7';
                }
            })
            .catch(error => {
                console.error('停止音乐失败:', error);
                const statusDiv = document.getElementById('musicStatus');
                statusDiv.innerHTML = '❌ 停止请求失败，请检查网络连接';
                statusDiv.style.background = '#fff5f5';
                statusDiv.style.color = '#c53030';
                statusDiv.style.borderColor = '#fed7d7';
            });
        }
    </script>
</body>
</html>
        )";
        request->send(200, "text/html; charset=UTF-8", html);
    });
    
    // 处理WiFi设置更新
    server.on("/wifi", HTTP_POST, [](AsyncWebServerRequest *request)
    {
        String ssid = request->getParam("ssid", true)->value();
        String pass = request->getParam("pass", true)->value();
        
        if (ssid.length() > 0) {
            // 保存新的WiFi配置
            DynamicJsonDocument doc(1024);
            doc["ssid"] = ssid;
            doc["pass"] = pass;
            doc["city"] = cityname;
            doc["api"] = weatherapi;
            
            fs::File file = SPIFFS.open(ssidFile, "w");
            if (file) {
                serializeJson(doc, file);
                file.close();
            }
            
            request->send(200, "text/html; charset=UTF-8", 
                "<!DOCTYPE html><html><head><meta charset='UTF-8'></head><body>"
                "<h1>WiFi设置已更新，设备将重启并尝试连接新WiFi</h1>"
                "<script>setTimeout(function(){window.location.href='/';}, 3000);</script>"
                "</body></html>");
            
            delay(2000);
            ESP.restart();
        } else {
            request->send(400, "text/plain", "Invalid SSID");
        }
    });
    
    // 处理天气设置更新
    server.on("/weather", HTTP_POST, [](AsyncWebServerRequest *request)
    {
        String city = request->getParam("city", true)->value();
        String api = request->getParam("api", true)->value();
        
        cityname = city;
        weatherapi = api;
        
        // 保存到配置文件
        DynamicJsonDocument doc(1024);
        if (SPIFFS.exists(ssidFile)) {
            fs::File file = SPIFFS.open(ssidFile, "r");
            if (file) {
                deserializeJson(doc, file);
                file.close();
            }
        }
        doc["city"] = city;
        doc["api"] = api;
        
        fs::File file = SPIFFS.open(ssidFile, "w");
        if (file) {
            serializeJson(doc, file);
            file.close();
        }
        
        // 重新获取天气
        initweather = false;
        fetchWeather();
        
        request->send(200, "text/html; charset=UTF-8", 
            "<!DOCTYPE html><html><head><meta charset='UTF-8'></head><body>"
            "<h1>天气设置已更新</h1>"
            "<script>setTimeout(function(){window.location.href='/';}, 2000);</script>"
            "</body></html>");
    });
    
    // 处理显示设置更新
    server.on("/display", HTTP_POST, [](AsyncWebServerRequest *request)
    {
        useAmPmFormat = request->hasParam("ampm", true);
        showLunar = request->hasParam("lunar", true);
        autoRotatePages = request->hasParam("autorotate", true);
        
        if (request->hasParam("interval", true)) {
            pageRotateInterval = request->getParam("interval", true)->value().toInt();
        }
        
        saveDisplaySettings();
        
        request->send(200, "text/html; charset=UTF-8", 
            "<!DOCTYPE html><html><head><meta charset='UTF-8'></head><body>"
            "<h1>显示设置已更新</h1>"
            "<script>setTimeout(function(){window.location.href='/';}, 2000);</script>"
            "</body></html>");
    });
    
    // 添加闹钟设置路由
    server.on("/alarms", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        String html = R"(
<!DOCTYPE html>
<html lang='zh-CN'>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>闹钟设置 - TV-PRO</title>
    <style>
        body { 
            font-family: 'Microsoft YaHei', Arial, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            margin: 0; padding: 20px; min-height: 100vh; color: #333;
        }
        .container { 
            max-width: 600px; margin: 0 auto; 
            background: rgba(255,255,255,0.95); 
            padding: 30px; border-radius: 15px; 
            box-shadow: 0 10px 30px rgba(0,0,0,0.2);
        }
        h1 { color: #4a5568; text-align: center; margin-bottom: 20px; }
        .alarm-card {
            background: #f7fafc;
            padding: 20px;
            margin: 15px 0;
            border-radius: 8px;
            border-left: 4px solid #3182ce;
        }
        .form-group { margin: 15px 0; }
        label { display: block; margin-bottom: 5px; font-weight: bold; color: #4a5568; }
        input[type="time"] { 
            width: 150px; 
            padding: 8px; 
            border: 2px solid #e2e8f0; 
            border-radius: 8px; 
            font-size: 16px;
        }
        input[type="text"] { 
            width: 100%; 
            padding: 8px; 
            border: 2px solid #e2e8f0; 
            border-radius: 8px; 
            font-size: 16px;
            box-sizing: border-box;
        }
        input[type="checkbox"] { 
            transform: scale(1.5); 
            margin-right: 10px; 
        }
        .button {
            padding: 12px 24px;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            color: white;
            border: none;
            border-radius: 8px;
            font-size: 16px;
            font-weight: bold;
            cursor: pointer;
            margin: 10px 0;
            width: 100%;
        }
        .button:hover { transform: translateY(-2px); }
    </style>
</head>
<body>
    <div class='container'>
        <h1>⏰ 闹钟设置</h1>
        <form action='/save-alarms' method='POST'>)";
        
        // 为每个闹钟生成设置卡片
        for (int i = 0; i < MAX_ALARMS; i++) {
            html += "<div class='alarm-card'>";
            html += "<h3>闹钟 " + String(i + 1) + "</h3>";
            
            // 启用开关
            html += "<div class='form-group'><label>";
            html += "<input type='checkbox' name='enabled" + String(i) + "' " + 
                   (alarms[i].enabled ? "checked" : "") + ">";
            html += "启用此闹钟</label></div>";
            
            // 时间设置
            html += "<div class='form-group'><label>时间</label>";
            char timeStr[6];
            snprintf(timeStr, sizeof(timeStr), "%02d:%02d", alarms[i].hour, alarms[i].minute);
            html += "<input type='time' name='time" + String(i) + "' value='" + String(timeStr) + "'>";
            html += "</div>";
            
            // 标签设置
            html += "<div class='form-group'><label>标签</label>";
            html += "<input type='text' name='label" + String(i) + "' value='" + 
                   alarms[i].label + "' placeholder='闹钟标签'>";
            html += "</div>";
            
            html += "</div>";
        }
        
        html += R"(
            <input type='submit' class='button' value='保存所有闹钟设置'>
            <a href='/' style='text-decoration: none;'><button type='button' class='button' style='background: #718096;'>返回主页</button></a>
        </form>
    </div>
</body>
</html>
        )";
        
        request->send(200, "text/html; charset=UTF-8", html);
    });
    
    // 处理闹钟设置保存
    server.on("/save-alarms", HTTP_POST, [](AsyncWebServerRequest *request)
    {
        for (int i = 0; i < MAX_ALARMS; i++) {
            // 更新启用状态
            alarms[i].enabled = request->hasParam("enabled" + String(i), true);
            
            // 更新时间
            if (request->hasParam("time" + String(i), true)) {
                String timeStr = request->getParam("time" + String(i), true)->value();
                int hour, minute;
                sscanf(timeStr.c_str(), "%d:%d", &hour, &minute);
                alarms[i].hour = hour;
                alarms[i].minute = minute;
            }
            
            // 更新标签
            if (request->hasParam("label" + String(i), true)) {
                alarms[i].label = request->getParam("label" + String(i), true)->value();
            }
        }
        
        // 保存闹钟设置
        saveAlarms();
        
        request->send(200, "text/html; charset=UTF-8", 
            "<!DOCTYPE html><html><head><meta charset='UTF-8'></head><body>"
            "<h1>闹钟设置已保存</h1>"
            "<script>setTimeout(function(){window.location.href='/alarms';}, 2000);</script>"
            "</body></html>");
    });
    
    // 添加事件设置页面
    server.on("/events", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        String html = R"(
<!DOCTYPE html>
<html lang='zh-CN'>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>事件提醒设置 - TV-PRO</title>
    <style>
        body { 
            font-family: 'Microsoft YaHei', Arial, sans-serif;
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            margin: 0; padding: 20px; min-height: 100vh; color: #333;
        }
        .container { 
            max-width: 800px; margin: 0 auto; 
            background: rgba(255,255,255,0.95); 
            padding: 30px; border-radius: 15px; 
            box-shadow: 0 10px 30px rgba(0,0,0,0.2);
        }
        h1 { color: #4a5568; text-align: center; margin-bottom: 20px; }
        .event-card {
            background: #f7fafc;
            padding: 20px;
            margin: 15px 0;
            border-radius: 8px;
            border-left: 4px solid #38b2ac;
        }
        .form-group { margin: 15px 0; }
        label { display: block; margin-bottom: 5px; font-weight: bold; color: #4a5568; }
        input[type="date"], input[type="time"] { 
            width: 150px; 
            padding: 8px; 
            border: 2px solid #e2e8f0; 
            border-radius: 8px; 
            font-size: 16px;
            margin-right: 10px;
        }
        input[type="text"], textarea { 
            width: 100%; 
            padding: 8px; 
            border: 2px solid #e2e8f0; 
            border-radius: 8px; 
            font-size: 16px;
            box-sizing: border-box;
        }
        textarea {
            height: 60px;
            resize: vertical;
        }
        input[type="checkbox"] { 
            transform: scale(1.5); 
            margin-right: 10px; 
        }
        .button {
            padding: 12px 24px;
            background: linear-gradient(135deg, #38b2ac 0%, #319795 100%);
            color: white;
            border: none;
            border-radius: 8px;
            font-size: 16px;
            font-weight: bold;
            cursor: pointer;
            margin: 10px 0;
            width: 100%;
        }
        .button:hover { transform: translateY(-2px); }
        .back-button {
            background: linear-gradient(135deg, #718096 0%, #4a5568 100%);
        }
        .datetime-group {
            display: flex;
            align-items: center;
            gap: 10px;
            flex-wrap: wrap;
        }
    </style>
</head>
<body>
    <div class='container'>
        <h1>📅 事件提醒设置</h1>
        <p style='text-align: center; color: #666;'>设置重要事件，系统会在事件发生前一天和当天进行提醒</p>
        
        <form action='/save-events' method='POST'>)";
        
        // 为每个事件生成设置卡片
        for (int i = 0; i < MAX_EVENTS; i++) {
            html += "<div class='event-card'>";
            html += "<h3>📌 事件 " + String(i + 1) + "</h3>";
            
            // 启用开关
            html += "<div class='form-group'><label>";
            html += "<input type='checkbox' name='enabled" + String(i) + "' " + 
                   (events[i].enabled ? "checked" : "") + ">";
            html += "启用此事件提醒</label></div>";
            
            // 事件标题
            html += "<div class='form-group'><label>事件标题</label>";
            html += "<input type='text' name='title" + String(i) + "' value='" + 
                   events[i].title + "' placeholder='如：张三生日、重要会议等'>";
            html += "</div>";
            
            // 事件描述
            html += "<div class='form-group'><label>事件描述（可选）</label>";
            html += "<textarea name='description" + String(i) + "' placeholder='详细描述或备注信息'>" + 
                   events[i].description + "</textarea>";
            html += "</div>";
            
            // 日期设置
            html += "<div class='form-group'><label>事件日期</label>";
            char dateStr[11];
            snprintf(dateStr, sizeof(dateStr), "%04d-%02d-%02d", events[i].year, events[i].month, events[i].day);
            html += "<input type='date' name='date" + String(i) + "' value='" + String(dateStr) + "'>";
            html += "</div>";
            
            // 是否设置具体时间
            html += "<div class='form-group'><label>";
            html += "<input type='checkbox' name='hasTime" + String(i) + "' " + 
                   (events[i].hasTime ? "checked" : "") + " onchange='toggleTime(" + String(i) + ")'>";
            html += "设置具体时间</label></div>";
            
            // 时间设置（默认隐藏）
            html += "<div class='form-group' id='timeGroup" + String(i) + "' style='display:" + 
                   (events[i].hasTime ? "block" : "none") + "'>";
            html += "<label>提醒时间</label>";
            char timeStr[6];
            snprintf(timeStr, sizeof(timeStr), "%02d:%02d", events[i].hour, events[i].minute);
            html += "<input type='time' name='time" + String(i) + "' value='" + String(timeStr) + "'>";
            html += "</div>";
            
            // 提前提醒设置
            html += "<div class='form-group'><label>";
            html += "<input type='checkbox' name='notifyBefore" + String(i) + "' " + 
                   (events[i].notifyBefore ? "checked" : "") + ">";
            html += "提前一天提醒</label></div>";
            
            html += "</div>";
        }
        
        html += R"(
            <input type='submit' class='button' value='💾 保存所有事件设置'>
            <a href='/' style='text-decoration: none;'>
                <button type='button' class='button back-button'>🏠 返回主页</button>
            </a>
        </form>
    </div>
    
    <script>
        function toggleTime(index) {
            var checkbox = document.querySelector('[name="hasTime' + index + '"]');
            var timeGroup = document.getElementById('timeGroup' + index);
            timeGroup.style.display = checkbox.checked ? 'block' : 'none';
        }
    </script>
</body>
</html>
        )";
        
        request->send(200, "text/html; charset=UTF-8", html);
    });
    
    // 处理事件设置保存
    server.on("/save-events", HTTP_POST, [](AsyncWebServerRequest *request)
    {
        for (int i = 0; i < MAX_EVENTS; i++) {
            // 更新启用状态
            events[i].enabled = request->hasParam("enabled" + String(i), true);
            
            // 更新标题
            if (request->hasParam("title" + String(i), true)) {
                events[i].title = request->getParam("title" + String(i), true)->value();
            }
            
            // 更新描述
            if (request->hasParam("description" + String(i), true)) {
                events[i].description = request->getParam("description" + String(i), true)->value();
            }
            
            // 更新日期
            if (request->hasParam("date" + String(i), true)) {
                String dateStr = request->getParam("date" + String(i), true)->value();
                int year, month, day;
                sscanf(dateStr.c_str(), "%d-%d-%d", &year, &month, &day);
                events[i].year = year;
                events[i].month = month;
                events[i].day = day;
            }
            
            // 更新是否有具体时间
            events[i].hasTime = request->hasParam("hasTime" + String(i), true);
            
            // 更新时间
            if (request->hasParam("time" + String(i), true)) {
                String timeStr = request->getParam("time" + String(i), true)->value();
                int hour, minute;
                sscanf(timeStr.c_str(), "%d:%d", &hour, &minute);
                events[i].hour = hour;
                events[i].minute = minute;
            }
            
            // 更新提前提醒设置
            events[i].notifyBefore = request->hasParam("notifyBefore" + String(i), true);
        }
        
        // 保存事件设置
        saveEvents();
        
        request->send(200, "text/html; charset=UTF-8", 
            "<!DOCTYPE html><html><head><meta charset='UTF-8'></head><body>"
            "<h1>✅ 事件设置已保存</h1>"
            "<p>事件提醒功能已更新，系统将在设定时间进行提醒。</p>"
            "<script>setTimeout(function(){window.location.href='/events';}, 3000);</script>"
            "</body></html>");
    });
    
    // 处理重启请求
    server.on("/restart", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        request->send(200, "text/plain", "Device restarting...");
        delay(1000);
        ESP.restart();
    });
    
    // 添加设备状态API
    server.on("/status", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        DynamicJsonDocument doc(1024);
        doc["device"] = "TV-PRO Smart Clock";
        doc["ip"] = WiFi.localIP().toString();
        doc["wifi_ssid"] = WiFi.SSID();
        doc["wifi_rssi"] = WiFi.RSSI();
        doc["uptime"] = millis() / 1000;
        doc["city"] = cityname;
        doc["temperature"] = temperature;
        doc["humidity"] = humidity;
        doc["weather"] = weather;
        doc["ampm_format"] = useAmPmFormat;
        doc["show_lunar"] = showLunar;
        doc["auto_rotate"] = autoRotatePages;
        doc["rotate_interval"] = pageRotateInterval;
        doc["current_page"] = currentPage;
        doc["alarm_ringing"] = alarmRinging ? "true" : "false";
        doc["flame_alarm"] = flameAlarmActive ? "true" : "false"; // 新增火焰警报状态
        
        String output;
        serializeJson(doc, output);
        request->send(200, "application/json", output);
    });
    
    // 添加帮助页面
    server.on("/help", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        String helpHtml = R"(
<!DOCTYPE html>
<html lang='zh-CN'>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>TV-PRO 帮助页面</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; background: #f5f5f5; }
        .container { max-width: 600px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
        h1 { color: #333; text-align: center; }
        .info { background: #e8f4fd; padding: 15px; margin: 15px 0; border-radius: 5px; }
        .button { display: inline-block; padding: 10px 20px; background: #007cba; color: white; text-decoration: none; border-radius: 5px; margin: 5px; }
        .button:hover { background: #005a87; }
    </style>
</head>
<body>
    <div class='container'>
        <h1>🕐 TV-PRO 智能时钟</h1>
        
        <div class='info'>
            <h3>📱 当前设备状态</h3>
            <p><strong>设备IP：</strong>)" + WiFi.localIP().toString() + R"(</p>
            <p><strong>WiFi网络：</strong>)" + WiFi.SSID() + R"(</p>
            <p><strong>信号强度：</strong>)" + String(WiFi.RSSI()) + R"( dBm</p>
            <p><strong>运行时间：</strong>)" + String(millis() / 1000) + R"( 秒</p>
        </div>
        
        <div class='info'>
            <h3>🎯 快速访问</h3>
            <a href='/' class='button'>🏠 设置主页</a>
            <a href='/status' class='button'>📊 设备状态</a>
            <a href='/restart' class='button'>🔄 重启设备</a>
        </div>
        
        <div class='info'>
            <h3>⚙️ 功能说明</h3>
            <p><strong>AM/PM设置：</strong>在设置页面可以切换12/24小时制显示</p>
            <p><strong>农历显示：</strong>可以在主页面和专用农历页面显示农历日期</p>
            <p><strong>自动轮转：</strong>可设置页面自动切换和间隔时间</p>
            <p><strong>按键操作：</strong>左右键切换页面，中键回主页或长按进入配置</p>
        </div>
        
        <div class='info'>
            <h3>📱 页面说明</h3>
            <p><strong>页面0：</strong>主页面 - 时钟、日期、天气信息</p>
            <p><strong>页面1：</strong>天气页面 - 实时温湿度</p>
            <p><strong>页面2：</strong>闹钟页面 - 闹钟设置与时间</p>
            <p><strong>页面3：</strong>事件提醒页面 - 事件设置与时间</p>
            <p><strong>页面4：</strong>网络信息页面 - WiFi与IP信息</p>
            <p><strong>页面5：</strong>模拟时钟页面 - 经典指针时钟</p>
        </div>
    </div>
</body>
</html>
        )";
        request->send(200, "text/html; charset=UTF-8", helpHtml);
    });
    
    // Play beep sound once via web request
    server.on("/beep", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        generateTone(1000, 500);
        request->send(200, "text/plain", "Beep played");
    });
    
    // Start continuous buzzer test
    server.on("/beep-start", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        startBuzzerTest();
        request->send(200, "text/plain", "Buzzer test started");
    });
    
    // Stop continuous buzzer test
    server.on("/beep-stop", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        stopBuzzerTest();
        request->send(200, "text/plain", "Buzzer test stopped");
    });
    
    // Music playback controls
    server.on("/play-music", HTTP_POST, [](AsyncWebServerRequest *request)
    {
        startMusicLoop();
        String response = "{";
        response += "\"success\":true,";
        response += "\"message\":\"音乐播放已开始\"";
        response += "}";
        request->send(200, "application/json", response);
    });
    
    server.on("/stop-music", HTTP_POST, [](AsyncWebServerRequest *request)
    {
        stopMusicLoop();
        String response = "{";
        response += "\"success\":true,";
        response += "\"message\":\"音乐播放已停止\"";
        response += "}";
        request->send(200, "application/json", response);
    });
    
    server.begin();
    webServerStarted = true;
    Serial.println("Web设置服务器已启动，访问地址：http://" + WiFi.localIP().toString());
    Serial.println("可用页面：");
    Serial.println("  主设置页面: http://" + WiFi.localIP().toString() + "/");
    Serial.println("  帮助页面:   http://" + WiFi.localIP().toString() + "/help");
    Serial.println("  设备状态:   http://" + WiFi.localIP().toString() + "/status");
}

// DHT11 读取函数
void updateDHT11() {
    unsigned long now = millis();
    if (now - lastDHTRead > dhtReadInterval) {
        float t = dht.readTemperature();
        float h = dht.readHumidity();
        if (!isnan(t) && !isnan(h)) {
            dht11_temp = t;
            dht11_humi = h;
        }
        lastDHTRead = now;
    }
}

// 加载闹钟配置
void loadAlarms() {
    if (SPIFFS.exists(alarmsFile)) {
        fs::File file = SPIFFS.open(alarmsFile, "r");
        if (file) {
            DynamicJsonDocument doc(1024);
            DeserializationError error = deserializeJson(doc, file);
            if (!error) {
                JsonArray alarmsArray = doc["alarms"].as<JsonArray>();
                int i = 0;
                for (JsonObject alarmObj : alarmsArray) {
                    if (i < MAX_ALARMS) {
                        alarms[i].enabled = alarmObj["enabled"] | false;
                        alarms[i].hour = alarmObj["hour"] | 0;
                        alarms[i].minute = alarmObj["minute"] | 0;
                        alarms[i].label = alarmObj["label"] | String("Alarm " + String(i + 1));
                        i++;
                    }
                }
                Serial.println("Alarms configuration loaded");
            }
            file.close();
        }
    }
}

// 保存闹钟配置
void saveAlarms() {
    DynamicJsonDocument doc(1024);
    JsonArray alarmsArray = doc.createNestedArray("alarms");
    
    for (int i = 0; i < MAX_ALARMS; i++) {
        JsonObject alarmObj = alarmsArray.createNestedObject();
        alarmObj["enabled"] = alarms[i].enabled;
        alarmObj["hour"] = alarms[i].hour;
        alarmObj["minute"] = alarms[i].minute;
        alarmObj["label"] = alarms[i].label;
    }
    
    fs::File file = SPIFFS.open(alarmsFile, "w");
    if (file) {
        serializeJson(doc, file);
        file.close();
        Serial.println("Alarms configuration saved");
    }
}

// 加载事件配置
void loadEvents() {
    if (SPIFFS.exists(eventsFile)) {
        fs::File file = SPIFFS.open(eventsFile, "r");
        if (file) {
            DynamicJsonDocument doc(1024);
            DeserializationError error = deserializeJson(doc, file);
            if (!error) {
                JsonArray eventsArray = doc["events"].as<JsonArray>();
                int i = 0;
                for (JsonObject eventObj : eventsArray) {
                    if (i < MAX_EVENTS) {
                        events[i].enabled = eventObj["enabled"] | false;
                        events[i].year = eventObj["year"] | 0;
                        events[i].month = eventObj["month"] | 0;
                        events[i].day = eventObj["day"] | 0;
                        events[i].hour = eventObj["hour"] | 0;
                        events[i].minute = eventObj["minute"] | 0;
                        events[i].title = eventObj["title"] | String("Event " + String(i + 1));
                        events[i].description = eventObj["description"] | String("");
                        events[i].hasTime = eventObj["hasTime"] | false;
                        events[i].notifyBefore = eventObj["notifyBefore"] | true;
                        i++;
                    }
                }
                Serial.println("Events configuration loaded");
            }
            file.close();
        }
    }
}

// 保存事件配置
void saveEvents() {
    DynamicJsonDocument doc(1024);
    JsonArray eventsArray = doc.createNestedArray("events");
    
    for (int i = 0; i < MAX_EVENTS; i++) {
        JsonObject eventObj = eventsArray.createNestedObject();
        eventObj["enabled"] = events[i].enabled;
        eventObj["year"] = events[i].year;
        eventObj["month"] = events[i].month;
        eventObj["day"] = events[i].day;
        eventObj["hour"] = events[i].hour;
        eventObj["minute"] = events[i].minute;
        eventObj["title"] = events[i].title;
        eventObj["description"] = events[i].description;
        eventObj["hasTime"] = events[i].hasTime;
        eventObj["notifyBefore"] = events[i].notifyBefore;
    }
    
    fs::File file = SPIFFS.open(eventsFile, "w");
    if (file) {
        serializeJson(doc, file);
        file.close();
        Serial.println("Events configuration saved");
    }
}

// 检查事件提醒
void checkEvents() {
    if (WiFi.status() != WL_CONNECTED || !timeClient.isTimeSet()) {
        return; // 没有网络或时间未同步时不检查
    }
    
    // 获取当前时间
    time_t epochTime = timeClient.getEpochTime();
    struct tm *ptm = localtime(&epochTime);
    int currentYear = ptm->tm_year + 1900;
    int currentMonth = ptm->tm_mon + 1;
    int currentDay = ptm->tm_mday;
    int currentHour = ptm->tm_hour;
    int currentMinute = ptm->tm_min;
    
    String nearestEvent = "";
    bool foundNearEvent = false;
    
    for (int i = 0; i < MAX_EVENTS; i++) {
        if (!events[i].enabled) continue;
        
        bool shouldNotify = false;
        String notificationText = "";
        
        // 检查是否是事件当天
        if (events[i].year == currentYear && 
            events[i].month == currentMonth && 
            events[i].day == currentDay) {
            
            if (events[i].hasTime) {
                // 有具体时间的事件，计算时间差（分钟）
                int eventTotalMinutes = events[i].hour * 60 + events[i].minute;
                int currentTotalMinutes = currentHour * 60 + currentMinute;
                int timeDiff = eventTotalMinutes - currentTotalMinutes;
                
                // 提前5分钟到事件结束后5分钟内提醒
                if (timeDiff <= 5 && timeDiff >= -5) {
                    shouldNotify = true;
                    if (timeDiff > 0) {
                        notificationText = "In " + String(timeDiff) + " minutes: " + events[i].title;
                    } else if (timeDiff == 0) {
                        notificationText = "Now: " + events[i].title;
                    } else {
                        notificationText = "Just finished: " + events[i].title;
                    }
                    if (events[i].description.length() > 0) {
                        notificationText += " - " + events[i].description;
                    }
                }
            } else {
                // 没有具体时间的全天事件
                shouldNotify = true;
                notificationText = "Today: " + events[i].title;
                if (events[i].description.length() > 0) {
                    notificationText += " - " + events[i].description;
                }
            }
        }
        // 检查是否是明天的事件（提前一天提醒）
        else if (events[i].notifyBefore) {
            // 简单的明天检查：年月相同，日期+1
            bool isTomorrow = false;
            
            if (events[i].year == currentYear && events[i].month == currentMonth) {
                if (events[i].day == currentDay + 1) {
                    isTomorrow = true;
                }
                // 处理月末情况（简化处理）
                else if (currentDay >= 28 && events[i].day == 1 && currentMonth < 12) {
                    isTomorrow = true;
                }
                // 处理年末情况
                else if (currentDay == 31 && currentMonth == 12 && 
                        events[i].year == currentYear + 1 && events[i].month == 1 && events[i].day == 1) {
                    isTomorrow = true;
                }
            }
            
            if (isTomorrow) {
                shouldNotify = true;
                notificationText = "Tomorrow: " + events[i].title;
                if (events[i].hasTime) {
                    char timeStr[6];
                    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", events[i].hour, events[i].minute);
                    notificationText += " (" + String(timeStr) + ")";
                }
                if (events[i].description.length() > 0) {
                    notificationText += " - " + events[i].description;
                }
            }
        }
        
        // 找到最近的事件就显示（优先显示当天的）
        if (shouldNotify) {
            nearestEvent = notificationText;
            foundNearEvent = true;
            
            // 如果是当天事件，优先显示，跳出循环
            if (events[i].year == currentYear && 
                events[i].month == currentMonth && 
                events[i].day == currentDay) {
                break;
            }
        }
    }
    
    // 显示最近的事件提醒
    if (foundNearEvent) {
        // 如果没有当前提醒或者提醒时间已过，设置新的提醒
        if (currentReminder.length() == 0 || millis() - reminderStartTime > reminderDuration) {
            currentReminder = nearestEvent;
            reminderStartTime = millis();
            Serial.println("事件提醒: " + nearestEvent);
        }
    } else {
        // 没有需要提醒的事件，清除提醒
        if (millis() - reminderStartTime > reminderDuration) {
            currentReminder = "";
        }
    }
}

// 时钟初始化函数
void ClockInit() {
    int i=0;
    unsigned int x0,y0,x1,y1;
    mylcd.fillScreen(TFT_BLACK);

    mylcd.fillCircle(119, 119, 117,TFT_GREEN);
    mylcd.fillCircle(119, 119, 107,TFT_BLACK);
    for(i=0;i<360;i+=30)
    {
        x0 = cos((i-90)*0.0174532925)*114+120;
        y0 = sin((i-90)*0.0174532925)*114+120;
        x1 = cos((i-90)*0.0174532925)*98+120;
        y1 = sin((i-90)*0.0174532925)*98+120;
        mylcd.drawLine(x0, y0, x1, y1,TFT_GREEN);
    }
    for(i=0;i<360;i+=6)
    {
        x0 = cos((i-90)*0.0174532925)*102+120;
        y0 = sin((i-90)*0.0174532925)*102+120;
        mylcd.drawPixel(x0,y0,TFT_WHITE);
        if(i==0 || i==180)
            mylcd.fillCircle(x0, y0, 2,TFT_BLUE);
        if(i==90 || i==270)
            mylcd.fillCircle(x0, y0, 2,TFT_BLUE);
    }
    mylcd.fillCircle(119, 120, 4,TFT_BLUE);
    total_time = millis()+1000;
}

// 时钟显示页面
void showClockPage() {
    if (!initialized) {
        ClockInit();
        initialized = true;
        flag = true; // 初始化flag，确保首次显示指针
        total_time = millis(); // 重置时间计数器
    }
    
    timeClient.update();//NTP更新
    hours=timeClient.getHours();
    mins=timeClient.getMinutes();
    secs=timeClient.getSeconds();
    unsigned long epochTime = timeClient.getEpochTime();
    //将epochTime换算成年月日
    struct tm *ptm = gmtime((time_t *)&epochTime);
    int monthDay = ptm->tm_mday;
    int currentMonth = ptm->tm_mon + 1;
    
    if(total_time<millis())
    {
        total_time+=1000;
        secs++;
        if(secs>=60)
        {
            secs=0;
            mins++;
            if(mins>59)
            {
                mins=0;
                hours++;
                if(hours>23)
                {
                    hours=0;
                }
            }
        }
        if((secs==0)||flag)
        {
            flag=0;
            // 清除旧的时分针
            mylcd.drawLine(hours_x, hours_y, 119, 120,TFT_BLACK);
            mylcd.drawLine(mins_x, mins_y, 119, 120,TFT_BLACK);
            
            // 计算新的时分针位置
            hours_x = cos((hours*30+(mins*6+(secs*6)*0.01666667)*0.0833333-90)*0.0174532925)*62+120;
            hours_y = sin((hours*30+(mins*6+(secs*6)*0.01666667)*0.0833333-90)*0.0174532925)*62+120;
            mins_x = cos((mins*6+(secs*6)*0.01666667-90)*0.0174532925)*84+119;
            mins_y = sin((mins*6+(secs*6)*0.01666667-90)*0.0174532925)*84+120;
        }
        
        // 清除旧的秒针
        mylcd.drawLine(secs_x, secs_y, 119, 120,TFT_BLACK);
        
        // 计算新的秒针位置
        secs_x = cos((secs*6-90)*0.0174532925)*90+120;
        secs_y = sin((secs*6-90)*0.0174532925)*90+120;
        
        // 绘制指针
        mylcd.drawLine(hours_x, hours_y, 119, 120,TFT_YELLOW);  // 时针
        mylcd.drawLine(mins_x, mins_y, 119, 120,TFT_WHITE);     // 分针
        mylcd.drawLine(secs_x, secs_y, 119, 120,TFT_RED);       // 秒针
        mylcd.fillCircle(119, 120, 4,TFT_RED);                  // 中心点
    }
    //mylcd.drawString("JLC EDA TV Lite", mylcd.width() / 2 - 50, 160, 2);
}

// 显示农历天干地支年份
void displayLunarYear(int year) {
    // 天干: 甲、乙、丙、丁、戊、己、庚、辛、壬、癸
    // 地支: 子、丑、寅、卯、辰、巳、午、未、申、酉、戌、亥
    
    // 计算天干
    int tianGan = (year - 4) % 10;
    // 计算地支
    int diZhi = (year - 4) % 12;
    
    // 显示天干
    int x = 40; // 起始X坐标
    int y = 115; // 起始Y坐标
    
    // 显示天干
    switch(tianGan) {
        case 0: // 甲
            mylcd.pushImage(x, y, 14, 14, jia);
            break;
        case 1: // 乙
            mylcd.pushImage(x, y, 14, 14, yii);
            break;
        case 2: // 丙
            mylcd.pushImage(x, y, 14, 14, bing);
            break;
        case 3: // 丁
            mylcd.pushImage(x, y, 14, 14, ding);
            break;
        case 4: // 戊
            mylcd.pushImage(x, y, 14, 14, wuu);
            break;
        case 5: // 己
            mylcd.pushImage(x, y, 14, 14, ji);
            break;
        case 6: // 庚
            mylcd.pushImage(x, y, 14, 14, geng);
            break;
        case 7: // 辛
            mylcd.pushImage(x, y, 14, 14, xin);
            break;
        case 8: // 壬
            mylcd.pushImage(x, y, 14, 14, ren);
            break;
        case 9: // 癸
            mylcd.pushImage(x, y, 14, 14, gui);
            break;
    }
    
    // 显示地支（向右偏移20像素）
    x += 20;
    
    switch(diZhi) {
        case 0: // 子
            mylcd.pushImage(x, y, 14, 14, zi);
            break;
        case 1: // 丑
            mylcd.pushImage(x, y, 14, 14, chou);
            break;
        case 2: // 寅
            mylcd.pushImage(x, y, 14, 14, yan);
            break;
        case 3: // 卯
            mylcd.pushImage(x, y, 14, 14, mao);
            break;
        case 4: // 辰
            mylcd.pushImage(x, y, 14, 14, chen);
            break;
        case 5: // 巳
            mylcd.pushImage(x, y, 14, 14, sii);
            break;
        case 6: // 午
            mylcd.pushImage(x, y, 14, 14, zhongwu);
            break;
        case 7: // 未
            mylcd.pushImage(x, y, 14, 14, wei);
            break;
        case 8: // 申
            mylcd.pushImage(x, y, 14, 14, shen);
            break;
        case 9: // 酉
            mylcd.pushImage(x, y, 14, 14, you);
            break;
        case 10: // 戌
            mylcd.pushImage(x, y, 14, 14, xu);
            break;
        case 11: // 亥
            mylcd.pushImage(x, y, 14, 14, hai);
            break;
    }
    x += 20;
    mylcd.pushImage(x, y, 14, 14, niannian);

}

// 农历月份数据结构
struct LunarDate {
    int year;      // 农历年
    int month;     // 农历月
    int day;       // 农历日
    bool isLeap;   // 是否闰月
};

// 农历数据表（2023-2024年的部分数据示例）
const uint16_t LUNAR_MONTH_DAYS[] = {
    0x4ae0, 0xa2d0, 0x24d0, 0x54d0, 0xd260, 0xd950, 0x16aa, // 2023
    0xb550, 0x56a0, 0x4ad0, 0xa4d0, 0x24d0, 0x8d50, 0xd4d0  // 2024
};

// // 显示农历完整日期
// void displayLunarDate(int year, int month, int day) {
//     // 计算农历年
//     displayLunarYear(year);  // 使用之前的函数显示天干地支年
    
//     // 农历月份名称
//     const char* monthNames[] = {"正", "二", "三", "四", "五", "六", "七", "八", "九", "十", "冬", "腊"};
    
//     // 农历日期名称
//     const char* dayNames[] = {
//         "初一", "初二", "初三", "初四", "初五", "初六", "初七", "初八", "初九", "初十",
//         "十一", "十二", "十三", "十四", "十五", "十六", "十七", "十八", "十九", "二十",
//         "廿一", "廿二", "廿三", "廿四", "廿五", "廿六", "廿七", "廿八", "廿九", "三十"
//     };

//     // 简化版转换（这里使用简单映射，实际应该使用农历算法）
//     int lunarMonth = month;
//     int lunarDay = day;
    
//     // 显示月份（在天干地支年后面）
//     int x = 50;  // 月份起始X坐标
//     int y = 115; // 与年份同一行
    
//     // 显示"月"字
//     if (lunarMonth >= 1 && lunarMonth <= 12) {
//         // 显示月份数字
//         switch(lunarMonth) {
//             case 1: mylcd.pushImage(x, y, 14, 14, yi); break;
//             case 2: mylcd.pushImage(x, y, 14, 14, er); break;
//             case 3: mylcd.pushImage(x, y, 14, 14, san); break;
//             case 4: mylcd.pushImage(x, y, 14, 14, si); break;
//             case 5: mylcd.pushImage(x, y, 14, 14, wu); break;
//             case 6: mylcd.pushImage(x, y, 14, 14, liu); break;
//             case 7: mylcd.pushImage(x, y, 14, 14, qi); break;
//             case 8: mylcd.pushImage(x, y, 14, 14, ba); break;
//             case 9: mylcd.pushImage(x, y, 14, 14, jiu); break;
//             case 10: mylcd.pushImage(x, y, 14, 14, shii); break;
//             case 11: mylcd.pushImage(x, y, 14, 14, dong); break; // 冬月
//             case 12: mylcd.pushImage(x, y, 14, 14, la); break;   // 腊月
//         }
//     }
    
//     // 显示"月"字
//     x += 20;
//     mylcd.pushImage(x, y, 14, 14, yue);
    
//     // 显示日期
//     x += 30;
//     if (lunarDay >= 1 && lunarDay <= 30) {
//         if (lunarDay <= 10) {
//             // 显示"初"字
//             mylcd.pushImage(x, y, 14, 14, chu);
//             x += 20;
//             // 显示数字
//             switch(lunarDay) {
//                 case 1: mylcd.pushImage(x, y, 14, 14, yi); break;
//                 case 2: mylcd.pushImage(x, y, 14, 14, er); break;
//                 case 3: mylcd.pushImage(x, y, 14, 14, san); break;
//                 case 4: mylcd.pushImage(x, y, 14, 14, si); break;
//                 case 5: mylcd.pushImage(x, y, 14, 14, wu); break;
//                 case 6: mylcd.pushImage(x, y, 14, 14, liu); break;
//                 case 7: mylcd.pushImage(x, y, 14, 14, qi); break;
//                 case 8: mylcd.pushImage(x, y, 14, 14, ba); break;
//                 case 9: mylcd.pushImage(x, y, 14, 14, jiu); break;
//                 case 10: mylcd.pushImage(x, y, 14, 14, shii); break;
//             }
//         } else if (lunarDay < 20) {
//             // 显示"十"字
//             mylcd.pushImage(x, y, 14, 14, shii);
//             x += 20;
//             // 显示个位数
//             switch(lunarDay % 10) {
//                 case 1: mylcd.pushImage(x, y, 14, 14, yi); break;
//                 case 2: mylcd.pushImage(x, y, 14, 14, er); break;
//                 case 3: mylcd.pushImage(x, y, 14, 14, san); break;
//                 case 4: mylcd.pushImage(x, y, 14, 14, si); break;
//                 case 5: mylcd.pushImage(x, y, 14, 14, wu); break;
//                 case 6: mylcd.pushImage(x, y, 14, 14, liu); break;
//                 case 7: mylcd.pushImage(x, y, 14, 14, qi); break;
//                 case 8: mylcd.pushImage(x, y, 14, 14, ba); break;
//                 case 9: mylcd.pushImage(x, y, 14, 14, jiu); break;
//             }
//         } else if (lunarDay == 20) {
//             mylcd.pushImage(x, y, 14, 14, er);
//             x += 20;
//             mylcd.pushImage(x, y, 14, 14, shii);
//         } else if (lunarDay < 30) {
//             // 显示"廿"字
//             mylcd.pushImage(x, y, 14, 14, nian);
//             x += 20;
//             // 显示个位数
//             switch(lunarDay % 10) {
//                 case 1: mylcd.pushImage(x, y, 14, 14, yi); break;
//                 case 2: mylcd.pushImage(x, y, 14, 14, er); break;
//                 case 3: mylcd.pushImage(x, y, 14, 14, san); break;
//                 case 4: mylcd.pushImage(x, y, 14, 14, si); break;
//                 case 5: mylcd.pushImage(x, y, 14, 14, wu); break;
//                 case 6: mylcd.pushImage(x, y, 14, 14, liu); break;
//                 case 7: mylcd.pushImage(x, y, 14, 14, qi); break;
//                 case 8: mylcd.pushImage(x, y, 14, 14, ba); break;
//                 case 9: mylcd.pushImage(x, y, 14, 14, jiu); break;
//             }
//         } else {
//             // 显示"三十"
//             mylcd.pushImage(x, y, 14, 14, san);
//             x += 20;
//             mylcd.pushImage(x, y, 14, 14, shii);
//         }
//     }
// }

// 获取农历日期
LunarDate getLunarDateFromSolar(int year, int month, int day) {
    LunarDate lunar;
    // 这里应该实现具体的公历转农历算法
    // 目前使用简单映射，实际使用时需要完整的转换算法
    
    // 简单示例（这不是真实的转换）：
    lunar.year = year;
    lunar.month = month;
    lunar.day = day;
    lunar.isLeap = false;
    
    return lunar;
}

// I2S pin definitions (same as original project)
#define I2S_BCLK 15
#define I2S_LRC  16
#define I2S_DOUT 7

#define I2S_PORT I2S_NUM_0

// I2S configuration structure
const i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate = 44100,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 6,
    .dma_buf_len = 512,
    .use_apll = false,
    .tx_desc_auto_clear = true,
    .fixed_mclk = 0
};

const i2s_pin_config_t pin_config = {
    .bck_io_num = I2S_BCLK,
    .ws_io_num = I2S_LRC,
    .data_out_num = I2S_DOUT,
    .data_in_num = I2S_PIN_NO_CHANGE
};

// =========================
// I2S & Alarm sound support
// =========================

void initI2S() {
    // Install and configure I2S for buzzer output
    i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_PORT, &pin_config);
    i2s_zero_dma_buffer(I2S_PORT);
}

// Generate a simple sine-wave tone using I2S
void generateTone(int frequency, int durationMs) {
    const int sampleRate = 44100;
    const int samples_per_channel = (sampleRate * durationMs) / 1000;
    const int total_samples_in_buffer = samples_per_channel * 2; // For stereo
    int16_t *buffer = (int16_t *)malloc(total_samples_in_buffer * sizeof(int16_t));
    if (!buffer) return; // allocation failed

    float phase_increment = 2.0f * PI * frequency / sampleRate;
    for (int i = 0; i < samples_per_channel; ++i) {
        float sample_value = sinf(i * phase_increment);
        int16_t pcm_value = (int16_t)(sample_value * 16000); // Using amplitude from example
        buffer[i * 2]     = pcm_value; // Left channel
        buffer[i * 2 + 1] = pcm_value; // Right channel
    }

    size_t bytesWritten;
    i2s_write(I2S_PORT, buffer, total_samples_in_buffer * sizeof(int16_t), &bytesWritten, portMAX_DELAY);
    free(buffer);
}

void startAlarm() {
    alarmRinging = true;
    currentPage = 6; // switch to alarm page
    audioJob = JOB_MUSIC_LOOP; // 切换到音乐音频
    Serial.println("Alarm started - playing music audio");
}

void stopAlarm() {
    alarmRinging = false;
    currentPage = 0; // return to main page
    audioJob = JOB_NONE; // 停止所有音频
    Serial.println("Alarm dismissed");
}

// Check alarms every minute
void checkAlarms() {
    if (!timeClient.isTimeSet()) return;

    static int lastCheckedMinute = -1;
    int hour = timeClient.getHours();
    int minute = timeClient.getMinutes();
    int second = timeClient.getSeconds();

    // Only evaluate when the minute changes (reduce computation)
    if (minute == lastCheckedMinute) return;
    lastCheckedMinute = minute;

    for (int i = 0; i < MAX_ALARMS; i++) {
        if (!alarms[i].enabled) continue;
        if (hour == alarms[i].hour && minute == alarms[i].minute) {
            Serial.printf("Alarm triggered: %s\n", alarms[i].label.c_str());
            startAlarm();
            break; // only one alarm at a time
        }
    }
}

// Start continuous buzzer test
void startBuzzerTest() {
    buzzerTesting = true;
    audioJob = JOB_BEEP_LOOP; // 播放蜂鸣音频
    Serial.println("Buzzer test started - playing beep audio");
}

// Stop continuous buzzer test
void stopBuzzerTest() {
    buzzerTesting = false;
    audioJob = JOB_NONE; // 停止音频
    Serial.println("Buzzer test stopped");
}

// =========================
// End of I2S & Alarm support
// =========================

// =========================
// Music Playback Functions
// =========================

// 音乐播放器初始化 - 单例模式
void initMusicPlayer() {
    // 初始化音频输出
    audioOut = new AudioOutputI2S();
    audioOut->SetPinout(SPK_BCLK, SPK_LRC, SPK_DIN);
    audioOut->SetGain(0.8); // 设置音量 (0.0 到 4.0)
    
    // 初始化音频文件和解码器（单例）
    musicFile = new AudioFileSourceSPIFFS();
    mp3 = new AudioGeneratorMP3();
    
    Serial.println("音乐播放器初始化完成（单例模式）");
    Serial.println("BCLK: GPIO15  LRC: GPIO16  DIN: GPIO7");
}

// 开始音乐循环播放
void startMusicLoop() {
    audioJob = JOB_MUSIC_LOOP;
    Serial.println("开始音乐循环播放");
}

// 停止音乐播放
void stopMusicLoop() {
    audioJob = JOB_NONE;
    Serial.println("停止音乐播放");
}

// 统一音频服务函数
void serviceAudio() {
    static bool busy = false;          // mp3 对象是否正在运行
    static bool loopingMusic = false;  // 当前是否在循环主音乐

    switch (audioJob) {
        case JOB_MUSIC_LOOP:        // 主音乐循环播放
            if (!busy) {                              // 首次启动或切换时
                if (!SPIFFS.exists("/compressed_smaller.mp3")) {
                    Serial.println("未找到音乐文件 /compressed_smaller.mp3");
                    audioJob = JOB_NONE;
                    break;
                }
                musicFile->open("/compressed_smaller.mp3");
                mp3->begin(musicFile, audioOut);
                busy = loopingMusic = true;
                Serial.println("开始播放主音乐");
            }
            if (!mp3->loop()) {                       // 播放完毕，重新开始
                mp3->stop();
                musicFile->close();
                busy = false;
                Serial.println("主音乐播放完成，准备重新开始");
            }
            break;

        case JOB_BEEP_LOOP:        // 蜂鸣音频循环播放
            if (!busy) {
                if (!SPIFFS.exists("/beep_burst.mp3")) {
                    Serial.println("未找到蜂鸣文件 /beep_burst.mp3");
                    audioJob = JOB_NONE;
                    break;
                }
                musicFile->open("/beep_burst.mp3");
                mp3->begin(musicFile, audioOut);
                busy = true;
                loopingMusic = false;
                Serial.println("开始循环蜂鸣音频");
            }
            if (!mp3->loop()) {
                // 播放完毕，重新开始蜂鸣
                mp3->stop();
                musicFile->close();
                busy = false;
            }
            break;

        case JOB_NONE:             // 不播放任何内容
        default:
            if (busy) { 
                mp3->stop(); 
                musicFile->close(); 
                // 保留 I2S 运行以避免关闭/重新开启导致卡顿
                // i2s_zero_dma_buffer(I2S_PORT);
                busy = false; 
                loopingMusic = false;
                Serial.println("音频已停止并清空缓冲区");
            }
            break;
    }
}

// =========================
// End of Music Playback Functions
// =========================

// 新增：音频服务任务，实现高频率调用 serviceAudio()
void audioTask(void *parameter) {
    for (;;) {
        serviceAudio();
        vTaskDelay(2 / portTICK_PERIOD_MS); // 约 2ms 一次
    }
}

// 新增：火焰传感器检测函数
void checkFlameSensor() {
    if (flameAlarmActive) return; // 如果警报已激活，则不进行检测

    bool fireDigital = digitalRead(PIN_DO) == LOW; // 有火时 DO 输出低电平
    int  rawValue    = analogRead(PIN_AO);
    int  percentage  = map(rawValue, 0, 4095, 100, 0); // 值越小火焰越强

    // 触发条件：DO 为低或 AO 百分比超过阈值
    if (fireDigital || percentage > FLAME_THRESHOLD) {
        startFlameAlarm();
    }
}

// 新增：启动火焰警报
void startFlameAlarm() {
    if (flameAlarmActive) return; // 防止重复触发
    flameAlarmActive = true;
    currentPage = 7; // 切换到火焰警报页面
    audioJob = JOB_BEEP_LOOP; // 播放蜂鸣音频
    Serial.println("🔥 FIRE ALARM TRIGGERED! ��");
    
    // 发送火警邮件（如果尚未发送）
    if (!emailSent) {
        String subject = "智能万年历 火焰警报触发通知";
        String body = "您的 智能万年历 设备检测到火焰！\n";
        body += "时间: " + String(timeClient.getFormattedTime()) + "\n";
        body += "请立即检查现场情况，确保安全！";
        
        if (sendFlameAlertEmail(subject, body)) {
            emailSent = true;
            emailErrorMsg = "邮件发送成功";
            Serial.println("✅ 火警邮件发送成功");
        } else {
            Serial.println("❌ 火警邮件发送失败: " + emailErrorMsg);
        }
    }
}

// 新增：停止火焰警报
void stopFlameAlarm() {
    flameAlarmActive = false;
    emailSent = false; // 重置邮件发送状态，允许下次火警时重新发送
    currentPage = 0; // 返回主页面
    audioJob = JOB_NONE; // 停止所有音频
    Serial.println("Flame alarm dismissed by user.");
}

/* =========================
   SMTP 邮件发送实现
   ========================= */

String base64Encode(const String &data)
{
    const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    String result;
    int i = 0;
    unsigned char arr3[3];
    unsigned char arr4[4];
    int len = data.length();
    const unsigned char *bytes = (const unsigned char*)data.c_str();
    while (len--) {
        arr3[i++] = *(bytes++);
        if (i == 3) {
            arr4[0] = (arr3[0] & 0xfc) >> 2;
            arr4[1] = ((arr3[0] & 0x03) << 4) + ((arr3[1] & 0xf0) >> 4);
            arr4[2] = ((arr3[1] & 0x0f) << 2) + ((arr3[2] & 0xc0) >> 6);
            arr4[3] = arr3[2] & 0x3f;
            for (i = 0; i < 4; i++) result += b64[arr4[i]];
            i = 0;
        }
    }
    if (i) {
        for (int j = i; j < 3; j++) arr3[j] = '\0';
        arr4[0] = (arr3[0] & 0xfc) >> 2;
        arr4[1] = ((arr3[0] & 0x03) << 4) + ((arr3[1] & 0xf0) >> 4);
        arr4[2] = ((arr3[1] & 0x0f) << 2) + ((arr3[2] & 0xc0) >> 6);
        for (int k = 0; k < i + 1; k++) result += b64[arr4[k]];
        while (i++ < 3) result += '=';
    }
    return result;
}

bool smtpAwait(WiFiClientSecure &client, int expectCode, const char* stage, uint32_t timeout = 10000)
{
    Serial.printf("等待SMTP响应 [%s] 期望代码: %d\n", stage, expectCode);
    
    uint32_t start = millis();
    while (client.connected() && !client.available() && millis() - start < timeout) {
        delay(10);
    }
    
    if (!client.available()) {
        emailErrorMsg = String(stage) + " timeout";
        Serial.printf("❌ [%s] 超时\n", stage);
        return false;
    }

    String fullResponse = "";
    bool foundExpectedCode = false;
    bool hasError = false;
    
    // 读取所有响应行
    while (client.available()) {
        String line = client.readStringUntil('\n');
        line.trim();
        if (line.length() > 0) {
            fullResponse += line + "\n";
            Serial.printf("接收: %s\n", line.c_str());
            
            // 检查响应码
            if (line.length() >= 3) {
                int code = line.substring(0,3).toInt();
                bool isLastLine = (line.length() > 3 && line[3] == ' ');  // 检查是否是多行响应的最后一行
                
                if (code == expectCode && isLastLine) {
                    foundExpectedCode = true;
                } else if (code >= 400) {  // 4xx 和 5xx 是错误响应
                    hasError = true;
                    emailErrorMsg = line;
                    Serial.printf("❌ [%s] 错误: %s\n", stage, line.c_str());
                    break;
                }
            }
        }
    }
    
    if (foundExpectedCode) {
        Serial.printf("✅ [%s] 成功\n", stage);
        return true;
    }
    
    if (!hasError) {
        emailErrorMsg = String(stage) + " unexpected response: " + fullResponse;
        Serial.printf("❌ [%s] 意外响应:\n%s\n", stage, fullResponse.c_str());
    }
    
    return false;
}

bool sendFlameAlertEmail(const String &subject, const String &body)
{
    emailErrorMsg = ""; // 清空错误信息
    
    Serial.println("\n========= 开始发送邮件 =========");
    Serial.printf("SMTP服务器: %s:%d\n", smtpServer, smtpPort);
    Serial.printf("发件人: %s\n", smtpUser);
    Serial.printf("收件人: %s\n", emailRecipient);
    
    if (WiFi.status() != WL_CONNECTED) {
        emailErrorMsg = "WiFi not connected";
        Serial.println("❌ WiFi未连接，无法发送邮件");
        return false;
    }

    WiFiClientSecure client;
    client.setTimeout(10000);
    client.setInsecure(); // 跳过证书验证

    Serial.println("\n正在连接SMTP服务器...");
    if (!client.connect(smtpServer, smtpPort)) {
        emailErrorMsg = "Cannot connect to SMTP server";
        Serial.printf("❌ 无法连接到SMTP服务器 %s:%d\n", smtpServer, smtpPort);
        return false;
    }
    Serial.println("✅ SSL连接已建立");

    if (!smtpAwait(client, 220, "Server greeting")) return false;

    // 使用完整的域名进行EHLO
    client.println("EHLO esp32");
    if (!smtpAwait(client, 250, "EHLO")) return false;

    // 等待一小段时间
    delay(100);

    // 直接使用AUTH LOGIN
    client.println("AUTH LOGIN");
    if (!smtpAwait(client, 334, "Auth")) return false;

    // 发送Base64编码的用户名（完整邮箱地址）
    String username = "15253286380@163.com";
    client.println(base64Encode(username));
    if (!smtpAwait(client, 334, "Username")) return false;

    // 发送Base64编码的密码（授权码）
    String password = "WP7KhS4Y9a5KSnmt";
    client.println(base64Encode(password));
    if (!smtpAwait(client, 235, "Password")) return false;

    // 等待一小段时间
    delay(100);

    // 发件人必须与登录用户名相同
    client.print("MAIL FROM:<");
    client.print(username);
    client.println(">");
    if (!smtpAwait(client, 250, "Sender")) return false;

    client.print("RCPT TO:<");
    client.print(emailRecipient);
    client.println(">");
    if (!smtpAwait(client, 250, "Recipient")) return false;

    client.println("DATA");
    if (!smtpAwait(client, 354, "Data")) return false;

    // 格式化邮件头
    client.println("From: 智能万年历 <15253286380@163.com>");
    client.println("To: " + String(emailRecipient));
    client.println("Subject: " + subject);
    client.println("Content-Type: text/plain; charset=utf-8");
    client.println();
    
    // 添加温湿度信息
    float temperature = dht.readTemperature();
    float humidity = dht.readHumidity();
    String envInfo = "当前环境信息：\n温度: " + String(temperature, 1) + "°C\n湿度: " + String(humidity, 1) + "%\n\n";
    
    client.println(envInfo + body);
    client.println(".");
    if (!smtpAwait(client, 250, "Message")) return false;

    client.println("QUIT");
    smtpAwait(client, 221, "Quit");
    client.stop();
    Serial.println("✅ Email: success");
    emailErrorMsg = "Email sent successfully";
    return true;
}

