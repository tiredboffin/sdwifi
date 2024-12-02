/*
 *  sdwifi: simple server to upload files to Fysetc SD WiFi card.
 *
 *  Based on a simple WebServer.
 */

#ifdef ASYNCWEBSERVER_REGEX
# define PUT_UPLOAD
#endif

#define MIN_MEM_THRESHOLD 32768

// #define USE_SD

#include <WiFi.h>
#include <WiFiClient.h>

#include <ESPAsyncWebServer.h>

#include <ESPmDNS.h>
#ifdef USE_SD
#include <SPI.h>
#include <SD.h>
#define SD_CS_PIN 13
#define SD_MISO_PIN 2
#define SD_MOSI_PIN 15
#define SD_SCLK_PIN 14
#else
#include <SD_MMC.h>
#endif
#include <Preferences.h>
#include <mbedtls/sha1.h>
#include <esp_mac.h>
#include <SPIFFS.h>
#include <time.h>
#include "IniFile.h"

#if defined __has_include
# if __has_include(<mbedtls/compat-2.x.h>)
#  include <mbedtls/compat-2.x.h>
# endif
#endif

static const char *default_name = "sdwifi";

static unsigned mount_counter = 0;
static unsigned umount_counter = 0;

#define SD_SWITCH_PIN GPIO_NUM_26
#define SD_POWER_PIN GPIO_NUM_27
#define CS_SENSE_PIN GPIO_NUM_33

#define PREF_RW_MODE false
#define PREF_RO_MODE true
#define PREF_NS "wifi"

#define SDWIFI_INI_FILENAME "/sdwifi_config.ini"

#define WIFI_STA_TIMEOUT 5000

#define HOST_ACTIVITY_GRACE_PERIOD_MILLIS 2000

enum
{
  NOERROR,
  ERROR_MOUNT_BUSY,
  ERROR_MOUNT_FAILED,
  ERROR_FILE_NOT_FOUND,
  ERROR_FILE_OPEN_FAILED,
  ERROR_INI_INVALID,
};

AsyncWebServer server(80);

Preferences prefs;

File dataFile;
static int mem_warning;

#ifdef USE_SD
fs::FS &fileSystem = SD;
#else
fs::FS &fileSystem = SD_MMC;
#endif

volatile struct
{
  bool mount_is_safe = true;
  bool activity_detected = false;
  bool host_activity_detected = false;
  unsigned isr_counter = 0;
  unsigned long host_last_activity_millis;
} sd_state;

static bool esp32_controls_sd = false;
static int fs_is_mounted = 0;

void IRAM_ATTR sd_isr(void);

void debug_meminfo(char *txt, unsigned count)
{
  log_e("%s, %u, %u, %u", txt, count, heap_caps_get_free_size(MALLOC_CAP_DEFAULT), heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
}

void setup(void)
{
  sd_state.host_last_activity_millis = millis();

  pinMode(SD_SWITCH_PIN, OUTPUT);
#ifdef USE_SD
  pinMode(SD_POWER_PIN, OUTPUT);
#endif
  /* check for config file on sd, if found get its values and remove it */
  (void)loadConfigIni(SDWIFI_INI_FILENAME, true);


  sd_state.mount_is_safe = false;

  attachInterrupt(CS_SENSE_PIN, sd_isr, CHANGE);

  /* Make SD card available to the Host early in the process */
  digitalWrite(SD_SWITCH_PIN, HIGH);
  Serial.setDebugOutput(true);

  setupWiFi();
  SPIFFS.begin();
  setupWebServer();
  server.begin();
  debug_meminfo("setup", 0);
}

void IRAM_ATTR sd_isr(void)
{
  detachInterrupt(CS_SENSE_PIN);
  sd_state.isr_counter++;
  sd_state.activity_detected = true;
  if (digitalRead(SD_SWITCH_PIN) == HIGH)
  {
    sd_state.mount_is_safe = false;
    sd_state.host_activity_detected = true;
    sd_state.host_last_activity_millis = millis();
  }
}

/* should go into monitor task */
void monitor_sd(void)
{
  static unsigned long previousMillis;
  unsigned long currentMillis = millis();
  if ((currentMillis - previousMillis) > 100)
  {
    if (sd_state.activity_detected)
    {
      sd_state.activity_detected = false;
      if (sd_state.host_activity_detected)
      {
        sd_state.host_activity_detected = false;
        sd_state.host_last_activity_millis = currentMillis; // overwrite to be more conservative
      }
      attachInterrupt(CS_SENSE_PIN, sd_isr, CHANGE);
    }
    sd_state.mount_is_safe = (currentMillis - sd_state.host_last_activity_millis) >= HOST_ACTIVITY_GRACE_PERIOD_MILLIS;
    previousMillis = currentMillis;
  }
}

void loop(void)
{
  monitor_sd();

  /* warn and reboot on low memory */
  if (!mem_warning && heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT) < MIN_MEM_THRESHOLD * 2)
  {
    debug_meminfo("low mem", ++mem_warning);
  }
  if (mem_warning && heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT) < MIN_MEM_THRESHOLD)
  {
    debug_meminfo("crtical mem", ++mem_warning);
    ESP.restart();
  }
}
void setupWiFi()
{

  prefs.begin(PREF_NS, PREF_RO_MODE);

  String hostname = prefs.isKey("hostname") ? prefs.getString("hostname") : default_name;

  WiFi.hostname(hostname);

  /* assume STA mode if sta_ssid is defined */
  if (prefs.isKey("sta_ssid") && setupSTA())
  {
    if (!MDNS.begin(hostname))
    {
      log_e("Error setting up MDNS responder");
    }
    else
    {
      log_i("Set MDNS service name to %s", hostname);
    }
  }
  else
  {
    if (!setupAP())
      while (1)
        delay(1);
  }
  prefs.end();
}
void setupWebServer()
{
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Methods", "GET, POST, PUT");
  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Headers", "Content-Type");

  /* TODO: rethink the API to simplify scripting  */
  server.on("/ping", [](AsyncWebServerRequest *request) {
    httpOK(request);
  });
  server.on("/sysinfo", handleInfo);
  server.on("/config", handleConfig);
  server.on("/exp", handleExperimental);
  /* file ops */
  server.on("/upload", HTTP_POST, handleUpload, handleUploadProcess);
#ifdef PUT_UPLOAD
  server.on("^/upload/(.+)$", HTTP_PUT, handleUpload, nullptr, handleUploadProcessPUT);
#endif
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/sha1", handleSha1);
  server.on("/remove", handleRemove);
  server.on("/list", handleList);
  server.on("/rename", handleRename);
  server.on("/mkdir", handleMkdir);
  server.on("/rmdir", handleRmdir);

  /* Testing: For compatibility with original Fysetc web app code */
  server.on("/relinquish", HTTP_GET, [](AsyncWebServerRequest *request) {
    httpOK(request);
  });
  server.on("/wificonnect", HTTP_POST, handleWiFiConnect);
  server.on("/wifiap", HTTP_POST, handleWiFiAP);
  server.on("/delete", handleRemove);

  /* Static content */
  server.serveStatic("/", SPIFFS, "/");
  server.onNotFound([](AsyncWebServerRequest *request) {
    switch(request->method()) {
      case HTTP_OPTIONS: httpOK(request); break;
      default: httpNotFound(request); break;
    }
  });

  log_i("HTTP server started");
}

