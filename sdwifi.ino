/*
 *  sdwifi: simple server to upload files to Fysetc SD WiFi card.
 *
 *  Based on a simple WebServer.
 */

#define PUT_UPLOAD

#define MIN_MEM_THRESHOLD 32768

//#define USE_SD 

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>

#include <ESPmDNS.h>
#ifdef USE_SD
#include <SPI.h>
#include <SD.h>
#define SD_CS_PIN		  13
#define SD_MISO_PIN		 2
#define SD_MOSI_PIN		15
#define SD_SCLK_PIN		14
#else
  #include <SD_MMC.h>
#endif
#include <Preferences.h>
#include <mbedtls/sha1.h>
#include <esp_mac.h>
#include <SPIFFS.h>

#if defined __has_include
#if __has_include(<mbedtls/compat-2.x.h>)
#include <mbedtls/compat-2.x.h>
#endif
#endif

#ifdef PUT_UPLOAD
#include <uri/UriRegex.h>
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

#define WIFI_STA_TIMEOUT 5000

#define HOST_ACTIVITY_GRACE_PERIOD_MILLIS 2000

enum
{
  MOUNT_OK,
  MOUNT_BUSY,
  MOUNT_FAILED
};

WebServer server(80);

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
  bool mount_is_safe = false;
  bool activity_detected = false;
  bool host_activity_detected = false;
  unsigned isr_counter = 0;
  unsigned long host_last_activity_millis;
} sd_state;

static bool esp32_controls_sd = false;
static bool fs_is_mounted = false;

void IRAM_ATTR sd_isr(void);

