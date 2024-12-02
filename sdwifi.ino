/*
 *  sdwifi: simple server to upload files to Fysetc SD WiFi card.
 *
 *  Based on a simple WebServer.
 */

#define USE_NTP

#ifdef ASYNCWEBSERVER_REGEX
# define PUT_UPLOAD
#endif

#include <WiFi.h>
#include <WiFiClient.h>

#include <ESPAsyncWebServer.h>

#include <ESPmDNS.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <mbedtls/sha1.h>
#include <esp_mac.h>
#include <SPIFFS.h>
#include <time.h>

#if defined __has_include
# if __has_include(<mbedtls/compat-2.x.h>)
#  include <mbedtls/compat-2.x.h>
# endif
#endif

#ifdef USE_NTP
# ifdef CONFIG_LWIP_TCPIP_CORE_LOCKING
#  include "lwip/tcpip.h" // for UN/LOCK_TCPIP_CORE()
# endif /* CONFIG_LWIP_TCPIP_CORE_LOCKING */

static const char* ntpServer = "pool.ntp.org";
static const long  gmtOffset_sec = 0;
static const int   daylightOffset_sec = 3600;

#endif /* USE_NTP */

static const char *default_name = "sdwifi";

#define SD_SWITCH_PIN GPIO_NUM_26
#define SD_POWER_PIN GPIO_NUM_27
#define CS_SENSE_PIN GPIO_NUM_33

#define PREF_RW_MODE false
#define PREF_RO_MODE true
#define PREF_NS "wifi"

#define WIFI_STA_TIMEOUT 5000

#define HOST_ACTIVITY_GRACE_PERIOD_MILLIS 2000

enum
{
  MOUNT_OK,
  MOUNT_BUSY,
  MOUNT_FAILED
};

AsyncWebServer server(80);

Preferences prefs;
fs::FS &fileSystem = SD_MMC;

volatile struct
{
  bool mount_is_safe = false;
  bool activity_detected = false;
  bool host_activity_detected = false;
  unsigned isr_counter = 0;
  unsigned long host_last_activity_millis;
} sd_state;

static bool esp32_controls_sd = false;
static int fs_is_mounted = 0;

void IRAM_ATTR sd_isr(void);

void setup(void)
{
  sd_state.host_last_activity_millis = millis();

  pinMode(SD_SWITCH_PIN, OUTPUT);
  attachInterrupt(CS_SENSE_PIN, sd_isr, CHANGE);

  /* Make SD card available to the Host early in the process */
  digitalWrite(SD_SWITCH_PIN, HIGH);
  Serial.setDebugOutput(true);

  setupWiFi();
  SPIFFS.begin();
  setupWebServer();
  server.begin();

#ifdef USE_NTP
  // To use NTP, we have to lock ourselves the TCP core while operating
  // https://github.com/espressif/arduino-esp32/issues/10526#issuecomment-2439483380
# ifdef CONFIG_LWIP_TCPIP_CORE_LOCKING
  if (!sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER))
      LOCK_TCPIP_CORE();
# endif /* CONFIG_LWIP_TCPIP_CORE_LOCKING */
  // Send an NTP request to configure local time
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
# ifdef CONFIG_LWIP_TCPIP_CORE_LOCKING
  if (sys_thread_tcpip(LWIP_CORE_LOCK_QUERY_HOLDER))
      UNLOCK_TCPIP_CORE();
# endif /* CONFIG_LWIP_TCPIP_CORE_LOCKING */
#endif /* USE_NTP */
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
}

