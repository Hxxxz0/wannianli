#include <Arduino.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ArduinoWebsockets.h>
#include <NTPClient.h>
#include <WiFiUdp.h>
#include <mbedtls/md.h>
#include <mbedtls/base64.h>
#include <time.h>
#include <SPIFFS.h>
#include "Audio.h"
#include <driver/i2s.h>
#include <mbedtls/sha256.h>
#include <ESPAsyncWebServer.h>
#include "image.cpp"
using namespace websockets;
const char *ssidFile = "/ssid.json";
AsyncWebServer server(80);
const char *weatherAPI = "http://api.seniverse.com/v3/weather/daily.json?key=";
int weatherpic=0;
String temperature = "";
String humidity = "";
String weather = "";
// 全局变量，用于累积大模型回复
String chatAggregated = "";
unsigned long lastChatMsgTime = 0;
bool chatFinalized = false;
int currentState = 3; // 0: 默认状态, 1: 说话状态, 2: 机器人状态, 3: 时间状态
int lastState = 0; // 记录上次的显示状态
String cityname = "";
String weatherapi = "";
static bool initweather = false; // 天气初始化
////////////////////////////
// 配置参数（请根据实际情况修改）
////////////////////////////
const char *ssid = "TV-PRO";
const char *password = "";
////////////////////////////
// 新增：用于显示“正在说话”表情的多核任务相关变量
////////////////////////////
TaskHandle_t displayTaskHandle = NULL;
volatile bool isTalkingDisplayActive = false;
// 按键引脚（IO21）
#define BUTTON_MID 21
#define BUTTON_LEFT 39
#define BUTTON_RIGHT 40
int BTNow = 0;
TFT_eSPI mylcd = TFT_eSPI();
// inmp441麦克风I2S引脚及参数
#define INMP441_WS 4
#define INMP441_SCK 5
#define INMP441_SD 6
#define SAMPLE_RATE 16000
#define CHANNELS 1
#define BIT_DEPTH 16
#define FRAME_SIZE 1280 // 16-bit PCM，每帧 1280B 对应 40ms

// max98357播放的I2S引脚
#define I2S_BCLK 15
#define I2S_LRC 16
#define I2S_DOUT 7
// API及鉴权参数
const char *hostName = "itrans.xfyun.cn";
const char *urlPath = "/v2/its"; // 请求路径
const int httpsPort = 443;
String apiKey = "";
String apiSecret = "";
String appId = "";

// 科大讯飞（语音转文字）API相关

const char *speechHost = "iat-api.xfyun.cn";
const char *speechPath = "/v2/iat";

// 大模型对话 API（spark接口）相关

const char *chatHost = "spark-api.xf-yun.com";
const char *chatPath = "/v4.0/chat";

// 文字转语音（TTS）API相关
const char *ttsApiUrl = "https://wcode.net/api/audio/gpt/text-to-audio/v3/transcription";
String ttsApiKey = "";
// 全局变量定义
volatile bool minuteTimerExpired = false; // 定时器触发标志
////////////////////////////
// 全局对象及变量
////////////////////////////
WiFiUDP udp;
NTPClient timeClient(udp, "pool.ntp.org", 0, 60000);
WiFiMulti wifiMulti;
Audio audio;

WebsocketsClient wsSpeech; // 用于语音转文字
WebsocketsClient wsChat;   // 用于大模型对话

// 状态标记及结果存储
volatile bool isRecording = false;    // 当前是否正在录音
volatile bool speechFinished = false; // 语音识别结果是否返回（结束帧）
volatile bool chatFinished = false;   // 大模型对话回复是否返回
String speechText = "";               // 语音识别拼接后的文本
String chatResponse = "";             // 大模型回复文本

// 计时变量（音频发送）
unsigned long startTime = 0;
unsigned long lastSendTime = 0;
unsigned long globalEpochTime = 0;