void debug_meminfo(char *txt, unsigned count) {
    log_e("%s, %u, %u, %u", txt, count, heap_caps_get_free_size(MALLOC_CAP_DEFAULT), heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
}

void setup(void)
{
  sd_state.host_last_activity_millis = millis();

  pinMode(SD_SWITCH_PIN, OUTPUT);
  #ifdef USE_SD
  pinMode(SD_POWER_PIN, OUTPUT);
  #endif
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

  /* handle one client at a time */
  server.handleClient();
  
  /* warn and reboot on low memory */
  if (!mem_warning && heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT) < MIN_MEM_THRESHOLD*2)
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
  server.enableCORS();
  /* TODO: rethink the API to simplify scripting  */
  server.on("/ping", []()
            { httpOK("pong\r\n"); });
  server.on("/sysinfo", handleInfo);
  server.on("/config", handleConfig);
  server.on("/exp", handleExperimental);
  /* file ops */
  server.on("/upload", HTTP_POST, handleUpload, handleUploadProcess);
#ifdef PUT_UPLOAD
  server.on(UriRegex("/upload/(.*)"), HTTP_PUT, handleUpload, handleUploadProcessPUT);
#endif
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/sha1", handleSha1);
  server.on("/remove", handleRemove);
  server.on("/list", handleList);
  server.on("/rename", handleRename);
  server.on("/mkdir", handleMkdir);
  server.on("/rmdir", handleRmdir);

  /* Testing: For compatibility with original Fysetc web app code */
  server.on("/relinquish", HTTP_GET, []()
            { httpOK(); });
  server.on("/wificonnect", HTTP_POST, handleWiFiConnect);
  server.on("/wifiap", HTTP_POST, handleWiFiAP);
  server.on("/delete", handleRemove);

  /* Static content */
  server.serveStatic("/", SPIFFS, "/");
  server.onNotFound([]() {
    switch(server.method()) {
      case HTTP_OPTIONS: httpOK(); break;
      default: httpNotFound(); break;
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
#ifdef USE_SD
    SPI.begin(SD_SCLK_PIN,SD_MISO_PIN,SD_MOSI_PIN,SD_CS_PIN);
#endif
  }
}

static inline void sd_unlock(void)
{
  if (!esp32_controls_sd)
  {
 #ifdef USE_SD
    #define SD_D0_PIN		   2
    #define SD_D1_PIN		   4
    #define SD_D2_PIN		  12
    #define SD_D3_PIN		  13
    #define SD_CLK_PIN	  14
    #define SD_CMD_PIN    15
    pinMode(SD_D0_PIN,  INPUT_PULLUP);
    pinMode(SD_D1_PIN,  INPUT_PULLUP);
    pinMode(SD_D2_PIN,  INPUT_PULLUP);
    pinMode(SD_D3_PIN,  INPUT_PULLUP);
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
  if (fs_is_mounted)
  {
    log_e("Double mount: ignore");
    return MOUNT_OK;
  }

  log_i("SD Card mount: %u", mount_counter);

  ++mount_counter;

  if (!sd_state.mount_is_safe)
  {
    log_i("SD Card mount: card is busy");
    return MOUNT_BUSY;
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
    log_i("SD Card mount: %u", mount_counter);
    return MOUNT_FAILED;
  }
  fs_is_mounted = true;
  return MOUNT_OK;
}

/* Unmount SD card */
static void umountSD(void)
{
  if (!fs_is_mounted)
  {
    log_e("Double Unmount: ignore");
    return;
  }
#ifdef USE_SD
  SD.end();
#else
  SD_MMC.end();
#endif
  sd_unlock();
  log_i("In SD Card Unmount: %u", umount_counter);

  ++umount_counter;
  fs_is_mounted = false;
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
void handleInfo(void)
{

  String txt;

  uint8_t tmp[6];

  txt = "{\"info\":{\"filesystem\":{";
  if (server.hasArg("sd") && server.arg("sd") == "none") {
      txt += "\"status\":\"info disabled\"";
  } else {
    switch (mountSD())
    {
    case MOUNT_OK:
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
    case MOUNT_BUSY:
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
  txt +=  "\"esp-idf\":\"";
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
  server.send(200, "application/json", txt);
}


/* CMD: Update configuration parameters */
void handleConfig(void)
{

  String txt;
  if (server.args() == 0)
  {
    txt = "Configuration parameters:\n\n";
    for (int i = 0; i < sizeof(cfgparams) / sizeof(cfgparams[0]); i++)
    {
      txt += cfgparams[i];
      txt += "\n";
    }
    httpOK(txt);
  }

  prefs.begin(PREF_NS, PREF_RW_MODE);
  for (int i = 0; i < server.args(); i++)
  {
    String n = server.argName(i);
    String v = server.arg(i);
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
  httpOK(txt);
}

/* CMD: wificonnect: compatibility with original Fysetc web app */
void handleWiFiConnect(void)
{

  String txt;

  prefs.begin(PREF_NS, PREF_RW_MODE);
  for (int i = 0; i < server.args(); i++)
  {
    String n = "sta_" + server.argName(i);
    String v = server.arg(i);
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
  httpOK();
  delay(50);
  ESP.restart();
}

/* CMD: wificonnect: compatibility with original Fysetc web app */
void handleWiFiAP(void)
{
  prefs.begin(PREF_NS, PREF_RW_MODE);
  prefs.remove("sta_ssid");
  prefs.remove("sta_password");
  prefs.end();
  httpOK();
  delay(50);
  ESP.restart();
}

void handleExperimental(void)
{
  String txt;

  for (int i = 0; i < server.args(); i++)
  {
    String n = server.argName(i);
    String v = server.arg(i);
    txt += n + "=" + v;
    /* enforce IO26 pin value */
    if (n == "io26")
    {
      if (v == "esp32" || v == "low")
      {
        if (!esp32_controls_sd) {
          sd_lock();
          esp32_controls_sd = true;
        }
        txt += " SD controlled by ESP32";
      }
      else if (v == "host" || v == "high")
      {
        if (esp32_controls_sd) {
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
        httpOK(txt);
        delay(50);
        ESP.restart();
      }
      else if (v == "off" || v == "shutdown")
      {
        httpOK(txt);
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
        httpOK(txt);
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
  httpOK(txt);
}

#include "ff.h"
void get_sfn(char *out_sfn, File *file)
{
#if FF_USE_LFN
  FILINFO info;
  f_stat(file->path(), &info);
  strncpy(out_sfn, info.altname, FF_SFN_BUF + 1);
#else /* FF_USE_LFN */
  strncpy(out_sfn, file->name().c_str(), FF_SFN_BUF + 1);
#endif /* FF_USE_LFN */
}

/* CMD list */
void handleList()
{

  String path;

  if (server.hasArg("path"))
  {
    path = server.arg("path");
  }
  else if (server.hasArg("dir"))
  {
    path = server.arg("dir");
  }
  else
  {
    httpInvalidRequest("LIST:BADARGS");
    return;
  }

  File root;

  if (path[0] != '/')
    path = "/" + path;

  String txt;

  if (mountSD() != MOUNT_OK)
  {
    httpServiceUnavailable("LIST:SDBUSY");
    return;
  }

  if (fileSystem.exists((char *)path.c_str()))
  {
    root = fileSystem.open(path);

    // Chunked mode
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "application/json", "");

    char sfn[FF_SFN_BUF + 1];
    if (root.isDirectory())
    {
      int count = 0;
      txt = "[";
      while (File file = root.openNextFile())
      {
        get_sfn(sfn, &file);

        if (count++)
        {
          txt += ",";
        }
        txt += "{\"id\":";
        txt += count;
        txt += ",\"type\":";
        if (file.isDirectory())
        {
          txt += "\"dir\",";
        }
        else
        {
          txt += "\"file\",";
        }
        txt += "\"name\":\"";
        txt += file.name();
        txt += "\",\"size\":";
        txt += file.size();
        txt += ",\"sfn\":\"";
        txt += sfn;
        txt += "\"}";

        if (txt.length() > 1024)
        {
          server.sendContent(txt);
          txt = "";
        }
      }
      txt += "]";
    }
    else
    {
      get_sfn(sfn, &root);

      txt = "{\"item\": {\"type\":\"file\",";
      txt += "\"name\":\"";
      txt += root.name();
      txt += "\",\"size\":";
      txt += root.size();
      txt += ",\"sfn\":\"";
      txt += sfn;
      txt += "\"}}";
    }

    if (root)
      root.close();

    server.sendContent(txt);
  }
  else
  {
    httpNotFound();
  }
  umountSD();
}

/* CMD Download a file */
void handleDownload(void)
{

  if (!server.hasArg("path"))
  {
    httpInvalidRequest("DOWNLOAD:BADARGS");
    return;
  }
  String path = server.arg("path");

  if (path[0] != '/')
    path = "/" + path;

  if (mountSD() != MOUNT_OK)
  {
    httpServiceUnavailable("DOWNLOAD:SDBUSY");
    return;
  }

  if (fileSystem.exists((char *)path.c_str()))
  {
    dataFile = fileSystem.open(path.c_str(), FILE_READ);
    if (!dataFile)
    {
      httpServiceUnavailable("Failed to open file");
    }
    else if (dataFile.isDirectory())
    {
      dataFile.close();
      httpNotAllowed("Path is a directory");
    }
    else
    {
      server.sendHeader("Content-Disposition", "attachment; filename=\"" + String(dataFile.name()) + "\"");
      unsigned long sentSize = server.streamFile(dataFile, "application/octet-stream");
      if (sentSize != dataFile.size())
        log_e("Sent less data %ul than expected %ul", sentSize, dataFile.size());
      dataFile.close();
      httpOK("Sent less data than expected");
    }
  }
  else
  {
    httpNotFound("DOWNLOAD:FileNotFound");
  }
  umountSD();
}

/* CMD Rename a file */
void handleRename()
{
  String nameFrom;
  String nameTo = "/";

  for (int i = 0; i < server.args(); i++)
  {
    String n = server.argName(i);
    String v = server.arg(i);
    if (n == "from")
      nameFrom = "/" + v;
    else if (n == "to")
      nameTo = "/" + v;
    else
    {
      httpInvalidRequest("RENAME:BADARGS");
      return;
    }
  }
  if (!nameFrom || !nameTo)
  {
    httpInvalidRequest("Both 'to' and 'from' has to be specified in 'rename' command");
    return;
  }
  if (nameFrom == nameTo)
  {
    httpInvalidRequest("'to' must not be equal to 'from'");
    return;
  }

  if (mountSD() != MOUNT_OK)
  {
    httpServiceUnavailable("RENAME:SDBUSY");
    return;
  }
  if (fileSystem.exists((char *)nameFrom.c_str()))
  {
    if (fileSystem.exists((char *)nameTo.c_str()))
    {
      fileSystem.remove(nameTo.c_str());
    }
    if (fileSystem.rename(nameFrom.c_str(), nameTo.c_str()))
      httpOK("OK");
    else
      httpServiceUnavailable("Failed to rename");
  }
  else
    httpNotFound(nameFrom + " not found");
  umountSD();
}

/* CMD Remove a file */
void handleRemove()
{
  if (!server.hasArg("path"))
  {
    httpInvalidRequest("DELETE:BADARGS");
    return;
  }

  if (mountSD() != MOUNT_OK)
  {
    httpServiceUnavailable("DELETE:SDBUSY");
    return;
  }

  String path = server.arg("path");

  if (path[0] != '/')
    path = "/" + path;

  if (fileSystem.exists((char *)path.c_str()))
  {
    fileSystem.remove(path.c_str());
    httpOK();
  }
  else
    httpNotFound();

  umountSD();
}

/* CMD Return SHA1 of a file */
void handleSha1()
{

  if (!server.hasArg("path"))
  {
    httpInvalidRequest("SHA1:BADARGS");
    return;
  }

  if (mountSD() != MOUNT_OK)
  {
    httpServiceUnavailable("SHA1:SDBUSY");
    return;
  }

  String path = server.arg("path");
  if (path[0] != '/')
    path = "/" + path;

  if (!fileSystem.exists((char *)path.c_str()) || !(dataFile = fileSystem.open(path.c_str(), FILE_READ)))
  {
    umountSD();
    httpNotFound();
    return;
  };
  if (dataFile.isDirectory())
  {
    dataFile.close();
    umountSD();
    httpNotAllowed();
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
  server.send(200, "application/json", result);
  return;
}

/* CMD Upload a file */
void handleUpload()
{
  httpOK("");
}

#ifdef PUT_UPLOAD
void handleUploadProcessPUT()
{
  HTTPRaw &reqState = server.raw();
  if (&reqState == nullptr)
  {
    httpInvalidRequest("UPLOAD:BADARGS");
    return;
  };

  String path = server.pathArg(0);

  if (path == nullptr || path == "")
  {
    httpInvalidRequest("UPLOAD:BADARGS");
    ;
    return;
  }

  if (path[0] != '/')
    path = "/" + path;

  if (reqState.status == RAW_START)
  {
    if (mountSD() != MOUNT_OK)
    {
      httpServiceUnavailable("UPLOAD:SDBUSY");
      return;
    }
    if (fileSystem.exists((char *)path.c_str()))
      fileSystem.remove((char *)path.c_str()); // should fail if the path is a directory

    dataFile = fileSystem.open(path.c_str(), FILE_WRITE);
    if (!dataFile)
    {
      httpServiceUnavailable("File open failed");
      umountSD();
      return;
    }
    if (dataFile.isDirectory())
    {
      dataFile.close();
      httpNotAllowed("Path is a directory");
      umountSD();
      return;
    }
    log_v("Upload: START, filename: %s", path.c_str());
    return;
  }

  if (!dataFile)
    return;

  switch (reqState.status)
  {
  case RAW_WRITE:
    dataFile.write(reqState.buf, reqState.currentSize);
    break;
  case RAW_END:
    dataFile.close();
    umountSD();
    log_v("Upload PUT: END, Size: %d", reqState.totalSize);
    break;
  case RAW_ABORTED:
    // BUGBUG: is it safe to remove open file?
    fileSystem.remove(dataFile.path());
    dataFile.close();
    umountSD();
    log_v("Upload PUT: file aborted");
    break;
  default:
    log_w("Upload PUT: raw status %d", reqState.status);
  }
}
#endif

void handleMkdir()
{
  if (!server.hasArg("path"))
  {
    httpInvalidRequest("MKDIR:BADARGS");
    return;
  }

  if (mountSD() != MOUNT_OK)
  {
    httpServiceUnavailable("MKDIR:SDBUSY");
    return;
  }

  String path = server.arg("path");

  if (path[0] != '/')
  {
    path = "/" + path;
  }

  if (fileSystem.exists(path) || fileSystem.mkdir(path))
  {
    httpOK();
  }
  else
  {
    httpNotFound();
  }

  umountSD();
}

void handleRmdir()
{
  if (!server.hasArg("path"))
  {
    httpInvalidRequest("RMDIR:BADARGS");
    return;
  }

  if (mountSD() != MOUNT_OK)
  {
    httpServiceUnavailable("RMDIR:SDBUSY");
    return;
  }

  String path = server.arg("path");

  if (path[0] != '/') {
    path = "/" + path;
  }
  /* Trying to delete root */
  if (path.length() < 2) {
     httpInvalidRequest("RMDIR:BADARGS");
  } else if (!fileSystem.exists(path)) {
    httpNotFound();
  } else {
    if (!fileSystem.rmdir(path)) {
      httpInternalError();
    } else {
      httpOK();
    }
  }
  umountSD();
}

void handleUploadProcess()
{

  HTTPUpload &reqState = server.upload();
  if (&reqState == nullptr)
  {
    httpInvalidRequest("UPLOAD:BADARGS");
    return;
  };

  String path = (server.hasArg("path")) ? server.arg("path") : reqState.filename;

  if (path == nullptr || path == "")
  {
    httpInvalidRequest("UPLOAD:BADARGS");
    return;
  }

  if (path[0] != '/')
    path = "/" + path;

  if (reqState.status == UPLOAD_FILE_START)
  {
    if (mountSD() != MOUNT_OK)
    {

      httpServiceUnavailable("UPLOAD:SDBUSY");
      return;
    }
    if (fileSystem.exists((char *)path.c_str()))
      fileSystem.remove((char *)path.c_str()); // should fail if the path is a directory

    dataFile = fileSystem.open(path.c_str(), FILE_WRITE);
    if (!dataFile)
    {
      httpServiceUnavailable("File open failed");
      umountSD();
      return;
    }
    if (dataFile.isDirectory())
    {
      dataFile.close();
      httpNotAllowed("Path is a directory");
      umountSD();
      return;
    }
    log_v("Upload: START, filename: %s", path.c_str());
    return;
  }

  if (!dataFile)
    return;

  switch (reqState.status)
  {
  case UPLOAD_FILE_WRITE:
    dataFile.write(reqState.buf, reqState.currentSize);
    break;
  case UPLOAD_FILE_END:
    dataFile.close();
    umountSD();
    log_v("Upload POST: END, Size: %d", reqState.totalSize);
    break;
  case UPLOAD_FILE_ABORTED:
    // BUGBUG: is it safe to remove the incomplete file that is stil open?
    fileSystem.remove(dataFile.path());
    dataFile.close();
    umountSD();
    log_v("Upload POST: file aborted");
    break;
  default:
    log_w("Upload POST: unknown update status %d", reqState.status);
  }
}

/* */
inline void httpOK(void)
{
  server.send(200, "text/plain");
}

inline void httpOK(String msg)
{
  server.send(200, "text/plain", msg + "\r\n");
}

inline void httpInvalidRequest(void)
{
  server.send(400, "text/plain");
}

inline void httpInvalidRequest(String msg)
{
  server.send(400, "text/plain", msg + "\r\n");
}

inline void httpNotFound(void)
{
  server.send(404, "text/plain");
}

inline void httpNotFound(String msg)
{
  server.send(404, "text/plain", msg + "\r\n");
}

inline void httpNotAllowed()
{
  server.send(405, "text/plain");
}

inline void httpNotAllowed(String msg)
{
  server.send(405, "text/plain", msg + "\r\n");
}

inline void httpInternalError()
{
  server.send(500, "text/plain");
}

inline void httpInternalError(String msg)
{
  server.send(500, "text/plain", msg + "\r\n");
}

inline void httpServiceUnavailable(String msg)
{
  server.send(503, "text/plain", msg + "\r\n");
}