static bool setupAP(void)
{
  String ssid = prefs.isKey("ap_ssid") ? prefs.getString("ap_ssid") : default_name;
  String password = prefs.isKey("ap_password") ? prefs.getString("ap_password") : default_name;

  WiFi.mode(WIFI_AP);

  if (prefs.isKey("ip"))
  {
    IPAddress ip, mask;
    ip.fromString(prefs.getString("ip"));
    mask.fromString(prefs.getString("mask"));
    WiFi.softAPConfig(ip, ip, mask);
  }

  if (!WiFi.softAP(ssid, password))
  {
    log_e("Fallback to unprotected AP %s", ssid.c_str()); // typically happens when the password is too short
    delay(100);
    if (!WiFi.softAP(ssid))
    {
      log_e("Fallback to default AP name %s and ip address", default_name);
      delay(100);
      WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
      if (!WiFi.softAP(default_name))
      {
        log_e("Soft AP creation failed");
        return false;
      }
    }
  }

  log_i("Soft AP created: %s", WiFi.softAPSSID());
  return true;
}

static bool setupSTA(void)
{
  String ssid = prefs.getString("sta_ssid");
  String password = prefs.isKey("sta_password") ? prefs.getString("sta_password") : "";

  WiFi.mode(WIFI_STA);

  if (prefs.isKey("ip"))
  {
    IPAddress ip, dns, gateway, subnet;
    ip.fromString(prefs.getString("ip"));
    dns.fromString(prefs.getString("dns"));
    gateway.fromString(prefs.getString("gateway"));
    subnet.fromString(prefs.getString("mask"));
    WiFi.config(ip, dns, gateway, subnet);
  }

  WiFi.begin(ssid, password);

  WiFi.waitForConnectResult(WIFI_STA_TIMEOUT);
  if (WiFi.status() == WL_CONNECTED)
  {
    log_i("Connected to %s with IP address: %s", ssid, WiFi.localIP().toString());
    return true;
  }
  else
  {
    log_e("Connection to %s failed with status %d", ssid, WiFi.status());
    return false;
  }
}

static inline bool is_safe_to_mount(void)
{
  return sd_state.mount_is_safe;
}

static inline void sd_lock(void)
{
  if (!esp32_controls_sd)
  {
    digitalWrite(SD_SWITCH_PIN, LOW);
#ifdef USE_SD
    SPI.begin(SD_SCLK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);
#endif
  }
}

static inline void sd_unlock(void)
{
  if (!esp32_controls_sd)
  {
#ifdef USE_SD
#define SD_D0_PIN 2
#define SD_D1_PIN 4
#define SD_D2_PIN 12
#define SD_D3_PIN 13
#define SD_CLK_PIN 14
#define SD_CMD_PIN 15
    pinMode(SD_D0_PIN, INPUT_PULLUP);
    pinMode(SD_D1_PIN, INPUT_PULLUP);
    pinMode(SD_D2_PIN, INPUT_PULLUP);
    pinMode(SD_D3_PIN, INPUT_PULLUP);
    pinMode(SD_CLK_PIN, INPUT_PULLUP);
    pinMode(SD_CMD_PIN, INPUT_PULLUP);
    SPI.end();
#endif
    digitalWrite(SD_SWITCH_PIN, HIGH);
  }
}

