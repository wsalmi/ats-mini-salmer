#include "Common.h"
#include "Storage.h"
#include "Themes.h"
#include "Utils.h"
#include "Menu.h"
#include "Draw.h"
#include "Morse.h"
#include "Remote.h"
#include "BleMode.h"

#include <WiFi.h>
#include <WiFiMulti.h>
#include <WiFiUdp.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <NTPClient.h>
#include <ESPmDNS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#define CONNECT_TIME  3000  // Time of inactivity to start connecting WiFi
#define WIFI_MULTI_TOTAL_TIMEOUT  30000

WiFiMulti wifiMulti;

//
// Access Point (AP) mode settings
//
static const char *apSSID    = RECEIVER_NAME;
static const char *apPWD     = 0;       // No password
static const int   apChannel = 10;      // WiFi channel number (1..13)
static const bool  apHideMe  = false;   // TRUE: disable SSID broadcast
static const int   apClients = 3;       // Maximum simultaneous connected clients

static uint16_t ajaxInterval = 2500;

static bool itIsTimeToWiFi = false; // TRUE: Need to connect to WiFi
static uint32_t connectTime = millis();

// Settings
String loginUsername = "";
String loginPassword = "";
static bool wifiScanHidden = false;

// AsyncWebServer object on port 80
AsyncWebServer server(80);

// NTP Client to get time
WiFiUDP ntpUDP;
NTPClient ntpClient(ntpUDP, "pool.ntp.org");

static bool wifiInitAP();
static bool wifiConnect();
static void webInit();

static void webSetConfig(AsyncWebServerRequest *request);

static const String webInputField(const String &name, const String &value, bool pass = false);
static const String webStyleSheet();
static const String webNav();
static const String webPage(const String &body);
static const String webUtcOffsetSelector();
static const String webThemeSelector();
static const String webAppPage();
static const String webStatusJson();
static const String webScanJson();
static const String webMemoryJson();
static void webMemoryCommand(AsyncWebServerRequest *request);
static const String webConfigBody();

//
// Web remote control command queue
//
// Web requests run in the async TCP task, but radio control (I2C access)
// must happen in the main loop to avoid racing with it. Commands are
// therefore queued here and drained by webRemoteLoop() from the main loop,
// reusing the same ad hoc remote-control protocol as the USB serial port.
//
#define WEB_CMD_QUEUE_LEN 256

static QueueHandle_t webCmdQueue = nullptr;
static RemoteState webRemoteState;

// Pending direct setting changes applied from the main loop (-1 = none).
// Web requests run in the async TCP task; anything that touches the radio
// (I2C), the display, WiFi or BLE must be applied from the main loop.
static volatile int webPendingMorse = -1;
static volatile int webPendingOverride = -1;
static volatile int webPendingBrt = -1;
static volatile int webPendingRds = -1;
static volatile int webPendingRegion = -1;
static volatile int webPendingTheme = -1;
static volatile int webPendingUI = -1;
static volatile int webPendingZoom = -1;       // 0/1
static volatile int webPendingScroll = -1;     // 0/1 (1 = reverse)
static volatile int webPendingSleep = -1;
static volatile int webPendingSleepMode = -1;
static volatile int webPendingUtc = -1;
static volatile int webPendingUsb = -1;
static volatile int webPendingBle = -1;
static volatile int webPendingWifi = -1;
static volatile bool webPendingScan = false;   // TRUE: run an RSSI scan
static volatile int webPendingMemTune = -1;    // 1-based memory slot to tune
static volatile int webPendingStep = -1;       // requested tuning step in kHz
static volatile int webPendingAgc = -1;        // 1 = AGC auto on, 0 = manual/attenuator
static volatile int webPendingMute = -1;       // 1 = muted, 0 = unmuted
static volatile int webPendingMode = -1;       // target modulation mode index
static volatile int webPendingBand = -1;       // target band index

// Web-initiated RSSI waterfall mode. While active the device main loop locks
// (normal tuning/listening paused) and audio is muted steadily for the whole
// session instead of flapping on/off between scans.
#define WEB_WATERFALL_TIMEOUT 3000             // Auto-clear if no scan request (ms)
static volatile bool     webWaterfallOn = false;     // TRUE: waterfall mode active
static volatile uint32_t webWaterfallLastReq = 0;    // millis() of last scan request
static bool              webWaterfallMuted = false;  // TRUE: we hold the mute
static volatile int      webWaterfallStep = 10;      // Scan step (band units) = span/points

bool webWaterfallActive() { return webWaterfallOn; }

static inline int clampInt(int v, int lo, int hi)
{
  return v<lo? lo : v>hi? hi : v;
}

// A Stream backed by the web command queue. Reads pull queued bytes;
// writes (command responses) are discarded.
class WebStream : public Stream
{
public:
  int available() override
  {
    return webCmdQueue ? (int)uxQueueMessagesWaiting(webCmdQueue) : 0;
  }
  int read() override
  {
    uint8_t b;
    if(webCmdQueue && xQueueReceive(webCmdQueue, &b, 0)==pdTRUE) return b;
    return -1;
  }
  int peek() override
  {
    uint8_t b;
    if(webCmdQueue && xQueuePeek(webCmdQueue, &b, 0)==pdTRUE) return b;
    return -1;
  }
  size_t write(uint8_t) override { return 1; }
  size_t write(const uint8_t *, size_t size) override { return size; }
};

static WebStream webStream;

static void webEnqueueCommand(const String &cmd)
{
  if(!webCmdQueue) return;
  for(size_t i=0 ; i<cmd.length() ; i++)
  {
    uint8_t b = (uint8_t)cmd[i];
    xQueueSend(webCmdQueue, &b, 0);
  }
}

//
// Drain and execute queued web commands from the main loop
//
int webRemoteLoop()
{
  int event = 0;

  // Apply pending direct setting changes
  if(webPendingMorse >= 0)
  {
    morseModeIdx = (uint8_t)webPendingMorse;
    webPendingMorse = -1;
    morseReset();
    event |= REMOTE_CHANGED | REMOTE_PREFS;
  }
  if(webPendingOverride >= 0)
  {
    freqOverride = webPendingOverride != 0;
    webPendingOverride = -1;
    event |= REMOTE_CHANGED | REMOTE_PREFS;
  }
  if(webPendingBrt >= 0)
  {
    currentBrt = clampInt(webPendingBrt, 10, 255);
    webPendingBrt = -1;
    if(!sleepOn()) ledcWrite(PIN_LCD_BL, currentBrt);
    event |= REMOTE_CHANGED | REMOTE_PREFS;
  }
  if(webPendingRds >= 0)
  {
    rdsModeIdx = clampInt(webPendingRds, 0, getTotalRDSModes()-1);
    webPendingRds = -1;
    if(!(getRDSMode() & RDS_CT)) clockReset();
    event |= REMOTE_CHANGED | REMOTE_PREFS;
  }
  if(webPendingRegion >= 0)
  {
    FmRegionIdx = clampInt(webPendingRegion, 0, getTotalFmRegions()-1);
    webPendingRegion = -1;
    if(currentMode==FM) rx.setFMDeEmphasis(fmRegions[FmRegionIdx].value);
    event |= REMOTE_CHANGED | REMOTE_PREFS;
  }
  if(webPendingTheme >= 0)
  {
    themeIdx = clampInt(webPendingTheme, 0, getTotalThemes()-1);
    webPendingTheme = -1;
    event |= REMOTE_CHANGED | REMOTE_PREFS;
  }
  if(webPendingUI >= 0)
  {
    uiLayoutIdx = clampInt(webPendingUI, 0, getTotalUILayouts()-1);
    webPendingUI = -1;
    event |= REMOTE_CHANGED | REMOTE_PREFS;
  }
  if(webPendingZoom >= 0)
  {
    zoomMenu = webPendingZoom != 0;
    webPendingZoom = -1;
    event |= REMOTE_CHANGED | REMOTE_PREFS;
  }
  if(webPendingScroll >= 0)
  {
    scrollDirection = webPendingScroll ? -1 : 1;
    webPendingScroll = -1;
    event |= REMOTE_CHANGED | REMOTE_PREFS;
  }
  if(webPendingSleep >= 0)
  {
    currentSleep = clampInt(webPendingSleep, 0, 255);
    webPendingSleep = -1;
    event |= REMOTE_CHANGED | REMOTE_PREFS;
  }
  if(webPendingSleepMode >= 0)
  {
    sleepModeIdx = clampInt(webPendingSleepMode, 0, getTotalSleepModes()-1);
    webPendingSleepMode = -1;
    event |= REMOTE_CHANGED | REMOTE_PREFS;
  }
  if(webPendingUtc >= 0)
  {
    utcOffsetIdx = clampInt(webPendingUtc, 0, getTotalUTCOffsets()-1);
    webPendingUtc = -1;
    clockRefreshTime();
    event |= REMOTE_CHANGED | REMOTE_PREFS;
  }
  if(webPendingUsb >= 0)
  {
    usbModeIdx = clampInt(webPendingUsb, 0, getTotalUSBModes()-1);
    webPendingUsb = -1;
    event |= REMOTE_CHANGED | REMOTE_PREFS;
  }
  if(webPendingBle >= 0)
  {
    int mode = clampInt(webPendingBle, 0, getTotalBleModes()-1);
    webPendingBle = -1;
    bleInit(mode);
    event |= REMOTE_CHANGED | REMOTE_PREFS;
  }
  if(webPendingMemTune > 0)
  {
    int slot = webPendingMemTune;
    webPendingMemTune = -1;
    if(slot>=1 && slot<=getTotalMemories())
      if(tuneToMemory(&memories[slot-1]))
        event |= REMOTE_CHANGED;
  }
  if(webPendingStep > 0)
  {
    int khz = webPendingStep;
    webPendingStep = -1;
    setStepKHz(khz);
    event |= REMOTE_CHANGED | REMOTE_PREFS;
  }
  if(webPendingAgc >= 0)
  {
    bool autoOn = webPendingAgc != 0;
    webPendingAgc = -1;
    // Mirror doAgc()'s tail: agcIdx 0 = AGC automatic, >=1 = manual/attenuator.
    // Toggling to manual selects the first attenuation step (agcIdx 1).
    int target = autoOn ? 0 : 1;
    if(currentMode==FM)    agcIdx = FmAgcIdx  = target;
    else if(isSSB())       agcIdx = SsbAgcIdx = target;
    else                   agcIdx = AmAgcIdx  = target;
    disableAgc = agcIdx>0 ? 1 : 0;
    agcNdx     = agcIdx>1 ? agcIdx - 1 : 0;
    rx.setAutomaticGainControl(disableAgc, agcNdx);
    event |= REMOTE_CHANGED | REMOTE_PREFS;
  }
  if(webPendingMute >= 0)
  {
    bool m = webPendingMute != 0;
    webPendingMute = -1;
    muteOn(MUTE_MAIN, m ? 1 : 0);
    if(!m) rx.setVolume(volume);
    event |= REMOTE_CHANGED;
  }
  if(webPendingMode >= 0)
  {
    int mode = webPendingMode;
    webPendingMode = -1;
    if(setMode((uint8_t)mode))
      event |= REMOTE_CHANGED | REMOTE_PREFS;
  }
  if(webPendingBand >= 0)
  {
    int idx = webPendingBand;
    webPendingBand = -1;
    if(setBand((uint8_t)idx))
      event |= REMOTE_CHANGED | REMOTE_PREFS;
  }

  // Web waterfall mode: auto-clear if the web client stopped polling (e.g. the
  // tab was closed) so the device never stays locked forever.
  if(webWaterfallOn && (millis() - webWaterfallLastReq > WEB_WATERFALL_TIMEOUT))
    webWaterfallOn = false;

  // Hold the audio mute for the whole waterfall session (set once on entry,
  // released once on exit) so it does not flap on/off between scans.
  if(webWaterfallOn && !webWaterfallMuted)
  {
    muteOn(MUTE_TEMP, true);
    webWaterfallMuted = true;
  }
  else if(!webWaterfallOn && webWaterfallMuted)
  {
    muteOn(MUTE_TEMP, false);
    webWaterfallMuted = false;
    event |= REMOTE_CHANGED;
  }

  // Run a (blocking) RSSI scan for the waterfall display. While in waterfall
  // mode the mute is held by us (holdMute), so scanRun() does not toggle it.
  if(webPendingScan)
  {
    webPendingScan = false;
    scanRun(currentFrequency, (uint16_t)webWaterfallStep, WATERFALL_POINTS, WATERFALL_TUNE_DELAY, webWaterfallOn);
    event |= REMOTE_CHANGED;
  }

  // Execute queued ad hoc protocol commands
  while(webStream.available())
    event |= remoteDoCommand(&webStream, &webRemoteState, webStream.read());

  // WiFi mode changes are applied last so the response can still be sent.
  // Note: this may drop the current connection (intended for Off/Connect).
  if(webPendingWifi >= 0)
  {
    int mode = clampInt(webPendingWifi, 0, getTotalWiFiModes()-1);
    webPendingWifi = -1;
    wifiModeIdx = mode;
    prefsRequestSave(SAVE_SETTINGS, true);
    netInit(wifiModeIdx);
  }

  return event;
}