void handleWiFiConfig()
{
    server.on("/connect", HTTP_POST, [](AsyncWebServerRequest *request)
    {
        // 获取POST参数：ssid、pass、uid、city、api
        String ssid = request->getParam("ssid", true)->value();
        String pass = request->getParam("pass", true)->value();
        String ttsapikey = request->getParam("ttsapikey", true)->value();
        String apisecret = request->getParam("apisecret", true)->value();
        String apikey = request->getParam("apikey", true)->value();
        String appid = request->getParam("appid", true)->value();
        String city = request->getParam("city", true)->value();
        String api = request->getParam("api", true)->value();

        // 打印接收到的参数
        Serial.println(ssid);
        Serial.println(pass);

        // 保存WiFi信息到JSON文件
        DynamicJsonDocument doc(2048);
        doc["ssid"] = ssid;
        doc["pass"] = pass;
        doc["ttsapikey"] = ttsapikey;
        doc["apisecret"] = apisecret;
        doc["apikey"] = apikey;
        doc["appid"] = appid;
        doc["city"] = city;
        doc["api"] = api;
        fs::File file = SPIFFS.open(ssidFile, "w");  // 打开文件进行写入
        if (file) {
            serializeJson(doc, file);  // 将JSON内容写入文件
            file.close();  // 关闭文件
        }

        // 更新全局变量
        ttsApiKey = ttsapikey;
        apiSecret = apisecret;
        appId = appid;
        apiKey = apikey;
        cityname = city;
        weatherapi = api;

        // 开始连接WiFi
        WiFi.begin(ssid.c_str(), pass.c_str());
        // 发送HTML响应，告知用户正在连接
        request->send(200, "text/html; charset=UTF-8", 
            "<!DOCTYPE html>"
            "<html>"
            "<head>"
            "    <meta charset='UTF-8'>"
            "    <title>状态</title>"
            "</head>"
            "<body>"
            "    <h1>请返回使用在线功能，如果能正常获取则配置成功！</h1>"
            "</body>"
            "</html>"
        );});
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
    {
        // 检查SPIFFS文件系统中是否存在index.html文件
        if (SPIFFS.exists("/index.html")) {
            fs::File file = SPIFFS.open("/index.html", "r");  // 打开index.html文件
            if (file) {
                size_t fileSize = file.size();  // 获取文件大小
                String fileContent;

                // 逐字节读取文件内容
                while (file.available()) {
                    fileContent += (char)file.read();
                }
                file.close();  // 关闭文件

                // 返回HTML内容
                request->send(200, "text/html", fileContent);
                return;
            }
        }
        // 如果文件不存在，返回404错误
        request->send(404, "text/plain", "File Not Found"); });
    // 启动服务器
    server.begin();
}

void loadWiFiConfig()
{
    if (SPIFFS.begin())
    {
        fs::File file = SPIFFS.open(ssidFile, "r");
        if (file)
        {
            DynamicJsonDocument doc(2048);
            DeserializationError error = deserializeJson(doc, file);
            if (!error)
            {
                String ssid = doc["ssid"];
                String pass = doc["pass"];
                String appid = doc["appid"];
                String apikey = doc["apikey"];
                String apisecret = doc["apisecret"];
                String ttsapikey = doc["ttsapikey"];
                String city = doc["city"];
                String api = doc["api"];
                // 更新全局变量
                ttsApiKey = ttsapikey;
                apiSecret = apisecret;
                appId = appid;
                apiKey = apikey;
                cityname = city;
                weatherapi = api;
                WiFi.begin(ssid.c_str(), pass.c_str());
                // 尝试连接WiFi，最多等待10秒
                unsigned long startAttemptTime = millis();
                while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 5000)
                {
                    delay(500);
                }
                // 如果连接失败，打印状态
                if (WiFi.status() != WL_CONNECTED)
                {
                    Serial.println("WiFi connection failed, starting captive portal...");
                    handleWiFiConfig();
                }
                else
                {
                    Serial.println("WiFi connected");

                }
            }
            file.close();
        }
    }
}
void fetchWeather()
{ // 天气捕捉
    if (initweather == false)
    {
        //  mylcd.fillScreen(TFT_BLACK);
        if (WiFi.status() == WL_CONNECTED)
        {
            WiFiClient client3;
            HTTPClient http3;
            if (http3.begin(client3, weatherAPI + weatherapi + "&location=" + cityname + "&language=zh-Hans&unit=c&start=0&days=1"))
            {
                int httpCode = http3.GET();
                if (httpCode > 0)
                {
                    String payload = http3.getString();
                    Serial.println("JSON Response:");
                    Serial.println(payload);
                    DynamicJsonDocument doc(2048);
                    deserializeJson(doc, payload);
                    String temperature2 = doc["results"][0]["daily"][0]["high"];
                    String humidity2 = doc["results"][0]["daily"][0]["humidity"];
                    String weathe2r = doc["results"][0]["daily"][0]["text_day"];
                    temperature = temperature2;
                    humidity = humidity2;
                    weather = weathe2r;
                    initweather = true;
                    Serial.print("Data received: ");
                    Serial.println(temperature);
                    Serial.println(humidity);
                    Serial.println(weather);
                }
                else
                {
                    Serial.printf("HTTP GET request failed, error: %s\n", http3.errorToString(httpCode).c_str());
                }
                http3.end();
            }
            else
            {
                Serial.println("Failed to connect to server");
            }
            //  mylcd.fillScreen(TFT_BLACK);
        }
    }
    if (weather == "阴" || weather == "多云")
    {
        weatherpic=0;
    }
    else if (weather == "小雨" || weather == "大雨" || weather == "暴雨" || weather == "雨")
    {
        weatherpic=1;
    }
    else 
    {
        weatherpic=2;
    }
}
////////////////////////////
// 工具函数：Base64、HMAC、时间格式转换等
////////////////////////////
String base64Encode(const uint8_t *data, size_t len)
{
    if (len == 0 || data == nullptr)
    {
        Serial.println("Base64编码错误：无数据");
        return "";
    }
    size_t outputLen = 0;
    size_t bufSize = ((len + 2) / 3) * 4 + 1;
    char *buf = (char *)malloc(bufSize);
    if (!buf)
        return "";
    int ret = mbedtls_base64_encode((unsigned char *)buf, bufSize, &outputLen, data, len);
    if (ret != 0)
    {
        free(buf);
        return "";
    }
    String encoded = String(buf);
    free(buf);
    return encoded;
}

