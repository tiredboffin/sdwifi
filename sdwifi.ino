/*
 *  sdwifi: simple server to upload files to Fysetc SD WiFi card.
 *
 *  Based on ESPAsynCWebServer
 *
 */

#include <WiFi.h>
#include <WiFiClient.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <mbedtls/sha1.h>
#include <esp_mac.h>
#if defined __has_include
#if __has_include(<mbedtls/compat-2.x.h>)
#include <mbedtls/compat-2.x.h>
#endif
#endif

static const char *default_name = "sdwifi";

#define SD_SWITCH_PIN GPIO_NUM_26
#define CS_SENSE_PIN GPIO_NUM_33

#define PREF_RW_MODE false
#define PREF_RO_MODE true
#define PREF_NS "wifi"

#define WIFI_STA_TIMEOUT 5000

AsyncWebServer server(80);

Preferences prefs;

static bool esp32_controls_sd;
static bool is_mounted;

void setup(void)
{
  /* Make SD card available to the Host early in the process */
  pinMode(SD_SWITCH_PIN, OUTPUT);
  digitalWrite(SD_SWITCH_PIN, HIGH);
  esp32_controls_sd = false;
  is_mounted = false;

  Serial.setDebugOutput(true);

  prefs.begin(PREF_NS, PREF_RO_MODE);
  /* assume STA mode if sta_ssid is defined */
  if (prefs.isKey("sta_ssid") && setupSTA())
  {
    String hostname = prefs.isKey("hostname") ? prefs.getString("hostname") : default_name;
    if (!MDNS.begin(hostname))
      log_e("Error setting up MDNS responder");
  }
  else
  {
    if (!setupAP())
      while (1)
        delay(1);
  }
  prefs.end();

  /* TODO: rethink the API to simplify scripting  */
  server.onNotFound([](AsyncWebServerRequest *request)
                    { httpNotFound(request, "unknown cmd: " + request->url()); });
  server.on("/info", handleInfo);
  server.on("/config", handleConfig);
  server.on("/exp", handleExperimental);
  /* file ops */
  server.on("/upload", HTTP_POST, handleUpload, handleUploadProcess);
  server.on("/download", HTTP_GET, handleDownload);
  server.on("/sha1", handleSha1);
  server.on("/remove", handleRemove);
  server.on("/list", handleList);
  server.on("/rename", handleRename);
  server.begin();
  log_i("HTTP server started");
}

void loop(void)
{
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
  // Do I have to mount/umount the card every time I lock/unlock it?
  log_i("SD mount %d", is_mounted);

  /* get control over flash NAND */
  if (!esp32_controls_sd)
  {
    digitalWrite(SD_SWITCH_PIN, LOW);
  }
  if (!SD_MMC.begin())
  {
    log_e("Card Mount Failed");
    if (!esp32_controls_sd)
      digitalWrite(SD_SWITCH_PIN, HIGH);
  }
  is_mounted = true;
  return true;
}

/* Unmount SD card */
static void unmountSD(void)
{
  if (!is_mounted)
  {
    log_e("Double unmount: ignore");
    return;
  }
  SD_MMC.end();
  if (!esp32_controls_sd)
  {
    digitalWrite(SD_SWITCH_PIN, HIGH);
  }
  is_mounted = false;
}

static const char *cfgparams[] = {
    "sta_ssid", "sta_password", "ap_ssid", "ap_password", "hostname"};

static bool param_is_valid(const char *n)
{
  for (int i = 0; i < sizeof(cfgparams) / sizeof(cfgparams[0]); i++)
  {
    if (!strcmp(n, cfgparams[i]))
      return true;
  }
  return false;
}

/* CMD: Return some info */
void handleInfo(AsyncWebServerRequest *request)
{
  String txt = "Info\n\n";
  uint8_t tmp[6];

  esp_read_mac(tmp, ESP_MAC_WIFI_STA);
  for (int i = 0; i < sizeof(tmp); i++)
  {
    if (i)
      txt += ':';
    if (tmp[i] < 0x10)
      txt += "0";
    txt += String(tmp[i], 16);
  }
  txt += "\nMAC WifFi AP: ";
  esp_read_mac(tmp, ESP_MAC_WIFI_SOFTAP);
  for (int i = 0; i < sizeof(tmp); i++)
  {
    if (i)
      txt += ':';
    if (tmp[i] < 0x10)
      txt += "0";
    txt += String(tmp[i], 16);
  }
  txt += "\nMAC Bluetooth: ";
  esp_read_mac(tmp, ESP_MAC_BT);
  for (int i = 0; i < sizeof(tmp); i++)
  {
    if (i)
      txt += ':';
    if (tmp[i] < 0x10)
      txt += "0";
    txt += String(tmp[i], 16);
  }
  httpOK(request, txt);
}