/* Mount SD card */
static int mountSD(void)
{
  if (fs_is_mounted > 0)
  {
    ++fs_is_mounted;
    log_e("Double mount ignored %u", mount_counter);
    return NOERROR;
  }

  if (!sd_state.mount_is_safe)
  {
    log_i("Card is busy: %u", mount_counter);
    return ERROR_MOUNT_BUSY;
  }

  /* get control over flash NAND */
  sd_lock();
#ifdef USE_SD
  if (!SD.begin(SD_CS_PIN))
#else
  if (!SD_MMC.begin())
#endif
  {
    sd_unlock();
    log_i("Failed: %u", mount_counter);
    return ERROR_MOUNT_FAILED;
  }
  ++fs_is_mounted;
  ++mount_counter;
  log_i("Success: %u", mount_counter);
  return NOERROR;
}

/* Unmount SD card */
static void umountSD(void)
{
  if (fs_is_mounted > 1)
  {
    log_e("Double unmount ignored %u", umount_counter);
    --fs_is_mounted;
    return;
  }
#ifdef USE_SD
  SD.end();
#else
  SD_MMC.end();
#endif
  sd_unlock();
  --fs_is_mounted;
  umount_counter++;
  log_i("Success: %u", umount_counter);
}
// wifi config parameters recognized in config?param=value and in sdwifi_config.ini file
static const char *cfgparams[] = {
    "sta_ssid", "sta_password", "ap_ssid", "ap_password", "hostname", "ip", "gateway", "mask", "dns"};

static bool cfgparamVerify(const char *n)
{
  for (int i = 0; i < sizeof(cfgparams) / sizeof(cfgparams[0]); i++)
  {
    if (!strcmp(n, cfgparams[i]))
      return true;
  }
  return false;
}

static String getInterfaceMacAddress(esp_mac_type_t interface)
{
  String mac = "";

  unsigned char mac_base[6] = {0};

  if (esp_read_mac(mac_base, interface) == ESP_OK)
  {
    char buffer[18];
    sprintf(buffer, "%02X:%02X:%02X:%02X:%02X:%02X", mac_base[0], mac_base[1], mac_base[2], mac_base[3], mac_base[4], mac_base[5]);
    mac = buffer;
  }

  return mac;
}

/* load configuration parameters from config.ini file if it is present on sd card */
int loadConfigIni(const char *filename, bool removeFile)
{

  if (mountSD() != NOERROR)
  {
    log_w("mount failed");
    return ERROR_MOUNT_FAILED;
  }

  int err = 0;
  IniFile ini(filename);
  const size_t bufferLen = 80;
  char buffer[bufferLen];

  if (!fileSystem.exists(filename))
  {
    log_i("File %s not found", filename);
    err = ERROR_FILE_NOT_FOUND;
  }
  else if (!ini.open())
  {
    log_e("Failed to open file %s", filename);
    err = ERROR_FILE_OPEN_FAILED;
  }
  else if (!ini.validate(buffer, bufferLen))
  { // Check the file is valid. This can be used to warn if any lines are longer than the buffer.
    log_e("ini file %s not valid: %d", ini.getFilename(), ini.getError());
    err = ERROR_INI_INVALID;
  }

  if (err)
  {
    umountSD();
    return err;
  }

  log_i("Ini file %s", filename);

  prefs.begin(PREF_NS, PREF_RW_MODE);
  prefs.clear();

  for (int i = 0; i < sizeof(cfgparams) / sizeof(cfgparams[0]); i++)
  {
    const char *str = cfgparams[i];
    if (ini.getValue(PREF_NS, str, buffer, bufferLen))
    {
      log_i("%s %s: %s", PREF_NS, str, buffer);
      prefs.putString(str, buffer);
    }
  }
  prefs.end();
  ini.close();
  if (removeFile) {
    log_i("remove config file: %s", filename);
    fileSystem.remove(filename);
  }
  umountSD();
  ESP.restart();
  // should never get here
  log_e("esp restart failed");
  for (;;)
    ;
  return -1;
}