String base64EncodeUserInput(const String &userInput)
{
    // 将String转换为uint8_t数组
    const uint8_t *data = reinterpret_cast<const uint8_t *>(userInput.c_str());
    size_t len = userInput.length() * sizeof(char); // 计算字节长度

    return base64Encode(data, len);
}

String getDate()
{
    // ntpClient.getEpochTime() 返回当前秒数（UTC）
    time_t epochTime = timeClient.getEpochTime();
    struct tm *ptm = gmtime(&epochTime); // 转换为 GMT 时间
    char timeString[40];
    // 格式化为 RFC1123 格式，例如 "Wed, 20 Nov 2019 03:14:25 GMT"
    strftime(timeString, sizeof(timeString), "%a, %d %b %Y %H:%M:%S GMT", ptm);
    return String(timeString);
}

String hmacSHA256(const String &key, const String &data)
{
    unsigned char hmacResult[32];
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)key.c_str(), key.length());
    mbedtls_md_hmac_update(&ctx, (const unsigned char *)data.c_str(), data.length());
    mbedtls_md_hmac_finish(&ctx, hmacResult);
    mbedtls_md_free(&ctx);
    size_t outLen;
    unsigned char base64Result[64];
    mbedtls_base64_encode(base64Result, sizeof(base64Result), &outLen, hmacResult, sizeof(hmacResult));
    return String((char *)base64Result);
}
// 计算请求 body 的 SHA256 并 Base64 编码，结果形如 "SHA-256=xxxxx"
String calculateDigest(const String &body)
{
    unsigned char hash[32];
    // 使用 mbedtls 计算 SHA256
    mbedtls_sha256_context sha_ctx;
    mbedtls_sha256_init(&sha_ctx);
    mbedtls_sha256_starts_ret(&sha_ctx, 0); // 0 表示 SHA256（而非 SHA224）
    mbedtls_sha256_update_ret(&sha_ctx, (const unsigned char *)body.c_str(), body.length());
    mbedtls_sha256_finish_ret(&sha_ctx, hash);
    mbedtls_sha256_free(&sha_ctx);

    // Base64编码
    unsigned char base64Digest[64];
    size_t olen = 0;
    mbedtls_base64_encode(base64Digest, sizeof(base64Digest), &olen, hash, sizeof(hash));

    String result = "SHA-256=";
    result += String((char *)base64Digest);
    return result;
}

// 使用 HMAC-SHA256 签名并 Base64 编码
String calculateSignature(const String &signatureOrigin, const char *secret)
{
    unsigned char hmacResult[32];
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, md_info, 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)secret, strlen(secret));
    mbedtls_md_hmac_update(&ctx, (const unsigned char *)signatureOrigin.c_str(), signatureOrigin.length());
    mbedtls_md_hmac_finish(&ctx, hmacResult);
    mbedtls_md_free(&ctx);

    // Base64 编码
    unsigned char base64Signature[64];
    size_t sigLen = 0;
    mbedtls_base64_encode(base64Signature, sizeof(base64Signature), &sigLen, hmacResult, sizeof(hmacResult));
    return String((char *)base64Signature);
}

// 生成科大讯飞语音转文字的鉴权URL
String wsSpeechURL = "";
String generateSpeechAuthURL()
{
    String date = getDate();
    if (date == "")
        return "";
    String tmp = "host: " + String(speechHost) + "\n";
    tmp += "date: " + date + "\n";
    tmp += "GET " + String(speechPath) + " HTTP/1.1";
    String signature = hmacSHA256(apiSecret, tmp);
    String authOrigin = "api_key=\"" + String(apiKey) + "\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"" + signature + "\"";
    unsigned char authBase64[256] = {0};
    size_t authLen = 0;
    int ret = mbedtls_base64_encode(authBase64, sizeof(authBase64) - 1, &authLen, (const unsigned char *)authOrigin.c_str(), authOrigin.length());
    if (ret != 0)
        return "";
    String authorization = String((char *)authBase64);
    String encodedDate = "";
    for (int i = 0; i < date.length(); i++)
    {
        char c = date.charAt(i);
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            encodedDate += c;
        }
        else if (c == ' ')
        {
            encodedDate += "+";
        }
        else if (c == ',')
        {
            encodedDate += "%2C";
        }
        else if (c == ':')
        {
            encodedDate += "%3A";
        }
        else
        {
            encodedDate += "%" + String(c, HEX);
        }
    }
    String url = "ws://" + String(speechHost) + String(speechPath) +
                 "?authorization=" + authorization +
                 "&date=" + encodedDate +
                 "&host=" + speechHost;
    wsSpeechURL = url;
    return url;
}

