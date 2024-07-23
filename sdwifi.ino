/*
 *  sdwifi: simple server to upload files to Fysetc SD WiFi card.
 *
 *  Description
 *
 *  The primary use case is the research project on camera firmwares however it may prove to be useful in other similar simple
 *  scenarios (like 3D printing) when it could be reasonably guaranteed by some means that the SD card is not used actively by
 *  the host (camera, printer etc) at the time this server is accessing the card or if the host is capable to recover gracefully
 *  from SD card access errors.
 *
 *  Usage
 *
 *  By default the server starts as unprotected Wifi Access Point "sdwifi" with ip address 192.168.4.1
 *
 *  The server accepts the following commands:
 *
 *    file operations
 *        upload   - upload a file to SD card.
 *        download - download a file
 *        remove   - remove a file
 *        rename   - rename a file
 *        sha1     - get sha1 sum of a file
 *        list     - get dir/file info
 *
 *    config   - change wifi settings
 *        param=value
 *    exp      - experimental commands
 *        power=off|reset
 *        sleep=sense|ms (sense puts ESP32 to sleep until there is an activity on D3/CS SD finger, ms -- sleep for N ms)
 *    info
 *
 *
 *  Usage examples:
 *
 *  Change AP name and AP password
 *      curl http://192.168.4.1/config?ap_ssid=sdwifi4&ap_password=012345678
 *
 *  Change wifi mode to STA
 *      curl http://192.168.4.1/config?sta_ssid=mywifinetwork&sta_password=mywifinetworkpassword
 *
 *  Reset wifi settings to default (AP mode, no password)
 *      curl http://192.168.4.1/config?reset=all
 *
 *  Upload file xe2-ffboot.bin as ffboot.bin to the root of SD card
 *      curl -T xe2-ffboot.bin http://192.168.4.1/upload/ffboot.bin
 *
 *      or when in STA mode
 *
 *      curl -T xe2-ffboot.bin http://sdwifi.local/upload/ffboot.bin
 *
 *  Remove a file
 *      curl http://192.168.4.1/remove/ffboot.bin
 *
 *  Get sha1 sum of a file
 *      curl http://192.168.4.1/sha1/ffboot.bin
 *
 *  Rename a file
 *      curl http://192.168.4.1/rename/?from=ffboot_new.bin&to=ffboot.bin
 *
 *  Give permanent control over SD to ESP32, blocks access to NAND SD card from the host side
 *      curl http://192.168.4.1/exp?io26=esp32
 *
 *  Relinquish SD control
 *      curl http://192.168.4.1/exp?io26=host
 *
 *  Reboot ESP32
 *      curl http://192.168.4.1/exp?power=reset
 *
 *
 */

#include <WiFi.h>
#include <WiFiClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <uri/UriRegex.h>
#include <SD_MMC.h>
#include <Preferences.h>
#include <mbedtls/sha1.h>


static char *default_name = "sdwifi";

#define SD_SWITCH_PIN GPIO_NUM_26
#define CS_SENSE_PIN  GPIO_NUM_33

#define PREF_RW_MODE false
#define PREF_RO_MODE true
#define PREF_NS "wifi"

#define WIFI_STA_TIMEOUT 5000

WebServer server(80);

Preferences prefs;
File dataFile;

bool esp32_controls_sd;

void setup(void) {
  /* Make SD card available to the Host early in the process */
  pinMode(SD_SWITCH_PIN, OUTPUT);
  digitalWrite(SD_SWITCH_PIN,HIGH);
  esp32_controls_sd = false;

  Serial.setDebugOutput(true);

  prefs.begin(PREF_NS, PREF_RO_MODE);
  /* assume STA mode if sta_ssid is defined */
  if (prefs.isKey("sta_ssid") && setupSTA())
  {
    String hostname =  prefs.isKey("hostname") ?  prefs.getString("hostname") : default_name;
    if (!MDNS.begin(hostname))
        log_e("Error setting up MDNS responder");
  } else {
      if (!setupAP())
          while(1) delay(1);
  }
  prefs.end();

  /* TODO: rethink the API to simplify scripting  */
  server.onNotFound([]() { httpNotFound("wrong usage");} );
  server.on("/info", handleInfo);
  server.on("/config", handleConfig);
  server.on("/exp", handleExperimental);
  /* file ops */
  server.on(UriRegex("/upload/(.*)"), HTTP_PUT, handleUpload, handleUploadProcess);
  server.on(UriRegex("/download/(.*)"), HTTP_GET, handleDownload);
  server.on(UriRegex("/sha1/(.*)"), handleSha1);
  server.on(UriRegex("/remove/(.*)"), handleRemove);
  server.on(UriRegex("/list/(.*)"), handleList);
  server.on("/rename", handleRename);
  server.begin();
  log_i("HTTP server started");
}