void setupWiFi()
{
  prefs.begin(PREF_NS, PREF_RO_MODE);
  /* assume STA mode if sta_ssid is defined */
  if (prefs.isKey("sta_ssid") && setupSTA())
  {
    String hostname = prefs.isKey("hostname") ? prefs.getString("hostname") : default_name;
    if (!MDNS.begin(hostname))
    {
      log_e("Error setting up MDNS responder");
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
  String password = prefs.getString("ap_password");

  WiFi.mode(WIFI_AP);

  if (!WiFi.softAP(ssid, password))
  {
    log_e("Fallback to unprotected AP %s", ssid.c_str()); // typically happens when the password is too short
    delay(100);
    if (!WiFi.softAP(ssid))
    {
      log_e("Fallback to default AP name %s", default_name);
      delay(100);
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
  String password = prefs.getString("sta_password");
  int i = 0;

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);

  WiFi.waitForConnectResult(WIFI_STA_TIMEOUT);
  if (WiFi.status() == WL_CONNECTED)
  {
    log_i("Connected to %s with IP address: %s", ssid, WiFi.localIP().toString());
    return true;
  }
  else
  {
    log_e("Connection to %s failed with status %d after %d attempts", ssid, WiFi.status(), i);
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
  }
}

static inline void sd_unlock(void)
{
  if (!esp32_controls_sd)
  {
    digitalWrite(SD_SWITCH_PIN, HIGH);
  }
}

/* Mount SD card */
static int mountSD(void)
{
  if (fs_is_mounted > 0)
  {
    ++fs_is_mounted;
    log_e("Double mount: ignore");
    return MOUNT_OK;
  }

  log_i("SD Card mount");

  if (!sd_state.mount_is_safe)
  {
    log_i("SD Card mount: card is busy");
    return MOUNT_BUSY;
  }

  /* get control over flash NAND */
  sd_lock();
  if (!SD_MMC.begin())
  {
    log_e("SD Card Mount Failed");
    sd_unlock();
    return MOUNT_FAILED;
  }

  ++fs_is_mounted;
  return MOUNT_OK;
}

/* Unmount SD card */
static void umountSD(void)
{
  if (fs_is_mounted > 1)
  {
    log_e("Double Unmount: ignore");
    --fs_is_mounted;
    return;
  }
  SD_MMC.end();
  sd_unlock();
  --fs_is_mounted;
  log_i("In SD Card Unmount");
}

static const char *cfgparams[] = {
    "sta_ssid", "sta_password", "ap_ssid", "ap_password", "hostname"};

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

/* CMD: Return some info */
void handleInfo(AsyncWebServerRequest *request)
{
  String txt;

  uint8_t tmp[6];
  txt = "{\"info\":{\"filesystem\":{";
  switch (mountSD())
  {
  case MOUNT_OK:
    txt += "\"status\":\"free\",";
    txt += "\"cardsize\":";
    txt += SD_MMC.cardSize();
    txt += ",\"totalbytes\":";
    txt += SD_MMC.totalBytes();
    txt += ",\"usedbytes\":";
    txt += SD_MMC.usedBytes();

    umountSD();
    break;
  case MOUNT_BUSY:
    txt += "\"status\":\"busy\"";
    break;
  default:
    txt += "\"status\":\"failed\"";
    break;
  }
  txt += "},";
  txt += "\"isr\":{";
  txt += "\"count\":";
  txt += sd_state.isr_counter;
  txt += ",";
  txt += "\"activity_millis_ago\":";
  txt += millis() - sd_state.host_last_activity_millis;
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
  txt += "\"network\":{";
  txt += "\"SSID\":\"";
  txt += WiFi.SSID();
  txt += "\",";
  txt += "\"WifiStatus\":\"";
  txt += WiFi.status();
  txt += "\",";
  txt += "\"Wifi Strength\":\"";
  txt += WiFi.RSSI();
  txt += " dBm\",";
  txt += "\"IP\":\"";
  txt += WiFi.localIP().toString();
  txt += "/";
  txt += WiFi.subnetMask().toString();
  txt += "\",";
  txt += "\"Gateway\":\"";
  txt += WiFi.gatewayIP().toString();
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
    txt = "Configuration parameters:\n\n";
    for (int i = 0; i < sizeof(cfgparams) / sizeof(cfgparams[0]); i++)
    {
      txt += cfgparams[i];
      txt += "\n";
    }
    httpOK(request, txt);
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
        digitalWrite(SD_SWITCH_PIN, LOW);
        esp32_controls_sd = true;
        txt += " SD controlled by ESP32";
      }
      else if (v == "host" || v == "high")
      {
        digitalWrite(SD_SWITCH_PIN, HIGH);
        esp32_controls_sd = false;
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
  strncpy(out_sfn, info.altname, FF_SFN_BUF + 1);
  if (strlen(out_sfn) == 0)
#endif /* FF_USE_LFN */
  {
    strncpy(out_sfn, file->name(), FF_SFN_BUF + 1);
  }
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

  if (mountSD() != MOUNT_OK) {
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

  String path = request->getParam("path")->value();

  if (path[0] != '/')
    path = "/" + path;

  if (mountSD() != MOUNT_OK)
  {
    httpServiceUnavailable(request, "DOWNLOAD:SDBUSY");
    return;
  }

  if (fileSystem.exists(path))
  {
    request->_tempFile = fileSystem.open(path, FILE_READ);
    if (!request->_tempFile)
    {
      httpServiceUnavailable(request, "Failed to open file");
    }
    else if (request->_tempFile.isDirectory())
    {
      request->_tempFile.close();
      httpNotAllowed(request, "Path is a directory");
    }
    else
    {
      log_i("Download request %s %lu bytes", path, request->_tempFile.size());
      AsyncWebServerResponse *response = request->beginResponse("application/octet-stream", request->_tempFile.size(),
        [request](uint8_t *buffer, size_t maxLen, size_t index) -> size_t
        {
          uint32_t readBytes;
          uint32_t bytes = 0;
          if (!request->_tempFile)
          {
            log_e("request->_tempFile is NULL in callback");
            return 0;
          }
          uint32_t avaliableBytes = request->_tempFile.available();
          if (avaliableBytes > maxLen)
          {
            bytes = request->_tempFile.readBytes((char *)buffer, maxLen);
          }
          else
          {
            bytes = request->_tempFile.readBytes((char *)buffer, avaliableBytes);
            request->_tempFile.close();
            umountSD();
          }
          return bytes;
        });
      request->send(response);
      log_i("download response sent");
    }
  }
  else
  {
    httpNotFound(request, "DOWNLOAD:FileNotFound");
    umountSD();
  }
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

  if (mountSD() != MOUNT_OK)
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

  if (mountSD() != MOUNT_OK)
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

  if (mountSD() != MOUNT_OK)
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

    if (mountSD() != MOUNT_OK) {
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

  String path = request->arg("path");

  if (path[0] != '/') {
    path = "/" + path;
  }

  if (mountSD() != MOUNT_OK) {
    httpServiceUnavailableJson(request, "{\"error\":\"MKDIR:SDBUSY\"}");
    return;
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

  if (mountSD() != MOUNT_OK)
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

  if (!fileSystem.exists(path))
  {
    httpNotFound(request);
    return;
  }

  if (!fileSystem.rmdir(path))
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
    if (mountSD() != MOUNT_OK) {
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