// 生成大模型对话的鉴权URL
String wsChatURL = "";
String generateChatAuthURL()
{
    String date = getDate();
    if (date == "")
        return "";
    String tmp = "host: " + String(chatHost) + "\n";
    tmp += "date: " + date + "\n";
    tmp += "GET " + String(chatPath) + " HTTP/1.1";
    String signature = hmacSHA256(apiSecret, tmp);
    String authOrigin = "api_key=\"" + String(apiKey) + "\", algorithm=\"hmac-sha256\", headers=\"host date request-line\", signature=\"" + signature + "\"";
    unsigned char authBase64[256] = {0};
    size_t authLen = 0;
    int ret = mbedtls_base64_encode(authBase64, sizeof(authBase64) - 1, &authLen, (const unsigned char *)authOrigin.c_str(), authOrigin.length());
    if (ret != 0)
        return "";
    String authorization = String((char *)authBase64);
    String encodedDate = "";
    for (int i = 0; i < date.length(); i++)
    {
        char c = date.charAt(i);
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
        {
            encodedDate += c;
        }
        else if (c == ' ')
        {
            encodedDate += "+";
        }
        else if (c == ',')
        {
            encodedDate += "%2C";
        }
        else if (c == ':')
        {
            encodedDate += "%3A";
        }
        else
        {
            encodedDate += "%" + String(c, HEX);
        }
    }
    String url = "ws://" + String(chatHost) + String(chatPath) +
                 "?authorization=" + authorization +
                 "&date=" + encodedDate +
                 "&host=" + chatHost;
    wsChatURL = url;
    return url;
}

////////////////////////////
// 语音转文字相关函数（inmp441采集、数据发送及WebSocket回调）
////////////////////////////
// 配置 I2S 接收（麦克风）
i2s_config_t i2sIn_config = {
        .mode = static_cast<i2s_mode_t>(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_I2S_MSB,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 512};

const i2s_pin_config_t inmp441_pin_config = {
        .bck_io_num = INMP441_SCK,
        .ws_io_num = INMP441_WS,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = INMP441_SD};

// 第一次握手
void sendHandshake()
{
    currentState=1;
    DynamicJsonDocument jsonDoc(2048);
    jsonDoc["common"]["app_id"] = appId;
    jsonDoc["business"]["language"] = "zh_cn";
    jsonDoc["business"]["domain"] = "iat";
    jsonDoc["business"]["accent"] = "mandarin";
    jsonDoc["business"]["vad_eos"] = 3000;
    jsonDoc["data"]["status"] = 0;
    jsonDoc["data"]["format"] = "audio/L16;rate=16000";
    jsonDoc["data"]["encoding"] = "raw";
    char buf[512];
    serializeJson(jsonDoc, buf);
    wsSpeech.send(buf);
    Serial.println("已发送语音握手数据");
}

void sendAudioData(bool firstFrame = false)
{
    static uint8_t buffer[FRAME_SIZE]; // 音频数据缓冲区
    size_t bytesRead = 0;
    static unsigned long lastSendTime = 0;

    unsigned long currentMillis = millis();

    // 每40ms发送一次音频
    if (currentMillis - lastSendTime < 40)
    {
        return; // 如果间隔不到40ms，不发送数据
    }

    lastSendTime = currentMillis; // 更新发送时间

    // 读取 I2S 音频数据
    esp_err_t result = i2s_read(I2S_NUM_1, buffer, FRAME_SIZE, &bytesRead, portMAX_DELAY);
    if (result != ESP_OK || bytesRead == 0)
    {
        Serial.println("I2S Read Failed or No Data!");
        return;
    }

    // Base64 编码
    String base64Audio = base64Encode(buffer, bytesRead);
    if (base64Audio.length() == 0)
    {
        Serial.println("Base64 Encoding Failed!");
        return;
    }

    // 发送 JSON 数据
    DynamicJsonDocument jsonDoc(2048);
    jsonDoc["data"]["status"] = firstFrame ? 0 : 1; // 第一帧 status = 0，其他帧 status = 1
    jsonDoc["data"]["format"] = "audio/L16;rate=16000";
    jsonDoc["data"]["encoding"] = "raw";
    jsonDoc["data"]["audio"] = base64Audio; // 确保 Base64 编码成功

    char jsonBuffer[2048];
    serializeJson(jsonDoc, jsonBuffer);

    wsSpeech.send(jsonBuffer); // 发送音频数据
    Serial.printf("Sent %d bytes, status: %d\n", bytesRead, firstFrame ? 0 : 1);
}

void startRecording()
{
    Serial.println("开始录音...");
    isRecording = true;
    startTime = millis();
    sendAudioData(true); // 第一帧 status = 0
}

void stopRecording()
{
    isRecording = false;
    DynamicJsonDocument jsonDoc(2048);
    jsonDoc["data"]["status"] = 2; // 结束传输
    char buf[128];
    serializeJson(jsonDoc, buf);
    if(!wsSpeech.send(buf)){
        mylcd.fillScreen(TFT_BLACK);
        mylcd.setTextSize(2);
        minuteTimerExpired= true;
        speechFinished = false;
        speechText = "";
        currentState=3;
    }
    Serial.println("录音结束，已发送结束信号");


}

// 语音WebSocket回调：解析返回的 JSON 并拼接文字
void onSpeechMessage(WebsocketsMessage message)
{
    Serial.println("语音服务器返回：" + message.data());
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, message.data());
    if (err.c_str()!="ok")
    {
        Serial.print("语音JSON解析错误：");
        Serial.println(err.c_str());
        mylcd.fillScreen(TFT_BLACK);
        mylcd.setTextSize(2);
        minuteTimerExpired= true;
        speechFinished = false;
        speechText = "";
        currentState=3;
        return;
    }
    String tempText = "";
    if (doc.containsKey("data") && doc["data"].containsKey("result") && doc["data"]["result"].containsKey("ws"))
    {
        for (JsonObject wsObj : doc["data"]["result"]["ws"].as<JsonArray>())
        {
            for (JsonObject cwObj : wsObj["cw"].as<JsonArray>())
            {
                tempText += cwObj["w"].as<String>() + " ";
            }
        }
        tempText.trim();
        speechText = tempText;
        Serial.println("识别结果：" + speechText);
 
        if(speechText.length()>0) {
            speechFinished = true;
            currentState=1;
            minuteTimerExpired= false;
        }else{
            mylcd.fillScreen(TFT_BLACK);
            mylcd.setTextSize(2);
            minuteTimerExpired= true;
            speechFinished = false;
            speechText = "";

            currentState=3;
        }
    }
}
//////////////////////////
// 文字转语音及播放函数（TTS via max98357）
////////////////////////////
void playTTS(String textToSpeak)
{

    HTTPClient http2;
    DynamicJsonDocument doc(2048);
    doc["model"] = "cosyvoice-v2";
    doc["text"] = textToSpeak;

    String postData;
    serializeJson(doc, postData);

    int maxRetries = 5;  // 最多重试 5 次
    int retryDelay = 2000; // 每次重试间隔 2 秒
    int attempt = 0;
    int httpCode;
    String payload;

    while (attempt < maxRetries)
    {
        Serial.println("发送TTS请求 (尝试 " + String(attempt + 1) + " / " + String(maxRetries) + ")：" + postData);
        http2.begin(ttsApiUrl);
        http2.addHeader("Content-Type", "application/json");
        http2.addHeader("Authorization", String("Bearer ") + ttsApiKey);
        httpCode = http2.POST(postData);

        if (httpCode == HTTP_CODE_OK)
        {
            payload = http2.getString();
            Serial.println("TTS响应: " + payload);
            break; // 请求成功，跳出循环
        }
        else
        {
            Serial.println("TTS请求失败，错误码：" + String(httpCode));
            attempt++;
            if (attempt < maxRetries)
            {
                Serial.println("等待 " + String(retryDelay / 1000) + " 秒后重试...");
                delay(retryDelay);
            }
        }
        http2.end();
    }

    if (httpCode != HTTP_CODE_OK)
    {
        Serial.println("TTS请求多次失败，放弃！");
        mylcd.fillScreen(TFT_BLACK);
        mylcd.setTextSize(2);
        minuteTimerExpired= true;
        speechFinished = false;
        speechText = "";
        currentState=3;
        return;
    }

    // 解析 JSON 并获取音频 URL
    DynamicJsonDocument docResp(2048);
    DeserializationError err = deserializeJson(docResp, payload);
    if (err.c_str()!="ok")
    {
        Serial.print("TTS JSON解析错误：");
        Serial.println(err.c_str());
        mylcd.fillScreen(TFT_BLACK);
        mylcd.setTextSize(2);

        return;
    }

    String audioUrl;
    for (int i = 0; i < 10; i++)  // 最多等待 10 次
    {
        audioUrl = docResp["data"]["result"]["audio_file_temp_url"].as<String>();
        if (!audioUrl.isEmpty())
            break;
        delay(1000);
        Serial.println("等待音频生成中...");
    }

    if (!audioUrl.isEmpty())
    {
        Serial.println("播放音频URL: " + audioUrl);
        wsSpeech.close();
        wsChat.close();
        delay(100);
        audio.connecttohost(audioUrl.c_str());
        mylcd.fillScreen(TFT_BLACK);
        mylcd.setTextSize(2);
        currentState=3;


    }
    else
    {
        Serial.println("TTS生成失败，未获取到音频URL！");
    }

    mylcd.fillScreen(TFT_BLACK);
    mylcd.setTextSize(2);
    minuteTimerExpired = true;
}