void loop(void) {
  /* handle one client at a time */
  server.handleClient();
  delay(2);
}

static bool setupAP(void) {
  String ssid = prefs.isKey("ap_ssid") ?  prefs.getString("ap_ssid") : default_name;
  String password = prefs.getString("ap_password");

  WiFi.mode(WIFI_AP);

  if (!WiFi.softAP(ssid, password) ) {
      log_e("Fallback to unprotected AP %s", ssid.c_str()); //typically happens when the password is too short
      delay(100);
      if (!WiFi.softAP(ssid) ) {
        log_e("Fallback to default AP name %s", default_name);
        delay(100);
        if (!WiFi.softAP(default_name) ) {
          log_e("Soft AP creation failed");
          return false;
        }
      }
  }
  log_i("Soft AP created: %s",  WiFi.softAPSSID());
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
      if (WiFi.status() == WL_CONNECTED) {
          log_i("Connected to %s with IP address: %s", ssid, WiFi.localIP().toString());
          return true;
      } else {
          log_e("Connection to %s failed with status %d after %d attempts", ssid, WiFi.status(), i);
          return false;
      }
}

/* Mount SD card */
static bool mountSD(void)
{
    /* get control over flash NAND */
    if (!esp32_controls_sd) {
      digitalWrite(SD_SWITCH_PIN,LOW);
      delay(50);
    }
    if(!SD_MMC.begin() )
    {
        log_e("Card Mount Failed");
        if (!esp32_controls_sd)
            digitalWrite(SD_SWITCH_PIN,HIGH);
        return false;
    }
    return true;
}

/* Unmount SD card */
static void unmountSD(void)
{
    SD_MMC.end();
    if (!esp32_controls_sd)
    {
        delay(50);
        digitalWrite(SD_SWITCH_PIN,HIGH);
    }
}


static const char *cfgparams [] = {
    "sta_ssid", "sta_password", "ap_ssid", "ap_password", "hostname"
};

static bool param_is_valid(const char * n) {
  for (int i = 0; i < sizeof(cfgparams)/sizeof(cfgparams[0]); i++)
  {
      if (!strcmp(n, cfgparams[i]))
        return true;
  }
  return false;
}

/* CMD: Return some info */
void handleInfo(void) {
  String txt = "Info\n\n";
  uint8_t tmp[6];

  txt += "MAC WiFi STA: ";
  esp_read_mac(tmp, ESP_MAC_WIFI_STA);
  for (int i = 0; i < sizeof(tmp); i++) {
      if (i) txt += ':';
      if (tmp[i] < 0x10) txt += "0";
      txt += String(tmp[i], 16);
  }
  txt += "\nMAC WifFi AP: ";
  esp_read_mac(tmp, ESP_MAC_WIFI_SOFTAP);
  for (int i = 0; i < sizeof(tmp); i++) {
      if (i) txt += ':';
      if (tmp[i] < 0x10) txt += "0";
      txt += String(tmp[i], 16);
  }
  txt += "\nMAC Bluetooth: ";
  esp_read_mac(tmp, ESP_MAC_BT);
  for (int i = 0; i < sizeof(tmp); i++) {
      if (i) txt += ':';
      if (tmp[i] < 0x10) txt += "0";
      txt += String(tmp[i], 16);
  }
  httpOK(txt);
}

/* CMD: Update configuration parameters */
void handleConfig(void) {

  String txt;

  if (server.args() == 0) {
      txt =  "Supported configuration parameters\n\n";
      for (int i = 0; i < sizeof(cfgparams)/sizeof(cfgparams[0]); i++)
      {
          txt += cfgparams[i];
          txt += "\n";
      }
      httpOK(txt);
  }

  prefs.begin(PREF_NS, PREF_RW_MODE);
  for (int i = 0; i < server.args(); i++) {
      String n = server.argName(i);
      String v = server.arg(i);
      if (param_is_valid(n.c_str()))
      {
          prefs.putString(n.c_str(), v.c_str());
          txt += n + "=" + v + "\n";
      } else if (n == "clear" || n == "reset") {
          if (v == "all")
              prefs.clear();
          else
              prefs.remove(v.c_str());
          txt += n + " " + v + "\n";
      } else {
          txt += "unknown parameter " + n + " ignored\n";
      }
  }
  prefs.end();
  httpOK(txt);
}