/* CMD: Update configuration parameters */
void handleConfig(AsyncWebServerRequest *request)
{

  String txt;

  if (request->args() == 0)
  {
    txt = "Supported configuration parameters\n\n";
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
    if (param_is_valid(n.c_str()))
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
        txt += " Ignored\n";
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
        txt += " Ignored\n";
      }
    }
    else
    {
      txt += " Ignored\n";
    }
  }
  httpOK(request, txt);
}

/* CMD list */
void handleList(AsyncWebServerRequest *request)
{
  if (!request->hasParam("path"))
  {
    httpInvalidRequest(request);
    return;
  }
  String parentDir;
  File root;
  String msg;

  String path = request->getParam("path")->value();
  msg += "Path ";
  msg += path;

  parentDir = String(path);
  parentDir[strrchr(path.c_str(), '/') - path.c_str() + 1] = 0;

  if (!mountSD())
  {
    httpServiceUnavailable(request, "SD Card Mount Failed");
    return;
  }
  if (SD_MMC.exists((char *)path.c_str()))
  {
    msg += "\n\n";
    root = SD_MMC.open(path);
    if (root.isDirectory())
    {
      while (File file = root.openNextFile())
      {
        if (file.isDirectory())
        {
          msg += "<DIR> ";
          msg += file.name();
          msg += "\n";
        }
        else
        {
          msg += file.size();
          msg += " ";
          msg += file.name();
          msg += "\n";
        }
      }
    }
    else
    {
      msg += root.size();
      msg += " ";
      msg += root.name();
      msg += "\n";
    }
    if (root)
      root.close();
    httpOK(request, msg);
  }
  else
    httpNotFound(request, msg + " not found");
  unmountSD();
}

size_t SDFillBuffer(uint8_t *data, size_t len, size_t ofs)
{
  log_i("SD Fill Buffer: %d %d", len, ofs);
  return len;
}