////////////////////////////
// 翻译API
////////////////////////////
void Translation(const String &userInput)
{
    // 获取格式化时间
    String rfc1123Time = getDate();
    Serial.println("当前时间: " + rfc1123Time);
    // 使用示例：
    String encoded = base64EncodeUserInput(userInput);
    // 构造请求 JSON 数据
    String requestBody="";
    String requestBodyCN = "{\"common\":{\"app_id\":\"" + String(appId) + "\"},"
                                                                          "\"business\":{\"from\":\"cn\",\"to\":\"en\"},"
                                                                          "\"data\":{\"text\":\"" +
                           encoded + "\"}}";
    String requestBodyEN = "{\"common\":{\"app_id\":\"" + String(appId) + "\"},"
                                                                          "\"business\":{\"from\":\"en\",\"to\":\"cn\"},"
                                                                          "\"data\":{\"text\":\"" +
                           encoded + "\"}}";
    if(BTNow==3){
        requestBody=requestBodyCN;
    }else{
        requestBody=requestBodyEN;
    }
    Serial.println("请求体: " + requestBody);

    // 计算 body 的 digest 值
    String digest = calculateDigest(requestBody);
    Serial.println("Digest: " + digest);

    // 构造 signature 原始字符串（注意换行符及空格需严格按照接口要求）
    String signatureOrigin = "host: " + String(hostName) + "\n" +
                             "date: " + rfc1123Time + "\n" +
                             "POST " + String(urlPath) + " HTTP/1.1" + "\n" +
                             "digest: " + digest;
    Serial.println("Signature Origin: " + signatureOrigin);

    // 使用 apiSecret 对 signatureOrigin 进行 HMAC-SHA256 签名后 Base64 编码
    String signature = calculateSignature(signatureOrigin, apiSecret.c_str());
    Serial.println("Signature: " + signature);

    // 构造 Authorization 请求头
    String authorization = "api_key=\"" + String(apiKey) + "\", algorithm=\"hmac-sha256\", headers=\"host date request-line digest\", signature=\"" + signature + "\"";
    Serial.println("Authorization: " + authorization);

    // 连接 HTTPS 服务端
    WiFiClientSecure client;
    client.setInsecure(); // 若没有证书，可先禁用验证，正式使用时请设置证书
    Serial.print("连接到主机: ");
    Serial.println(hostName);
    if (!client.connect(hostName, httpsPort))
    {
        Serial.println("连接失败");
        return;
    }

    // 构造 HTTP 请求
    String httpRequest = String("POST ") + urlPath + " HTTP/1.1\r\n" +
                         "Host: " + String(hostName) + "\r\n" +
                         "Content-Type: application/json\r\n" +
                         "Accept: application/json,version=1.0\r\n" +
                         "Date: " + rfc1123Time + "\r\n" +
                         "Digest: " + digest + "\r\n" +
                         "Authorization: " + authorization + "\r\n" +
                         "Content-Length: " + String(requestBody.length()) + "\r\n" +
                         "\r\n" +
                         requestBody;
    Serial.println("发送请求：");
    Serial.println(httpRequest);

    // 发送 HTTP 请求
    client.print(httpRequest);

    // 读取响应（先读取头部，再读取正文）
    while (client.connected())
    {
        String line = client.readStringUntil('\n');
        if (line == "\r")
            break; // 空行表示 header 结束
    }
    String response = client.readString();
    Serial.println("响应:");
    Serial.println(response);
    DynamicJsonDocument doc(2048);  // 512字节的JSON解析缓冲区
    DeserializationError error = deserializeJson(doc, response);

    if (error) {
        Serial.print("JSON解析失败: ");
        Serial.println(error.c_str());
        return;
    }

    // 提取 "dst" 字段的内容
    const char* dst = doc["data"]["result"]["trans_result"]["dst"];
    Serial.print("提取的 dst 内容: ");
    Serial.println(dst);
    playTTS(dst);
}
////////////////////////////
// 大模型对话相关函数
////////////////////////////
void sendChatRequest(const String &userInput)
{
    currentState=2;
    DynamicJsonDocument doc(2048);
    JsonObject header = doc.createNestedObject("header");
    header["app_id"] = appId;
    JsonObject parameter = doc.createNestedObject("parameter");
    JsonObject chat = parameter.createNestedObject("chat");
    chat["domain"] = "4.0Ultra";
    chat["temperature"] = 0.5;
    chat["max_tokens"] = 1024;
    JsonObject payload = doc.createNestedObject("payload");
    JsonObject message = payload.createNestedObject("message");
    JsonArray textArr = message.createNestedArray("text");
    // 系统提示（可根据需要修改）
    JsonObject systemMsg = textArr.createNestedObject();
    systemMsg["role"] = "system";
    systemMsg["content"] = "你是小嘉，只能用一句话回答";//回复太多会导致POST请求的TTS响应过慢，可以尝试分段请求或WS请求
    // 用户输入，使用语音识别的结果
    JsonObject userMsg = textArr.createNestedObject();
    userMsg["role"] = "user";
    userMsg["content"] = userInput;
    String output;
    serializeJson(doc, output);
    wsChat.send(output);
    if(!wsChat.send(output)){
        mylcd.fillScreen(TFT_BLACK);
        mylcd.setTextSize(2);
        minuteTimerExpired= true;
        speechFinished = false;
        speechText = "";
        currentState=3;
        }
    Serial.println("已发送大模型对话请求，内容：" + userInput);
}