/* CMD: Return some info */
void handleInfo(AsyncWebServerRequest *request)
{

  String txt;

  uint8_t tmp[6];

  txt = "{\"info\":{\"filesystem\":{";
  if (request->hasArg("sd") && request->arg("sd") == "none")
  {
    txt += "\"status\":\"info disabled\"";
  }
  else
  {
    switch (mountSD())
    {
    case NOERROR:
      /* Requests for total/used bytes take too long depending on the number and size of files on the card. */
      uint64_t card_size, total_bytes, used_bytes;
#ifdef USE_SD
      card_size = SD.cardSize();
      total_bytes = SD.totalBytes();
      used_bytes = SD.usedBytes();
#else
      card_size = SD_MMC.cardSize();
      total_bytes = SD_MMC.totalBytes();
      used_bytes = SD_MMC.usedBytes();
#endif
      txt += "\"status\":\"free\",";
      txt += "\"cardsize\":";
      txt += card_size;
      txt += ",\"totalbytes\":";
      txt += total_bytes;
      txt += ",\"usedbytes\":";
      txt += used_bytes;
      umountSD();
      break;
    case ERROR_MOUNT_BUSY:
      txt += "\"status\":\"busy\"";
      break;
    default:
      txt += "\"status\":\"failed\"";
      break;
    }
  }
  txt += "},";
  txt += "\"isr\":{";
  txt += "\"count\":";
  txt += sd_state.isr_counter;
  txt += ",";
  txt += "\"activity_millis_ago\":";
  txt += millis() - sd_state.host_last_activity_millis;
  txt += "},";
  txt += "\"meminfo\":{";
  txt += "\"free_size\":";
  txt += heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
  txt += ",";
  txt += "\"minimum_free_size\":";
  txt += heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT);
  txt += "},";
  txt += "\"build\":{";
  txt += "\"board\":\"";
  txt += ARDUINO_BOARD;
  txt += "\",";
  txt += "\"esp-idf\":\"";
  txt += esp_get_idf_version();
  txt += "\"";
  txt += "},";
  txt += "\"cpu\":{";
  txt += "\"model\":\"";
  txt += ESP.getChipModel();
  txt += "\",";
  txt += "\"revision\":";
  txt += ESP.getChipRevision();
  txt += ",";
  txt += "\"coreid\":";
  txt += xPortGetCoreID();
  txt += ",";
  txt += "\"millis\":";
  txt += millis();
  txt += ",";
  txt += "\"reset_reason\":";
  txt += esp_reset_reason();
  txt += "},";

  int wifiMode = WiFi.getMode();
  txt += "\"network\":{";
  txt += "\"Mode\":\"";
  txt += (wifiMode == WIFI_MODE_AP) ? "AP" : "STA";
  txt += "\",";
  txt += "\"SSID\":\"";
  txt += (wifiMode == WIFI_MODE_AP) ? WiFi.softAPSSID() : WiFi.SSID();
  txt += "\",";
  txt += "\"WifiStatus\":\"";
  txt += WiFi.status();
  txt += "\",";
  txt += "\"Wifi Strength\":\"";
  txt += WiFi.RSSI();
  txt += " dBm\",";
  txt += "\"IP\":\"";
  txt += (wifiMode == WIFI_MODE_AP) ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  txt += "/";
  txt += (wifiMode == WIFI_MODE_AP) ? WiFi.softAPSubnetMask().toString() : WiFi.subnetMask().toString();
  txt += "\",";
  txt += "\"Gateway\":\"";
  txt += WiFi.gatewayIP().toString();
  txt += "\",";
  txt += "\"Hostname\":\"";
  txt += WiFi.getHostname();
  txt += "\",";
  txt += "\"DNS\":[\"";
  txt += WiFi.dnsIP(0).toString();
  txt += "\",\"";
  txt += WiFi.dnsIP(1).toString();
  txt += "\",\"";
  txt += WiFi.dnsIP(2).toString();
  txt += "\"],\"MAC\":{";
  txt += "\"STA\":\"";
  txt += getInterfaceMacAddress(ESP_MAC_WIFI_STA);
  txt += "\",\"AP\":\"";
  txt += getInterfaceMacAddress(ESP_MAC_WIFI_SOFTAP);
  txt += "\",\"BT\":\"";
  txt += getInterfaceMacAddress(ESP_MAC_BT);
  txt += "\"}}}}";
  request->send(200, "application/json", txt);
}

/* CMD: Update configuration parameters */
void handleConfig(AsyncWebServerRequest *request)
{

  String txt;
  if (request->args() == 0)
  {
    txt = "Supported configuration parameters:\n\n";
    for (int i = 0; i < sizeof(cfgparams) / sizeof(cfgparams[0]); i++)
    {
      txt += cfgparams[i];
      txt += "\n";
    }
    httpOK(request, txt);
  }

  if (request->args() == 1 && request->argName(0) == "load")
  {
    String v = request->arg((int)0);
    v = "/" + v;
    int err = loadConfigIni(v.c_str(), false);

    if (err == NOERROR)
      httpOK(request);
    else
      httpNotFound(request);

    return;
  }

  prefs.begin(PREF_NS, PREF_RW_MODE);
  for (int i = 0; i < request->args(); i++)
  {
    String n = request->argName(i);
    String v = request->arg(i);
    if (cfgparamVerify(n.c_str()))
    {
      prefs.putString(n.c_str(), v.c_str());
      txt += n + "=" + v + "\n";
    }
    else if (n == "clear" || n == "reset")
    {
      if (v == "all")
        prefs.clear();
      else
        prefs.remove(v.c_str());
      txt += n + " " + v + "\n";
    }
    else
    {
      txt += "unknown parameter " + n + " ignored\n";
    }
  }
  prefs.end();
  httpOK(request, txt);
}