//
// Delayed WiFi connection
//
void netRequestConnect()
{
  connectTime = millis();
  itIsTimeToWiFi = true;
}

void netTickTime()
{
  // Connect to WiFi if requested
  if(itIsTimeToWiFi && ((millis() - connectTime) > CONNECT_TIME))
  {
    netInit(wifiModeIdx);
    connectTime = millis();
    itIsTimeToWiFi = false;
  }
}

//
// Get current connection status
// (-1 - not connected, 0 - disabled, 1 - connected, 2 - connected to network)
//
int8_t getWiFiStatus()
{
  wifi_mode_t mode = WiFi.getMode();

  switch(mode)
  {
    case WIFI_MODE_NULL:
      return(0);
    case WIFI_AP:
      return(WiFi.softAPgetStationNum()? 1 : -1);
    case WIFI_STA:
      return(WiFi.status()==WL_CONNECTED? 2 : -1);
    case WIFI_AP_STA:
      return((WiFi.status()==WL_CONNECTED)? 2 : WiFi.softAPgetStationNum()? 1 : -1);
    default:
      return(-1);
  }
}

char *getWiFiIPAddress()
{
  static char ip[16];
  return strcpy(ip, WiFi.status()==WL_CONNECTED ? WiFi.localIP().toString().c_str() : "");
}

//
// Stop WiFi hardware
//
void netStop()
{
  wifi_mode_t mode = WiFi.getMode();

  MDNS.end();

  // If network connection up, shut it down
  if((mode==WIFI_STA) || (mode==WIFI_AP_STA))
    WiFi.disconnect(true);

  // If access point up, shut it down
  if((mode==WIFI_AP) || (mode==WIFI_AP_STA))
    WiFi.softAPdisconnect(true);

  WiFi.mode(WIFI_MODE_NULL);
}

//
// Initialize WiFi network and services
//
void netInit(uint8_t netMode, bool showStatus)
{
  // Always disable WiFi first
  netStop();

  switch(netMode)
  {
    case NET_OFF:
      // Do not initialize WiFi if disabled
      return;
    case NET_AP_ONLY:
      // Start WiFi access point if requested
      WiFi.mode(WIFI_AP);
      // Let user see connection status if successful
      if(wifiInitAP() && showStatus) delay(2000);
      break;
    case NET_AP_CONNECT:
      // Start WiFi access point if requested
      WiFi.mode(WIFI_AP_STA);
      // Let user see connection status if successful
      if(wifiInitAP() && showStatus) delay(2000);
      break;
    default:
      // No access point
      WiFi.mode(WIFI_STA);
      break;
  }

  // Initialize WiFi and try connecting to a network
  if(netMode>NET_AP_ONLY && wifiConnect())
  {
    // Let user see connection status if successful
    if(netMode!=NET_SYNC && showStatus) delay(2000);

    // NTP time updates will happen every 5 minutes
    ntpClient.setUpdateInterval(5*60*1000);

    // Get NTP time from the network
    clockReset();
    for(int j=0 ; j<10 ; j++)
      if(ntpSyncTime()) break; else delay(500);
  }

  // If only connected to sync...
  if(netMode==NET_SYNC)
  {
    // Drop network connection
    WiFi.disconnect(true);
    WiFi.mode(WIFI_MODE_NULL);
  }
  else
  {
    // Initialize web server for remote configuration
    webInit();

    // Initialize mDNS
    MDNS.begin("atsmini"); // Set the hostname to "atsmini.local"
    MDNS.addService("http", "tcp", 80);
  }
}

//
// Returns TRUE if NTP time is available
//
bool ntpIsAvailable()
{
  return(ntpClient.isTimeSet());
}

//
// Update NTP time and synchronize clock with NTP time
//
bool ntpSyncTime()
{
  if(WiFi.status()==WL_CONNECTED)
  {
    ntpClient.update();

    if(ntpClient.isTimeSet())
      return(clockSet(
        ntpClient.getHours(),
        ntpClient.getMinutes(),
        ntpClient.getSeconds()
      ));
  }
  return(false);
}

//
// Initialize WiFi access point (AP)
//
static bool wifiInitAP()
{
  // These are our own access point (AP) addresses
  IPAddress ip(10, 1, 1, 1);
  IPAddress gateway(10, 1, 1, 1);
  IPAddress subnet(255, 255, 255, 0);

  // Start as access point (AP)
  WiFi.softAP(apSSID, apPWD, apChannel, apHideMe, apClients);
  WiFi.softAPConfig(ip, gateway, subnet);

  drawScreen(
    ("Use Access Point " + String(apSSID)).c_str(),
    ("IP : " + WiFi.softAPIP().toString() + " or atsmini.local").c_str()
  );

  ajaxInterval = 2500;
  return(true);
}

//
// Connect to a WiFi network
//
static bool wifiConnect()
{
  String status = "Connecting to WiFi network...";

  // Clean credentials
  wifiMulti.APlistClean();

  // Get the preferences
  prefs.begin("network", true, STORAGE_PARTITION);
  loginUsername = prefs.getString("loginusername", "");
  loginPassword = prefs.getString("loginpassword", "");
  wifiScanHidden = prefs.getBool("wifiscanhidden", false);

  // Try connecting to known WiFi networks
  for(int j=0 ; (j<3) ; j++)
  {
    char nameSSID[16], namePASS[16];
    sprintf(nameSSID, "wifissid%d", j+1);
    sprintf(namePASS, "wifipass%d", j+1);

    String ssid = prefs.getString(nameSSID, "");
    String password = prefs.getString(namePASS, "");

    if(ssid != "")
      wifiMulti.addAP(ssid.c_str(), password.c_str());
  }

  // Done with preferences
  prefs.end();

  drawScreen(status.c_str());

  consumeAbortPending();
  wl_status_t wifiStatus = WL_NO_SSID_AVAIL;
  uint32_t start = millis();
  while(((millis() - start)<WIFI_MULTI_TOTAL_TIMEOUT) && (wifiStatus!=WL_CONNECTED))
  {
    wifiStatus = (wl_status_t)wifiMulti.run(5000, wifiScanHidden);

    if(consumeAbortPending())
    {
      WiFi.disconnect();
      break;
    }

    if((wifiStatus!=WL_CONNECTED) && ((millis() - start)<WIFI_MULTI_TOTAL_TIMEOUT))
      delay(1000);
  }

  // If failed connecting to WiFi network...
  if (wifiStatus != WL_CONNECTED)
  {
    // WiFi connection failed
    drawScreen(status.c_str(), "No WiFi connection");
    // Done
    return(false);
  }
  else
  {
    // WiFi connection succeeded
    drawScreen(
      ("Connected to WiFi network (" + WiFi.SSID() + ")").c_str(),
      ("IP : " + WiFi.localIP().toString() + " or atsmini.local").c_str()
    );
    // Done
    ajaxInterval = 1000;
    return(true);
  }
}

//
// Initialize internal web server
//
static void webInit()
{
  // Create the web command queue once
  if(!webCmdQueue)
    webCmdQueue = xQueueCreate(WEB_CMD_QUEUE_LEN, sizeof(uint8_t));

  // Single-page tabbed app (Spectrum / Control / Memory / Config). If web
  // login credentials are configured, the whole UI requires authentication
  // (previously only the Config page did). The legacy /memory and /config
  // routes still resolve to the SPA, opening the matching tab via #hash.
  auto serveApp = [] (AsyncWebServerRequest *request) {
    if(loginUsername != "" && loginPassword != "")
      if(!request->authenticate(loginUsername.c_str(), loginPassword.c_str()))
        return request->requestAuthentication();
    request->send(200, "text/html", webAppPage());
  };
  server.on("/", HTTP_ANY, serveApp);
  server.on("/memory", HTTP_ANY, serveApp);
  server.on("/config", HTTP_ANY, serveApp);

  // Live status as JSON, polled by the control page
  server.on("/api/status", HTTP_GET, [] (AsyncWebServerRequest *request) {
    AsyncWebServerResponse *r = request->beginResponse(200, "application/json", webStatusJson());
    r->addHeader("Cache-Control", "no-store");
    request->send(r);
  });

  // Queue an ad hoc protocol command string (e.g. ?c=V or ?c=F107900000%0D)
  server.on("/api/cmd", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    if(request->hasParam("c"))
      webEnqueueCommand(request->getParam("c")->value());
    else if(request->hasParam("c", true))
      webEnqueueCommand(request->getParam("c", true)->value());
    request->send(200, "text/plain", "OK");
  });

  // Set a direct frequency in Hz, queued as the F command (honors override)
  server.on("/api/freq", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    const AsyncWebParameter *p = request->getParam("hz", true);
    if(!p) p = request->getParam("hz");
    if(p)
      webEnqueueCommand("F" + p->value() + "\r");
    request->send(200, "text/plain", "OK");
  });

  // Set direct radio/display settings, applied from the main loop
  server.on("/api/set", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    const AsyncWebParameter *p;
    if((p = request->getParam("morse", true)) || (p = request->getParam("morse")))
    {
      int v = p->value().toInt();
      if(v>=0 && v<getTotalMorseModes()) webPendingMorse = v;
    }
    if((p = request->getParam("override", true)) || (p = request->getParam("override")))
      webPendingOverride = p->value().toInt() ? 1 : 0;
    if((p = request->getParam("brt", true)) || (p = request->getParam("brt")))
      webPendingBrt = p->value().toInt();
    if((p = request->getParam("rds", true)) || (p = request->getParam("rds")))
      webPendingRds = p->value().toInt();
    if((p = request->getParam("region", true)) || (p = request->getParam("region")))
      webPendingRegion = p->value().toInt();
    if((p = request->getParam("theme", true)) || (p = request->getParam("theme")))
      webPendingTheme = p->value().toInt();
    if((p = request->getParam("ui", true)) || (p = request->getParam("ui")))
      webPendingUI = p->value().toInt();
    if((p = request->getParam("zoom", true)) || (p = request->getParam("zoom")))
      webPendingZoom = p->value().toInt() ? 1 : 0;
    if((p = request->getParam("scroll", true)) || (p = request->getParam("scroll")))
      webPendingScroll = p->value().toInt() ? 1 : 0;
    if((p = request->getParam("sleep", true)) || (p = request->getParam("sleep")))
      webPendingSleep = p->value().toInt();
    if((p = request->getParam("sleepmode", true)) || (p = request->getParam("sleepmode")))
      webPendingSleepMode = p->value().toInt();
    if((p = request->getParam("utc", true)) || (p = request->getParam("utc")))
      webPendingUtc = p->value().toInt();
    if((p = request->getParam("usb", true)) || (p = request->getParam("usb")))
      webPendingUsb = p->value().toInt();
    if((p = request->getParam("ble", true)) || (p = request->getParam("ble")))
      webPendingBle = p->value().toInt();
    if((p = request->getParam("wifi", true)) || (p = request->getParam("wifi")))
      webPendingWifi = p->value().toInt();
    if((p = request->getParam("step", true)) || (p = request->getParam("step")))
      webPendingStep = p->value().toInt();
    if((p = request->getParam("agc", true)) || (p = request->getParam("agc")))
      webPendingAgc = p->value().toInt() ? 1 : 0;
    if((p = request->getParam("mute", true)) || (p = request->getParam("mute")))
      webPendingMute = p->value().toInt() ? 1 : 0;
    if((p = request->getParam("mode", true)) || (p = request->getParam("mode")))
      webPendingMode = p->value().toInt();
    if((p = request->getParam("band", true)) || (p = request->getParam("band")))
      webPendingBand = p->value().toInt();
    request->send(200, "text/plain", "OK");
  });

  // Trigger a blocking RSSI scan (?run=1) or fetch the latest scan data (GET)
  server.on("/api/scan", HTTP_ANY, [] (AsyncWebServerRequest *request) {
    // Waterfall mode on/off signal from the web client (?auto=1 / ?auto=0).
    // This locks/unlocks the device and controls steady audio muting.
    const AsyncWebParameter *pa;
    if((pa = request->getParam("auto", true)) || (pa = request->getParam("auto")))
    {
      webWaterfallOn = pa->value().toInt() != 0;
      webWaterfallLastReq = millis();
    }
    // Waterfall span control: the per-point scan step in band units. The covered
    // span is WATERFALL_POINTS * step (FM units are 10kHz, AM/SSB units are kHz).
    if((pa = request->getParam("span", true)) || (pa = request->getParam("span")))
      webWaterfallStep = clampInt(pa->value().toInt(), 1, 200);
    if(request->hasParam("run") || request->hasParam("run", true))
    {
      webPendingScan = true;
      webWaterfallLastReq = millis();
      request->send(200, "text/plain", "OK");
      return;
    }
    AsyncWebServerResponse *r = request->beginResponse(200, "application/json", webScanJson());
    r->addHeader("Cache-Control", "no-store");
    request->send(r);
  });

  // Memory list as JSON
  server.on("/api/memory", HTTP_GET, [] (AsyncWebServerRequest *request) {
    AsyncWebServerResponse *r = request->beginResponse(200, "application/json", webMemoryJson());
    r->addHeader("Cache-Control", "no-store");
    request->send(r);
  });

  // Edit a memory slot: action=save|clear|tune (+ optional band/hz/mode for set)
  server.on("/api/mem", HTTP_ANY, webMemoryCommand);

  server.onNotFound([] (AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "Not found");
  });

  // This method saves configuration form contents
  server.on("/setconfig", HTTP_ANY, webSetConfig);

  // Start web server
  server.begin();
}