void onChatMessage(WebsocketsMessage message)
{
    currentState = 1;
    Serial.println("大模型服务器返回：" + message.data());
    DynamicJsonDocument doc(2048);
    DeserializationError err = deserializeJson(doc, message.data());
    if (err.c_str()!="ok")
    {
        Serial.print("大模型JSON解析错误：");
        Serial.println(err.c_str());
        return;
    }

    int code = doc["header"]["code"];
    if (code == 0)
    {
        // 提取当前回复内容和序号
        int seq = doc["payload"]["choices"]["seq"];
        String content = doc["payload"]["choices"]["text"][0]["content"].as<String>();

        // 如果是第一条回复，直接赋值；否则在前面添加“，”再累加
        if (seq == 0)
        {
            chatAggregated = content;
        }
        else
        {
            chatAggregated += content;
        }

        // 更新最后一次收到回复的时间，并重置最终标志
        lastChatMsgTime = millis();
        chatFinalized = false;
        Serial.println("当前累计回复：" + chatAggregated);
    }
    else
    {
        Serial.println("大模型请求失败，错误码：" + String(code));
    }

    // 如果累计回复为空，则执行清屏操作
    if (chatAggregated.isEmpty())
    {
        chatFinalized = false;
        Serial.println("未收到有效回复，执行清屏...");
        mylcd.fillScreen(TFT_BLACK);
        mylcd.setTextSize(2);
        currentState=3;
        minuteTimerExpired = true;
    }

}