/* CMD: wificonnect: compatibility with original Fysetc web app */
void handleWiFiConnect(AsyncWebServerRequest *request)
{

  String txt;

  prefs.begin(PREF_NS, PREF_RW_MODE);
  for (int i = 0; i < request->args(); i++)
  {
    String n = "sta_" + request->argName(i);
    String v = request->arg(i);
    if (cfgparamVerify(n.c_str()))
    {
      prefs.putString(n.c_str(), v.c_str());
      txt += n + "=" + v + "\n";
    }
    else if (n == "clear" || n == "reset")
    {
      if (v == "all")
        prefs.clear();
      else
        prefs.remove(v.c_str());
      txt += n + " " + v + "\n";
    }
    else
    {
      txt += "unknown parameter " + n + " ignored\n";
    }
  }
  prefs.end();
  httpOK(request);
  delay(50);
  ESP.restart();
}

/* CMD: wificonnect: compatibility with original Fysetc web app */
void handleWiFiAP(AsyncWebServerRequest *request)
{
  prefs.begin(PREF_NS, PREF_RW_MODE);
  prefs.remove("sta_ssid");
  prefs.remove("sta_password");
  prefs.end();
  httpOK(request);
  delay(50);
  ESP.restart();
}

void handleExperimental(AsyncWebServerRequest *request)
{
  String txt;

  for (int i = 0; i < request->args(); i++)
  {
    String n = request->argName(i);
    String v = request->arg(i);
    txt += n + "=" + v;
    /* enforce IO26 pin value */
    if (n == "io26")
    {
      if (v == "esp32" || v == "low")
      {
        if (!esp32_controls_sd)
        {
          sd_lock();
          esp32_controls_sd = true;
        }
        txt += " SD controlled by ESP32";
      }
      else if (v == "host" || v == "high")
      {
        if (esp32_controls_sd)
        {
          esp32_controls_sd = false;
          sd_unlock();
        }
        txt += " SD controlled by Host";
      }
      else
      {
        txt += " Ignored";
      }
    }
    else if (n == "io27")
    {
      txt += " io27 ";
      if (v == "output")
      {
        pinMode(SD_POWER_PIN, OUTPUT);
        txt += "OUTPUT";
      }
      else if (v == "low")
      {
        digitalWrite(SD_POWER_PIN, LOW);
        txt += "LOW";
      }
      else if (v == "high")
      {
        digitalWrite(SD_POWER_PIN, HIGH);
        txt += "HIGH";
      }
      else if (v == "read")
      {
        digitalRead(SD_POWER_PIN);
        txt += "read ";
        txt += digitalRead(SD_POWER_PIN);
      }
    }
    else if (n == "power")
    {
      if (v == "restart" || v == "reboot" || v == "reset")
      {
        httpOK(request, txt);
        delay(50);
        ESP.restart();
      }
      else if (v == "off" || v == "shutdown")
      {
        httpOK(request, txt);
        delay(10);
        ESP.deepSleep(-1);
        /* never get here */
        log_e("Unexpected return from poweroff");
        return;
      }
      else
      {
        txt += " Ignored";
      }
    }
    else if (n == "sleep")
    {
      long i = v.toInt();
      if (i > 0)
      {
        httpOK(request, txt);
        log_i("deep sleep %ld ms", i);
        delay(10);
        ESP.deepSleep(i * 1000);
        /* never get here */
        log_e("Unexpected return from deep sleep %ld ms", i);
        return;
      }
      else if (v == "sense")
      {
        esp_sleep_enable_ext0_wakeup(CS_SENSE_PIN, 0); // 1 = High, 0 = Low
        log_i("deep sleep %ld ms", i);
        esp_deep_sleep_start();
        log_e("Unexpected return from deep sleep on pin %d", CS_SENSE_PIN);
        return;
      }
      else
      {
        txt += " Ignored";
      }
    }
    else
    {
      txt += " Ignored";
    }
  }
  httpOK(request, txt);
}

#include "ff.h"
void get_sfn(char *out_sfn, File *file)
{
#if FF_USE_LFN
  FILINFO info;
  f_stat(file->path(), &info);
  if (strlen(info.altname)) {
      strncpy(out_sfn, info.altname, FF_SFN_BUF + 1);
      return;
  }
#endif /* FF_USE_LFN */
  strncpy(out_sfn, file->name(), FF_SFN_BUF + 1);
}

struct HandleListStates {
  File root;
  String txt;
  int count;
  bool finished;
};