void webSetConfig(AsyncWebServerRequest *request)
{
  uint32_t prefsSave = 0;

  // Start modifying preferences
  prefs.begin("network", false, STORAGE_PARTITION);

  // Save user name and password
  if(request->hasParam("username", true) && request->hasParam("password", true))
  {
    loginUsername = request->getParam("username", true)->value();
    loginPassword = request->getParam("password", true)->value();

    prefs.putString("loginusername", loginUsername);
    prefs.putString("loginpassword", loginPassword);
  }

  // Save SSIDs and their passwords
  bool haveSSID = false;
  for(int j=0 ; j<3 ; j++)
  {
    char nameSSID[16], namePASS[16];

    sprintf(nameSSID, "wifissid%d", j+1);
    sprintf(namePASS, "wifipass%d", j+1);

    if(request->hasParam(nameSSID, true) && request->hasParam(namePASS, true))
    {
      String ssid = request->getParam(nameSSID, true)->value();
      String pass = request->getParam(namePASS, true)->value();
      prefs.putString(nameSSID, ssid);
      prefs.putString(namePASS, pass);
      haveSSID |= ssid != "" && pass != "";
    }
  }

  // Save hidden SSID scanning preference
  wifiScanHidden = request->hasParam("wifiscanhidden", true);
  prefs.putBool("wifiscanhidden", wifiScanHidden);

  // Save time zone
  if(request->hasParam("utcoffset", true))
  {
    String utcOffset = request->getParam("utcoffset", true)->value();
    utcOffsetIdx = utcOffset.toInt();
    clockRefreshTime();
    prefsSave |= SAVE_SETTINGS;
  }

  // Save theme
  if(request->hasParam("theme", true))
  {
    String theme = request->getParam("theme", true)->value();
    themeIdx = theme.toInt();
    prefsSave |= SAVE_SETTINGS;
  }

  // Save scroll direction and menu zoom (only when this form submitted them)
  if(request->hasParam("uiform", true))
  {
    scrollDirection = request->hasParam("scroll", true)? -1 : 1;
    zoomMenu        = request->hasParam("zoom", true);
    prefsSave |= SAVE_SETTINGS;
  }

  // Done with the preferences
  prefs.end();

  // Save preferences immediately
  prefsRequestSave(prefsSave, true);

  // Show config page again
  request->redirect("/config");

  // If we are currently in AP mode, and infrastructure mode requested,
  // and there is at least one SSID / PASS pair, request network connection
  if(haveSSID && (wifiModeIdx>NET_AP_ONLY) && (WiFi.status()!=WL_CONNECTED))
    netRequestConnect();
}

static const String webInputField(const String &name, const String &value, bool pass)
{
  String newValue(value);

  newValue.replace("\"", "&quot;");
  newValue.replace("'", "&apos;");

  return(
    "<INPUT TYPE='" + String(pass? "PASSWORD":"TEXT") + "' NAME='" +
    name + "' VALUE='" + newValue + "'>"
  );
}

static const String webStyleSheet()
{
  return
":root{"
  "--bg:#0a0e13;--card:#121a23;--card2:#0d141c;--bd:#23303d;--tx:#e7eef5;--mut:#8a99a8;"
  "--acc:#ffb24a;--acc2:#16c79a;--warn:#f5b342;--bad:#ff5a5a;--good:#37d67a;"
  "--sh:0 2px 6px rgba(0,0,0,.45);"
"}"
"*{box-sizing:border-box}"
"BODY{margin:0;padding:0 0 2.5em;background:var(--bg);color:var(--tx);"
  "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;"
  "font-size:16px;line-height:1.4;-webkit-text-size-adjust:100%}"
"H1{display:none}"
"A{color:var(--acc)}"
".bar{position:sticky;top:0;z-index:5;display:flex;align-items:center;justify-content:space-between;"
  "gap:.5em;padding:.6em 1em;background:rgba(10,14,19,.96);border-bottom:1px solid var(--bd)}"
".brand{font-weight:700;letter-spacing:.05em;font-size:1.05em}"
".brand SPAN{color:var(--acc)}"
".nav{display:flex;gap:.3em}"
".nav A{color:var(--mut);text-decoration:none;padding:.4em .85em;border-radius:999px;font-size:.88em;"
  "border:1px solid transparent}"
".nav A:hover{color:var(--tx);background:var(--card)}"
".nav A.on{color:#0a0e13;background:var(--acc);font-weight:700}"
".wrap{max-width:860px;margin:0 auto;padding:1em}"
".card{background:var(--card);border:1px solid var(--bd);border-radius:14px;padding:1em;"
  "margin:0 0 1em;box-shadow:var(--sh)}"
".ttl{font-size:.72em;text-transform:uppercase;letter-spacing:.13em;color:var(--mut);"
  "margin:0 0 .7em;font-weight:700;display:flex;align-items:center;gap:.5em}"
".ttl::before{content:'';width:.5em;height:.5em;border-radius:2px;background:var(--acc)}"
/* frequency readout */
".freqcard{text-align:center;background:radial-gradient(120% 140% at 50% 0%,#15202c,#0a1016)}"
".freq{font-family:ui-monospace,'SF Mono',Menlo,Consolas,monospace;font-weight:700;"
  "font-size:clamp(2.3em,12vw,4em);color:var(--acc);text-shadow:0 0 16px rgba(255,178,74,.4);"
  "font-variant-numeric:tabular-nums;letter-spacing:.02em;line-height:1.05}"
".freq .u{font-size:.3em;color:var(--tx);margin-left:.45em;letter-spacing:.12em;"
  "vertical-align:middle;text-shadow:none;font-weight:600}"
".meta{margin-top:.45em;color:var(--mut);font-size:.86em;letter-spacing:.07em}"
".meta B{color:var(--tx)}"
".rdstop{margin-top:.5em;color:var(--acc);font-weight:600;font-size:1.05em;"
  "overflow-wrap:anywhere}"
".rdsrt{margin-top:.2em;color:var(--mut);font-size:.82em;overflow-wrap:anywhere}"
/* control groups */
".grid{display:grid;grid-template-columns:1fr 1fr;gap:.8em}"
"@media(max-width:560px){.grid{grid-template-columns:1fr}}"
".seg{display:flex;gap:.35em}"
"BUTTON{font-family:inherit;cursor:pointer;border:1px solid var(--bd);background:var(--card2);"
  "color:var(--tx);padding:.65em .9em;border-radius:10px;font-size:.95em;font-weight:600;transition:.12s}"
"BUTTON:hover{border-color:var(--acc);color:var(--acc)}"
"BUTTON:active{transform:translateY(1px)}"
"BUTTON:disabled{opacity:.5;cursor:default}"
".btn,.seg BUTTON,.row BUTTON{flex:1;padding:.75em .4em}"
".btn.acc,BUTTON.acc{background:var(--acc);color:#0a0e13;border-color:var(--acc)}"
".btn.acc:hover,BUTTON.acc:hover{filter:brightness(1.08);color:#0a0e13}"
".setrow{display:flex;gap:.5em;align-items:stretch}"
".setrow INPUT{flex:1}"
".setrow .u{display:flex;align-items:center;color:var(--acc);font-weight:700;min-width:3em;"
  "justify-content:center}"
".setrow .btn{flex:0 0 auto;padding-left:1.3em;padding-right:1.3em}"
".hint{color:var(--mut);font-size:.82em;margin:.6em 0 0}"
".row{display:flex;gap:.5em;flex-wrap:wrap;margin:.2em 0 0}"
".row BUTTON{min-width:7em}"
/* read-only value indicator (e.g. current step) */
".stepval{flex:0 0 auto;display:flex;align-items:center;gap:.45em;padding:.5em .7em;"
  "background:var(--card);border:1px solid var(--bd);border-radius:10px;cursor:default;"
  "box-shadow:inset 0 1px 2px rgba(0,0,0,.35)}"
".stepval .svl{color:var(--mut);font-size:.68em;text-transform:uppercase;letter-spacing:.06em}"
".stepval .svv{color:var(--tx);font-family:ui-monospace,Menlo,monospace;font-variant-numeric:tabular-nums}"
/* badges */
".badges{display:flex;flex-wrap:wrap;gap:.4em;margin:0 0 .9em}"
".badge{font-size:.68em;font-weight:700;letter-spacing:.07em;text-transform:uppercase;"
  "padding:.3em .65em;border-radius:999px;border:1px solid var(--bd);color:var(--mut);background:var(--card2)}"
".badge.on{color:#0a0e13;background:var(--acc2);border-color:var(--acc2)}"
".badge.warn{color:#0a0e13;background:var(--warn);border-color:var(--warn)}"
/* meters */
".meter{display:grid;grid-template-columns:3.4em 1fr 4.6em;align-items:center;gap:.6em;margin:.5em 0}"
".meter .ml{color:var(--mut);font-size:.78em;text-transform:uppercase;letter-spacing:.05em}"
".track{height:13px;background:var(--card2);border:1px solid var(--bd);border-radius:7px;overflow:hidden}"
".fill{height:100%;width:0;border-radius:6px;transition:width .35s ease,background .35s}"
".fill.g{background:linear-gradient(90deg,#1b8f5a,var(--good))}"
".fill.a{background:linear-gradient(90deg,#b9851f,var(--warn))}"
".fill.r{background:linear-gradient(90deg,#a13434,var(--bad))}"
".mv{font-family:ui-monospace,Menlo,monospace;font-size:.8em;text-align:right;"
  "font-variant-numeric:tabular-nums}"
".scale{display:flex;justify-content:space-between;color:var(--mut);font-size:.6em;"
  "margin:.15em 0 .3em;padding-left:4em}"
"CANVAS#wf{width:100%;height:150px;border:1px solid var(--bd);border-radius:10px;background:#000;"
  "image-rendering:pixelated;margin-top:.6em;display:block}"
".wfbar{display:flex;justify-content:space-between;color:var(--mut);font-size:.72em;margin-top:.3em}"
".wfruler{display:flex;justify-content:space-between;color:var(--mut);font-size:.62em;"
  "font-variant-numeric:tabular-nums;margin-top:.15em}"
".wfruler SPAN:first-child{text-align:left}.wfruler SPAN:last-child{text-align:right}"
".wfbanner{background:linear-gradient(90deg,#b9851f,var(--warn));color:#1a1205;font-weight:600;"
  "padding:.6em .9em;border-radius:12px;margin:0 0 1em;text-align:center;box-shadow:var(--sh)}"
".lockable.locked{opacity:.4;pointer-events:none;filter:grayscale(.6)}"
/* legacy tables (memory/config) styled as cards */
"TABLE{width:100%;border-collapse:collapse;margin:0 0 1em;background:var(--card);"
  "border:1px solid var(--bd);border-radius:14px;overflow:hidden;box-shadow:var(--sh)}"
"TH,TD{padding:.65em .85em;text-align:left;vertical-align:middle}"
"TH.HEADING{background:var(--card2);color:var(--acc);text-transform:uppercase;letter-spacing:.1em;"
  "font-size:.76em;border-bottom:1px solid var(--bd);text-align:left}"
"TD{border-top:1px solid rgba(255,255,255,.05)}"
"TD.LABEL{color:var(--mut);width:42%}"
"TD.HINT{color:var(--mut);font-size:.82em}"
"TD.MONO{font-family:ui-monospace,Menlo,monospace}"
".CENTER{text-align:center}"
"INPUT,SELECT{background:var(--card2);border:1px solid var(--bd);color:var(--tx);border-radius:10px;"
  "padding:.6em;font-size:1em;width:100%}"
"INPUT:focus,SELECT:focus{outline:none;border-color:var(--acc)}"
"INPUT[type=submit]{background:var(--acc);color:#0a0e13;border:none;font-weight:700;width:auto;"
  "padding:.7em 1.6em;cursor:pointer}"
"INPUT[type=checkbox]{width:auto;transform:scale(1.3);accent-color:var(--acc)}"
"INPUT[type=range]{width:78%;accent-color:var(--acc);padding:0;border:none;background:none}"
/* SPA tab bar + panes */
".nav A{cursor:pointer}"
".pane{display:none}"
".pane.on{display:block}"
/* spectrum view */
".specwrap{max-width:1400px}"
".sdrbar{display:flex;flex-wrap:wrap;align-items:center;gap:.45em;margin:0 0 .6em;font-size:.82em}"
".sdrbar .sp{flex:0 0 auto}"
".sdrbar BUTTON{flex:0 0 auto;padding:.45em .8em;font-size:.85em}"
".sdrbar INPUT[type=number]{width:5.5em;padding:.4em .5em}"
".sdrbar SELECT{width:auto;padding:.4em .5em}"
".sdrbar LABEL{display:inline-flex;align-items:center;gap:.3em;color:var(--mut);cursor:pointer;white-space:nowrap}"
".sdrbar .stat{margin-left:auto;color:var(--mut);font-variant-numeric:tabular-nums}"
".readout{font-family:ui-monospace,Menlo,monospace;font-size:.78em;color:var(--mut);"
  "margin:.2em 0 .5em;font-variant-numeric:tabular-nums}"
".readout B{color:var(--tx)}"
"CANVAS#spec{width:100%;height:300px;display:block;background:#070b10;border:1px solid var(--bd);"
  "border-radius:10px 10px 0 0;cursor:crosshair}"
"CANVAS#wfs{width:100%;height:240px;display:block;background:#000;border:1px solid var(--bd);"
  "border-top:none;border-radius:0 0 10px 10px;image-rendering:pixelated;cursor:crosshair}"
".specstack{position:relative;line-height:0}"
"#xline{position:absolute;top:0;width:1px;background:rgba(231,238,245,.65);pointer-events:none;display:none;z-index:2}"
"#xhair{position:absolute;top:2px;transform:translateX(-50%);pointer-events:none;display:none;z-index:3;"
  "background:rgba(10,14,19,.92);color:var(--acc);border:1px solid var(--bd);border-radius:6px;"
  "padding:.15em .45em;font:600 11px ui-monospace,Menlo,monospace;white-space:nowrap}"
;
}