void handleExperimental(void) {
  String txt;

  for (int i = 0; i < server.args(); i++) {
      String n = server.argName(i);
      String v = server.arg(i);
      txt += n + "=" + v;
      /* enforce IO26 pin value */
      if (n == "io26") {
          if (v == "esp32" || v == "low") {
              digitalWrite(SD_SWITCH_PIN,LOW);
              esp32_controls_sd = true;
              txt += " SD controlled by ESP32\n";
          } else if (v == "host" || v == "high") {
              digitalWrite(SD_SWITCH_PIN,HIGH);
              esp32_controls_sd = false;
              txt += " SD controlled by Host\n";
          } else {
              txt += " Ignored\n";
          }
      } else if (n == "power") {
          if (v == "restart" || v == "reboot" || v == "reset") {
              httpOK(txt);
              delay(50);
              ESP.restart();
          } else if (v == "off" || v == "shutdown") {
              httpOK(txt);
              delay(10);
              ESP.deepSleep(-1);
              /* never get here */
              log_e("Unexpected return from poweroff");
              return;
          } else {
              txt += " Ignored\n";
          }
      } else if (n == "sleep") {
          long i = v.toInt();
          if (i > 0) {
              httpOK(txt);
              log_i("deep sleep %ld ms", i);
              delay(10);
              ESP.deepSleep(i*1000);
              /* never get here */
              log_e("Unexpected return from deep sleep %ld ms", i);
              return;
          } else if (v == "sense") {
              esp_sleep_enable_ext0_wakeup(CS_SENSE_PIN,0); //1 = High, 0 = Low
              log_i("deep sleep %ld ms", i);
              esp_deep_sleep_start();
              log_e("Unexpected return from deep sleep on pin %d", CS_SENSE_PIN) ;
              return;
          } else {
              txt += " Ignored\n";
          }
      } else {
          txt += " Ignored\n";
      }
  }
  httpOK(txt);
}

/* CMD list */
void handleList()
{
  String path = "/" + server.pathArg(0);
  String parentDir;
  File root;

  String msg;
  msg += "Path ";
  msg += path;

  parentDir = String(path);
  parentDir[strrchr(path.c_str(), '/') - path.c_str() + 1] = 0;

  if (!mountSD())
  {
    httpServiceUnavailable("SD Card Mount Failed");
    return;
  }
  if (SD_MMC.exists((char *)path.c_str()))
  {
    msg += "\n\n";
    root = SD_MMC.open(path);
    if (root.isDirectory()) {
      while(File file = root.openNextFile()) {
         if (file.isDirectory()) {
              msg += "<DIR> ";
              msg += file.name();
              msg += "\n";
         } else {
              msg += file.size();
              msg += " ";
              msg += file.name();
              msg += "\n";
         }
      }
    } else {
              msg += root.size();
              msg += " ";
              msg += root.name();
              msg += "\n";
    }
    if (root)
        root.close();
    httpOK(msg);
  } else httpNotFound(msg + " not found");
  unmountSD();
}

/* CMD Download a file */
void handleDownload(void) {
  String path = "/" + server.pathArg(0);
  String dataType = "application/octet-stream";

  if (!mountSD())
  {
    httpServiceUnavailable("SD Card Mount Failed");
    return;
  }

  if (SD_MMC.exists((char *)path.c_str()))
  {
      dataFile = SD_MMC.open(path.c_str(), FILE_READ);
      if  (!dataFile) {
          httpServiceUnavailable("Failed to open file");
      } else if(dataFile.isDirectory()) {
          dataFile.close();
          httpNotAllowed("Path is a directory");
      } else {
           unsigned long sentSize = server.streamFile(dataFile, dataType);
           if (sentSize != dataFile.size())
               log_e("Sent less data %ul than expected %ul", sentSize, dataFile.size());
          dataFile.close();
          httpOK("Sent less data than expected");
      }
  } else
      httpNotFound();
  unmountSD();
}

/* CMD Rename a file */
void handleRename()
{
  String nameFrom;
  String nameTo = "/";

  for (int i = 0; i < server.args(); i++) {
      String n = server.argName(i);
      String v = server.arg(i);
      if (n == "from")
        nameFrom = "/" + v;
      else if (n == "to")
        nameTo =  "/" + v;
      else {
        httpInvalidRequest("Unrecognized parameter in 'rename' command");
        return;
      }
  }
  if (!nameFrom || !nameTo) {
        httpInvalidRequest("Both 'to' and 'from' has to be specified in 'rename' command");
        return;
  }
  if (nameFrom == nameTo) {
        httpInvalidRequest("'to' must not be equal to 'from'");
        return;
  }
  if (!mountSD())
  {
    httpServiceUnavailable("SD Card Mount Failed");
    return;
  }
  
  if (SD_MMC.exists((char *)nameFrom.c_str()))
  {
      if (SD_MMC.exists((char *)nameTo.c_str()) ) {
          SD_MMC.remove(nameTo.c_str());
      }
      if (SD_MMC.rename(nameFrom.c_str(), nameTo.c_str()))
          httpOK("OK");
      else
          httpServiceUnavailable("Failed to rename");
  } else httpNotFound(nameFrom + " not found");
  unmountSD();
}