/* CMD Download a file */
void handleDownload(AsyncWebServerRequest *request)
{

  if (!request->hasParam("path"))
  {
    httpInvalidRequest(request);
    return;
  }
  // BUGBUG: should only one (or a limited number of) download request(s) be allowed at a time? Perhaps add a semahore to mountSD?
  if (!mountSD())
  {
    httpServiceUnavailable(request, "SD Card Mount Failed");
    return;
  }
  String path = request->getParam("path")->value();
  String dataType = "application/octet-stream";

  if (SD_MMC.exists((char *)path.c_str()))
  {
    request->_tempFile = SD_MMC.open(path.c_str(), FILE_READ);
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
      log_d("Download request %s %lu bytes", path.c_str(), request->_tempFile.size());
      AsyncWebServerResponse *response = request->beginResponse(dataType, request->_tempFile.size(),
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
          //TODO	    block sd, acquire semaphore?
          if (avaliableBytes > maxLen)
          {
            bytes = request->_tempFile.readBytes((char *)buffer, maxLen);
          }
          else
          {
            bytes = request->_tempFile.readBytes((char *)buffer, avaliableBytes);
            request->_tempFile.close(); //Is it needed or will _tempFile be closed automatically?
            unmountSD();
          }
          //TODO	    unblock sd
          return bytes;
        });
      request->send(response);
      log_i("download response sent");
    }
  }
  else {
    httpNotFound(request);
    unmountSD();
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
    if (n == "path" || n == "from")
      nameFrom = "/" + v;
    else if (n == "to" || n == "newname")
      nameTo = "/" + v;
    else
    {
      httpInvalidRequest(request, "Unrecognized parameter in 'rename' command");
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

  if (!mountSD())
  {
    httpServiceUnavailable(request, "SD Card Mount Failed");
    return;
  }

  if (SD_MMC.exists((char *)nameFrom.c_str()))
  {
    if (SD_MMC.exists((char *)nameTo.c_str()))
    {
      SD_MMC.remove(nameTo.c_str());
    }
    if (SD_MMC.rename(nameFrom.c_str(), nameTo.c_str()))
      httpOK(request, "OK");
    else
      httpServiceUnavailable(request, "Failed to rename");
  }
  else
    httpNotFound(request, nameFrom + " not found");
  unmountSD();
}

/* CMD Remove a file */
void handleRemove(AsyncWebServerRequest *request)
{
  if (!request->hasParam("path"))
  {
    httpInvalidRequest(request);
    return;
  }
  String path = request->getParam("path")->value();

  if (!mountSD())
  {
    httpServiceUnavailable(request, "SD Card Mount Failed");
    return;
  }
  if (SD_MMC.exists((char *)path.c_str()))
  {
    SD_MMC.remove(path.c_str());
    httpOK(request);
  }
  else
    httpNotFound(request);
  unmountSD();
}

/* CMD Return SHA1 of a file */
void handleSha1(AsyncWebServerRequest *request)
{
  if (!request->hasParam("path"))
  {
    httpInvalidRequest(request);
    return;
  }

  String path = request->getParam("path")->value();

  if (!mountSD())
  {
    httpServiceUnavailable(request, "SD Card Mount Failed");
    return;
  }

  if (SD_MMC.exists((char *)path.c_str()))
  {
    File dataFile = SD_MMC.open(path.c_str(), FILE_READ);
    unsigned long fileSize = dataFile.size();

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

    String result;
    {
      uint8_t tmp[20];
      mbedtls_sha1_finish_ret(&ctx, tmp);
      mbedtls_sha1_free(&ctx);

      for (int i = 0; i < sizeof(tmp); i++)
      {
        if (tmp[i] < 0x10)
          result += "0";
        result += String(tmp[i], 16);
      }
    }
    dataFile.close();
    httpOK(request, result);
  }
  else
    httpNotFound(request);

  unmountSD();
}

/* CMD Upload a file */
void handleUpload(AsyncWebServerRequest *request)
{
  log_i("handleUpload()");
  httpOK(request, "Upload");
}

void handleUploadProcess(AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final)
{

  if (!index)
  {
    if (!mountSD())
    {
      /* BUGBUG: how to return the error to the handler */
      httpServiceUnavailable(request, "SD Card Mount Failed");
      return;
    }
    String path;

    if (request->hasParam("path"))
      path = request->getParam("path")->value();
    else
      path = filename;

    if (path[0] != '/')
      path = "/" + path;

    if (SD_MMC.exists((char *)path.c_str()))
      SD_MMC.remove((char *)path.c_str());
    request->_tempFile = SD_MMC.open(path.c_str(), FILE_WRITE);
    ;
    if (!request->_tempFile) {
        unmountSD();
        log_e("Upload: Failed to open filename: %s", path.c_str());
    } else {
        log_i("Upload: START, filename: %s", path.c_str());
    }
  }

  if (len)
  {
    if (request->_tempFile)
      request->_tempFile.write(data, len);
  }
  if (final)
  {
    if (request->_tempFile) {
      request->_tempFile.close();
      unmountSD();
    }
    log_i("Upload: END, Size: %d", len);
  }
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

inline void httpInvalidRequest(AsyncWebServerRequest *request)
{
  request->send(400, "text/plain");
}

inline void httpInvalidRequest(AsyncWebServerRequest *request, String msg)
{
  request->send(400, "text/plain", msg + "\r\n");
}

inline void httpNotFound(AsyncWebServerRequest *request)
{
  request->send(404, "text/plain");
}

inline void httpNotFound(AsyncWebServerRequest *request, String msg)
{
  request->send(404, "text/plain", msg + "\r\n");
}

inline void httpNotAllowed(AsyncWebServerRequest *request, String msg)
{
  request->send(405, "text/plain", msg + "\r\n");
}

inline void httpInternalError(AsyncWebServerRequest *request, String msg)
{
  request->send(500, "text/plain", msg + "\r\n");
}

inline void httpServiceUnavailable(AsyncWebServerRequest *request, String msg)
{
  request->send(503, "text/plain", msg + "\r\n");
}
