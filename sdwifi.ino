/*
 *  sdwifi: simple server to upload files to Fysetc SD WiFi card.
 *
 *  Based on a simple WebServer.
 */

#define PUT_UPLOAD

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>

#include <ESPmDNS.h>
#include <SD_MMC.h>
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

#define SD_SWITCH_PIN GPIO_NUM_26
#define CS_SENSE_PIN GPIO_NUM_33

#define PREF_RW_MODE false
#define PREF_RO_MODE true
#define PREF_NS "wifi"

#define WIFI_STA_TIMEOUT 5000

WebServer server(80);

Preferences prefs;
File dataFile;
fs::FS &fileSystem = SD_MMC;

static bool esp32_controls_sd = false;
static bool is_mounted = false;

void setup(void)
{
  /* Make SD card available to the Host early in the process */
  pinMode(SD_SWITCH_PIN, OUTPUT);
  digitalWrite(SD_SWITCH_PIN, HIGH);
  Serial.setDebugOutput(true);

  setupWiFi();
  SPIFFS.begin();
  setupWebServer();
  server.begin();
}

void loop(void)
{
  /* handle one client at a time */
  server.handleClient();
  delay(2);
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
  /* TODO: rethink the API to simplify scripting  */
  server.on("/ping", []()
            { httpOK(); });
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

  /* Testing: For compatibility with original Fysetc web app code */
  server.on("/relinquish", HTTP_GET, []()
            { httpOK(); });
  server.on("/wificonnect", HTTP_POST, handleWiFiConnect);
  server.on("/wifiap", HTTP_POST, handleWiFiAP);
  server.on("/delete", handleRemove);

  /* Static content */
  server.serveStatic("/", SPIFFS, "/");
  server.onNotFound([]()
                    { httpNotFound(); });

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

/* Mount SD card */
static bool mountSD(void)
{
  if (is_mounted)
  {
    log_e("Double mount: ignore");
    return true;
  }

  log_i("SD Card mount");

  /* get control over flash NAND */
  if (!esp32_controls_sd)
  {
    digitalWrite(SD_SWITCH_PIN, LOW);
  }
  if (!SD_MMC.begin())
  {
    log_e("SD Card Mount Failed");
    if (!esp32_controls_sd)
      digitalWrite(SD_SWITCH_PIN, HIGH);
  }
  is_mounted = true;
  return true;
}

/* Unmount SD card */
static void umountSD(void)
{
  if (!is_mounted)
  {
    log_e("Double Unmount: ignore");
    return;
  }
  SD_MMC.end();
  if (!esp32_controls_sd)
  {
    digitalWrite(SD_SWITCH_PIN, HIGH);
  }
  is_mounted = false;
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
void handleInfo(void)
{
  String txt;

  uint8_t tmp[6];
  mountSD();
  txt = "{\"info\":{\"filesystem\":{";
  txt += "\"cardsize\":";
  txt += SD_MMC.cardSize();
  txt += ", \"totalbytes\":";
  txt += SD_MMC.totalBytes();
  txt += ",\"usedbytes\":";
  txt += SD_MMC.usedBytes();
  txt += "},";
  umountSD();

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
        digitalWrite(SD_SWITCH_PIN, LOW);
        esp32_controls_sd = true;
        txt += " SD controlled by ESP32\n";
      }
      else if (v == "host" || v == "high")
      {
        digitalWrite(SD_SWITCH_PIN, HIGH);
        esp32_controls_sd = false;
        txt += " SD controlled by Host\n";
      }
      else
      {
        txt += " Ignored\n";
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
        txt += " Ignored\n";
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
        txt += " Ignored\n";
      }
    }
    else
    {
      txt += " Ignored\n";
    }
  }
  httpOK(txt);
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
    httpInvalidRequest();
    return;
  }

  String parentDir;
  File root;

  if (path[0] != '/')
    path = "/" + path;

  String txt;

  parentDir = String(path);
  parentDir[strrchr(path.c_str(), '/') - path.c_str() + 1] = 0;

  if (!mountSD())
  {
    httpServiceUnavailable("LIST:SDBUSY");
    return;
  }

  if (fileSystem.exists((char *)path.c_str()))
  {
    root = fileSystem.open(path);
    if (root.isDirectory())
    {
      int count = 0;
      txt = "[";
      while (File file = root.openNextFile())
      {
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
        txt += "\",";
        txt += "\"size\":";
        txt += file.size();
        txt += "}";
      }
      txt += "]";
    }
    else
    {
      txt = "{\"item\": {\"type\":\"file\",";
      txt += "\"name\":\"";
      txt += root.name();
      txt += "\"size\":\"";
      txt += root.size();
      txt += "\"}}";
    }
    if (root)
      root.close();
    server.send(200, "application/json", txt);
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
    httpInvalidRequest();
    return;
  }
  String path = server.arg("path");

  if (path[0] != '/')
    path = "/" + path;

  if (!mountSD())
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
      httpInvalidRequest("Unrecognized parameter in 'rename' command");
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

  if (!mountSD())
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

  if (!mountSD())
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
    httpInvalidRequest();
    return;
  }

  if (!mountSD())
  {
    httpServiceUnavailable("SHA1:SDBUSY");
    return;
  }

  String path = server.arg("path");
  if (path[0] != '/')
    path = "/" + path;

  if (fileSystem.exists((char *)path.c_str()))
  {
    dataFile = fileSystem.open(path.c_str(), FILE_READ);
    if (!dataFile.isDirectory())
    {
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
    }
    else
    {
      httpNotAllowed("Path is a directory");
    }
    dataFile.close();
  }
  else
  {
    httpNotFound();
  }
  umountSD();
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
    httpInvalidRequest();
    return;
  };

  String path = server.pathArg(0);

  if (path == nullptr || path == "")
  {
    httpInvalidRequest();
    return;
  }

  if (path[0] != '/')
    path = "/" + path;

  if (reqState.status == RAW_START)
  {
    if (!mountSD())
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

void handleUploadProcess()
{

  HTTPUpload &reqState = server.upload();
  if (&reqState == nullptr)
  {
    httpInvalidRequest();
    return;
  };

  String path = (server.hasArg("path")) ? server.arg("path") : reqState.filename;

  if (path == nullptr || path == "")
  {
    httpInvalidRequest();
    return;
  }

  if (path[0] != '/')
    path = "/" + path;

  if (reqState.status == UPLOAD_FILE_START)
  {
    if (!mountSD())
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