void displayTask(void *parameter)
{


    for (;;)
    {



        // 只有状态变化时才刷新屏幕
        if (currentState != lastState)
        {
            mylcd.fillScreen(TFT_BLACK);
            mylcd.setTextSize(2);
            lastState = currentState;
        }

        switch (currentState)
        {
            case 1: // 说话状态

                mylcd.setCursor(10, mylcd.height() / 2 - 10);
                mylcd.pushImage(60, 60, 120, 120, speak);
                break;

            case 2: // 机器人状态

                mylcd.setCursor(10, mylcd.height() / 2 - 10);
                mylcd.pushImage(60, 60, 120, 120, robot);
                break;

            case 3: // 时间状态
                timeClient.update();
                int currentHour = (timeClient.getHours() + 8) % 24;
                int currentMinute = timeClient.getMinutes();

                char timeBuffer[6];
                snprintf(timeBuffer, sizeof(timeBuffer), "%02d:%02d", currentHour, currentMinute);

                time_t rawTime = (time_t)timeClient.getEpochTime();
                struct tm *ptm = gmtime(&rawTime);

                char dateStr[11];
                snprintf(dateStr, sizeof(dateStr), "%04d.%02d.%02d", ptm->tm_year + 1900, ptm->tm_mon + 1, ptm->tm_mday);

                const char* weekdays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
                int weekday = (ptm->tm_wday >= 0 && ptm->tm_wday < 7) ? ptm->tm_wday : 0;


                mylcd.setTextSize(6);
                mylcd.drawString(timeBuffer, 15, 25, 2);
                mylcd.pushImage(0, 0, 158, 30, edalogo);
                mylcd.pushImage(13, 155, 75, 75, sun);
                mylcd.setTextSize(2);
                mylcd.drawLine(0, 148, 240, 148, TFT_YELLOW);
                mylcd.drawString(weekdays[weekday], 170, 0, 2);
                mylcd.drawString(dateStr, 40, 115, 2);
                mylcd.drawString(cityname, 108, 150, 2);
                mylcd.drawString("H: " + humidity + " %", 108, 178, 2);
                mylcd.drawString("T: " + temperature + " C ", 108, 205, 2);


                break;
        }

        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}
String removeNonUTF8(String chatAggregated)
{
    String result = "";
    for (size_t i = 0; i < chatAggregated.length(); i++)
    {
        uint8_t c = chatAggregated[i];  // 使用 uint8_t 来处理每个字节

        // 检查英文字符
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
        {
            result += (char)c;
        }
            // 检查数字
        else if (c >= '0' && c <= '9')
        {
            result += (char)c;
        }
            // 检查英文符号 “,” “.”
        else if (c == ',' || c == '.')
        {
            result += (char)c;
        }
            // 检查中文字符（UTF-8，3字节）
        else if (i + 2 < chatAggregated.length() && (uint8_t)chatAggregated[i] >= 0xE4 && (uint8_t)chatAggregated[i] <= 0xE9)
        { // 判断UTF-8中文起始字节范围
            uint8_t c2 = (uint8_t)chatAggregated[i + 1];
            uint8_t c3 = (uint8_t)chatAggregated[i + 2];
            if (c2 >= 0x80 && c2 <= 0xBF && c3 >= 0x80 && c3 <= 0xBF)
            {
                result += chatAggregated.substring(i, i + 3);
                i += 2;  // 跳过后两个字节
            }
        }
    }
    return result;
}


////////////////////////////
// setup()函数：初始化WiFi、NTP、I2S、Audio、WebSocket等
////////////////////////////
void setup()
{

    mylcd.init();
    mylcd.setRotation(0);
    mylcd.fillScreen(TFT_BLACK);
    mylcd.setTextSize(2);
    mylcd.pushImage(60,0,120,106,openlogo);
    mylcd.drawString("WIFI: TV-PRO", 0, 120, 2);
    mylcd.drawString("URL: 192.168.4.1", 0, 150, 2);
    // 创建周期性定时器（重要参数说明）
    minuteTimerExpired = true;
    // sprite.createSprite(mylcd.width(), mylcd.height());
    Serial.begin(115200);
    mylcd.setTextSize(1);
    //xTaskCreatePinnedToCore(memoryMonitor, "Memory", 2048, NULL, 1, NULL, 0);
    mylcd.drawString("wait wifi connect...", 0, 200, 2);
    pinMode(BUTTON_MID, INPUT_PULLUP);
    pinMode(BUTTON_LEFT, INPUT_PULLUP);
    pinMode(BUTTON_RIGHT, INPUT_PULLUP);
    // 设置WiFi为热点模式
    WiFi.softAP(ssid, password);
    Serial.println("热点已启动");
    // 访问的IP地址是 ESP8266 的默认IP：192.168.4.1
    Serial.print("访问地址: ");
    Serial.print(WiFi.softAPIP());
    // 加载WiFi配置
    loadWiFiConfig();
    if (WiFi.status() != WL_CONNECTED)
    {
        Serial.println("Starting captive portal...");
        handleWiFiConfig();
    }
    else
    {
        handleWiFiConfig();
        Serial.println("WiFi connected");
        mylcd.drawString("wait NTP connect...", 0, 220, 2);
        timeClient.begin();
        timeClient.update(); // 获取初始时间
    }



    delay(5000);



    fetchWeather();
    // 获取格式化时间
    String rfc1123Time = getDate();
    Serial.println("当前时间: " + rfc1123Time);

    // 初始化麦克风I2S
    i2s_driver_install(I2S_NUM_1, &i2sIn_config, 0, NULL);
    i2s_set_pin(I2S_NUM_1, &inmp441_pin_config);
    i2s_zero_dma_buffer(I2S_NUM_1);

    // 初始化音频播放（max98357）
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(100);
    xTaskCreatePinnedToCore(displayTask, "DisplayTask", 10000, NULL, 1, &displayTaskHandle, 1);
    Serial.println(ttsApiKey);
    Serial.println(apiSecret);
    Serial.println(appId);
    Serial.println(apiKey);
    Serial.println(cityname);
    Serial.println(weatherapi);

}


// WebSocket 连接处理函数
void connectWebSocket()
{
    if (wsSpeech.available())
    {
        wsSpeech.close();
    }
    String speechURL = generateSpeechAuthURL();
    Serial.println("语音WS URL：" + speechURL);
    wsSpeech.onMessage(onSpeechMessage);
    wsSpeech.connect(speechURL);

    // 等待 WebSocket 连接建立
    unsigned long startTime = millis();
    while (!wsSpeech.available() && millis() - startTime < 1000)
    {
        delay(10);
    }
    sendHandshake();
}

// 按键处理函数
void handleButtonPress(int button, bool &lastState, bool currentState, int buttonID)
{
    if (lastState == HIGH && currentState == LOW)
    {
        Serial.printf("BUTTON_%d 按键按下\n", buttonID);
        connectWebSocket();

        if (!isRecording)
        {
            startRecording();
        }
        BTNow = buttonID;
        isTalkingDisplayActive = true;
    }

    if (lastState == LOW && currentState == HIGH)
    {
        Serial.printf("BUTTON_%d 按键松开\n", buttonID);

        if (isRecording)
        {
            stopRecording();
        }
        isTalkingDisplayActive = false;
        currentState= 2;
        displayTaskHandle = NULL;
    }

    lastState = currentState;
}

// 处理语音识别结果
void processSpeechResult()
{
    if (speechFinished)
    {
        if (speechText.length() > 0 && !chatFinished)
        {
            if (BTNow == 1)
            {
                if (!wsChat.available())
                {
                    String chatURL = generateChatAuthURL();
                    Serial.println("大模型对话WS URL：" + chatURL);
                    wsChat.onMessage(onChatMessage);
                    wsChat.connect(chatURL);

                    unsigned long startTime = millis();
                    while (!wsChat.available() && millis() - startTime < 1000)
                    {
                        delay(10);
                    }
                }
                sendChatRequest(speechText);
            }
            else if (BTNow == 2 || BTNow == 3)
            {
                Translation(speechText);
            }
        }
        else
        {
            mylcd.fillScreen(TFT_BLACK);
            mylcd.setTextSize(2);
            minuteTimerExpired = true;
        }

        // 重置变量
        speechFinished = false;
        speechText = "";
        BTNow = 0;
        currentState = 2;
    }
}

// 处理聊天结果
void processChatResult()
{
    if (chatAggregated.length() > 0 && (millis() - lastChatMsgTime > 2000) && !chatFinalized)
    {

        chatFinalized = true;
        Serial.println("最终回复：" + chatAggregated);
        String filteredText = removeNonUTF8(chatAggregated);
        playTTS(filteredText);
        chatAggregated = "";

    }
}

void loop()
{
    wsSpeech.poll();
    wsChat.poll();
    audio.loop();

    // 读取按键状态
    static bool lastButtonMIDState = HIGH;
    static bool lastButtonLEFTState = HIGH;
    static bool lastButtonRIGHTState = HIGH;

    bool currentButtonMIDState = digitalRead(BUTTON_MID);
    bool currentButtonLEFTState = digitalRead(BUTTON_LEFT);
    bool currentButtonRIGHTState = digitalRead(BUTTON_RIGHT);

    // 处理按键
    handleButtonPress(BUTTON_MID, lastButtonMIDState, currentButtonMIDState, 1);
    handleButtonPress(BUTTON_LEFT, lastButtonLEFTState, currentButtonLEFTState, 2);
    handleButtonPress(BUTTON_RIGHT, lastButtonRIGHTState, currentButtonRIGHTState, 3);

    // 发送音频数据
    if (isRecording)
    {
        sendAudioData(false);
    }

    // 处理语音识别结果
    processSpeechResult();

    // 处理聊天结果
    processChatResult();
}

