#include <Arduino.h>
#include <ArduinoJson.h>
#include <DEV_Config.h>
#include <EPD.h>
#include <HTTPClient.h>
#include <StreamUtils.h>
#include <WiFi.h>
#include <config.h>

// globals
uint8_t *epd_fb;  // e-paper framebuffer
uint32_t epd_fb_size = ((EPD_7IN5_V2_WIDTH % 8 == 0) ? (EPD_7IN5_V2_WIDTH / 8) : (EPD_7IN5_V2_WIDTH / 8 + 1)) * EPD_7IN5_V2_HEIGHT;

StaticJsonDocument<4096> info;  // ArduinoJSON document to hold fetched info

// function prototypes
void initWiFi();
bool httpGetJson(const char *url, JsonDocument &info);
void httpGetFramebuffer(const char *url, uint8_t *buf, uint32_t bufsize);

// setup & loop ###############################################################
void setup() {
    Serial.begin(115200);

    initWiFi();

    if ((epd_fb = (uint8_t *)heap_caps_malloc(epd_fb_size, MALLOC_CAP_DMA)) == NULL) {
        DEBUG("Failed to allocate framebuffer of %i bytes...\r\n", epd_fb_size);
        delay(UINT32_MAX);  // sleep for a long long time :-)
    }

    DEV_Module_Init();
}

unsigned long time_now = 0;               // current time (in epoch)
unsigned long time_nextupdate = 0;        // when to do a next update (in epoch)
RTC_DATA_ATTR unsigned long framebuffer_id = LONG_MAX;  // unique identifier for the current framebuffer data, retain during deepsleep
const char *framebuffer_url = NULL;       // url to download framebuffer data

void loop() {
    unsigned long t1, t2;
    char tmp[2048];

    // get new info
    t1 = millis();
    sprintf(tmp, "%s?model=%s&mac=%s&ip=%s&battery=3700", CFG_URL_INFO, CFG_MODEL, WiFi.macAddress().c_str(), WiFi.localIP().toString().c_str());
    // WiFi.setSleep(false); // temporarily disabling wifi modemsleep make the download go a lot faster (up to 500ms and more)
    bool success = httpGetJson(tmp, info);
    // WiFi.setSleep(true);
    t2 = millis();
    TIMING_DEBUG("fetch info - %lums\n", t2 - t1);

    if (success) {
        time_now = info["time"]["now"] | (millis() / 1000);
        time_nextupdate = info["time"]["next"] | (time_now + 300);
        // framebuffer_id = info["framebuffer"]["id"];
        framebuffer_url = info["framebuffer"]["url"];
        DEBUG("time_now=%lu time_nextupdate=%lu framebuffer_id=%lu framebuffer_url=%s\n", time_now, time_nextupdate, framebuffer_id, framebuffer_url);
    } else {
        // could not fetch info correctly... maybe display an error screen on the e-paper?
    }

    if (success && (info["framebuffer"]["id"] != framebuffer_id)) {
        t1 = millis();
        // WiFi.setSleep(false); // temporarily disabling wifi modemsleep make the download go a lot faster (up to 500ms and more)
        httpGetFramebuffer(framebuffer_url, epd_fb, epd_fb_size);
        // WiFi.setSleep(true);
        t2 = millis();
        TIMING_DEBUG("fetch framebuffer - %lums\n", t2 - t1);

        WiFi.disconnect();
        WiFi.mode(WIFI_OFF);

        framebuffer_id = info["framebuffer"]["id"];  // only update framebuffer_id if fetched successfully

        t1 = millis();
        EPD_7IN5_V2_Init(); 
        t2 = millis();
        TIMING_DEBUG("init e-paper - %lums\n", t2 - t1);

        t1 = millis();      
        EPD_7IN5_V2_Display(epd_fb);  // MODIFIED to play with clockspeeds! (see //BC547 comments)
        t2 = millis();
        TIMING_DEBUG("display framebuffer - %lums\n", t2 - t1);

        t1 = millis();
        EPD_7IN5_V2_Sleep();
        t2 = millis();
        TIMING_DEBUG("sleep e-paper - %lums\n", t2 - t1);
    }
    // delay(10000);
    // esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH,   ESP_PD_OPTION_OFF);
    // esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);
    // esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
    // esp_sleep_pd_config(ESP_PD_DOMAIN_XTAL,         ESP_PD_OPTION_OFF);
    esp_sleep_enable_timer_wakeup(30 * uS_TO_S_FACTOR);
    esp_deep_sleep_start();
}

// #############################################################################
void initWiFi() {
    unsigned long t1, t2;

    WIFI_DEBUG("wireless MAC=%s\n", WiFi.macAddress().c_str());
    WIFI_DEBUG("connecting to WiFi");
    t1 = millis();
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(true);
    WiFi.begin(CFG_WIFI_SSID, CFG_WIFI_PSK);
    while (WiFi.status() != WL_CONNECTED) {
        WIFI_DEBUG_APPEND(".");
        delay(100);
    }
    t2 = millis();
    WIFI_DEBUG_APPEND("ok\n");
    TIMING_DEBUG("time to connect - %lums\n", t2 - t1);
    WIFI_DEBUG("local IP=%s\n", WiFi.localIP().toString());
}

// See https://arduinojson.org/v6/how-to/use-arduinojson-with-httpclient/#how-to-parse-a-json-document-from-an-http-response
bool httpGetJson(const char *url, JsonDocument &info) {
    WiFiClient client;
    HTTPClient http;
    DeserializationError error;
    bool result = false;

    // Your Domain name with URL path or IP address with path
    HTTP_DEBUG("GET url: %s\n", url);
    // client.setInsecure();
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.useHTTP10(true);
    http.begin(client, url);

    // If you need Node-RED/server authentication, insert user and password below
    // http.setAuthorization("REPLACE_WITH_SERVER_USERNAME", "REPLACE_WITH_SERVER_PASSWORD");

    // Send HTTP POST request
    int httpCode = http.GET();
    HTTP_DEBUG("http status code: %d\n", httpCode);

    // file found at server
    if (httpCode == HTTP_CODE_OK) {
        error = deserializeJson(info, http.getStream());
        // ReadLoggingStream loggingStream(http.getStream(), Serial);
        // deserializeJson(doc, loggingStream);
        HTTP_DEBUG("parse status: %s\n", error.c_str());
        result = true;
    }

    http.end();

    return result;
}

// See https://github.com/espressif/arduino-esp32/blob/master/libraries/HTTPClient/examples/StreamHttpClient/StreamHttpClient.ino
void httpGetFramebuffer(const char *url, uint8_t *buf, uint32_t bufsize) {
    WiFiClient client;
    HTTPClient http;

    // Your Domain name with URL path or IP address with path
    HTTP_DEBUG("GET url: %s\n", url);
    // client.setInsecure();
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.begin(client, url);

    int httpCode = http.GET();
    HTTP_DEBUG("http status code: %d\n", httpCode);

    // file found at server
    if (httpCode == HTTP_CODE_OK) {
        // get length of document (is -1 when Server sends no Content-Length header)
        int len = http.getSize();
        HTTP_DEBUG("content size: %d\n", len);

        // get tcp stream
        WiFiClient *stream = http.getStreamPtr();

        if (len == bufsize) {
            len = stream->readBytes(buf, bufsize);
            HTTP_DEBUG("read %d bytes in framebuffer\n", len);
        } else {
            HTTP_DEBUG("data size mismatch: request=%d bytes framebuffer=%d bytes\n", len, bufsize);
        }
    }

    http.end();
}