static const String webNav()
{
  static const char *lbl[4] = { "Spectrum", "Control", "Memory", "Config" };
  static const char *id[4]  = { "spectrum", "control", "memory", "config" };

  String n = "<DIV CLASS='bar'><DIV CLASS='brand'>ATS<SPAN>Mini</SPAN></DIV><DIV CLASS='nav'>";
  for(int i=0 ; i<4 ; i++)
    n += "<A DATA-TAB='" + String(id[i]) + "' ONCLICK=\"showTab('" + String(id[i]) + "')\""
         + (i==0? " CLASS='on'":"") + ">" + lbl[i] + "</A>";
  n += "</DIV></DIV>";
  return n;
}

static const String webPage(const String &body)
{
  return
"<!DOCTYPE HTML>"
"<HTML>"
"<HEAD>"
  "<META CHARSET='UTF-8'>"
  "<META NAME='viewport' CONTENT='width=device-width, initial-scale=1.0'>"
  "<META NAME='theme-color' CONTENT='#0a0e13'>"
  "<TITLE>ATS-Mini</TITLE>"
  "<STYLE>" + webStyleSheet() + "</STYLE>"
"</HEAD>"
"<BODY>" + body + "</BODY>"
"</HTML>"
;
}

static const String webUtcOffsetSelector()
{
  String result = "";

  for(int i=0 ; i<getTotalUTCOffsets(); i++)
  {
    char text[64];

    sprintf(text,
      "<OPTION VALUE='%d'%s>%s</OPTION>",
      i, utcOffsetIdx==i? " SELECTED":"",
      utcOffsets[i].desc
    );

    result += text;
  }

  return(result);
}

static const String webThemeSelector()
{
  String result = "";

  for(int i=0 ; i<getTotalThemes(); i++)
  {
    char text[64];

    sprintf(text,
      "<OPTION VALUE='%d'%s>%s</OPTION>",
       i, themeIdx==i? " SELECTED":"", theme[i].name
    );

    result += text;
  }

  return(result);
}

//
// JSON-escape a string value
//
static const String webJsonEscape(const String &s)
{
  String out;
  for(size_t i=0 ; i<s.length() ; i++)
  {
    char c = s[i];
    if(c=='"' || c=='\\') { out += '\\'; out += c; }
    else if(c=='\n') out += "\\n";
    else if((uint8_t)c < 0x20) ; // skip other control characters
    else out += c;
  }
  return out;
}

//
// Live radio status as JSON, polled by the control page
//
static const String webStatusJson()
{
  bool fm = currentMode==FM;
  double freqNum = fm? currentFrequency/100.0 : (currentFrequency + currentBFO/1000.0);
  uint32_t freqHz = fm? (uint32_t)currentFrequency*10000 : (uint32_t)currentFrequency*1000 + currentBFO;

  String ip = WiFi.status()==WL_CONNECTED? WiFi.localIP().toString() : WiFi.softAPIP().toString();

  // Skip the long-station-name marker byte if present
  const char *station = getStationName();
  if((uint8_t)*station == 0xFF) station++;

  String json = "{";
  json += "\"ver\":\"" + String(getVersion(true)) + "\",";
  json += "\"band\":\"" + webJsonEscape(getCurrentBand()->bandName) + "\",";
  json += "\"mode\":\"" + String(bandModeDesc[currentMode]) + "\",";
  json += "\"freq\":\"" + String(freqNum, fm? 2 : 3) + "\",";
  json += "\"unit\":\"" + String(fm? "MHz" : "kHz") + "\",";
  json += "\"freqHz\":" + String(freqHz) + ",";
  json += "\"step\":\"" + String(getCurrentStep()->desc) + "\",";
  // Supported steps for the current band/mode, in kHz (deduplicated, sub-kHz
  // SSB steps omitted). Lets the web UI build matching quick-select buttons.
  json += "\"steps\":[";
  {
    int prev = -1;
    bool first = true;
    for(int i=0; i<getStepCount(); i++)
    {
      int khz = getStepValueKHz(i);
      if(khz<=0 || khz==prev) continue;
      prev = khz;
      if(!first) json += ",";
      json += String(khz);
      first = false;
    }
  }
  json += "],";
  json += "\"bw\":\"" + String(getCurrentBandwidth()->desc) + "\",";
  json += "\"agc\":" + String(agcIdx) + ",";
  // TRUE when AGC is automatic (agcIdx 0); FALSE in manual/attenuator mode
  json += "\"agcAuto\":" + String(agcIdx==0? "true" : "false") + ",";
  json += "\"vol\":" + String(volume) + ",";
  json += "\"muted\":" + String(muteOn(MUTE_MAIN)? "true" : "false") + ",";
  // Current modulation mode (index) and the modes valid for the current band.
  // FM bands can only be FM; AM/SW bands can switch among AM/LSB/USB.
  json += "\"modeIdx\":" + String(currentMode) + ",";
  json += "\"modes\":[";
  {
    bool first = true;
    for(int i=0 ; i<getTotalModes() ; i++)
    {
      bool ok = (currentMode==FM) ? (i==FM) : (i!=FM);
      if(!ok) continue;
      if(!first) json += ",";
      json += "{\"i\":" + String(i) + ",\"n\":\"" + String(bandModeDesc[i]) + "\"}";
      first = false;
    }
  }
  json += "],";
  // Current band (index) and all selectable bands. All bands are always listed:
  // selecting one switches the mode to that band's mode (setBand), which is how
  // the band/mode dependency is honored (e.g. picking the FM "VHF" band from an
  // AM/SSB mode switches the radio to FM). Each entry carries a "fm" flag so the
  // client can format the edges correctly (FM in 10kHz units, others in kHz).
  json += "\"bandIdx\":" + String(bandIdx) + ",";
  json += "\"bands\":[";
  {
    bool first = true;
    for(int i=0 ; i<getTotalBands() ; i++)
    {
      if(!first) json += ",";
      json += "{\"i\":" + String(i) + ",\"n\":\"" + webJsonEscape(bands[i].bandName) +
              "\",\"fm\":" + String(bands[i].bandType==FM_BAND_TYPE? 1 : 0) +
              ",\"lo\":" + String(bands[i].minimumFreq) +
              ",\"hi\":" + String(bands[i].maximumFreq) + "}";
      first = false;
    }
  }
  json += "],";
  json += "\"rssi\":" + String(rssi) + ",";
  json += "\"snr\":" + String(snr) + ",";
  json += "\"batt\":" + String(batteryMonitor(), 2) + ",";
  json += "\"ip\":\"" + ip + "\",";
  json += "\"station\":\"" + webJsonEscape(station) + "\",";
  json += "\"rt\":\"" + webJsonEscape(getRadioText()) + "\",";
  json += "\"morse\":" + String(morseModeIdx) + ",";
  json += "\"morseAudio\":true,";
  json += "\"morseText\":\"" + webJsonEscape(morseGetText()) + "\",";
  json += "\"override\":" + String(freqOverride? "true" : "false") + ",";
  // Configuration mirror (so the config page can show current values)
  json += "\"brt\":" + String(currentBrt) + ",";
  json += "\"rdsMode\":" + String(rdsModeIdx) + ",";
  json += "\"region\":" + String(FmRegionIdx) + ",";
  json += "\"theme\":" + String(themeIdx) + ",";
  json += "\"ui\":" + String(uiLayoutIdx) + ",";
  json += "\"zoom\":" + String(zoomMenu? 1 : 0) + ",";
  json += "\"scroll\":" + String(scrollDirection<0? 1 : 0) + ",";
  json += "\"sleep\":" + String(currentSleep) + ",";
  json += "\"sleepMode\":" + String(sleepModeIdx) + ",";
  json += "\"utc\":" + String(utcOffsetIdx) + ",";
  json += "\"usb\":" + String(usbModeIdx) + ",";
  json += "\"ble\":" + String(bleModeIdx) + ",";
  json += "\"wifi\":" + String(wifiModeIdx);
  json += "}";
  return json;
}

//
// Latest RSSI scan data as JSON, used to draw the waterfall
//
static const String webScanJson()
{
  bool fm = currentMode==FM;
  uint16_t start = scanGetStartFreq();
  uint16_t step  = scanGetStep();
  int count      = scanGetCount();

  uint32_t startHz = fm? (uint32_t)start*10000 : (uint32_t)start*1000;
  uint32_t stepHz  = fm? (uint32_t)step*10000  : (uint32_t)step*1000;

  String json = "{";
  json += "\"done\":" + String(scanIsDone()? "true":"false") + ",";
  json += "\"busy\":" + String((webPendingScan || scanIsRunning())? "true":"false") + ",";
  json += "\"count\":" + String(count) + ",";
  json += "\"startHz\":" + String(startHz) + ",";
  json += "\"stepHz\":" + String(stepHz) + ",";
  json += "\"unit\":\"" + String(fm? "MHz":"kHz") + "\",";
  json += "\"rssi\":[";
  for(int i=0 ; i<count ; i++)
  {
    if(i) json += ',';
    json += String(scanGetRawRSSI(i));
  }
  json += "],\"snr\":[";
  for(int i=0 ; i<count ; i++)
  {
    if(i) json += ',';
    json += String(scanGetRawSNR(i));
  }
  json += "]}";
  return json;
}

//
// Memory slots as JSON (used + total), edited via /api/mem
//
static const String webMemoryJson()
{
  String json = "{\"total\":" + String(getTotalMemories()) + ",\"used\":[";
  bool first = true;
  for(int j=0 ; j<getTotalMemories() ; j++)
  {
    if(!memories[j].freq) continue;
    if(!first) json += ',';
    first = false;
    json += "{\"s\":" + String(j+1);
    json += ",\"f\":" + String(memories[j].freq);
    json += ",\"b\":\"" + webJsonEscape(bands[memories[j].band].bandName) + "\"";
    json += ",\"m\":\"" + String(bandModeDesc[memories[j].mode]) + "\"}";
  }
  json += "]}";
  return json;
}