/* CMD list */
void handleList(AsyncWebServerRequest *request)
{
  String path;

  if (request->hasArg("path")) {
    path = request->arg("path");
  } else if (request->hasArg("dir")) {
    path = request->arg("dir");
  } else {
    httpInvalidRequest(request, "LIST:BADARGS");
    return;
  }

  if (path[0] != '/') {
    path = "/" + path;
  }

  String txt;

  if (mountSD() != NOERROR)
  {
    httpServiceUnavailable(request, "LIST:SDBUSY");
    return;
  }

  if (fileSystem.exists(path)) {
    File root = fileSystem.open(path);
    if (root.isDirectory()) {
      struct HandleListStates* states = new HandleListStates();
      states->root = root;
      request->_tempObject = states;

      request->onDisconnect([request](){
        struct HandleListStates* states = (struct HandleListStates*) request->_tempObject;
        if (states && states->root) {
          states->root.close();
          states->root = File();
        }
        umountSD();
        log_v("LIST aborted");
      });

      AsyncWebServerResponse *response = request->beginChunkedResponse(
        "application/json",
        [request, path](
            uint8_t* buffer,
            const size_t max_len,
            const size_t index) mutable -> size_t
        {
          struct HandleListStates* states = (struct HandleListStates*) request->_tempObject;

          char sfn[FF_SFN_BUF + 1];

          log_i("index %u, %u", index, max_len);
          if (index == 0) {
            states->txt = "[";
            states->count = 0;
            states->finished = false;
          }

          while (File file = states->root.openNextFile()) {
            get_sfn(sfn, &file);

            if (states->count++) {
              states->txt += ",";
            }

            states->txt += "{\"id\":";
            states->txt += states->count;
            states->txt += ",\"type\":";

            if (file.isDirectory()) {
              states->txt += "\"dir\",";
            } else {
              states->txt += "\"file\",";
            }

            states->txt += "\"name\":\"";
            states->txt += file.name();
            states->txt += "\",\"size\":";
            states->txt += file.size();
            states->txt += ",\"sfn\":\"";
            states->txt += sfn;
            states->txt += "\",\"last\":";
            states->txt += file.getLastWrite();
            states->txt += "}";

            if (states->txt.length() > max_len) {
              memcpy(buffer, states->txt.c_str(), max_len);
              states->txt = states->txt.substring(max_len);
              return max_len;
            }
          }

          if (!states->finished) {
            states->finished = true;
            states->txt += "]";
            memcpy(buffer, states->txt.c_str(), states->txt.length());
            states->root.close();
            umountSD();
            return states->txt.length();
          } else {
            return 0;
          }
        });
        request->send(response);
    }
    else
    {
      sendFileInfoJson(request, root);
      root.close();
      umountSD();
    }

  } else {
    httpNotFound(request);
    umountSD();
  }
}

/* CMD Download a file */
void handleDownload(AsyncWebServerRequest *request)
{

  if (!request->hasArg("path"))
  {
    httpInvalidRequest(request, "DOWNLOAD:BADARGS");
    return;
  }
  String path = request->arg("path");

  if (path[0] != '/')
    path = "/" + path;

  if (mountSD() != NOERROR)
  {
    httpServiceUnavailable(request, "DOWNLOAD:SDBUSY");
    return;
  }

  if (fileSystem.exists(path))
  {
    File dataFile = fileSystem.open(path, FILE_READ);
    if (!dataFile)
    {
      httpServiceUnavailable(request, "Failed to open file");
    }
    else if (dataFile.isDirectory())
    {
      dataFile.close();
      httpNotAllowed(request, "Path is a directory");
    }
    else
    {
      dataFile.close();
      AsyncWebServerResponse *response = request->beginResponse(fileSystem, path, "application/octet-stream");
      request->send(response);
    }
  }
  else
  {
    httpNotFound(request, "DOWNLOAD:FileNotFound");
  }
  umountSD();
}

/* CMD Rename a file */
void handleRename(AsyncWebServerRequest *request)
{
  String nameFrom;
  String nameTo = "/";

  for (int i = 0; i < request->args(); i++)
  {
    String n = request->argName(i);
    String v = request->arg(i);
    if (n == "from")
      nameFrom = "/" + v;
    else if (n == "to")
      nameTo = "/" + v;
    else
    {
      httpInvalidRequest(request, "RENAME:BADARGS");
      return;
    }
  }
  if (!nameFrom || !nameTo)
  {
    httpInvalidRequest(request, "Both 'to' and 'from' has to be specified in 'rename' command");
    return;
  }
  if (nameFrom == nameTo)
  {
    httpInvalidRequest(request, "'to' must not be equal to 'from'");
    return;
  }

  if (mountSD() != NOERROR)
  {
    httpServiceUnavailable(request, "RENAME:SDBUSY");
    return;
  }
  if (fileSystem.exists((char *)nameFrom.c_str()))
  {
    if (fileSystem.exists((char *)nameTo.c_str()))
    {
      fileSystem.remove(nameTo.c_str());
    }
    if (fileSystem.rename(nameFrom.c_str(), nameTo.c_str()))
      httpOK(request, "OK");
    else
      httpServiceUnavailable(request, "Failed to rename");
  }
  else
    httpNotFound(request, nameFrom + " not found");
  umountSD();
}

/* CMD Remove a file */
void handleRemove(AsyncWebServerRequest *request)
{
  if (!request->hasArg("path"))
  {
    httpInvalidRequest(request, "DELETE:BADARGS");
    return;
  }

  if (mountSD() != NOERROR)
  {
    httpServiceUnavailable(request, "DELETE:SDBUSY");
    return;
  }

  String path = request->arg("path");

  if (path[0] != '/')
    path = "/" + path;

  if (fileSystem.exists((char *)path.c_str()))
  {
    fileSystem.remove(path.c_str());
    httpOK(request);
  }
  else
    httpNotFound(request);

  umountSD();
}