/* CMD Remove a file */
void handleRemove()
{
  String path = "/" + server.pathArg(0);
  if (!mountSD())
  {
    httpServiceUnavailable("SD Card Mount Failed");
    return;
  }
  if (SD_MMC.exists((char *)path.c_str()))
  {
    SD_MMC.remove(path.c_str());
    httpOK();
  } else httpNotFound();
  unmountSD();
}

/* CMD Return SHA1 of a file */
void handleSha1() {
  String path = "/" + server.pathArg(0);
   if (!mountSD())
  {
    httpServiceUnavailable("SD Card Mount Failed");
    return;
  }

  if (SD_MMC.exists((char *)path.c_str()))
  {
    dataFile = SD_MMC.open(path.c_str(), FILE_READ);
    int fileSize = dataFile.size();

    log_i("sha1 file size  %d", fileSize);

    mbedtls_sha1_context ctx;
    mbedtls_sha1_init(&ctx);
    mbedtls_sha1_starts_ret(&ctx);

    #define bufferSize 1024

    uint8_t sha1_buffer[bufferSize];

    int N = fileSize / bufferSize;
    int r = fileSize % bufferSize;

    for (int i = 0; i < N; i++) {
        dataFile.readBytes((char *)sha1_buffer, bufferSize);
        mbedtls_sha1_update_ret(&ctx, sha1_buffer, bufferSize);
    }
    if (r) {
        dataFile.readBytes((char *)sha1_buffer, r);
        mbedtls_sha1_update_ret(&ctx, sha1_buffer, r);
    }

    String result;
    {
        uint8_t tmp[20];
        mbedtls_sha1_finish_ret(&ctx, tmp);
        mbedtls_sha1_free(&ctx);

        for (int i = 0; i < sizeof(tmp); i++) {
            if (tmp[i] < 0x10) result += "0";
            result += String(tmp[i], 16);
        }
    }
    dataFile.close();
    httpOK(result);
  } else httpNotFound();

  unmountSD();
}


/* CMD Upload a file */
void handleUpload() {
    httpOK("");
}

void handleUploadProcess() {
  String path = "/" + server.pathArg(0);
  HTTPRaw& raw = server.raw();
  if (raw.status == RAW_START) {
    if (!mountSD())
    {
        /* BUGBUG: how to return the error to the handler */
        httpServiceUnavailable("SD Card Mount Failed");
        return;
    }
    if (SD_MMC.exists((char *)path.c_str()))
        SD_MMC.remove((char *)path.c_str());
    dataFile = SD_MMC.open(path.c_str(), FILE_WRITE);
    if (dataFile)
        log_i("Upload: START, filename: %s", path.c_str());
    else
        log_e("Upload: Failed to open filename: %s", path.c_str());
  } else if (raw.status == RAW_WRITE)
  {
    if (dataFile)
      dataFile.write(raw.buf, raw.currentSize);
  } else if (raw.status == RAW_END) {
    if (dataFile)
      dataFile.close();
    unmountSD();
    log_i("Upload: END, Size: %d", raw.totalSize);
  }
}

/* */
inline void httpOK(void) {
  server.send(200, "text/plain");
}

inline void httpOK(String msg) {
  server.send(200, "text/plain", msg + "\r\n");
}

inline void httpInvalidRequest(void) {
  server.send(400, "text/plain");
}

inline void httpInvalidRequest(String msg) {
  server.send(400, "text/plain", msg + "\r\n");
}

inline void httpNotFound(void) {
  server.send(404, "text/plain");
}

inline void httpNotFound(String msg) {
  server.send(404, "text/plain", msg + "\r\n");
}

inline void httpNotAllowed(String msg) {
  server.send(405, "text/plain", msg + "\r\n");
}

inline void httpInternalError(String msg) {
  server.send(500, "text/plain", msg + "\r\n");
}

inline void httpServiceUnavailable(String msg) {
  server.send(503, "text/plain", msg + "\r\n");
}