//
// Edit a memory slot from the web UI. Reuses the ad hoc '#' protocol command
// (queued for the main loop) for set/clear and a pending flag for tune.
//
static void webMemoryCommand(AsyncWebServerRequest *request)
{
  const AsyncWebParameter *p;
  String action;
  if((p = request->getParam("action", true)) || (p = request->getParam("action")))
    action = p->value();

  int slot = 0;
  if((p = request->getParam("slot", true)) || (p = request->getParam("slot")))
    slot = p->value().toInt();

  if(slot<1 || slot>getTotalMemories())
  {
    request->send(400, "text/plain", "Bad slot");
    return;
  }

  if(action == "tune")
  {
    webPendingMemTune = slot;
  }
  else if(action == "clear")
  {
    // freq 0 clears the slot; band/mode just need to be valid tokens
    webEnqueueCommand("#" + String(slot) + "," + getCurrentBand()->bandName +
                      ",0," + bandModeDesc[currentMode] + "\r");
  }
  else if(action == "save")
  {
    // Store the currently tuned frequency in the slot
    uint32_t hz = freqToHz(currentFrequency, currentMode) + currentBFO;
    webEnqueueCommand("#" + String(slot) + "," + getCurrentBand()->bandName +
                      "," + String(hz) + "," + bandModeDesc[currentMode] + "\r");
  }
  else if(action == "set")
  {
    // Manual edit: explicit band / frequency (Hz) / mode
    String band = "", mode = "";
    uint32_t hz = 0;
    if((p = request->getParam("band", true)) || (p = request->getParam("band"))) band = p->value();
    if((p = request->getParam("hz", true))   || (p = request->getParam("hz")))   hz   = (uint32_t)p->value().toInt();
    if((p = request->getParam("mode", true)) || (p = request->getParam("mode"))) mode = p->value();
    if(band=="" || mode=="")
    {
      request->send(400, "text/plain", "Missing band/mode");
      return;
    }
    webEnqueueCommand("#" + String(slot) + "," + band + "," + String(hz) + "," + mode + "\r");
  }
  else
  {
    request->send(400, "text/plain", "Bad action");
    return;
  }

  request->send(200, "text/plain", "OK");
}

//
// Config tab body (WiFi form + live device settings). Returns the inner HTML
// for the Config pane; reads stored WiFi credentials from preferences.
//
static const String webConfigBody()
{
  prefs.begin("network", true, STORAGE_PARTITION);
  String ssid1 = prefs.getString("wifissid1", "");
  String pass1 = prefs.getString("wifipass1", "");
  String ssid2 = prefs.getString("wifissid2", "");
  String pass2 = prefs.getString("wifipass2", "");
  String ssid3 = prefs.getString("wifissid3", "");
  String pass3 = prefs.getString("wifipass3", "");
  bool scanHidden = prefs.getBool("wifiscanhidden", false);
  prefs.end();

  return String(
"<FORM ACTION='/setconfig' METHOD='POST'>"
  "<TABLE COLUMNS=2>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>WiFi Network 1</TH></TR>"
  "<TR><TD CLASS='LABEL'>SSID</TD><TD>") + webInputField("wifissid1", ssid1) + "</TD></TR>"
  "<TR><TD CLASS='LABEL'>Password</TD><TD>" + webInputField("wifipass1", pass1, true) + "</TD></TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>WiFi Network 2</TH></TR>"
  "<TR><TD CLASS='LABEL'>SSID</TD><TD>" + webInputField("wifissid2", ssid2) + "</TD></TR>"
  "<TR><TD CLASS='LABEL'>Password</TD><TD>" + webInputField("wifipass2", pass2, true) + "</TD></TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>WiFi Network 3</TH></TR>"
  "<TR><TD CLASS='LABEL'>SSID</TD><TD>" + webInputField("wifissid3", ssid3) + "</TD></TR>"
  "<TR><TD CLASS='LABEL'>Password</TD><TD>" + webInputField("wifipass3", pass3, true) + "</TD></TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>This Web UI Login Credentials</TH></TR>"
  "<TR><TD CLASS='LABEL'>Username</TD><TD>" + webInputField("username", loginUsername) + "</TD></TR>"
  "<TR><TD CLASS='LABEL'>Password</TD><TD>" + webInputField("password", loginPassword, true) + "</TD></TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>WiFi Options</TH></TR>"
  "<TR><TD CLASS='LABEL'>Scan Hidden SSIDs</TD>"
    "<TD><INPUT TYPE='CHECKBOX' NAME='wifiscanhidden' VALUE='on'" +
    (scanHidden? " CHECKED ":"") + "></TD></TR>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'><INPUT TYPE='SUBMIT' VALUE='Save WiFi'></TH></TR>"
  "</TABLE>"
"</FORM>"

"<TABLE COLUMNS=2>"
  "<TR><TH COLSPAN=2 CLASS='HEADING'>Display &amp; UI</TH></TR>"
  "<TR><TD CLASS='LABEL'>Brightness</TD><TD>"
    "<INPUT TYPE='RANGE' ID='brt' MIN='10' MAX='255' STEP='5' "
    "ONCHANGE=\"sset('brt',this.value)\" STYLE='width:70%'> <SPAN ID='brtv'></SPAN></TD></TR>"
  "<TR><TD CLASS='LABEL'>Theme</TD><TD>"
    "<SELECT ID='theme' ONCHANGE=\"sset('theme',this.value)\">" + webThemeSelector() + "</SELECT></TD></TR>"
  "<TR><TD CLASS='LABEL'>UI Layout</TD><TD>"
    "<SELECT ID='ui' ONCHANGE=\"sset('ui',this.value)\">"
      "<OPTION VALUE='0'>Default</OPTION><OPTION VALUE='1'>S-Meter</OPTION></SELECT></TD></TR>"
  "<TR><TD CLASS='LABEL'>Reverse Scrolling</TD><TD>"
    "<INPUT TYPE='CHECKBOX' ID='scroll' ONCHANGE=\"sset('scroll',this.checked?1:0)\"></TD></TR>"
  "<TR><TD CLASS='LABEL'>Zoomed Menu</TD><TD>"
    "<INPUT TYPE='CHECKBOX' ID='zoom' ONCHANGE=\"sset('zoom',this.checked?1:0)\"></TD></TR>"
  "<TR><TD CLASS='LABEL'>Sleep (s, 0=Off)</TD><TD>"
    "<INPUT TYPE='RANGE' ID='sleep' MIN='0' MAX='255' STEP='5' "
    "ONCHANGE=\"sset('sleep',this.value)\" STYLE='width:70%'> <SPAN ID='sleepv'></SPAN></TD></TR>"
  "<TR><TD CLASS='LABEL'>Sleep Mode</TD><TD>"
    "<SELECT ID='sleepmode' ONCHANGE=\"sset('sleepmode',this.value)\">"
      "<OPTION VALUE='0'>Locked</OPTION><OPTION VALUE='1'>Unlocked</OPTION>"
      "<OPTION VALUE='2'>CPU Sleep</OPTION></SELECT></TD></TR>"

  "<TR><TH COLSPAN=2 CLASS='HEADING'>Radio</TH></TR>"
  "<TR><TD CLASS='LABEL'>RDS Mode</TD><TD>"
    "<SELECT ID='rds' ONCHANGE=\"sset('rds',this.value)\">"
      "<OPTION VALUE='0'>PS</OPTION><OPTION VALUE='1'>PS+CT</OPTION>"
      "<OPTION VALUE='2'>PS+PI</OPTION><OPTION VALUE='3'>PS+PI+CT</OPTION>"
      "<OPTION VALUE='4'>ALL-CT (EU)</OPTION><OPTION VALUE='5'>ALL-CT (US)</OPTION>"
      "<OPTION VALUE='6'>ALL (EU)</OPTION><OPTION VALUE='7'>ALL (US)</OPTION></SELECT></TD></TR>"
  "<TR><TD CLASS='LABEL'>FM Region</TD><TD>"
    "<SELECT ID='region' ONCHANGE=\"sset('region',this.value)\">"
      "<OPTION VALUE='0'>EU/JP/AU (50us)</OPTION><OPTION VALUE='1'>US (75us)</OPTION></SELECT></TD></TR>"
  "<TR><TD CLASS='LABEL'>Time Zone</TD><TD>"
    "<SELECT ID='utc' ONCHANGE=\"sset('utc',this.value)\">" + webUtcOffsetSelector() + "</SELECT></TD></TR>"

  "<TR><TH COLSPAN=2 CLASS='HEADING'>Connectivity</TH></TR>"
  "<TR><TD CLASS='LABEL'>USB Port</TD><TD>"
    "<SELECT ID='usb' ONCHANGE=\"sset('usb',this.value)\">"
      "<OPTION VALUE='0'>Off</OPTION><OPTION VALUE='1'>Ad hoc</OPTION></SELECT></TD></TR>"
  "<TR><TD CLASS='LABEL'>Bluetooth</TD><TD>"
    "<SELECT ID='ble' ONCHANGE=\"sset('ble',this.value)\">"
      "<OPTION VALUE='0'>Off</OPTION><OPTION VALUE='1'>Ad hoc</OPTION>"
      "<OPTION VALUE='2'>HID</OPTION></SELECT></TD></TR>"
  "<TR><TD CLASS='LABEL'>Wi-Fi</TD><TD>"
    "<SELECT ID='wifi' ONCHANGE=\"if(confirm('Changing Wi-Fi may drop this connection. Continue?'))sset('wifi',this.value);else cfgLoad();\">"
      "<OPTION VALUE='0'>Off</OPTION><OPTION VALUE='1'>AP Only</OPTION>"
      "<OPTION VALUE='2'>AP+Connect</OPTION><OPTION VALUE='3'>Connect</OPTION>"
      "<OPTION VALUE='4'>Sync Only</OPTION></SELECT></TD></TR>"
  "<TR><TD COLSPAN=2 CLASS='HINT'>These settings apply immediately and are saved.</TD></TR>"
"</TABLE>"
;
}