/* CMD Return SHA1 of a file */
void handleSha1(AsyncWebServerRequest *request)
{

  if (!request->hasArg("path"))
  {
    httpInvalidRequest(request, "SHA1:BADARGS");
    return;
  }

  if (mountSD() != NOERROR)
  {
    httpServiceUnavailable(request, "SHA1:SDBUSY");
    return;
  }

  String path = request->arg("path");
  if (path[0] != '/')
    path = "/" + path;

  File dataFile = fileSystem.open(path, FILE_READ);

  if (!fileSystem.exists(path) || !dataFile)
  {
    umountSD();
    httpNotFound(request);
    return;
  };
  if (dataFile.isDirectory())
  {
    dataFile.close();
    umountSD();
    httpNotAllowed(request);
    return;
  }
  size_t fileSize = dataFile.size();

  log_i("sha1 file %s size  %lu", path.c_str(), fileSize);

  mbedtls_sha1_context ctx;
  mbedtls_sha1_init(&ctx);
  mbedtls_sha1_starts_ret(&ctx);

#define bufferSize 1024

  uint8_t sha1_buffer[bufferSize];

  int N = fileSize / bufferSize;
  int r = fileSize % bufferSize;

  for (int i = 0; i < N; i++)
  {
    dataFile.readBytes((char *)sha1_buffer, bufferSize);
    mbedtls_sha1_update_ret(&ctx, sha1_buffer, bufferSize);
  }
  if (r)
  {
    dataFile.readBytes((char *)sha1_buffer, r);
    mbedtls_sha1_update_ret(&ctx, sha1_buffer, r);
  }
  dataFile.close();
  umountSD();

  String result = "{\"sha1sum\": \"";
  {
    unsigned char tmp[20];
    mbedtls_sha1_finish_ret(&ctx, tmp);
    mbedtls_sha1_free(&ctx);

    for (int i = 0; i < sizeof(tmp); i++)
    {
      if (tmp[i] < 0x10)
        result += "0";
      result += String(tmp[i], 16);
    }
  }
  result += "\"}";
  request->send(200, "application/json", result);
  return;
}

/* CMD Upload a file */
void handleUpload(AsyncWebServerRequest *request)
{
  // Do not answer, otherwise we erase the one sent by handler
}

#ifdef PUT_UPLOAD


struct HandlePUTStates {
  File dataFile;
};

void handleUploadProcessPUT(AsyncWebServerRequest *request, uint8_t* data, size_t len, size_t index, size_t total)
{
  String path = request->pathArg(0);
  struct HandlePUTStates * states;

  if (path == nullptr || path == "") {
    log_i("Upload failed: BADDARGS");
    httpInvalidRequestJson(request, "{\"error\":\"UPLOAD:BADARGS\"}");
    return;
  }

  if (path[0] != '/') {
    path = "/" + path;
  }

  if (index == 0) {
    states = new HandlePUTStates();
    states->dataFile = File();
    request->_tempObject = states;

    if (mountSD() != NOERROR) {
      log_i("Upload failed: SDBUSY");
      httpServiceUnavailableJson(request, "{\"error\":\"UPLOAD:SDBUSY\"}");
      return;
    }

    if (fileSystem.exists(path)) {
      fileSystem.remove(path); // should fail if the path is a directory
    }

    File dataFile = fileSystem.open(path, FILE_WRITE);

    if (!dataFile) {
      log_i("Upload failed: File open failed");
      umountSD();
      httpInternalErrorJson(request, "{\"error\":\"Failed to open file\"}");
      return;
    }

    if (dataFile.isDirectory()) {
      dataFile.close();
      log_i("Upload failed: Path is a directory");
      umountSD();
      httpNotAllowedJson(request, "{\"error\":\"Path is a directory\"}");
      return;
    }
    log_i("Upload: START, filename: %s", path.c_str());
    states->dataFile = dataFile;

    request->onDisconnect([states, path](){
      states->dataFile.close();
      states->dataFile = File();
      fileSystem.remove(path);
      umountSD();
      log_v("Upload PUT: file aborted");
    });
  } else {
    states = (struct HandlePUTStates *) request->_tempObject;
  }

  if (!states->dataFile) {
    log_i("Upload failed: !states->dataFile");
    return;
  }

  states->dataFile.write(data, len);
  log_i("Upload PUT: WRITE, length: %d", len);

  if(index + len == total) {
    states->dataFile.flush();
    sendFileInfoJson(request, states->dataFile);
    states->dataFile.close();
    states->dataFile = File();
    umountSD();
    log_i("Upload PUT: END, Size: %d", total);
  }
}
#endif

void handleMkdir(AsyncWebServerRequest *request)
{
  if (!request->hasArg("path")) {
    httpInvalidRequestJson(request, "{\"error\":\"MKDIR:BADARGS\"}");
    return;
  }

  if (mountSD() != NOERROR) {
    httpServiceUnavailableJson(request, "{\"error\":\"MKDIR:SDBUSY\"}");
    return;
  }

  String path = request->arg("path");

  if (path[0] != '/') {
    path = "/" + path;
  }


  if (fileSystem.exists(path) || fileSystem.mkdir(path)) {
    File dir = fileSystem.open(path);
    sendFileInfoJson(request, dir);
    dir.close();
  } else {
    httpNotFoundJson(request,"{\"error\":\"Failed to create directory\"}");
  }

  umountSD();
}