//
// Single-page tabbed app: Spectrum (SDR view), Control, Memory, Config.
// Tabs switch client-side; all panes share the /api/status, /api/scan,
// /api/memory, /api/set, /api/freq and /api/cmd endpoints.
//
static const String webAppPage()
{
  // Server-rendered band/mode option lists for the manual memory editor
  String bandOpts = "";
  for(int i=0 ; i<getTotalBands() ; i++)
    bandOpts += "<OPTION VALUE='" + String(bands[i].bandName) + "'>" + String(bands[i].bandName) + "</OPTION>";

  String modeOpts = "";
  for(int i=0 ; i<getTotalModes() ; i++)
    modeOpts += "<OPTION VALUE='" + String(bandModeDesc[i]) + "'>" + String(bandModeDesc[i]) + "</OPTION>";

  String body = webNav();

  // ---- Spectrum tab -------------------------------------------------------
  body +=
"<DIV ID='spectrum' CLASS='pane on'><DIV CLASS='wrap specwrap'>"
  "<DIV CLASS='card'>"
    "<DIV CLASS='sdrbar'>"
      "<BUTTON ID='runbtn' CLASS='acc sp' ONCLICK='setRun(!specRun)'>Run</BUTTON>"
      "<BUTTON CLASS='sp' ONCLICK='single()'>Single</BUTTON>"
      "<SPAN CLASS='sp'>&nbsp;</SPAN>"
      "<BUTTON CLASS='sp' DATA-SP='1' ONCLICK='setStepPreset(1)'>Fast</BUTTON>"
      "<BUTTON CLASS='sp acc' DATA-SP='2' ONCLICK='setStepPreset(2)'>Medium</BUTTON>"
      "<BUTTON CLASS='sp' DATA-SP='4' ONCLICK='setStepPreset(4)'>Wide</BUTTON>"
      "<SPAN CLASS='sp'>Span</SPAN>"
      "<INPUT CLASS='sp' TYPE='NUMBER' ID='spaninp' MIN='10' STEP='10' ONCHANGE='setSpanKHz()'>"
      "<SPAN CLASS='sp'>kHz</SPAN>"
      "<SPAN CLASS='sp'>Res <B ID='resv'>--</B> kHz</SPAN>"
      "<LABEL><INPUT TYPE='CHECKBOX' ID='tSnr' CHECKED ONCHANGE='redraw()'>SNR</LABEL>"
      "<LABEL><INPUT TYPE='CHECKBOX' ID='tPeak' CHECKED ONCHANGE='redraw()'>Peak</LABEL>"
      "<LABEL><INPUT TYPE='CHECKBOX' ID='tLabels' CHECKED ONCHANGE='redraw()'>Labels</LABEL>"
      "<LABEL><INPUT TYPE='CHECKBOX' ID='tAvg' ONCHANGE='redraw()'>Avg</LABEL>"
      "<LABEL><INPUT TYPE='CHECKBOX' ID='tWf' CHECKED ONCHANGE='toggleWf()'>Waterfall</LABEL>"
      "<SELECT CLASS='sp' ID='cmap' ONCHANGE='redraw()'>"
        "<OPTION VALUE='viridis'>Viridis</OPTION><OPTION VALUE='inferno'>Inferno</OPTION>"
        "<OPTION VALUE='gray'>Grayscale</OPTION></SELECT>"
      "<A CLASS='sp' STYLE='cursor:pointer' ONCLICK='resetPeak()'>reset peak</A>"
      "<SPAN CLASS='stat' ID='specstat'>idle</SPAN>"
    "</DIV>"
    "<DIV CLASS='readout' ID='readout'>Center -- &nbsp; Span -- &nbsp; Resolution -- &nbsp; -- pts</DIV>"
    "<DIV CLASS='specstack' ID='specstack'>"
      "<CANVAS ID='spec'></CANVAS>"
      "<CANVAS ID='wfs'></CANVAS>"
      "<DIV ID='xline'></DIV><DIV ID='xhair'></DIV>"
    "</DIV>"
    "<DIV CLASS='hint'>Hover for the frequency at the cursor; click the spectrum or waterfall to tune "
      "(detected channels snap to the nearest peak). Tuning works while running too.</DIV>"
  "</DIV>"
"</DIV></DIV>";

  // ---- Control tab --------------------------------------------------------
  body +=
"<DIV ID='control' CLASS='pane'><DIV CLASS='wrap'>"
  "<DIV ID='wfban' CLASS='wfbanner' STYLE='display:none'>Spectrum running &mdash; radio paused</DIV>"
  "<DIV CLASS='card freqcard'>"
    "<DIV CLASS='freq' ID='freq'>--<SPAN CLASS='u'></SPAN></DIV>"
    "<DIV CLASS='meta' ID='meta'>--</DIV>"
    "<DIV CLASS='rdstop' ID='topStation' STYLE='display:none'></DIV>"
    "<DIV CLASS='rdsrt' ID='topRt' STYLE='display:none'></DIV>"
  "</DIV>"
  "<DIV CLASS='card'>"
    "<DIV CLASS='ttl'>Signal</DIV>"
    "<DIV CLASS='badges'>"
      "<SPAN CLASS='badge' ID='bdgWifi'>Wi-Fi</SPAN>"
      "<SPAN CLASS='badge' ID='bdgBle'>BLE</SPAN>"
      "<SPAN CLASS='badge' ID='bdgRds'>RDS</SPAN>"
      "<SPAN CLASS='badge' ID='bdgMorse'>CW</SPAN>"
      "<SPAN CLASS='badge' ID='bdgOvr'>OVR</SPAN>"
      "<SPAN CLASS='badge' ID='bdgMute'>Mute</SPAN>"
    "</DIV>"
    "<DIV CLASS='meter'><SPAN CLASS='ml'>RSSI</SPAN>"
      "<DIV CLASS='track'><DIV CLASS='fill g' ID='rssiFill'></DIV></DIV>"
      "<SPAN CLASS='mv' ID='rssiVal'>--</SPAN></DIV>"
    "<DIV CLASS='scale'><SPAN>S1</SPAN><SPAN>S3</SPAN><SPAN>S5</SPAN><SPAN>S7</SPAN>"
      "<SPAN>S9</SPAN><SPAN>+20</SPAN><SPAN>+40</SPAN></DIV>"
    "<DIV CLASS='meter'><SPAN CLASS='ml'>SNR</SPAN>"
      "<DIV CLASS='track'><DIV CLASS='fill g' ID='snrFill'></DIV></DIV>"
      "<SPAN CLASS='mv' ID='snrVal'>--</SPAN></DIV>"
    "<DIV CLASS='meter'><SPAN CLASS='ml'>Batt</SPAN>"
      "<DIV CLASS='track'><DIV CLASS='fill g' ID='battFill'></DIV></DIV>"
      "<SPAN CLASS='mv' ID='battVal'>--</SPAN></DIV>"
  "</DIV>"
  "<DIV CLASS='grid lockable'>"
    "<DIV CLASS='card grp'><DIV CLASS='ttl'>Tuning</DIV>"
      "<DIV CLASS='seg' STYLE='margin-bottom:.35em'>"
        "<DIV CLASS='seg' ID='stepbtns' STYLE='flex:1'></DIV>"
        "<SPAN CLASS='stepval'><SPAN CLASS='svl'>Step</SPAN><SPAN CLASS='svv' ID='stepbox'>--</SPAN></SPAN></DIV>"
      "<DIV CLASS='seg'>"
        "<BUTTON ONCLICK=\"cmd('r')\">&laquo; Tune</BUTTON>"
        "<BUTTON ONCLICK=\"cmd('R')\">Tune &raquo;</BUTTON></DIV></DIV>"
    "<DIV CLASS='card grp'><DIV CLASS='ttl'>Band &amp; Mode</DIV>"
      "<DIV CLASS='seg' ID='modebtns' STYLE='margin-bottom:.35em'></DIV>"
      "<DIV CLASS='setrow' STYLE='margin-bottom:.35em'>"
        "<SELECT ID='bandsel' ONCHANGE=\"setVal('band',this.value)\"></SELECT></DIV>"
      "<DIV CLASS='seg'>"
        "<BUTTON ONCLICK=\"cmd('m')\">&laquo; Mode</BUTTON>"
        "<BUTTON ONCLICK=\"cmd('M')\">Mode &raquo;</BUTTON>"
        "<BUTTON ONCLICK=\"cmd('b')\">&laquo; Band</BUTTON>"
        "<BUTTON ONCLICK=\"cmd('B')\">Band &raquo;</BUTTON></DIV></DIV>"
    "<DIV CLASS='card grp'><DIV CLASS='ttl'>Step &amp; Bandwidth</DIV>"
      "<DIV CLASS='seg' STYLE='margin-bottom:.35em'>"
        "<BUTTON ONCLICK=\"cmd('s')\">&laquo; Step</BUTTON>"
        "<BUTTON ONCLICK=\"cmd('S')\">Step &raquo;</BUTTON></DIV>"
      "<DIV CLASS='seg'>"
        "<BUTTON ONCLICK=\"cmd('w')\">&laquo; BW</BUTTON>"
        "<BUTTON ONCLICK=\"cmd('W')\">BW &raquo;</BUTTON></DIV></DIV>"
    "<DIV CLASS='card grp'><DIV CLASS='ttl'>Volume &amp; AGC</DIV>"
      "<DIV CLASS='seg' STYLE='margin-bottom:.35em'>"
        "<BUTTON ID='mutebtn' ONCLICK='toggleMute()'>Mute</BUTTON>"
        "<BUTTON ONCLICK=\"cmd('v')\">Vol &minus;</BUTTON>"
        "<BUTTON ONCLICK=\"cmd('V')\">Vol &plus;</BUTTON></DIV>"
      "<DIV CLASS='seg'>"
        "<BUTTON ID='agcbtn' ONCLICK='toggleAgc()'>AGC</BUTTON>"
        "<BUTTON ONCLICK=\"cmd('a')\">AGC &minus;</BUTTON>"
        "<BUTTON ONCLICK=\"cmd('A')\">AGC &plus;</BUTTON></DIV></DIV>"
  "</DIV>"
  "<DIV CLASS='card lockable'>"
    "<DIV CLASS='ttl'>Set Frequency</DIV>"
    "<DIV CLASS='setrow'>"
      "<INPUT TYPE='TEXT' INPUTMODE='DECIMAL' ID='freqval' PLACEHOLDER='Set frequency'>"
      "<SPAN CLASS='u' ID='setunit'>--</SPAN>"
      "<BUTTON CLASS='btn acc' ONCLICK='setFreq()'>Set</BUTTON>"
    "</DIV>"
    "<DIV CLASS='hint' ID='sethint'>Enter the frequency in the unit shown.</DIV>"
  "</DIV>"
  "<TABLE>"
    "<TR><TH COLSPAN=4 CLASS='HEADING'>Receiver</TH></TR>"
    "<TR><TD CLASS='LABEL'>Step</TD><TD ID='step'>--</TD>"
        "<TD CLASS='LABEL'>Bandwidth</TD><TD ID='bw'>--</TD></TR>"
    "<TR><TD CLASS='LABEL'>Volume</TD><TD ID='vol'>--</TD>"
        "<TD CLASS='LABEL'>AGC/Att</TD><TD ID='agc'>--</TD></TR>"
    "<TR><TD CLASS='LABEL'>Station</TD><TD COLSPAN=3 ID='station'>--</TD></TR>"
    "<TR><TD CLASS='LABEL'>RDS Text</TD><TD COLSPAN=3 ID='rt'>--</TD></TR>"
  "</TABLE>"
  "<TABLE>"
    "<TR><TH COLSPAN=2 CLASS='HEADING'>Frequency Limit Override</TH></TR>"
    "<TR><TD CLASS='LABEL'>Allow tuning beyond band limits</TD>"
        "<TD><INPUT TYPE='CHECKBOX' ID='override' ONCHANGE=\"setVal('override', this.checked?1:0)\"></TD></TR>"
    "<TR><TD COLSPAN=2 CLASS='HINT'>When unlocked, tuning ignores the firmware band edges "
        "(still within the SI4732 physical range). Leave locked for normal behavior.</TD></TR>"
  "</TABLE>"
  "<TABLE>"
    "<TR><TH COLSPAN=2 CLASS='HEADING'>Morse (CW) Decoder</TH></TR>"
    "<TR><TD CLASS='LABEL'>Signal source</TD>"
        "<TD><SELECT ID='morse' ONCHANGE=\"setVal('morse', this.value)\">"
          "<OPTION VALUE='0'>Off</OPTION>"
          "<OPTION VALUE='1'>RSSI/SNR</OPTION>"
          "<OPTION VALUE='2'>CW (audio)</OPTION>"
        "</SELECT></TD></TR>"
    "<TR><TD COLSPAN=2 CLASS='HINT' ID='morsehint'>CW (audio) requires wiring the audio output "
        "to GPIO11 (ADC2) with an RC filter (routed from the factory only on the V4). "
        "Without the mod, use RSSI/SNR. "
        "See <A HREF='https://github.com/esp32-si4732/ats-mini/discussions/267' TARGET='_blank'>discussion #267</A>.</TD></TR>"
    "<TR><TD CLASS='LABEL'>Decoded</TD><TD CLASS='MONO' ID='morsetext'>--</TD></TR>"
  "</TABLE>"
"</DIV></DIV>";

  // ---- Memory tab ---------------------------------------------------------
  body +=
"<DIV ID='memory' CLASS='pane'><DIV CLASS='wrap'>"
  "<TABLE>"
    "<TR><TH COLSPAN=4 CLASS='HEADING'>Stored Stations <SPAN ID='mcount'></SPAN></TH></TR>"
    "<TBODY ID='mlist'><TR><TD COLSPAN=4 CLASS='CENTER'>Loading...</TD></TR></TBODY>"
  "</TABLE>"
  "<TABLE>"
    "<TR><TH COLSPAN=2 CLASS='HEADING'>Save Current Frequency</TH></TR>"
    "<TR><TD CLASS='LABEL'>Slot</TD>"
        "<TD><INPUT TYPE='NUMBER' ID='saveslot' MIN='1' VALUE='1' STYLE='width:5em'> "
        "<BUTTON ONCLICK='saveCur()'>Save here</BUTTON></TD></TR>"
    "<TR><TD COLSPAN=2 CLASS='HINT'>Stores the currently tuned frequency and mode into the chosen slot.</TD></TR>"
  "</TABLE>"
  "<TABLE>"
    "<TR><TH COLSPAN=2 CLASS='HEADING'>Manual Edit</TH></TR>"
    "<TR><TD CLASS='LABEL'>Slot</TD><TD><INPUT TYPE='NUMBER' ID='eslot' MIN='1' VALUE='1' STYLE='width:5em'></TD></TR>"
    "<TR><TD CLASS='LABEL'>Band</TD><TD><SELECT ID='eband'>" + bandOpts + "</SELECT></TD></TR>"
    "<TR><TD CLASS='LABEL'>Frequency</TD><TD>"
        "<INPUT TYPE='NUMBER' STEP='any' ID='efreq' STYLE='width:8em'> "
        "<SELECT ID='eunit'><OPTION VALUE='k'>kHz</OPTION><OPTION VALUE='M'>MHz</OPTION></SELECT></TD></TR>"
    "<TR><TD CLASS='LABEL'>Mode</TD><TD><SELECT ID='emode'>" + modeOpts + "</SELECT></TD></TR>"
    "<TR><TD COLSPAN=2 CLASS='CENTER'><BUTTON ONCLICK='setSlot()'>Write slot</BUTTON></TD></TR>"
  "</TABLE>"
"</DIV></DIV>";

  // ---- Config tab ---------------------------------------------------------
  body +=
"<DIV ID='config' CLASS='pane'><DIV CLASS='wrap'>" + webConfigBody() + "</DIV></DIV>";

  // ---- Scripts ------------------------------------------------------------
  body +=
"<SCRIPT>"
"var WP=" + String(WATERFALL_POINTS) + ";"
"var lastUnit='MHz',gFreqHz=0,gFm=true;"
"function $(i){return document.getElementById(i);}"
"function txt(i,v){var e=$(i);if(e)e.textContent=v;}"
"function cmd(c){fetch('/api/cmd?c='+encodeURIComponent(c));}"
"function setVal(k,v){fetch('/api/set?'+k+'='+encodeURIComponent(v));}"
"function setStep(khz){fetch('/api/set?step='+khz);}"
"function fmtFreq(s){var p=(''+s).split('.');p[0]=p[0].replace(/\\B(?=(\\d{3})+(?!\\d))/g,'\\u2009');return p.join('.');}"
"function setFreq(){var s=$('freqval').value.replace(/[\\s\\u2009,]/g,'');var v=parseFloat(s);if(isNaN(v))return;"
  "var hz=lastUnit=='MHz'?Math.round(v*1e6):Math.round(v*1000);fetch('/api/freq?hz='+hz);}"
"function tuneHz(hz){fetch('/api/freq?hz='+Math.round(hz));}"
/* ---- tabs ---- */
"var specActive=false;"
"function showTab(t){var ps=document.querySelectorAll('.pane');for(var i=0;i<ps.length;i++)ps[i].classList.toggle('on',ps[i].id===t);"
  "var as=document.querySelectorAll('.nav A');for(var i=0;i<as.length;i++)as[i].classList.toggle('on',as[i].getAttribute('data-tab')===t);"
  "specActive=(t==='spectrum');if(specActive){fitCanvas();redraw();if(specRun&&!scanning)scanOnce();}"
  "else if(specRun)fetch('/api/scan?auto=0').catch(function(){});}"
/* ---- control helpers ---- */
"function fmtStep(k){return k>=1000?((k%1000?(k/1000).toFixed(1):(k/1000))+'M'):(k+'k');}"
"var lastSteps='';"
"function renderSteps(a){if(!a)return;var key=a.join(',');if(key===lastSteps)return;lastSteps=key;"
  "var c=$('stepbtns');if(!c)return;c.innerHTML='';"
  "for(var i=0;i<a.length;i++){var b=document.createElement('BUTTON');b.textContent=fmtStep(a[i]);"
  "b.onclick=(function(k){return function(){setStep(k);};})(a[i]);c.appendChild(b);}}"
"var curAgcAuto=false,curMuted=false;"
"function toggleAgc(){setVal('agc',curAgcAuto?0:1);}"
"function toggleMute(){setVal('mute',curMuted?0:1);}"
"var lastModes='';"
"function renderModes(d){if(!d.modes)return;var c=$('modebtns');if(!c)return;"
  "var key=d.modes.map(function(m){return m.i;}).join(',');"
  "if(key!==lastModes){lastModes=key;c.innerHTML='';"
    "d.modes.forEach(function(m){var b=document.createElement('BUTTON');b.textContent=m.n;b.setAttribute('data-i',m.i);"
    "b.onclick=(function(i){return function(){setVal('mode',i);};})(m.i);c.appendChild(b);});}"
  "var bs=c.children;for(var i=0;i<bs.length;i++){bs[i].classList.toggle('acc',+bs[i].getAttribute('data-i')===d.modeIdx);}}"
"var lastBands='';"
"function fmt1(x){return x%1?x.toFixed(1):x.toFixed(0);}"
"function bandRange(b){if(b.fm)return fmt1(b.lo/100)+' ~ '+fmt1(b.hi/100)+' MHz';"
  "if(b.hi>=1000000)return fmt1(b.lo/1000)+' ~ '+fmt1(b.hi/1000)+' MHz';"
  "return fmtFreq(b.lo)+' ~ '+fmtFreq(b.hi)+' kHz';}"
"function renderBands(d){if(!d.bands)return;var sel=$('bandsel');if(!sel)return;"
  "var key=d.bands.map(function(b){return b.i;}).join(',');"
  "if(key!==lastBands&&sel!==document.activeElement){lastBands=key;sel.innerHTML='';"
    "d.bands.forEach(function(b){var o=document.createElement('OPTION');o.value=b.i;"
    "o.textContent=b.n+' ('+bandRange(b)+')';sel.appendChild(o);});}"
  "if(sel!==document.activeElement)sel.value=d.bandIdx;}"
"function grp(p){return p>=66?'fill g':p>=33?'fill a':'fill r';}"
"function meter(fi,vi,p,t){var f=$(fi);if(!f)return;p=Math.max(0,Math.min(100,p));"
  "f.style.width=p+'%';f.className=grp(p);txt(vi,t);}"
"function bdg(id,on,warn){var e=$(id);if(e)e.className='badge'+(on?(warn?' warn':' on'):'');}"
/* ---- status poll ---- */
"function poll(){fetch('/api/status').then(r=>r.json()).then(d=>{"
  "lastUnit=d.unit;gFm=(d.unit=='MHz');gFreqHz=d.freqHz;"
  "var fe=$('freq');if(fe)fe.innerHTML=fmtFreq(d.freq)+\"<span class='u'>\"+d.unit+\"</span>\";"
  "var me=$('meta');if(me)me.innerHTML='<b>'+d.band+'</b> &middot; '+d.mode;"
  "txt('setunit',d.unit);txt('sethint','Enter the frequency in '+d.unit+'.');"
  "var fq=$('freqval');if(fq){fq.placeholder='Set frequency ('+d.unit+')';if(fq!==document.activeElement)fq.value=fmtFreq(d.freq);}"
  "meter('rssiFill','rssiVal',d.rssi/90*100,d.rssi+' dB\\u00b5V');"
  "meter('snrFill','snrVal',d.snr/30*100,d.snr+' dB');"
  "meter('battFill','battVal',(d.batt-3.0)/1.2*100,d.batt+' V');"
  "bdg('bdgWifi',d.wifi>0);bdg('bdgBle',d.ble>0);bdg('bdgRds',!!(d.station&&d.station.length));"
  "bdg('bdgMorse',d.morse>0);bdg('bdgOvr',d.override,true);bdg('bdgMute',d.muted,true);"
  "txt('step',d.step);txt('bw',d.bw);txt('vol',d.muted?'Muted':d.vol);txt('agc',d.agcAuto?'AGC':('Att '+d.agc));"
  "var sb=$('stepbox');if(sb)sb.textContent=d.step;renderSteps(d.steps);"
  "curAgcAuto=!!d.agcAuto;curMuted=!!d.muted;"
  "var ab=$('agcbtn');if(ab)ab.classList.toggle('acc',curAgcAuto);"
  "var mb=$('mutebtn');if(mb)mb.classList.toggle('acc',curMuted);"
  "renderModes(d);renderBands(d);"
  "txt('station',d.station||'-');txt('rt',d.rt||'-');txt('morsetext',d.morseText||'-');"
  "var st=(d.station||'').trim(),rtx=(d.rt||'').trim();"
  "var ts=$('topStation');if(ts){ts.textContent=st;ts.style.display=st?'block':'none';}"
  "var trt=$('topRt');if(trt){trt.textContent=rtx;trt.style.display=rtx?'block':'none';}"
  "var ms=$('morse');if(ms&&ms!==document.activeElement)ms.value=d.morse;"
  "var ov=$('override');if(ov&&ov!==document.activeElement)ov.checked=d.override;"
  "var mh=$('morsehint');if(mh)mh.style.color=(d.morse==2&&!d.morseAudio)?'var(--bad)':'';"
  "cfgApply(d);"
  "}).catch(e=>{});}"
/* ---- config ---- */
"function sset(k,v){fetch('/api/set?'+k+'='+encodeURIComponent(v));}"
"function csel(id,v){var e=$(id);if(e&&e!==document.activeElement)e.value=v;}"
"function cfgApply(d){csel('brt',d.brt);csel('sleep',d.sleep);csel('theme',d.theme);csel('ui',d.ui);"
  "csel('sleepmode',d.sleepMode);csel('rds',d.rdsMode);csel('region',d.region);csel('utc',d.utc);"
  "csel('usb',d.usb);csel('ble',d.ble);csel('wifi',d.wifi);"
  "var z=$('zoom');if(z&&z!==document.activeElement)z.checked=d.zoom;"
  "var s=$('scroll');if(s&&s!==document.activeElement)s.checked=d.scroll;"
  "var bv=$('brtv');if(bv)bv.textContent=d.brt;var sv=$('sleepv');if(sv)sv.textContent=d.sleep?d.sleep+'s':'Off';}"
"function cfgLoad(){poll();}"
/* ---- memory ---- */
"function mfmt(hz,m){return m=='FM'?(hz/1e6).toFixed(2)+' MHz':(hz/1000).toFixed(0)+' kHz';}"
"function mload(){fetch('/api/memory').then(r=>r.json()).then(d=>{"
  "var t=$('mlist');if(!t)return;t.innerHTML='';"
  "txt('mcount','('+d.used.length+'/'+d.total+')');"
  "if(!d.used.length){t.innerHTML=\"<TR><TD COLSPAN=4 CLASS='CENTER'>No stations stored</TD></TR>\";return;}"
  "d.used.forEach(function(m){var tr=document.createElement('tr');"
    "tr.innerHTML=\"<TD CLASS='LABEL'>\"+(m.s<10?'0':'')+m.s+\"</TD><TD>\"+mfmt(m.f,m.m)+\"</TD>\"+"
    "\"<TD>\"+m.b+' '+m.m+\"</TD><TD><BUTTON onclick='act(\\\"tune\\\",\"+m.s+\")'>Tune</BUTTON> \"+"
    "\"<BUTTON onclick='act(\\\"clear\\\",\"+m.s+\")'>Clear</BUTTON></TD>\";t.appendChild(tr);});"
  "}).catch(e=>{});}"
"function mrefresh(){setTimeout(mload,600);}"
"function act(a,s){fetch('/api/mem?action='+a+'&slot='+s).then(mrefresh);}"
"function saveCur(){var s=$('saveslot').value;fetch('/api/mem?action=save&slot='+s).then(mrefresh);}"
"function setSlot(){var s=$('eslot').value,b=$('eband').value,m=$('emode').value,v=parseFloat($('efreq').value);"
  "if(isNaN(v)){alert('Enter a frequency');return;}"
  "var hz=$('eunit').value=='M'?Math.round(v*1e6):Math.round(v*1000);"
  "fetch('/api/mem?action=set&slot='+s+'&band='+encodeURIComponent(b)+'&hz='+hz+'&mode='+encodeURIComponent(m)).then(mrefresh);}"
/* ---- colormaps ---- */
"var CM={viridis:[[68,1,84],[59,82,139],[33,145,140],[94,201,98],[253,231,37]],"
  "inferno:[[0,0,4],[87,16,110],[188,55,84],[249,142,9],[252,255,164]],"
  "gray:[[0,0,0],[64,64,64],[128,128,128],[192,192,192],[255,255,255]]};"
"function cmap(v){v=Math.max(0,Math.min(0.999,v));var st=CM[$('cmap').value]||CM.viridis;"
  "var f=v*(st.length-1),i=Math.floor(f),t=f-i,a=st[i],b=st[i+1];"
  "return[a[0]+(b[0]-a[0])*t,a[1]+(b[1]-a[1])*t,a[2]+(b[2]-a[2])*t];}"
/* ---- spectrum/scan ---- */
"function lockRadio(on){var nn=document.querySelectorAll('.lockable');"
  "for(var i=0;i<nn.length;i++)nn[i].classList.toggle('locked',on);"
  "var ban=$('wfban');if(ban)ban.style.display=on?'block':'none';}"
"var specRun=false,scanning=false,scanPollT=null,lastScan=null;"
"var peakHold=null,avgAcc=null,avgN=0,wfRows=[],detPeaks=[];"
"function setRunBtn(){var b=$('runbtn');if(b){b.textContent=specRun?'Stop':'Run';b.classList.toggle('acc',specRun);}txt('specstat',specRun?'running':'idle');}"
"function setRun(on){specRun=on;setRunBtn();lockRadio(on);"
  "fetch('/api/scan?auto='+(on?1:0)).catch(function(){});"
  "if(on){if(!scanning)scanOnce();}else if(scanPollT){clearTimeout(scanPollT);scanPollT=null;}}"
"function single(){if(specRun)setRun(false);if(!scanning)scanOnce();}"
"function setStepPreset(s){curStep=s;var bs=document.querySelectorAll('.sdrbar BUTTON[data-sp]');"
  "for(var i=0;i<bs.length;i++)bs[i].classList.toggle('acc',+bs[i].getAttribute('data-sp')===s);"
  "$('spaninp').value='';wfRows=[];fetch('/api/scan?span='+s).catch(function(){});}"
"function setSpanKHz(){var v=parseFloat($('spaninp').value);if(isNaN(v)||v<=0)return;"
  "var uk=gFm?10:1,pts=lastScan?lastScan.count:WP;var st=Math.round(v/pts/uk);"
  "st=Math.max(1,Math.min(200,st));curStep=st;wfRows=[];fetch('/api/scan?span='+st).catch(function(){});}"
"var curStep=2;"
"function resetPeak(){peakHold=null;avgAcc=null;avgN=0;redraw();}"
"function toggleWf(){fitCanvas();redraw();}"
"function scanOnce(){if(scanning)return;scanning=true;txt('specstat',specRun?'running':'sweep');"
  "fetch('/api/scan?run=1'+(specRun?'&auto=1':'')).then(function(){pollScan(0);}).catch(function(){"
  "scanPollT=setTimeout(function(){pollScan(0);},150);});}"
"function pollScan(tries){scanPollT=null;fetch('/api/scan').then(r=>r.json()).then(d=>{"
  "if(d.busy){if(tries<240)scanPollT=setTimeout(function(){pollScan(tries+1);},120);else scanDone();return;}"
  "if(d.count>0)procScan(d);scanDone();"
  "if(specRun&&specActive)scanPollT=setTimeout(scanOnce,60);"
  "}).catch(function(){if(tries<240)scanPollT=setTimeout(function(){pollScan(tries+1);},120);else scanDone();});}"
"function scanDone(){scanning=false;if(!specRun)txt('specstat','idle');}"
"function procScan(d){lastScan=d;var n=d.count,a=d.rssi;"
  "if(!peakHold||peakHold.length!==n){peakHold=a.slice();avgAcc=a.slice();avgN=1;}"
  "else{for(var i=0;i<n;i++){if(a[i]>peakHold[i])peakHold[i]=a[i];avgAcc[i]+=a[i];}avgN++;}"
  "detectPeaks(d);pushWf(d);redraw();updateReadout(d);}"
"function detectPeaks(d){var a=d.rssi,n=d.count;detPeaks=[];if(n<3)return;"
  "var srt=a.slice().sort(function(x,y){return x-y;});var floor=srt[Math.floor(n*0.4)];"
  "var mx=srt[n-1],th=floor+Math.max(4,(mx-floor)*0.35);"
  "for(var i=1;i<n-1;i++){if(a[i]>=th&&a[i]>=a[i-1]&&a[i]>a[i+1])detPeaks.push(i);}}"
"function pushWf(d){if(!$('tWf').checked)return;var c=$('wfs'),W=c.width;if(!W)return;"
  "var a=d.rssi,n=d.count,mn=Math.min.apply(null,a),mx=Math.max.apply(null,a),rg=(mx-mn)||1;"
  "var row=new Float32Array(W);for(var i=0;i<W;i++){var fi=i/(W-1)*(n-1),lo=Math.floor(fi),t=fi-lo;"
  "var v0=a[lo],v1=a[Math.min(n-1,lo+1)];row[i]=((v0+(v1-v0)*t)-mn)/rg;}"
  "wfRows.unshift(row);if(wfRows.length>c.height)wfRows.pop();}"
/* frequency<->x mapping shared by spectrum and waterfall */
"function xToHz(x,W){if(!lastScan)return 0;var n=lastScan.count;var fi=x/W*(n-1);"
  "return lastScan.startHz+lastScan.stepHz*fi;}"
"function hzToX(hz,W){if(!lastScan)return -1;var n=lastScan.count;"
  "var fi=(hz-lastScan.startHz)/lastScan.stepHz;return fi/(n-1)*W;}"
"function gridLines(){if(!lastScan)return[];var n=lastScan.count;var lo=lastScan.startHz,hi=lastScan.startHz+lastScan.stepHz*(n-1);"
  "var span=hi-lo;if(span<=0)return[];var steps=[1e5,2e5,5e5,1e6,2e6,5e6,1e7];var st=steps[0];"
  "for(var i=0;i<steps.length;i++){if(span/steps[i]<=8){st=steps[i];break;}st=steps[i];}"
  "var g=[],f=Math.ceil(lo/st)*st;for(;f<=hi;f+=st)g.push(f);return g;}"
"function fmtMHz(hz){return(hz/1e6).toFixed(hz%1e6?2:1);}"
"function redraw(){drawSpec();drawWf();}"
"function drawSpec(){var c=$('spec');if(!c)return;var x=c.getContext('2d'),W=c.width,H=c.height;"
  "x.clearRect(0,0,W,H);x.fillStyle='#070b10';x.fillRect(0,0,W,H);"
  "if(!lastScan){return;}var d=lastScan,n=d.count;"
  "var all=d.rssi.concat(peakHold||[]);var mn=Math.min.apply(null,all),mx=Math.max.apply(null,all);var rg=(mx-mn)||1;"
  "var pad=18;function Y(v){return H-pad-((v-mn)/rg)*(H-pad*2);}"
  /* grid + labels */
  "x.strokeStyle='rgba(255,178,74,0.25)';x.fillStyle='#8a99a8';x.font='10px monospace';x.lineWidth=1;"
  "var gl=gridLines(),lab=$('tLabels').checked;"
  "for(var i=0;i<gl.length;i++){var gx=Math.round(hzToX(gl[i],W))+0.5;x.beginPath();x.moveTo(gx,0);x.lineTo(gx,H);x.stroke();"
  "if(lab){x.fillText(fmtMHz(gl[i])+' MHz',gx+2,11);}}"
  /* SNR trace (own scale) */
  "if($('tSnr').checked&&d.snr){var s=d.snr,smn=Math.min.apply(null,s),smx=Math.max.apply(null,s),srg=(smx-smn)||1;"
  "x.strokeStyle='#37d67a';x.lineWidth=1.5;x.beginPath();"
  "for(var i=0;i<n;i++){var px=i/(n-1)*W,py=H-pad-((s[i]-smn)/srg)*(H-pad*2)*0.85;i?x.lineTo(px,py):x.moveTo(px,py);}x.stroke();}"
  /* Avg trace */
  "if($('tAvg').checked&&avgAcc&&avgN){x.strokeStyle='#16c79a';x.lineWidth=1;x.beginPath();"
  "for(var i=0;i<n;i++){var px=i/(n-1)*W,py=Y(avgAcc[i]/avgN);i?x.lineTo(px,py):x.moveTo(px,py);}x.stroke();}"
  /* Peak hold */
  "if($('tPeak').checked&&peakHold){x.strokeStyle='#ffb24a';x.lineWidth=1.5;x.beginPath();"
  "for(var i=0;i<n;i++){var px=i/(n-1)*W,py=Y(peakHold[i]);i?x.lineTo(px,py):x.moveTo(px,py);}x.stroke();}"
  /* current level (blue, filled) */
  "x.strokeStyle='#5aa9ff';x.fillStyle='rgba(90,169,255,0.18)';x.lineWidth=1.5;x.beginPath();x.moveTo(0,H);"
  "for(var i=0;i<n;i++){var px=i/(n-1)*W,py=Y(d.rssi[i]);x.lineTo(px,py);}x.lineTo(W,H);x.closePath();x.fill();"
  "x.beginPath();for(var i=0;i<n;i++){var px=i/(n-1)*W,py=Y(d.rssi[i]);i?x.lineTo(px,py):x.moveTo(px,py);}x.stroke();"
  /* detected peaks */
  "x.fillStyle='#ffd27a';for(var i=0;i<detPeaks.length;i++){var pi=detPeaks[i],px=pi/(n-1)*W,py=Y(d.rssi[pi]);"
  "x.beginPath();x.arc(px,py-4,2.5,0,7);x.fill();}"
  /* tuned marker (dashed) */
  "var tx=hzToX(gFreqHz,W);if(tx>=0&&tx<=W){x.strokeStyle='#e7eef5';x.setLineDash([5,4]);x.lineWidth=1;"
  "x.beginPath();x.moveTo(tx,0);x.lineTo(tx,H);x.stroke();x.setLineDash([]);"
  "if(lab){x.fillStyle='#e7eef5';x.fillText('Tuned '+fmtMHz(gFreqHz)+' MHz',Math.min(tx+3,W-90),H-4);}}}"
"function drawWf(){var c=$('wfs');if(!c)return;c.style.display=$('tWf').checked?'block':'none';"
  "if(!$('tWf').checked)return;var x=c.getContext('2d'),W=c.width,H=c.height;"
  "var img=x.createImageData(W,H);for(var y=0;y<H;y++){var row=wfRows[y];for(var i=0;i<W;i++){"
  "var v=row?row[i]:0,col=cmap(v),o=(y*W+i)*4;img.data[o]=col[0];img.data[o+1]=col[1];img.data[o+2]=col[2];img.data[o+3]=255;}}"
  "x.putImageData(img,0,0);"
  "x.fillStyle='#cfe2f5';x.font='10px monospace';var gl=gridLines();"
  "for(var i=0;i<gl.length;i++){var gx=Math.round(hzToX(gl[i],W));x.strokeStyle='rgba(255,255,255,0.15)';"
  "x.beginPath();x.moveTo(gx+0.5,0);x.lineTo(gx+0.5,H);x.stroke();"
  "if($('tLabels').checked)x.fillText(fmtMHz(gl[i]),gx+2,H-3);}}"
"var lastReadout='',lastRes='';"
"function updateReadout(d){var n=d.count,lo=d.startHz,hi=d.startHz+d.stepHz*(n-1);var ctr=(lo+hi)/2;"
  "var html='Center <b>'+fmtMHz(ctr)+' MHz</b> &nbsp; "
  "Span <b>'+((hi-lo)/1e6).toFixed(3)+' MHz</b> &nbsp; Resolution <b>'+(d.stepHz/1000)+' kHz/pt</b> &nbsp; <b>'+n+'</b> pts';"
  "if(html!==lastReadout){lastReadout=html;var r=$('readout');if(r)r.innerHTML=html;}"
  "var res=''+(d.stepHz/1000);if(res!==lastRes){lastRes=res;var rv=$('resv');if(rv)rv.textContent=res;}}"
/* canvas click -> tune */
"function canvClick(c,ev){if(!lastScan)return;var rc=c.getBoundingClientRect();var x=(ev.clientX-rc.left)/rc.width*c.width;"
  "var hz=xToHz(x,c.width);var n=lastScan.count,best=-1,bd=1e18;"
  "for(var i=0;i<detPeaks.length;i++){var phz=lastScan.startHz+lastScan.stepHz*detPeaks[i];var dd=Math.abs(phz-hz);"
  "if(dd<bd){bd=dd;best=phz;}}"
  "var span=lastScan.stepHz*(n-1);if(best>=0&&bd<span*0.03)hz=best;tuneHz(hz);}"
"function fitCanvas(){var s=$('spec'),w=$('wfs');if(!s)return;var W=s.clientWidth||800;"
  "s.width=W;s.height=300;w.width=W;w.height=240;}"
/* ---- init ---- */
"window.addEventListener('resize',function(){if(specActive){fitCanvas();redraw();}});"
"$('spec').addEventListener('click',function(e){canvClick(this,e);});"
"$('wfs').addEventListener('click',function(e){canvClick(this,e);});"
"function fmtHov(hz){return gFm?(hz/1e6).toFixed(3)+' MHz':(hz/1000).toFixed(0)+' kHz';}"
"function hover(c,ev){if(!lastScan)return;var st=$('specstack'),sr=st.getBoundingClientRect(),cr=c.getBoundingClientRect();"
  "var frac=(ev.clientX-cr.left)/cr.width;frac=Math.max(0,Math.min(1,frac));"
  "var n=lastScan.count,hz=lastScan.startHz+lastScan.stepHz*frac*(n-1),xpx=ev.clientX-sr.left;"
  "var xl=$('xline');xl.style.left=xpx+'px';xl.style.height=st.clientHeight+'px';xl.style.display='block';"
  "var xh=$('xhair');xh.textContent=fmtHov(hz);xh.style.left=xpx+'px';xh.style.display='block';}"
"function hoverOut(){var xl=$('xline');if(xl)xl.style.display='none';var xh=$('xhair');if(xh)xh.style.display='none';}"
"$('spec').addEventListener('mousemove',function(e){hover(this,e);});"
"$('wfs').addEventListener('mousemove',function(e){hover(this,e);});"
"$('specstack').addEventListener('mouseleave',hoverOut);"
"var p=location.pathname;if(p.indexOf('memory')>=0)showTab('memory');else if(p.indexOf('config')>=0)showTab('config');"
"else showTab('spectrum');"
"setStepPreset(2);setRunBtn();txt('specstat','idle');"
"setInterval(poll,1000);poll();setInterval(mload,3000);mload();scanOnce();"
"</SCRIPT>";

  return webPage(body);
}