void handleRmdir(AsyncWebServerRequest *request)
{
  if (!request->hasArg("path"))
  {
    httpInvalidRequest(request, "RMDIR:BADARGS");
    return;
  }

  if (mountSD() != NOERROR)
  {
    httpServiceUnavailable(request, "RMDIR:SDBUSY");
    return;
  }

  String path = request->arg("path");

  if (path[0] != '/')
  {
    path = "/" + path;
  }
  /* Trying to delete root */
  if (path.length() < 2)
  {
    httpInvalidRequest(request, "RMDIR:BADARGS");
    return;
  }
  else if (!fileSystem.exists(path))
  {
    httpNotFound(request);
    return;
  }
  else
  {
    httpInternalError(request);
    return;
  }

  httpOK(request);
  umountSD();
}

void handleUploadProcess(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
{
  if (!index)
  {
    if (mountSD() != NOERROR) {
      log_i("Upload failed: SDBUSY");
      httpServiceUnavailableJson(request, "{\"error\":\"UPLOAD:SDBUSY\"}");
      return;
    }

    String path;

    if (request->hasParam("path")) {
      path = request->getParam("path")->value();
    } else {
      path = filename;
    }

    if (path == nullptr || path == "") {
      log_i("Upload failed: BADDARGS");
      httpInvalidRequestJson(request, "{\"error\":\"UPLOAD:BADARGS\"}");
      return;
    }

    if (path[0] != '/')
      path = "/" + path;

    if (fileSystem.exists(path)) {
      fileSystem.remove(path);
    }

    request->_tempFile = fileSystem.open(path, FILE_WRITE);

    if (!request->_tempFile) {
      umountSD();
      log_e("Upload: Failed to open filename: %s", path.c_str());
      httpInternalErrorJson(request, "{\"error\":\"Failed to open file\"}");
      return;
    } else {
      if (request->_tempFile.isDirectory()) {
        request->_tempFile.close();
        request->_tempFile = File();
        umountSD();
        log_i("Path is a directory");
        httpNotAllowedJson(request, "{\"error\":\"Path is a directory\"}");
        return;
      }
      log_i("Upload: START, filename: %s", path.c_str());
    }
  }

  if (!request->_tempFile) {
    log_i("Upload failed: !states->dataFile");
    return;
  }

  if (len) {
    request->_tempFile.write(data, len);
  }

  if (final)
  {
    log_i("Upload: END, Size: %d", len);
    request->_tempFile.flush();
    sendFileInfoJson(request, request->_tempFile);
    request->_tempFile.close();
    request->_tempFile = File();
    umountSD();
  }
}

void sendFileInfoJson(AsyncWebServerRequest *request, File file) {
  AsyncResponseStream *response = request->beginResponseStream("application/json");
  char sfn[FF_SFN_BUF + 1];
  get_sfn(sfn, &file);

  response->printf(
    "{\"item\": {\"type\":\"%s\",\"name\":\"%s\",\"size\":%u,\"sfn\":\"%s\",\"last\":%u}}",
    file.isDirectory() ? "dir" : "file",
    file.name(),
    file.size(),
    sfn,
    file.getLastWrite()
  );

  request->send(response);
}

/* */
inline void httpOK(AsyncWebServerRequest *request)
{
  request->send(200, "text/plain");
}

inline void httpOK(AsyncWebServerRequest *request, String msg)
{
  request->send(200, "text/plain", msg + "\r\n");
}

inline void httpOKJson(AsyncWebServerRequest *request, String msg)
{
  request->send(200, "application/json", msg);
}

inline void httpInvalidRequest(AsyncWebServerRequest *request)
{
  request->send(400, "text/plain");
}

inline void httpInvalidRequest(AsyncWebServerRequest *request, String msg)
{
  request->send(400, "text/plain", msg + "\r\n");
}

inline void httpInvalidRequestJson(AsyncWebServerRequest *request, String msg)
{
  request->send(400, "application/json", msg);
}

inline void httpNotFound(AsyncWebServerRequest *request)
{
  request->send(404, "text/plain");
}

inline void httpNotFound(AsyncWebServerRequest *request, String msg)
{
  request->send(404, "text/plain", msg + "\r\n");
}

inline void httpNotFoundJson(AsyncWebServerRequest *request, String msg)
{
  request->send(404, "application/json", msg);
}

inline void httpNotAllowed(AsyncWebServerRequest *request)
{
  request->send(405, "text/plain");
}

inline void httpNotAllowed(AsyncWebServerRequest *request, String msg)
{
  request->send(405, "text/plain", msg + "\r\n");
}

inline void httpNotAllowedJson(AsyncWebServerRequest *request, String msg)
{
  request->send(405, "application/json", msg);
}

inline void httpInternalError(AsyncWebServerRequest *request)
{
  request->send(500, "text/plain");
}

inline void httpInternalError(AsyncWebServerRequest *request, String msg)
{
  request->send(500, "text/plain", msg + "\r\n");
}

inline void httpInternalErrorJson(AsyncWebServerRequest *request, String msg)
{
  request->send(500, "application/json", msg);
}

inline void httpServiceUnavailable(AsyncWebServerRequest *request, String msg)
{
  request->send(503, "text/plain", msg + "\r\n");
}

inline void httpServiceUnavailableJson(AsyncWebServerRequest *request, String msg)
{
  request->send(503, "application/json", msg);
}
