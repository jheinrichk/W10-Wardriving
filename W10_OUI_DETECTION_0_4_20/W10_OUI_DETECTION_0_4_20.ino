#include <Arduino.h>
#include <Wire.h>
#include <FS.h>
#include <LittleFS.h>
#include <Arduino_GFX_Library.h>
#include <Adafruit_MCP23X17.h>
#include <TinyGPS++.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <SPI.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <cstring>
#include <cctype>
#include <strings.h>

struct AdvFields {
  uint16_t companyId=0xFFFF;
  uint16_t serviceId=0;
  bool hasMetaService=false;
  bool hasName=false;
  uint16_t serviceIds[20]={};
  uint8_t serviceCount=0;
  char name[48]="";
  bool malformed=false;
  char service128[4][33]={};
  uint8_t service128Count=0;

  bool appleFindMy=false;
  bool googleFmdn=false;
  bool axonBwcTag=false;

  bool ridPresent=false;
  bool ridHasLocation=false;
  uint8_t ridIdType=0, ridUaType=0;
  char ridSerial[21]="";
  double ridLat=0, ridLon=0;
  float ridAltM=0;
};

#include <cmath>
#include <stdint.h>
#include <ESP_I2S.h>
#include <es8311.h>

namespace W10Audio {
enum Kind : uint8_t { NEW_MATCH=1, REACQUIRED=2, PRIORITY=3, SUSTAINED=4, READY=5 };
struct Event { uint8_t kind; };
static constexpr unsigned SAMPLE_RATE = 44100;
static constexpr float W10_AUDIO_TWO_PI = 6.2831853071795864769f;
static I2SClass i2s;
static es8311_handle_t codec = nullptr;
static QueueHandle_t queue = nullptr;
static volatile bool enabled = true;
static bool ready = false;

static constexpr uint8_t VOLUME_DEFAULT = 70;
static constexpr uint8_t VOLUME_MAX = 85;
static constexpr float DIGITAL_GAIN = 26000.0f;
static uint8_t volume = VOLUME_DEFAULT;

static uint8_t repeatsFor(Kind k) { return k == PRIORITY ? 3 : k == NEW_MATCH ? 2 : 1; }
static volatile uint32_t played=0, suppressed=0;
static float sineTable[256];
static inline float fastSin(float angle){
  float x=angle*(256.0f/W10_AUDIO_TWO_PI);
  uint32_t i=(uint32_t)x;float frac=x-(float)i;
  float a=sineTable[i&255],b=sineTable[(i+1)&255];
  return a+(b-a)*frac;
}
static Adafruit_MCP23X17* amplifierGpio = nullptr;

static void renderOnce(Kind kind) {
  float seconds = kind == PRIORITY ? 0.76f : kind == SUSTAINED ? 0.91f :
                  kind == REACQUIRED ? 0.47f : kind == READY ? 0.55f : 0.62f;
  const uint32_t total = (uint32_t)(seconds * SAMPLE_RATE);
  int16_t pcm[256*2];
  float phi=0, phi2=0, phi3=0;
  for(uint32_t offset=0; offset<total; offset+=256){
    const uint32_t n = min((uint32_t)256, total-offset);
    for(uint32_t i=0;i<n;i++){
      float t=(float)(offset+i)/SAMPLE_RATE;
      float u=t/seconds;
      float attack=fminf(1.0f,t/0.018f);
      float decay=(1.0f-u)*(1.0f-u);
      float env=attack*decay;
      float hz=660.0f, h2=990.0f, h3=1320.0f;
      if(kind==NEW_MATCH){

        hz=850.0f-300.0f*u; h2=1275.0f-450.0f*u;h3=1700.0f-620.0f*u;
      }else if(kind==REACQUIRED){

        hz=520.0f-140.0f*u;h2=780.0f-210.0f*u;h3=1040.0f-300.0f*u;
        env*=0.57f;
      }else if(kind==PRIORITY){

        hz=430.0f+440.0f*u;h2=645.0f+660.0f*u;h3=860.0f+880.0f*u;
        float swell=(fastSin(fmodf(W10_AUDIO_TWO_PI*2.6f*t,W10_AUDIO_TWO_PI))*0.30f+0.70f);
        env*=swell;
      }else if(kind==READY){

        const float step = u < 0.33f ? 523.0f : u < 0.66f ? 659.0f : 784.0f;
        hz=step; h2=step*1.5f; h3=step*2.0f;
        env=fminf(1.0f,fmodf(t,0.1833f)/0.012f)*(1.0f-0.35f*u);
      }else{

        hz=380.0f+280.0f*u;h2=554.0f+400.0f*u;h3=760.0f+560.0f*u;
        env*=(0.82f+0.18f*fastSin(fmodf(W10_AUDIO_TWO_PI*3.2f*t,W10_AUDIO_TWO_PI)));
      }
      phi+=W10_AUDIO_TWO_PI*hz/SAMPLE_RATE;phi2+=W10_AUDIO_TWO_PI*h2/SAMPLE_RATE;phi3+=W10_AUDIO_TWO_PI*h3/SAMPLE_RATE;
      if(phi>W10_AUDIO_TWO_PI)phi-=W10_AUDIO_TWO_PI;
      if(phi2>W10_AUDIO_TWO_PI)phi2-=W10_AUDIO_TWO_PI;
      if(phi3>W10_AUDIO_TWO_PI)phi3-=W10_AUDIO_TWO_PI;
      float echo= (t>0.13f) ? 0.12f*fastSin(phi*0.5f) : 0.0f;
      float sample=(0.69f*fastSin(phi)+0.22f*fastSin(phi2)+0.08f*fastSin(phi3)+echo)*env;
      int16_t v=(int16_t)(fmaxf(-1.0f,fminf(1.0f,sample))*DIGITAL_GAIN);
      pcm[2*i]=v;pcm[2*i+1]=v;
    }
    if(!enabled)break;
    i2s.write((uint8_t*)pcm,(size_t)n*2*sizeof(int16_t));
  }

  memset(pcm,0,sizeof(pcm));
  i2s.write((uint8_t*)pcm,sizeof(pcm));
}
static void render(Kind kind) {
  const uint8_t repeats = repeatsFor(kind);
  for (uint8_t i = 0; i < repeats && enabled; ++i) {
    renderOnce(kind);
    if (i + 1 < repeats) {

      int16_t gap[256*2]; memset(gap, 0, sizeof(gap));
      for (unsigned b = 0; b < 16; ++b) i2s.write((uint8_t*)gap, sizeof(gap));
    }
  }
}

static void worker(void*) {
  Event event;
  while(true){
    if(xQueueReceive(queue,&event,portMAX_DELAY)==pdTRUE){
      if(ready && enabled){ render((Kind)event.kind); ++played; }
    }
  }
}

static bool begin(Adafruit_MCP23X17& mcp) {
  amplifierGpio=&mcp;
  for(unsigned i=0;i<256;i++)sineTable[i]=sinf(W10_AUDIO_TWO_PI*i/256.0f);

  amplifierGpio->pinMode(7,OUTPUT);
  amplifierGpio->digitalWrite(7,LOW);
  codec=es8311_create(I2C_NUM_0,ES8311_ADDRESS_0);
  if(!codec){Serial.println("AUDIO ERROR: ES8311 handle unavailable");return false;}
  const es8311_clock_config_t clock={false,false,true,SAMPLE_RATE*256,SAMPLE_RATE};
  esp_err_t ret=es8311_init(codec,&clock,ES8311_RESOLUTION_16,ES8311_RESOLUTION_16);
  if(ret!=ESP_OK){Serial.printf("AUDIO ERROR: codec init %d\n",(int)ret);return false;}
  es8311_voice_volume_set(codec,volume,nullptr);
  es8311_voice_mute(codec,false);

  Wire.beginTransmission(ES8311_ADDRESS_0);
  Wire.write((uint8_t)0x13);Wire.write((uint8_t)0x00);
  if(Wire.endTransmission()!=0){Serial.println("AUDIO ERROR: ES8311 line out config");return false;}
  i2s.setPins(2,4,5,-1,1);
  if(!i2s.begin(I2S_MODE_STD,SAMPLE_RATE,I2S_DATA_BIT_WIDTH_16BIT,
                I2S_SLOT_MODE_STEREO,I2S_STD_SLOT_BOTH)) {
    Serial.println("AUDIO ERROR: I2S failed");return false;
  }
  queue=xQueueCreate(6,sizeof(Event));
  if(!queue){Serial.println("AUDIO ERROR: queue unavailable");return false;}
  ready=true;
  amplifierGpio->digitalWrite(7,HIGH);
  if(xTaskCreatePinnedToCore(worker,"w10_sound",6144,nullptr,1,nullptr,0)!=pdPASS) {
    ready=false;amplifierGpio->digitalWrite(7,LOW);
    Serial.println("AUDIO ERROR: task allocation failed");return false;
  }
  Serial.printf("AUDIO READY: ES8311 + I2S, %u%% volume. SOUND TEST NEW to preview\n",(unsigned)volume);
  return true;
}

static bool schedule(Kind sound){
  if(!ready || !enabled || !queue)return false;

  Event ev={(uint8_t)sound};
  if(xQueueSend(queue,&ev,0)==pdTRUE)return true;
  ++suppressed;return false;
}
static void setEnabled(bool on) {
  enabled=on;
  if(queue && !on) xQueueReset(queue);
}
static bool setVolume(int v) {
  if(v<0||v>(int)VOLUME_MAX||!ready||!codec)return false;
  volume=(uint8_t)v;
  return es8311_voice_volume_set(codec,volume,nullptr)==ESP_OK;
}

static void presetVolume(int v) { if (v >= 0 && v <= (int)VOLUME_MAX) volume = (uint8_t)v; }
static const char* kindName(Kind k){
  switch(k){case NEW_MATCH:return "new";case REACQUIRED:return "repeat";
             case PRIORITY:return "priority";case SUSTAINED:return "sustained";case READY:return "ready";}
  return "invalid";
}
}

static constexpr int GPS_RX = 44, GPS_TX = 43;
static constexpr int I2C_SDA = 8, I2C_SCL = 7;
static constexpr int SPI_MISO_PIN = 11, SPI_SCLK_PIN = 12, SPI_MOSI_PIN = 13;
static constexpr int LORA_CS = 14, TFT_CS = 10, TFT_DC = 16;
static constexpr int TFT_RST = 10, TFT_BL = 6;

static constexpr uint8_t RGB_STATUS_PIN = 48;

static constexpr uint8_t AXP2101_I2C_ADDR = 0x34;
static constexpr uint8_t AXP2101_REG_CHGLED = 0x69;
static constexpr bool CHARGE_LED_OFF_AT_BOOT = true;
static constexpr uint32_t RGB_SLOT_MS = 2000;
static constexpr uint32_t RGB_ON_MS = 350;
static constexpr uint32_t RGB_NEW_MS = 650;
static constexpr uint8_t RGB_MAX_BRIGHTNESS = 96;
static constexpr uint32_t GPS_BAUD = 9600;
static constexpr uint32_t GPS_MAX_AGE_MS = 5000;
static constexpr uint32_t DISPLAY_PERIOD_MS = 1000;
static constexpr uint32_t WATCH_TIMEOUT_MS = 8000;

static constexpr uint16_t BLE_SCAN_INTERVAL_MS = 100;
static constexpr uint16_t BLE_SCAN_WINDOW_FULL_MS = 100;

static constexpr uint16_t BLE_SCAN_WINDOW_COEX_MS = 50;

static constexpr uint16_t BLE_SCAN_WINDOW_AP_MS = 20;
static constexpr uint32_t BLE_SCAN_STALL_MS = 10000;

static constexpr uint32_t BLE_QUIET_FOR_WIFI_MS = 8000;
static constexpr uint32_t SUSTAINED_MS = 60000;
static constexpr uint16_t MAX_WATCH_RULES = 160;

static constexpr uint16_t UI_BLACK = 0x0000;
static constexpr uint16_t UI_WHITE = 0xFFFF;
static constexpr uint16_t UI_CYAN = 0x07FF;
static constexpr uint16_t UI_GREEN = 0x07E0;
static constexpr uint16_t UI_AMBER = 0xFD20;
static constexpr uint16_t UI_RED = 0xF800;
static constexpr uint16_t UI_BLUE = 0x001F;
static constexpr uint16_t UI_DARK_BLUE = 0x0010;
static constexpr uint16_t UI_DARK_RED = 0x3000;
static constexpr uint16_t UI_DIM = 0x8410;
static constexpr uint16_t UI_DARK_AMBER = 0x4100;
static constexpr uint16_t UI_ORANGE = 0xFC00;
static constexpr uint16_t UI_MAGENTA = 0xF81F;
static constexpr uint16_t UI_LIGHT_BLUE = 0x3DFF;
static constexpr uint16_t UI_YELLOW = 0xFFE0;

static constexpr int HIT_BAR_X = 118, HIT_BAR_Y = 53, HIT_BAR_W = 116, HIT_BAR_H = 12;
static uint16_t hitBarColors[HIT_BAR_W];
static unsigned hitBarCount = 0, hitBarHead = 0;
static uint32_t hitBarVersion = 0;
static constexpr uint32_t STATUS_PERIOD_MS = 5000;

static constexpr uint32_t REPEAT_REPORT_MIN_MS = 60000;

static constexpr uint32_t FLASH_TOTAL_MS = 2000;
static constexpr uint32_t FLASH_PERIOD_MS = 125;

static constexpr uint32_t TICKER_STEP_MS = 220;
static constexpr uint32_t TICKER_HOLD_MS = 1200;

static constexpr int BOOT_KEY_PIN = 0;
static constexpr uint32_t KEY_DEBOUNCE_MS = 40;
static constexpr uint32_t KEY_LONG_MS = 1000;
static constexpr uint32_t PMU_KEY_POLL_MS = 100;
static constexpr uint8_t AXP2101_REG_INTEN2 = 0x41, AXP2101_REG_INTSTS2 = 0x49;

static constexpr uint32_t STARTUP_MENU_MS = 8000;
static constexpr uint32_t MENU_IDLE_MS = 25000;
static constexpr int MENU_ROW_Y0 = 22, MENU_ROW_H = 33;
static constexpr uint16_t MAX_DEVICES = 256;
static constexpr uint16_t BLE_QUEUE_DEPTH = 192;
static constexpr uint16_t BLE_RAW_MAX = 128;

static constexpr uint32_t BLE_SCAN_CYCLE_MS = 10000;

static constexpr uint32_t SCAN_RSP_MERGE_MS = 1000;

static HardwareSerial gpsSerial(2);
static TinyGPSPlus gps;
static Adafruit_MCP23X17 expander;
static Arduino_DataBus* tftBus = new Arduino_ESP32SPI(TFT_DC, TFT_CS,
                             SPI_SCLK_PIN, SPI_MOSI_PIN, SPI_MISO_PIN);
static Arduino_GFX* tft = new Arduino_ST7789(tftBus, TFT_RST, 0, true,
                                             240, 240, 0, 0);
static bool tftOK = false;
static bool expanderOK = false;

struct AdvRecord {
  char mac[18];
  int8_t rssi;
  uint8_t addrType;
  uint8_t payloadLength;
  bool truncated;

  bool scanRsp;
  uint8_t bytes[BLE_RAW_MAX];
  uint32_t timeMs;
};

struct TrackRecord {
  char mac[24];
  uint8_t addrType;
  uint32_t id;
  uint32_t hits;
  uint32_t packets;
  bool shown;
  uint32_t lastReported;
  uint32_t lastSeen;
  int8_t minRssi;
  int8_t maxRssi;
  bool used;
  bool priority;
  bool presenceEscalated;
  uint32_t continuousStart;
  int8_t recentRssi;
  char label[72];
  char method[24];
  char identifier[44];
};

static QueueHandle_t bleQueue = nullptr;
static NimBLEScan* bleScanner = nullptr;
static TrackRecord tracks[MAX_DEVICES] = {};
static uint32_t nextId = 1;
static uint32_t seenReports = 0;
static uint32_t matchedReports = 0;
static uint32_t duplicateSuppressed = 0;
static volatile uint32_t droppedQueue = 0;
static uint32_t tableEvictions = 0;

static uint32_t scanPackets = 0;
static uint32_t unmatchedPackets = 0;
static uint32_t prolongedEvents = 0;

static volatile uint32_t scanCallbackCount = 0;
static volatile uint32_t scanDiscoveredCount = 0;
static volatile uint32_t scanRspQueued = 0;
static volatile uint32_t scanEndEvents = 0;
static volatile uint32_t scanRestartFailures = 0;
static volatile bool scanRestartPending = false;
static uint32_t scanWatchdogRestarts = 0;
static uint32_t scanLastAliveMs = 0;
static uint32_t scanLastDiscoveredSeen = 0;
static uint16_t bleScanWindowMs = BLE_SCAN_WINDOW_FULL_MS;
static volatile uint32_t bleQuietUntilMs = 0;
static volatile bool bleQuietActive = false;
static uint32_t bleQuietEvents = 0;
static volatile uint32_t scanEmptyPackets = 0;
static uint32_t scanRspMerged = 0;
static uint32_t qaMalformed = 0;
static uint32_t qaTruncated = 0;
static uint32_t qaNameFields = 0;
static uint32_t qaCompanyFields = 0;
static uint32_t qaServiceFields = 0;
static uint32_t qaQueueHigh = 0;
static uint16_t actualQueueDepth = 0;
static uint32_t qaMatchTypes[7] = {};

struct SurveyEntry { uint32_t key; uint32_t count; };
struct SurveyName { char name[24]; uint32_t count; };
static constexpr unsigned SURVEY_SLOTS = 96;
static constexpr unsigned SURVEY_NAME_SLOTS = 48;
static SurveyEntry surveyOui[SURVEY_SLOTS];   static unsigned surveyOuiCount = 0;
static SurveyEntry surveyCid[SURVEY_SLOTS];   static unsigned surveyCidCount = 0;
static SurveyEntry surveyUuid[SURVEY_SLOTS];  static unsigned surveyUuidCount = 0;
static SurveyName  surveyNames[SURVEY_NAME_SLOTS]; static unsigned surveyNameCount = 0;
static uint32_t surveyPublicAdv = 0, surveyRandomAdv = 0, surveyOverflow = 0;
static uint32_t latestDeviceId = 0;
static bool latestWasPriority = false;
static bool latestWasRepeat = false;
static char latestLabel[38] = "Waiting for BLE";
static char latestMac[24] = "";
static int latestRSSI = -127;
static unsigned long lastTftMs = 0, lastStatusMs = 0;
static unsigned long lastRgbMs = 0;
static uint32_t rgbNewMatchTime = 0;
static uint32_t rgbNewMatchId = 0;
static uint8_t rgbNewMatchRssiPercent = 0;
static bool rgbNewMatchPriority = false;
static uint8_t rgbLastR = 255, rgbLastG = 255, rgbLastB = 255;

static constexpr bool FLASH_LOG_AT_BOOT = true;
static constexpr size_t FLASH_LOG_MIN_FREE = 65536;
static bool flashMounted = false;
static bool flashLogging = false;
static bool flashFull = false;
static File logFile;
static String serialBuffer;
static unsigned long lastLogFlush = 0;
static char logFilename[64] = "";

static constexpr bool WIFI_ON_DEFAULT = true;
static constexpr const char* WIFI_AP_SSID_DEFAULT = "W10-OUI";
static constexpr const char* WIFI_AP_PASS_DEFAULT = "w10detect";
static constexpr const char* WIFI_MDNS_NAME = "w10";
static constexpr size_t WEB_TAIL_CHUNK = 12288;
static WebServer webServer(80);
static bool wifiEnabled = WIFI_ON_DEFAULT;
static bool wifiStaMode = false;
static bool wifiUp = false;
static bool webServerStarted = false;
static String wifiStaSsid, wifiStaPass, wifiApSsid, wifiApPass;
static uint32_t wifiLastCheckMs = 0;
static uint32_t wifiConnectStartMs = 0;
static uint32_t webRequests = 0;
static char wifiIpText[20] = "";

enum DetCat : uint8_t { CAT_AXON=0, CAT_FLOCK=1, CAT_RAVEN=2, CAT_RING=3, CAT_DRONE=4,
                        CAT_TRACKER=5, CAT_GLASSES=6, CAT_FLIPPER=7, CAT_RADIO=8, CAT_LORA=9,
                        CAT_OTHER=10, CAT_COUNT=11 };
static constexpr uint8_t CAT_SHOW = 1, CAT_REC = 2, CAT_BOTH = 3;
struct CatInfo { const char* name; const char* key; uint16_t color; };
static constexpr uint16_t UI_VIOLET = 0x801F, UI_PINK = 0xFC9F;
static const CatInfo CATS[CAT_COUNT] = {
  {"AXON",    "axon",    UI_RED},
  {"FLOCK",   "flock",   UI_ORANGE},
  {"RAVEN",   "raven",   UI_WHITE},
  {"RING",    "ring",    UI_GREEN},
  {"DRONE",   "drone",   UI_LIGHT_BLUE},
  {"TRACKER", "tracker", UI_CYAN},
  {"GLASSES", "glasses", UI_MAGENTA},
  {"FLIPPER", "flipper", UI_YELLOW},
  {"RADIO",   "radio",   UI_VIOLET},
  {"LORA",    "lora",    UI_PINK},
  {"OTHER",   "other",   UI_AMBER},
};
static constexpr uint32_t CAT_MODES_DEFAULT = 0x3FFFFFu;
static constexpr uint16_t CAT_FLASH_DEFAULT = (1u << CAT_AXON) | (1u << CAT_FLOCK) | (1u << CAT_RAVEN) | (1u << CAT_RADIO);
static uint32_t catModes = CAT_MODES_DEFAULT;
static uint16_t catFlashMask = CAT_FLASH_DEFAULT;
static uint32_t selectionFiltered = 0;
static uint32_t sessionEpoch = 1;

enum UiMode : uint8_t { UI_SCAN=0, UI_MENU=1 };
enum UiKey : uint8_t { KEY_BOOT_SHORT=1, KEY_BOOT_LONG=2, KEY_PWR_SHORT=3, KEY_PWR_LONG=4 };
static constexpr unsigned MENU_ITEMS = CAT_COUNT + 2;
static constexpr unsigned MENU_ROWS = 6;
static UiMode uiMode = UI_SCAN;
static unsigned menuCursor = 0, menuTop = 0;
static bool menuDirty = true, menuConfirmClear = false;
static uint32_t menuCountdownUntil = 0, menuLastKeyMs = 0, menuClearedMs = 0;
static bool flashActive = false;
static uint32_t flashStartMs = 0;
static uint16_t flashColor = 0;
static int flashPhase = -1;
static uint32_t flashEvents = 0;

static bool bootRaw = false, bootDown = false, bootLongFired = false;
static uint32_t bootRawSinceMs = 0, bootDownMs = 0, pmuKeyLastPollMs = 0;
static bool pmuKeyOK = false;
static uint32_t keyEvents = 0;

enum WatchType : uint8_t { WT_OUI=0, WT_MAC=1, WT_CID=2, WT_SVC=3, WT_NAME=4, WT_META=5, WT_UUID128=6 };

enum RuleTier : uint8_t { TIER_LOW=0, TIER_MED=1, TIER_HIGH=2 };
struct WatchRule {
  WatchType type = WT_OUI;
  String ident;
  String description;
  uint8_t tier = TIER_MED;
};
struct SigMatch {
  bool matched=false;
  bool priority=false;
  String label;
  const char* method="";
  String identifier;
  const char* confidence="";
  uint8_t tier=TIER_MED;
};
static WatchRule watchRules[MAX_WATCH_RULES];
static uint16_t watchCount = 0;
static bool validatorsEnabled = true;

static uint16_t customRuleCount = 0;
static WatchRule importRules[MAX_WATCH_RULES];
static uint16_t importCount = 0;
static bool importingRules = false;
static Preferences watchPrefs;

struct DeviceAlias { String mac; String label; };
static constexpr unsigned MAX_ALIASES = 48;
static DeviceAlias deviceAliases[MAX_ALIASES];
static unsigned aliasCount = 0;

static const char* tierName(uint8_t t) { return t==TIER_HIGH ? "high" : t==TIER_LOW ? "low" : "medium"; }
static const char* watchTypeName(WatchType t) {
  switch (t) { case WT_OUI: return "OUI"; case WT_MAC: return "MAC";
    case WT_CID: return "CID"; case WT_SVC: return "SVC";
    case WT_NAME: return "NAME"; case WT_META: return "META";
    case WT_UUID128: return "UUID128"; }
  return "INVALID";
}
static String hexOnly(const String& input) {
  String result;
  for (size_t i = 0; i < input.length(); ++i) {
    char c = toupper((unsigned char)input[i]);
    if (isxdigit((unsigned char)c)) result += c;
  }
  return result;
}
static void resetWatchTracks() {
  memset(tracks, 0, sizeof(tracks));
  nextId = 1;
  latestDeviceId = 0;
  latestLabel[0] = 0;
  latestMac[0] = 0;
}
static String safeRuleText(String s) {
  s.replace("\r", " "); s.replace("\n", " "); s.replace("\t", " ");
  s.replace("|", "/"); s.trim(); return s;
}
static bool validRule(WatchType type, String& id, String& label) {
  id.trim(); label=safeRuleText(label);
  if (label.length()==0 || label.length()>70) return false;
  if (type == WT_META) { id="META"; return true; }
  if (type == WT_NAME) return id.length()>=2 && id.length()<=40;
  if (type == WT_UUID128) { id=hexOnly(id); return id.length()==32; }
  if (id.startsWith("0x") || id.startsWith("0X")) id=id.substring(2);
  id=hexOnly(id);
  const unsigned expected=type==WT_OUI ? 6 : type==WT_MAC ? 12 : 4;
  return id.length()==expected;
}
static bool parseWatchType(String s, WatchType& t) {
  s.toUpperCase(); s.trim();
  if (s=="OUI" || s=="0") t=WT_OUI;
  else if (s=="MAC" || s=="1") t=WT_MAC;
  else if (s=="CID" || s=="2") t=WT_CID;
  else if (s=="SVC" || s=="3") t=WT_SVC;
  else if (s=="NAME" || s=="4") t=WT_NAME;
  else if (s=="META" || s=="5") t=WT_META;
  else if (s=="UUID128" || s=="SVC128" || s=="6") t=WT_UUID128;
  else return false;
  return true;
}
static bool addToRuleSet(WatchRule* dest, uint16_t& n, String t, String id, String label, uint8_t tier = TIER_MED) {
  WatchType type;
  if (!parseWatchType(t,type) || !validRule(type,id,label) || n>=MAX_WATCH_RULES) return false;
  for(unsigned i=0;i<n;i++)
    if(dest[i].type==type && dest[i].ident.equalsIgnoreCase(id)) return false;
  dest[n++] = {type,id,label,tier};
  return true;
}

struct RepoWatchRule { const char* type; const char* id; const char* label; uint8_t tier; };
static const RepoWatchRule REPO_WATCHLIST[] = {
  {"OUI", "187F88", "Possible Ring doorbell, security camera or chime", TIER_MED},
  {"OUI", "242BD6", "Possible Ring doorbell, security camera or chime", TIER_MED},
  {"OUI", "343EA4", "Possible Ring doorbell, security camera or chime", TIER_MED},
  {"OUI", "54E019", "Possible Ring doorbell, security camera or chime", TIER_MED},
  {"OUI", "5C475E", "Possible Ring doorbell, security camera or chime", TIER_MED},
  {"OUI", "649A63", "Possible Ring doorbell, security camera or chime", TIER_MED},
  {"OUI", "90486C", "Possible Ring doorbell, security camera or chime", TIER_MED},
  {"OUI", "9C7613", "Possible Ring doorbell, security camera or chime", TIER_MED},
  {"OUI", "AC9FC3", "Possible Ring doorbell, security camera or chime", TIER_MED},
  {"OUI", "C4DBAD", "Possible Ring doorbell, security camera or chime", TIER_MED},
  {"OUI", "CC3BFB", "Possible Ring doorbell, security camera or chime", TIER_MED},
  {"OUI", "0025DF", "Possible Axon body camera or fleet device (IEEE OUI)", TIER_HIGH},
  {"CID", "034D", "Possible Axon body camera or TASER (manufacturer ID)", TIER_HIGH},
  {"SVC", "FC81", "Possible Axon device (advertised service UUID)", TIER_HIGH},
  {"META", "META", "Possible Ray-Ban or Oakley Meta smart glasses (composite)", TIER_HIGH},
  {"OUI", "B41E52", "Possible Flock Safety ALPR camera (IEEE OUI)", TIER_MED},
  {"OUI", "0C9AE6", "Possible DJI drone or related device", TIER_MED},
  {"OUI", "8C5823", "Possible DJI drone or related device", TIER_MED},
  {"OUI", "04A85A", "Possible DJI drone or related device", TIER_MED},
  {"OUI", "58B858", "Possible DJI drone or related device", TIER_MED},
  {"OUI", "E47A2C", "Possible DJI drone or related device", TIER_MED},
  {"OUI", "60601F", "Possible DJI drone or related device", TIER_MED},
  {"OUI", "481CB9", "Possible DJI drone or related device", TIER_MED},
  {"OUI", "34D262", "Possible DJI drone or related device", TIER_MED},
  {"OUI", "00121C", "Possible Parrot drone or related device", TIER_MED},
  {"OUI", "00267E", "Possible Parrot drone or related device", TIER_MED},
  {"OUI", "9003B7", "Possible Parrot drone or related device", TIER_MED},
  {"OUI", "903AE6", "Possible Parrot drone or related device", TIER_MED},
  {"OUI", "A0143D", "Possible Parrot drone or related device", TIER_MED},
  {"OUI", "381D14", "Possible Skydio drone or related device", TIER_MED},
};
static constexpr unsigned REPO_WATCHLIST_COUNT = sizeof(REPO_WATCHLIST)/sizeof(REPO_WATCHLIST[0]);
static_assert(REPO_WATCHLIST_COUNT == 30, "Expected 30 repository Detector Mode rules");
static unsigned mergeRepositoryRules(WatchRule* dest, uint16_t& n) {
  unsigned added = 0;
  for (const RepoWatchRule& rule : REPO_WATCHLIST) {
    if (addToRuleSet(dest, n, rule.type, rule.id, rule.label, rule.tier)) ++added;
  }
  return added;
}

static const RepoWatchRule FLOCK_BLE_EXTRA[] = {
  {"CID", "09C8", "XUNTONG manufacturer ID (NOT proof of Flock hardware)", TIER_LOW},
  {"NAME", "Penguin-", "Possible Flock Penguin battery", TIER_MED},
  {"NAME", "FS Ext Battery", "Possible Flock external battery", TIER_MED},
  {"UUID128", "E8CCBB38953246A89FE51814DF172E6F", "Possible Flock accessory GATT service", TIER_MED},
};
static constexpr unsigned FLOCK_BLE_EXTRA_COUNT = sizeof(FLOCK_BLE_EXTRA)/sizeof(FLOCK_BLE_EXTRA[0]);
static unsigned mergeFlockBleRules(WatchRule* dest, uint16_t& n) {
  unsigned added=0;
  for (const RepoWatchRule& r : FLOCK_BLE_EXTRA)
    if(addToRuleSet(dest,n,r.type,r.id,r.label,r.tier)) ++added;
  return added;
}

static const RepoWatchRule AXON_UPSTREAM_EXTRA[] = {
  {"SVC", "FE6B", "Possible Axon or TASER device (FE6B service UUID)", TIER_HIGH},
  {"SVC", "FE6C", "Possible Axon or TASER device (FE6C service UUID)", TIER_HIGH},
};
static constexpr unsigned AXON_UPSTREAM_EXTRA_COUNT = sizeof(AXON_UPSTREAM_EXTRA)/sizeof(AXON_UPSTREAM_EXTRA[0]);
static unsigned mergeAxonUpstreamRules(WatchRule* dest, uint16_t& n) {
  unsigned added=0;
  for (const RepoWatchRule& r : AXON_UPSTREAM_EXTRA)
    if (addToRuleSet(dest,n,r.type,r.id,r.label,r.tier)) ++added;
  return added;
}

static const RepoWatchRule SQUACH_BLE_EXTRA[] = {
  {"SVC", "FD5A", "Possible Samsung SmartTag", TIER_MED},
  {"SVC", "FEED", "Possible Tile tracker", TIER_MED},
  {"SVC", "FEEC", "Possible Tile tracker", TIER_MED},
  {"SVC", "FD84", "Possible Tile tracker", TIER_MED},
  {"SVC", "FCB2", "DULT compatible tracker signal", TIER_MED},
  {"SVC", "FE33", "Possible Chipolo tracker", TIER_MED},
  {"SVC", "FE65", "Possible Chipolo tracker", TIER_MED},
  {"SVC", "FD5F", "Possible Ray-Ban Meta service", TIER_MED},
  {"SVC", "FEB7", "Possible Meta radio service", TIER_MED},
  {"SVC", "FEB8", "Possible Meta radio service", TIER_MED},
  {"SVC", "FE45", "Possible Snap radio service", TIER_MED},
  {"SVC", "3081", "Possible Flipper Zero service", TIER_HIGH},
  {"SVC", "3082", "Possible Flipper Zero service", TIER_HIGH},
  {"SVC", "3083", "Possible Flipper Zero service", TIER_HIGH},
  {"SVC", "FFFA", "Drone Remote ID broadcast (ASTM F3411)", TIER_MED},
  {"SVC", "3100", "Possible Raven gunshot sensor service", TIER_HIGH},
  {"SVC", "3200", "Possible Raven gunshot sensor service", TIER_HIGH},
  {"SVC", "3300", "Possible Raven gunshot sensor service", TIER_HIGH},
  {"SVC", "3400", "Possible Raven gunshot sensor service", TIER_HIGH},
  {"SVC", "3500", "Possible Raven gunshot sensor service", TIER_HIGH},
  {"NAME", "Pigvision", "Possible Flock accessory by name", TIER_MED},
  {"NAME", "Flock_Setup", "Possible Flock setup mode by name", TIER_HIGH},
  {"NAME", "BodyWorn", "Possible body camera by name", TIER_MED},
  {"NAME", "Axon", "Possible Axon device by name", TIER_MED},
  {"CID", "060C", "Vuzix Bluetooth manufacturer (brand only)", TIER_LOW},
  {"CID", "03C2", "Snap Bluetooth manufacturer (brand only)", TIER_LOW},
  {"CID", "01AB", "Meta Bluetooth manufacturer (brand only, not glasses)", TIER_LOW},
  {"CID", "08C3", "Chipolo manufacturer ID (brand only)", TIER_LOW},
};
static constexpr unsigned SQUACH_BLE_EXTRA_COUNT = sizeof(SQUACH_BLE_EXTRA)/sizeof(SQUACH_BLE_EXTRA[0]);
static_assert(SQUACH_BLE_EXTRA_COUNT == 28, "Expected 28 SquachWatch BLE rules");
static unsigned mergeSquachBleRules(WatchRule* dest, uint16_t& n) {
  unsigned added=0;
  for (const RepoWatchRule& r : SQUACH_BLE_EXTRA)
    if (addToRuleSet(dest,n,r.type,r.id,r.label,r.tier)) ++added;
  return added;
}

static bool saveRules(const WatchRule* rules, uint16_t n);

static constexpr unsigned BUILTIN_RULE_COUNT = REPO_WATCHLIST_COUNT + FLOCK_BLE_EXTRA_COUNT + AXON_UPSTREAM_EXTRA_COUNT + SQUACH_BLE_EXTRA_COUNT;
static_assert(BUILTIN_RULE_COUNT == 64, "Cloud firmware expects 64 built-in rules");
static unsigned mergeAllRepositoryRules(WatchRule* dest, uint16_t& n) {
  const unsigned a = mergeRepositoryRules(dest, n);
  const unsigned b = mergeFlockBleRules(dest, n);
  const unsigned c = mergeAxonUpstreamRules(dest, n);
  return a + b + c + mergeSquachBleRules(dest, n);
}

static bool isBuiltinRule(WatchType type, const String& ident) {
  auto scan = [&](const RepoWatchRule* list, unsigned n) {
    for (unsigned i = 0; i < n; ++i) {
      WatchType t; if (!parseWatchType(String(list[i].type), t) || t != type) continue;
      String id = list[i].id;
      if (type == WT_UUID128 || type == WT_OUI || type == WT_MAC || type == WT_CID || type == WT_SVC) id = hexOnly(id);
      if (ident.equalsIgnoreCase(id)) return true;
    }
    return false;
  };
  return scan(REPO_WATCHLIST, REPO_WATCHLIST_COUNT) ||
         scan(FLOCK_BLE_EXTRA, FLOCK_BLE_EXTRA_COUNT) ||
         scan(AXON_UPSTREAM_EXTRA, AXON_UPSTREAM_EXTRA_COUNT) ||
         scan(SQUACH_BLE_EXTRA, SQUACH_BLE_EXTRA_COUNT);
}

static String packRules(const WatchRule* rules, uint16_t n) {
  String out="W10OUI4\n";
  for(unsigned i=0;i<n;i++){
    out += String((int)rules[i].type)+"\t"+rules[i].ident+"\t"+
      safeRuleText(rules[i].description)+"\n";
  }
  return out;
}
static bool saveRules(const WatchRule* rules, uint16_t n) {
  String packed=packRules(rules,n);
  if (packed.length()>13500 || !watchPrefs.begin("w10_watch", false)) {
    Serial.println("WATCH SAVE ERROR: NVS capacity/unavailable"); return false;
  }
  size_t wrote=watchPrefs.putBytes("rules_v4",packed.c_str(),packed.length()+1);
  watchPrefs.end();
  bool ok=wrote==packed.length()+1;
  Serial.printf("WATCH SAVE: %u rules, %u bytes (%s)\n",(unsigned)n,
    (unsigned)packed.length(),ok?"OK":"FAILED");
  return ok;
}

static bool loadPackedRules(const String& packed, unsigned& skipped) {
  skipped=0;
  if (!packed.startsWith("W10OUI4\n")) return false;
  uint16_t n=0; unsigned start=8;
  while(start<packed.length()) {
    int e=packed.indexOf('\n',start);
    String line = (e<0) ? packed.substring(start) : packed.substring(start,e);
    start = (e<0) ? packed.length() : (unsigned)e+1;
    if(line.endsWith("\r")) line.remove(line.length()-1);
    if(line.length()==0)continue;
    int a=line.indexOf('\t'),b=a<0 ? -1 : line.indexOf('\t',a+1);
    if(a<1||b<=a){++skipped;continue;}
    String type=line.substring(0,a),id=line.substring(a+1,b),label=line.substring(b+1);
    if (!addToRuleSet(importRules,n,type,id,label)) ++skipped;
  }

  for(unsigned i=0;i<n;i++) addToRuleSet(watchRules,watchCount,String((int)importRules[i].type),
                                         importRules[i].ident,importRules[i].description);
  return true;
}

static void loadWatchlist() {
  watchCount=0;
  const unsigned builtins = mergeAllRepositoryRules(watchRules, watchCount);
  bool decoded=false; unsigned skipped=0;
  if(!watchPrefs.begin("w10_watch",true)){
    Serial.println("WATCH: NVS read unavailable; built-ins only");
  } else {
    size_t bytes=watchPrefs.getBytesLength("rules_v4");
    if(bytes>0 && bytes<=15000) {
      char* buf=(char*)malloc(bytes+1);
      if(buf) {
        memset(buf,0,bytes+1);
        if(watchPrefs.getBytes("rules_v4",buf,bytes)==bytes)
          decoded=loadPackedRules(String(buf),skipped);
        free(buf);
      }
      if(!decoded) Serial.println("WATCH: Cannot decode saved v4 list, attempting v0.3 recovery");
    }
    if(!decoded) {

      unsigned legacy=watchPrefs.getUShort("count",0);
      for(unsigned i=0;i<legacy && watchCount<MAX_WATCH_RULES;i++){
        String t="t"+String(i),a="i"+String(i),b="d"+String(i);
        uint8_t type=watchPrefs.getUChar(t.c_str(),255);
        if(type>WT_META) continue;
        String id=watchPrefs.getString(a.c_str(),"");
        String label=watchPrefs.getString(b.c_str(),"");
        addToRuleSet(watchRules,watchCount,String(type),id,label);
      }
    }
    watchPrefs.end();
  }
  if (watchPrefs.begin("w10_watch", true)) { validatorsEnabled = watchPrefs.getUChar("validators", 1) != 0; watchPrefs.end(); }
  customRuleCount = watchCount - builtins;
  Serial.printf("WATCH: %u built-in rules active, %u saved custom rules appended",
                builtins, (unsigned)customRuleCount);
  if (skipped) Serial.printf(", %u unreadable/duplicate saved lines skipped", skipped);
  Serial.println();
}
static bool saveWatchlist(){return saveRules(watchRules,watchCount);}
static bool addWatchRule(String t,String id,String label) {
  if(importingRules) return addToRuleSet(importRules,importCount,t,id,label);
  if(!addToRuleSet(watchRules,watchCount,t,id,label))return false;
  if(!saveWatchlist()) {--watchCount;return false;}
  resetWatchTracks();return true;
}
static unsigned applyPreset(const char* name){
  unsigned added=0;
  String p=name;p.toUpperCase();
  if(p=="AXON"){
    added+=addWatchRule("OUI","0025DF","Possible Axon device (IEEE OUI)");
    added+=addWatchRule("CID","034D","Possible Axon or TASER device (CID)");
    added+=addWatchRule("SVC","FC81","Possible Axon device (service UUID)");
    added+=addWatchRule("SVC","FE6B","Possible Axon or TASER (FE6B service UUID)");
    added+=addWatchRule("SVC","FE6C","Possible Axon or TASER (FE6C service UUID)");
  }else if(p=="META"){
    added+=addWatchRule("META","META","Possible Meta smart glasses");
  }
  return added;
}
static String aliasFor(const char* mac){
  String normalized=hexOnly(String(mac));
  for(unsigned i=0;i<aliasCount;i++)
    if(deviceAliases[i].mac.equalsIgnoreCase(normalized))return deviceAliases[i].label;
  return "";
}
static void loadAliases(){
  aliasCount=0;
  if(!watchPrefs.begin("w10_watch",true))return;
  size_t bytes=watchPrefs.getBytesLength("aliases_v4");
  if(bytes && bytes<5500){
    char* buffer=(char*)malloc(bytes+1);
    if(buffer){
      memset(buffer,0,bytes+1);
      if(watchPrefs.getBytes("aliases_v4",buffer,bytes)==bytes){
        String all(buffer);unsigned at=0;
        while(at<all.length() && aliasCount<MAX_ALIASES){
          int e=all.indexOf('\n',at);if(e<0)break;
          String row=all.substring(at,e);at=e+1;
          int split=row.indexOf('\t');if(split<1)continue;
          String mac=hexOnly(row.substring(0,split));
          if(mac.length()!=12)continue;
          deviceAliases[aliasCount++]={mac,row.substring(split+1)};
        }
      }
      free(buffer);
    }
  }
  watchPrefs.end();
}
static bool saveAliases(){
  String blob;
  for(unsigned i=0;i<aliasCount;i++)blob+=deviceAliases[i].mac+"\t"+safeRuleText(deviceAliases[i].label)+"\n";
  if(!watchPrefs.begin("w10_watch",false))return false;
  bool ok=watchPrefs.putBytes("aliases_v4",blob.c_str(),blob.length()+1)==blob.length()+1;
  watchPrefs.end();return ok;
}
static void copyAscii(char* out,size_t cap,const uint8_t* data,size_t n){
  if(!out||!cap)return;
  size_t count=min(n,cap-1);
  for(size_t i=0;i<count;i++){
    char c=(char)data[i];out[i]=(c>=32 && c<=126)?c:'_';
  }
  out[count]=0;
}
static void addUUID(AdvFields& f,uint16_t id){
  f.serviceId=id;
  for(uint8_t i=0;i<f.serviceCount;i++)if(f.serviceIds[i]==id)return;
  if(f.serviceCount<20)f.serviceIds[f.serviceCount++]=id;
  f.hasMetaService|=id==0xFD5F;
}

static bool sigBaseUUID16(const uint8_t* b,uint16_t& id){
  const uint8_t base[12]={0xFB,0x34,0x9B,0x5F,0x80,0,0,0x80,0,0x10,0,0};
  if(memcmp(b,base,12)!=0||b[14]!=0||b[15]!=0)return false;
  id=uint16_t(b[12])|(uint16_t(b[13])<<8);return true;
}
static void addUUID128(AdvFields& f, const uint8_t* data);
static void ridParse(AdvFields& f, const uint8_t* msg, unsigned avail);
static AdvFields decodePayload(const AdvRecord& r){
  AdvFields f;
  for(unsigned pos=0;pos<r.payloadLength;){
    uint8_t len=r.bytes[pos];if(!len)break;
    if((unsigned)len+pos+1>r.payloadLength){f.malformed=true;break;}
    uint8_t type=r.bytes[pos+1];const uint8_t* d=&r.bytes[pos+2];
    const unsigned n=len-1;
    if(type==0xFF && n>=2){
      f.companyId=uint16_t(d[0]) | (uint16_t(d[1])<<8);

      if(f.companyId==0x004C && n>=29 && d[2]==0x12 && d[3]==0x19) f.appleFindMy=true;
    }else if((type==0x08||type==0x09) && n){

      if(type==0x09 || !f.hasName){copyAscii(f.name,sizeof(f.name),d,n);f.hasName=true;}
    }else if(type==0x02||type==0x03){
      for(unsigned i=0;i+1<n;i+=2)addUUID(f,uint16_t(d[i])|(uint16_t(d[i+1])<<8));
    }else if(type==0x04||type==0x05){
      for(unsigned i=0;i+3<n;i+=4)
        if(d[i+2]==0 && d[i+3]==0)addUUID(f,uint16_t(d[i])|(uint16_t(d[i+1])<<8));
    }else if(type==0x06||type==0x07){
      for(unsigned i=0;i+15<n;i+=16){uint16_t id;
        if(sigBaseUUID16(d+i,id))addUUID(f,id);
        addUUID128(f,d+i);}
    }else if(type==0x16 && n>=2){
      const uint16_t svc=uint16_t(d[0])|(uint16_t(d[1])<<8);
      addUUID(f,svc);

      if(svc==0xFEAA && n>=3 && (d[2]==0x40 || d[2]==0x41)) f.googleFmdn=true;

      if(svc==0xFFFA && n>=4+25 && d[2]==0x0D) ridParse(f, d+4, n-4);
    }else if(type==0x21 && n>=16){
      uint16_t id;if(sigBaseUUID16(d,id))addUUID(f,id);
      addUUID128(f,d);
    }
    pos+=(unsigned)len+1;
  }

  static const char* BWC="BWCDEVICE"; static const char* BWC_REV="ECIVEDCWB";
  for(unsigned i=0;i+9<=r.payloadLength && !f.axonBwcTag;i++)
    if(memcmp(r.bytes+i,BWC,9)==0 || memcmp(r.bytes+i,BWC_REV,9)==0) f.axonBwcTag=true;
  return f;
}

static void ridParseOne(AdvFields& f, const uint8_t* m) {
  const uint8_t type = m[0] >> 4;
  if (type == 0x0) {
    f.ridPresent = true;
    f.ridIdType = m[1] >> 4; f.ridUaType = m[1] & 0x0F;
    unsigned k = 0;
    for (unsigned i = 0; i < 20; ++i) { const char c = (char)m[2 + i]; if (c >= 32 && c <= 126) f.ridSerial[k++] = c; }
    f.ridSerial[k] = 0;
  } else if (type == 0x1) {
    f.ridPresent = true;
    const int32_t lat = (int32_t)((uint32_t)m[5] | ((uint32_t)m[6] << 8) | ((uint32_t)m[7] << 16) | ((uint32_t)m[8] << 24));
    const int32_t lon = (int32_t)((uint32_t)m[9] | ((uint32_t)m[10] << 8) | ((uint32_t)m[11] << 16) | ((uint32_t)m[12] << 24));
    if (lat != 0 || lon != 0) {
      f.ridHasLocation = true; f.ridLat = lat * 1e-7; f.ridLon = lon * 1e-7;
      const uint16_t geo = (uint16_t)(m[15] | (m[16] << 8));
      f.ridAltM = geo * 0.5f - 1000.0f;
    }
  }
}
static void ridParse(AdvFields& f, const uint8_t* msg, unsigned avail) {
  if (avail < 25) return;
  if ((msg[0] >> 4) == 0xF) {
    const unsigned size = msg[1], count = msg[2];
    if (size != 25) return;
    for (unsigned i = 0; i < count && i < 9 && 3 + (i + 1) * 25 <= avail; ++i) ridParseOne(f, msg + 3 + i * 25);
  } else ridParseOne(f, msg);
}

static void addUUID128(AdvFields& f, const uint8_t* data) {
  char uuid[33];
  const char* hex="0123456789ABCDEF";
  for(unsigned i=0;i<16;i++) {
    uint8_t v=data[15-i];
    uuid[i*2]=hex[v>>4];uuid[i*2+1]=hex[v&15];
  }
  uuid[32]=0;
  for(uint8_t j=0;j<f.service128Count;j++)
    if(strcmp(f.service128[j],uuid)==0) return;
  if(f.service128Count<4) strlcpy(f.service128[f.service128Count++],uuid,33);
}

static bool containsIgnoreCase(const char* haystack,const char* needle){
  if(!haystack||!needle)return false;
  String s(haystack),n(needle);s.toLowerCase();n.toLowerCase();
  return s.indexOf(n)>=0;
}
static bool hasUUID(const AdvFields& f,uint16_t id){
  for(uint8_t i=0;i<f.serviceCount;i++)if(f.serviceIds[i]==id)return true;
  return false;
}
static SigMatch matchSignature(const AdvRecord& r,const AdvFields& f){

  String mac=hexOnly(String(r.mac));
  int best=-1; SigMatch chosen;
  for(unsigned i=0;i<watchCount;i++){
    const WatchRule& w=watchRules[i];
    bool hit=false;int weight=0;
    const char* confidence="signature";
    switch(w.type){
      case WT_OUI:

        hit=mac.startsWith(w.ident);weight=20;
        confidence=(r.addrType==0 && ((strtoul(mac.substring(0,2).c_str(),nullptr,16)&2)==0))
          ? "oui_prefix" : "address_prefix_unverified";
        break;
      case WT_MAC:hit=mac.equalsIgnoreCase(w.ident);weight=85;confidence="mac_address";break;
      case WT_CID:hit=f.companyId!=0xFFFF && f.companyId==
          (uint16_t)strtoul(w.ident.c_str(),nullptr,16);weight=45;confidence="company_id";break;
      case WT_SVC:hit=hasUUID(f,(uint16_t)strtoul(w.ident.c_str(),nullptr,16));
        weight=55;confidence="service_uuid";break;
      case WT_NAME:hit=f.hasName && containsIgnoreCase(f.name,w.ident.c_str());
        weight=70;confidence="device_name";break;
      case WT_UUID128:
        for(uint8_t j=0;j<f.service128Count;j++)
          if(w.ident.equals(f.service128[j])) {hit=true;break;}
        weight=75;confidence="service_uuid128";break;
      case WT_META:
        hit=(f.companyId==0x0D53 && f.hasMetaService) ||
          (f.hasName && (containsIgnoreCase(f.name,"ray-ban") ||
           containsIgnoreCase(f.name,"wayfarer") ||
           containsIgnoreCase(f.name,"oakley meta")));
        weight=100;confidence="composite_or_name";break;
    }
    if(!hit || weight<=best)continue;
    best=weight;
    bool axon=(w.type==WT_OUI&&w.ident=="0025DF") ||
      (w.type==WT_CID&&w.ident=="034D") ||
      (w.type==WT_SVC&&(w.ident=="FC81"||w.ident=="FE6B"||w.ident=="FE6C"));
    const bool raven=(w.type==WT_SVC && w.ident.startsWith("3") && w.ident.endsWith("00") && w.ident.length()==4);
    chosen={true,axon||raven||w.type==WT_META||(w.type==WT_UUID128),w.description,watchTypeName(w.type),w.ident,confidence,w.tier};
  }

  if (validatorsEnabled) {
    auto take = [&](int weight, bool priority, const char* label, const char* ident, const char* evidence, uint8_t tier) {
      if (weight <= best) return;
      best = weight;
      chosen = SigMatch{true, priority, String(label), "VALIDATOR", String(ident), evidence, tier};
    };
    if (f.axonBwcTag) take(90, true, "Axon body camera advertisement tag (BWCDEVICE)", "BWCDEVICE", "payload_tag", TIER_HIGH);
    if (f.hasName) {

      const size_t len = strlen(f.name);
      if (len >= 7 && len <= 15 && strncmp(f.name, "FS-", 3) == 0) {
        bool hex = true; for (size_t i = 3; i < len; ++i) if (!isxdigit((unsigned char)f.name[i])) { hex = false; break; }
        if (hex) take(65, false, "Possible Flock accessory (FS-hex name)", "FS-hex", "name_pattern", TIER_MED);
      }

      if (len == 10) {
        bool digits = true; for (size_t i = 0; i < 10; ++i) if (!isdigit((unsigned char)f.name[i])) { digits = false; break; }
        if (digits) take(30, false, "Ten-digit name (possible Flock battery serial, weak)", "10-digit", "name_pattern", TIER_LOW);
      }
    }
    if (f.appleFindMy) take(50, false, "Apple Find My network advertisement (AirTag or other Find My device)", "4C00-12-19", "payload_structure", TIER_MED);
    if (f.googleFmdn) take(55, false, "Google Find Hub tracker frame (FMDN)", "FEAA-40/41", "payload_structure", TIER_MED);
  }
  return chosen;
}
static bool mountFlash(bool allowFormat) {
  if (flashMounted) return true;
  flashMounted = LittleFS.begin(false);
  if (!flashMounted && allowFormat) {

    Serial.println("FLASH: LittleFS not mounted; formatting the log partition once");
    if (LittleFS.format()) flashMounted = LittleFS.begin(false);
    Serial.println(flashMounted ? "FLASH: LittleFS formatted and mounted" : "FLASH: format failed");
  }
  if (!flashMounted) Serial.println("FLASH: LittleFS unavailable; USB NDJSON only");
  return flashMounted;
}
static size_t flashFreeBytes() {
  if (!flashMounted) return 0;
  const size_t total = LittleFS.totalBytes(), used = LittleFS.usedBytes();
  return total > used ? total - used : 0;
}
static void stopFlashLogging() {
  if (logFile) { logFile.flush(); logFile.close(); }
  flashLogging = false;
}

static bool startFlashLogging(bool allowFormat) {
  if (!mountFlash(allowFormat)) return false;
  if (!LittleFS.exists("/logs")) LittleFS.mkdir("/logs");
  if (flashFreeBytes() < FLASH_LOG_MIN_FREE) {
    flashFull = true;
    Serial.println("FLASH: log partition full (<64 KB free); LOG LIST / LOG DUMP to read, FORMATFS YES to erase");
    return false;
  }
  char pathname[48] = {0};
  for (unsigned index = 1; index < 10000; ++index) {
    snprintf(pathname, sizeof(pathname), "/logs/session_%04u.jsonl", index);
    if (!LittleFS.exists(pathname)) break;
    pathname[0] = '\0';
  }
  if (!pathname[0]) { Serial.println("FLASH: no free log filename"); return false; }
  strlcpy(logFilename, pathname, sizeof(logFilename));
  flashLogging = true; flashFull = false;
  Serial.printf("FLASH LOG ARMED: %s (file created on first detection), %u KB free\n",
                pathname, (unsigned)(flashFreeBytes() / 1024));
  return true;
}

static String wifiScreenLine();
static String storageStatusLine() {
  const String wifi = wifiScreenLine();
  if (wifi.length()) return wifi;
  return String(flashLogging ? "FLASH ON " : flashFull ? "FLASH FULL" : "FLASH OFF") + " USB ON";
}

static void emitLine(const String& line) {
  Serial.println(line);
  if (!flashLogging) return;
  if (!logFile) {
    logFile = LittleFS.open(logFilename, FILE_APPEND);
    if (!logFile) {
      flashLogging = false;
      Serial.println("{\"event\":\"error\",\"message\":\"Flash log open failed, logging disabled\"}");
      return;
    }
  }
  if (!logFile.println(line)) {
    stopFlashLogging();
    Serial.println("{\"event\":\"error\",\"message\":\"Flash write failed, logging disabled\"}");
    return;
  }
  logFile.flush();
}

static String asJsonString(const char* str) {
  String out = "\"";
  for (const unsigned char* p = (const unsigned char*)str; p && *p; ++p) {
    if (*p == '"' || *p == '\\') { out += '\\'; out += (char)*p; }
    else if (*p == '\n') out += "\\n";
    else if (*p == '\r') out += "\\r";
    else if (*p == '\t') out += "\\t";
    else if (*p < 32) out += '?';
    else out += (char)*p;
  }
  out += '"';
  return out;
}

static String rawHex(const AdvRecord& r) {
  static const char* HEX_DIGITS = "0123456789ABCDEF";
  String s;
  s.reserve((size_t)r.payloadLength * 2);
  for (unsigned i = 0; i < r.payloadLength; ++i) {
    s += HEX_DIGITS[r.bytes[i] >> 4];
    s += HEX_DIGITS[r.bytes[i] & 15];
  }
  return s;
}

static bool validFix() {
  return gps.location.isValid() && gps.location.age() < GPS_MAX_AGE_MS;
}

static void appendGps(String& out) {
  out += ",\"gps_fix\":";
  out += validFix() ? "true" : "false";
  if (validFix()) {
    out += ",\"lat\":" + String(gps.location.lat(), 7);
    out += ",\"lon\":" + String(gps.location.lng(), 7);
    out += ",\"gps_age_ms\":" + String(gps.location.age());
    if (gps.speed.isValid() && gps.speed.age() < GPS_MAX_AGE_MS) {

      out += ",\"speed_kmh\":" + String(gps.speed.kmph(), 2);
      out += ",\"speed_mph\":" + String(gps.speed.mph(), 2);
    }
    if (gps.course.isValid() && gps.course.age() < GPS_MAX_AGE_MS)
      out += ",\"heading_deg\":" + String(gps.course.deg(), 1);
    if (gps.hdop.isValid())
      out += ",\"hdop\":" + String(gps.hdop.hdop(), 2);
    if (gps.satellites.isValid())
      out += ",\"sats\":" + String(gps.satellites.value());
    if (gps.date.isValid() && gps.time.isValid() && gps.date.year() >= 2020) {
      char utc[28];
      snprintf(utc, sizeof(utc), "%04d-%02d-%02dT%02d:%02d:%02dZ",
        gps.date.year(), gps.date.month(), gps.date.day(),
        gps.time.hour(), gps.time.minute(), gps.time.second());
      out += ",\"utc\":" + asJsonString(utc);
    }
  }
}

static TrackRecord* findTrack(const AdvRecord& r) {
  for (TrackRecord& t : tracks)
    if (t.used && t.addrType == r.addrType && strcmp(t.mac, r.mac) == 0) return &t;
  return nullptr;
}
static TrackRecord* getTrack(const AdvRecord& r) {
  TrackRecord* oldest = &tracks[0];
  for (TrackRecord& t : tracks) {
    if (t.used && t.addrType == r.addrType && strcmp(t.mac, r.mac) == 0)
      return &t;
    if (!t.used) { oldest = &t; break; }
    if (t.lastSeen < oldest->lastSeen) oldest = &t;
  }
  if (oldest->used) ++tableEvictions;
  *oldest = {};
  oldest->used = true;
  strlcpy(oldest->mac, r.mac, sizeof(oldest->mac));
  oldest->addrType = r.addrType;
  oldest->id = nextId++;
  oldest->minRssi = r.rssi;
  oldest->maxRssi = r.rssi;
  return oldest;
}

class AdvertisementCallbacks : public NimBLEScanCallbacks {
  static void enqueue(const NimBLEAdvertisedDevice* dev, bool scanRsp) {
    if (!bleQueue) return;
    AdvRecord r = {};
    auto mac = dev->getAddress().toString();
    strlcpy(r.mac, mac.c_str(), sizeof(r.mac));
    r.rssi = (int8_t)dev->getRSSI();
    r.addrType = dev->getAddress().getType();
    r.timeMs = millis();
    r.scanRsp = scanRsp;
    const auto& payload = dev->getPayload();
    r.payloadLength = (uint8_t)min((size_t)BLE_RAW_MAX, payload.size());
    r.truncated = payload.size() > BLE_RAW_MAX;
    if (r.payloadLength) memcpy(r.bytes, payload.data(), r.payloadLength);
    else __atomic_fetch_add(&scanEmptyPackets,1,__ATOMIC_RELAXED);
    if (xQueueSend(bleQueue, &r, 0) != pdTRUE) __atomic_fetch_add(&droppedQueue, 1, __ATOMIC_RELAXED);
  }
public:
  void onDiscovered(const NimBLEAdvertisedDevice* dev) override {
    __atomic_fetch_add(&scanDiscoveredCount,1,__ATOMIC_RELAXED);
    enqueue(dev, false);
  }
  void onResult(const NimBLEAdvertisedDevice* dev) override {
    __atomic_fetch_add(&scanCallbackCount,1,__ATOMIC_RELAXED);

    if (dev->getPayload().size() <= dev->getAdvLength()) return;
    __atomic_fetch_add(&scanRspQueued,1,__ATOMIC_RELAXED);
    enqueue(dev, true);
  }
  void onScanEnd(const NimBLEScanResults& results, int reason) override {
    (void)results; (void)reason;
    __atomic_fetch_add(&scanEndEvents,1,__ATOMIC_RELAXED);

    if (bleQuietActive) return;
    if (bleScanner && !bleScanner->start(BLE_SCAN_CYCLE_MS, false, true)) {
      __atomic_fetch_add(&scanRestartFailures,1,__ATOMIC_RELAXED);
      scanRestartPending = true;
    }
  }
};
static AdvertisementCallbacks advertisementCallbacks;

static uint16_t colorBrightness(uint16_t c, uint8_t strengthPercent);

static void addHitSegment(uint16_t color) {
  if (hitBarCount < (unsigned)HIT_BAR_W) {
    hitBarColors[(hitBarHead + hitBarCount) % HIT_BAR_W] = color; ++hitBarCount;
  } else {
    hitBarColors[hitBarHead] = color; hitBarHead = (hitBarHead + 1) % HIT_BAR_W;
  }
  ++hitBarVersion;
}
static uint16_t hitBarSegment(unsigned i) { return hitBarColors[(hitBarHead + i) % HIT_BAR_W]; }

static uint8_t categoryOf(const char* label) {
  if (containsIgnoreCase(label, "Axon") || containsIgnoreCase(label, "TASER") || containsIgnoreCase(label, "BodyWorn") ||
      containsIgnoreCase(label, "BWCDEVICE") || containsIgnoreCase(label, "body camera")) return CAT_AXON;
  if (containsIgnoreCase(label, "Flock") || containsIgnoreCase(label, "XUNTONG") || containsIgnoreCase(label, "Penguin") ||
      containsIgnoreCase(label, "FS Ext") || containsIgnoreCase(label, "FS-hex") || containsIgnoreCase(label, "Pigvision")) return CAT_FLOCK;
  if (containsIgnoreCase(label, "Raven") || containsIgnoreCase(label, "gunshot")) return CAT_RAVEN;
  if (containsIgnoreCase(label, "DJI") || containsIgnoreCase(label, "Parrot") || containsIgnoreCase(label, "Skydio") ||
      containsIgnoreCase(label, "Remote ID") || containsIgnoreCase(label, "drone")) return CAT_DRONE;
  if (containsIgnoreCase(label, "Tile") || containsIgnoreCase(label, "Chipolo") || containsIgnoreCase(label, "SmartTag") ||
      containsIgnoreCase(label, "Find My") || containsIgnoreCase(label, "Find Hub") || containsIgnoreCase(label, "tracker") ||
      containsIgnoreCase(label, "DULT")) return CAT_TRACKER;
  if (containsIgnoreCase(label, "Meta") || containsIgnoreCase(label, "Ray-Ban") || containsIgnoreCase(label, "Snap") ||
      containsIgnoreCase(label, "Vuzix") || containsIgnoreCase(label, "glasses")) return CAT_GLASSES;
  if (containsIgnoreCase(label, "Flipper")) return CAT_FLIPPER;
  if (containsIgnoreCase(label, "Meshtastic") || containsIgnoreCase(label, "LoRaWAN") || containsIgnoreCase(label, "LoRa ")) return CAT_LORA;
  if (containsIgnoreCase(label, "radio keyed")) return CAT_RADIO;
  if (containsIgnoreCase(label, "Ring")) return CAT_RING;
  return CAT_OTHER;
}
static uint8_t catMode(uint8_t cat) { return (uint8_t)((catModes >> (2 * cat)) & 3u); }
static void setCatMode(uint8_t cat, uint8_t mode) {
  if (cat >= CAT_COUNT) return;
  catModes = (catModes & ~(3u << (2 * cat))) | ((uint32_t)(mode & 3u) << (2 * cat));
}
static const char* catModeName(uint8_t mode) {
  return mode == CAT_BOTH ? "SHOW+REC" : mode == CAT_SHOW ? "SHOW" : mode == CAT_REC ? "REC" : "OFF";
}
static uint16_t catColor(uint8_t cat) { return CATS[cat < CAT_COUNT ? cat : CAT_OTHER].color; }
static uint16_t categoryColor(const char* label, bool priority) {
  const uint8_t cat = categoryOf(label);
  if (cat == CAT_OTHER) return priority ? UI_WHITE : UI_AMBER;
  return catColor(cat);
}
static void loadSelection() {
  catModes = CAT_MODES_DEFAULT; catFlashMask = CAT_FLASH_DEFAULT;
  if (!watchPrefs.begin("w10_watch", true)) return;

  catModes = watchPrefs.getUInt("cat_modes2", CAT_MODES_DEFAULT) & CAT_MODES_DEFAULT;
  catFlashMask = (uint16_t)(watchPrefs.getUShort("cat_flash2", CAT_FLASH_DEFAULT) & 0x7FFu);
  watchPrefs.end();
}
static bool saveSelection() {
  if (!watchPrefs.begin("w10_watch", false)) return false;
  const bool ok = watchPrefs.putUInt("cat_modes2", catModes) == 4 && watchPrefs.putUShort("cat_flash2", catFlashMask) == 2;
  watchPrefs.end();
  return ok;
}
static void startFlash(uint16_t color);

static void applyScanWindow(bool wifiOn) {
  bleScanWindowMs = !wifiOn ? BLE_SCAN_WINDOW_FULL_MS : (wifiStaSsid.length() ? BLE_SCAN_WINDOW_COEX_MS : BLE_SCAN_WINDOW_AP_MS);
  if (!bleScanner) return;
  bleScanner->setInterval(BLE_SCAN_INTERVAL_MS);
  bleScanner->setWindow(bleScanWindowMs);
}
static bool scanAlive(uint32_t now) { return (uint32_t)(now - scanLastAliveMs) < BLE_SCAN_STALL_MS; }

static void bleQuietService(uint32_t now) {
  if (!bleScanner) return;
  const bool quiet = (int32_t)(bleQuietUntilMs - now) > 0;
  if (quiet && !bleQuietActive) {
    bleQuietActive = true;
    bleScanner->stop();
    ++bleQuietEvents;
  } else if (!quiet && bleQuietActive) {
    bleQuietActive = false;
    bleScanner->start(BLE_SCAN_CYCLE_MS, false, true);
    scanLastAliveMs = now;
  }
}
static void scanWatchdog(uint32_t now) {
  static uint32_t lastCheckMs = 0;
  if (now - lastCheckMs < 1000) return;
  lastCheckMs = now;
  const uint32_t discovered = scanDiscoveredCount;
  if (discovered != scanLastDiscoveredSeen) { scanLastDiscoveredSeen = discovered; scanLastAliveMs = now; }
  if (!bleScanner || bleQuietActive) return;
  static uint8_t idleObservations = 0;
  idleObservations = bleScanner->isScanning() ? 0 : (uint8_t)min(idleObservations + 1, 10);
  if (scanRestartPending || idleObservations >= 2) {
    if (bleScanner->start(BLE_SCAN_CYCLE_MS, false, true)) {
      idleObservations = 0;
      scanRestartPending = false;
      ++scanWatchdogRestarts;
      Serial.println("{\"event\":\"scan_restart\",\"reason\":\"watchdog\"}");
    }
  }
}

static void surveyBump(SurveyEntry* table, unsigned& n, uint32_t key) {
  for (unsigned i = 0; i < n; ++i) if (table[i].key == key) { ++table[i].count; return; }
  if (n < SURVEY_SLOTS) { table[n].key = key; table[n].count = 1; ++n; } else ++surveyOverflow;
}
static void surveyBumpName(const char* name) {
  if (!name || !name[0]) return;
  for (unsigned i = 0; i < surveyNameCount; ++i)
    if (strncmp(surveyNames[i].name, name, sizeof(surveyNames[i].name) - 1) == 0) { ++surveyNames[i].count; return; }
  if (surveyNameCount < SURVEY_NAME_SLOTS) {
    strlcpy(surveyNames[surveyNameCount].name, name, sizeof(surveyNames[0].name));
    surveyNames[surveyNameCount].count = 1; ++surveyNameCount;
  } else ++surveyOverflow;
}
static void surveyReset() {
  surveyOuiCount = surveyCidCount = surveyUuidCount = surveyNameCount = 0;
  surveyPublicAdv = surveyRandomAdv = surveyOverflow = 0;
}

static void processBle(const AdvRecord& r) {
  ++scanPackets;
  AdvFields fields = decodePayload(r);
  if(r.truncated) ++qaTruncated;
  if(fields.malformed) ++qaMalformed;
  if(fields.hasName) ++qaNameFields;
  if(fields.companyId!=0xFFFF) ++qaCompanyFields;
  if(fields.serviceCount || fields.service128Count) ++qaServiceFields;
  SigMatch sig = matchSignature(r, fields);
  if(sig.matched) {
    for(unsigned k=0;k<7;k++) if(strcmp(sig.method,watchTypeName((WatchType)k))==0) {
      ++qaMatchTypes[k];break;
    }
  }
  if (!r.scanRsp) {

    const bool publicAddr = (r.addrType == 0) &&
                            (((unsigned)strtoul(String(r.mac).substring(0, 2).c_str(), nullptr, 16) & 2) == 0);
    if (publicAddr) {
      ++surveyPublicAdv;
      surveyBump(surveyOui, surveyOuiCount, (uint32_t)strtoul(hexOnly(String(r.mac)).substring(0, 6).c_str(), nullptr, 16));
    } else ++surveyRandomAdv;
    if (fields.companyId != 0xFFFF) surveyBump(surveyCid, surveyCidCount, fields.companyId);
  }
  for (uint8_t i = 0; i < fields.serviceCount; ++i) surveyBump(surveyUuid, surveyUuidCount, fields.serviceIds[i]);
  if (fields.hasName) surveyBumpName(fields.name);
  if (!sig.matched) { ++unmatchedPackets; return; }

  const uint8_t cat = categoryOf(sig.label.c_str());
  const uint8_t mode = catMode(cat);
  if (mode == 0) { ++selectionFiltered; return; }
  const bool show = (mode & CAT_SHOW) != 0, rec = (mode & CAT_REC) != 0;

  bool upgrade = false;
  if (r.scanRsp) {
    TrackRecord* existing = findTrack(r);
    if (existing && existing->hits > 0 &&
        (uint32_t)(r.timeMs - existing->lastSeen) <= SCAN_RSP_MERGE_MS) {
      ++scanRspMerged;
      existing->recentRssi = r.rssi;
      existing->minRssi = min(existing->minRssi, r.rssi);
      existing->maxRssi = max(existing->maxRssi, r.rssi);
      if (strcmp(existing->method, sig.method) == 0 &&
          sig.identifier.equals(existing->identifier)) return;
      upgrade = true;
    }
  }

  TrackRecord* t = getTrack(r);
  bool report = false, firstReport = false;
  if (!upgrade) {
    ++t->packets;
    if (t->packets > 1 && (uint32_t)(r.timeMs - t->lastSeen) > WATCH_TIMEOUT_MS) {
      t->continuousStart = r.timeMs;
      t->presenceEscalated = false;
    }
    if (t->packets == 1) t->continuousStart = r.timeMs;
    t->minRssi = min(t->minRssi, r.rssi);
    t->maxRssi = max(t->maxRssi, r.rssi);

    firstReport = (t->hits == 0);
    report = firstReport || (uint32_t)(r.timeMs - t->lastReported) >= REPEAT_REPORT_MIN_MS;
    if (report) {
      ++t->hits;
      t->lastReported = r.timeMs;
      if (show) {
        ++matchedReports;

        if (sig.tier != TIER_LOW)
          W10Audio::schedule(sig.priority ? W10Audio::PRIORITY :
             (firstReport ? W10Audio::NEW_MATCH : W10Audio::REACQUIRED));
        const uint16_t color = categoryColor(sig.label.c_str(), sig.priority);
        addHitSegment(sig.tier == TIER_LOW ? colorBrightness(color, 40) : color);
        if (sig.tier != TIER_LOW && (catFlashMask & (1u << cat))) startFlash(catColor(cat));
        if (firstReport) {
          rgbNewMatchTime = r.timeMs;
          rgbNewMatchId = t->id;
          rgbNewMatchPriority = sig.priority;
          rgbNewMatchRssiPercent = (uint8_t)map(constrain((int)r.rssi, -100, -45), -100, -45, 22, 100);
        }
      }
    } else ++duplicateSuppressed;
  }
  t->lastSeen = r.timeMs;
  t->recentRssi = r.rssi;
  t->priority = sig.priority;
  t->shown = show;
  strlcpy(t->label, sig.label.c_str(), sizeof(t->label));
  strlcpy(t->method, sig.method, sizeof(t->method));
  strlcpy(t->identifier, sig.identifier.c_str(), sizeof(t->identifier));

  if (show && (report || upgrade)) {
    latestWasRepeat = t->hits > 1;
    latestWasPriority = sig.priority;
    latestDeviceId = t->id;
    { String nickname=aliasFor(t->mac);
      strlcpy(latestLabel,nickname.length()?nickname.c_str():t->label,sizeof(latestLabel)); }
    strlcpy(latestMac, r.mac, sizeof(latestMac));
    latestRSSI = r.rssi;
  }

  uint32_t presentFor = r.timeMs - t->continuousStart;
  if (!upgrade && t->packets > 1 && !t->presenceEscalated && presentFor >= SUSTAINED_MS) {
    t->presenceEscalated = true;
    ++prolongedEvents;
    if (show) W10Audio::schedule(W10Audio::SUSTAINED);
    if (rec) {
      String notice="{\"event\":\"watch_sustained\",\"device_id\":" + String(t->id);
      notice += ",\"device_guess\":" + asJsonString(t->label);
      notice += ",\"continuous_ms\":" + String(presentFor);
      notice += ",\"rssi\":" + String((int)t->recentRssi);
      appendGps(notice); notice += '}'; emitLine(notice);
    }

  }

  if (!report && !upgrade) return;

  if (!rec) return;
  ++seenReports;
  String line;
  line.reserve(900);
  line += "{\"event\":\"ble\",\"v\":2,\"watch_match\":true,\"uptime_ms\":" + String(r.timeMs);
  line += ",\"device_id\":" + String(t->id);
  line += ",\"mac\":" + asJsonString(r.mac);
  line += ",\"addr_type\":" + String(r.addrType);
  line += ",\"rssi\":" + String((int)r.rssi);
  line += ",\"rssi_min\":" + String((int)t->minRssi);
  line += ",\"rssi_max\":" + String((int)t->maxRssi);
  line += ",\"hits\":" + String(t->hits);
  line += ",\"packets\":" + String(t->packets);
  line += ",\"category\":" + asJsonString(CATS[cat].key);
  line += ",\"repeat\":" + String(t->hits > 1 ? "true" : "false");
  line += ",\"scan_response\":" + String(r.scanRsp ? "true" : "false");
  line += ",\"label_upgrade\":" + String(upgrade ? "true" : "false");
  line += ",\"priority_match\":" + String(sig.priority ? "true" : "false");
  line += ",\"visual_alert\":" + asJsonString(t->hits > 1 ? "watched_repeat" : "watched_new");
  line += ",\"match_method\":" + asJsonString(sig.method);
  line += ",\"matched_signature\":" + asJsonString(t->label);
  line += ",\"device_guess\":" + asJsonString(t->label);
  line += ",\"matched_identifier\":" + asJsonString(t->identifier);
  line += ",\"match_evidence\":" + asJsonString(sig.confidence);
  line += ",\"confidence\":" + asJsonString(tierName(sig.tier));
  if (fields.ridPresent) {
    line += ",\"remote_id\":{\"serial\":" + asJsonString(fields.ridSerial);
    line += ",\"id_type\":" + String(fields.ridIdType) + ",\"ua_type\":" + String(fields.ridUaType);
    if (fields.ridHasLocation) {
      line += ",\"aircraft_lat\":" + String(fields.ridLat, 7) + ",\"aircraft_lon\":" + String(fields.ridLon, 7);
      line += ",\"aircraft_alt_m\":" + String(fields.ridAltM, 1);
    }
    line += "}";
  }
  if (fields.appleFindMy) line += ",\"apple_find_my\":true";
  if (fields.googleFmdn) line += ",\"google_fmdn\":true";
  { String nick=aliasFor(t->mac);
    if(nick.length()) line += ",\"alias\":" + asJsonString(nick.c_str()); }
  line += ",\"name\":" + asJsonString(fields.name);
  if (fields.companyId != 0xFFFF) {
    char cid[7]; snprintf(cid, sizeof(cid), "%04X", fields.companyId);
    line += ",\"company_id\":" + asJsonString(cid);
  }
  if (fields.serviceId) {
    char svc[7]; snprintf(svc, sizeof(svc), "%04X", fields.serviceId);
    line += ",\"service_uuid16\":" + asJsonString(svc);
  }
  line += ",\"raw_adv_hex\":" + asJsonString(rawHex(r).c_str());
  line += ",\"raw_truncated\":" + String(r.truncated ? "true" : "false");
  appendGps(line);
  line += '}';
  emitLine(line);
}

static uint16_t colorBrightness(uint16_t c, uint8_t strengthPercent) {

  uint16_t r = ((c >> 11) & 31) * strengthPercent / 100;
  uint16_t g = ((c >> 5) & 63) * strengthPercent / 100;
  uint16_t b = (c & 31) * strengthPercent / 100;
  return (r << 11) | (g << 5) | b;
}
static uint8_t rssiBrightness(int rssi) {
  return (uint8_t)map(constrain(rssi, -100, -45), -100, -45, 22, 100);
}
static unsigned activeWatchedRepeats(uint32_t now) {
  unsigned count=0;
  for (const TrackRecord& t : tracks)
    if (t.used && t.shown && t.packets >= 2 && (uint32_t)(now - t.lastSeen) <= WATCH_TIMEOUT_MS) ++count;
  return count;
}
static uint16_t rssiBarColor(int rssi) {
  if (rssi >= -65) return UI_GREEN;
  if (rssi >= -80) return UI_AMBER;
  return UI_RED;
}

static void setAlertPixel(uint8_t r, uint8_t g, uint8_t b) {
  if (r == rgbLastR && g == rgbLastG && b == rgbLastB) return;
  rgbLastR=r; rgbLastG=g; rgbLastB=b;
  rgbLedWrite(RGB_STATUS_PIN,r,g,b);
}

static int axp2101Read(uint8_t reg) {
  Wire.beginTransmission(AXP2101_I2C_ADDR);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom((int)AXP2101_I2C_ADDR, 1) != 1) return -1;
  return Wire.read();
}
static bool axp2101Write(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(AXP2101_I2C_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}

static bool axp2101ChargeLed(uint8_t mode) {
  const int before = axp2101Read(AXP2101_REG_CHGLED);
  if (before < 0) { Serial.println("LED: AXP2101 not reachable on I2C (0x34)"); return false; }
  uint8_t val = (uint8_t)before;
  if (mode == 0)      val = (uint8_t)((val & 0xF9) | 0x01);
  else if (mode == 1) val = (uint8_t)((val & 0xC8) | 0x05);
  else                val = (uint8_t)(val & 0xC8);
  const bool ok = axp2101Write(AXP2101_REG_CHGLED, val);
  const int after = axp2101Read(AXP2101_REG_CHGLED);
  Serial.printf("LED: AXP2101 reg 0x69 was 0x%02X, wrote 0x%02X, reads back 0x%02X (%s)\n",
                before, val, after, ok && after == val ? "OK" : "MISMATCH");
  return ok && after == val;
}

static constexpr uint8_t AXP2101_REG_VBUS_VLIM = 0x15, AXP2101_REG_VBUS_ILIM = 0x16, AXP2101_REG_VOFF = 0x24;
static bool axp2101ConfigureInput() {
  const int v = axp2101Read(AXP2101_REG_VBUS_VLIM), i = axp2101Read(AXP2101_REG_VBUS_ILIM), off = axp2101Read(AXP2101_REG_VOFF);
  if (v < 0 || i < 0 || off < 0) { Serial.println("PMU: AXP2101 not reachable; input limits unchanged"); return false; }
  static const char* ilimNames[] = {"100", "500", "900", "1000", "1500", "2000", "?", "?"};
  const bool ok = axp2101Write(AXP2101_REG_VBUS_VLIM, (uint8_t)((v & 0xF0) | 0x06)) &&
                  axp2101Write(AXP2101_REG_VBUS_ILIM, (uint8_t)((i & 0xF8) | 0x04)) &&
                  axp2101Write(AXP2101_REG_VOFF, (uint8_t)(off & 0xF8));
  const int i2 = axp2101Read(AXP2101_REG_VBUS_ILIM);
  Serial.printf("PMU: VBUS current limit was %s mA, now %s mA; VBUS limit 4.36 V; VOFF 2.6 V (%s)\n",
                ilimNames[i & 7], i2 < 0 ? "?" : ilimNames[i2 & 7], ok ? "OK" : "WRITE FAILED");
  return ok;
}

static String axp2101PowerLine() {
  static uint32_t lastReadMs = 0;
  static String cached = "POWER ?";
  const uint32_t now = millis();
  if (cached.length() && now - lastReadMs < 1000) return cached;
  lastReadMs = now;
  const int st1 = axp2101Read(0x00);
  const int st2 = axp2101Read(0x01);
  if (st1 < 0 || st2 < 0) { cached = "PMU NOT READABLE"; return cached; }
  const bool vbus = st1 & 0x20, battery = st1 & 0x08;
  const uint8_t chg = (uint8_t)((st2 >> 5) & 0x03);
  if (!battery) { cached = vbus ? "USB POWER NO BATT" : "NO USB NO BATT"; return cached; }
  const int pct = axp2101Read(0xA4);
  String line = "BAT " + String(pct < 0 ? 0 : (pct & 0x7F)) + "% ";
  line += chg == 1 ? "CHARGING" : chg == 2 ? "DISCHARGE" : (vbus ? "ON USB" : "IDLE");
  cached = line; return cached;
}

static void updateAlertPixel(uint32_t now) {
  uint8_t r=0,g=0,b=0;

  uint16_t active[MAX_DEVICES]; unsigned count=0;
  for (uint16_t i=0;i<MAX_DEVICES;i++) {
    const TrackRecord& t=tracks[i];
    if (t.used && t.shown && t.packets>=2 && (uint32_t)(now-t.lastSeen)<=WATCH_TIMEOUT_MS)
      active[count++]=i;
  }
  if(count && now % RGB_SLOT_MS < RGB_ON_MS) {
    const TrackRecord& t=tracks[active[(now/RGB_SLOT_MS)%count]];
    const unsigned pct=rssiBrightness(t.recentRssi);
    const uint8_t peak=(uint8_t)(RGB_MAX_BRIGHTNESS*pct/100);
    if (t.priority) { r=peak; }
    else { r=peak; g=(uint8_t)(peak*56/100); }
  } else if (!count && rgbNewMatchId &&
             (uint32_t)(now-rgbNewMatchTime)<RGB_NEW_MS) {
    const uint8_t peak=(uint8_t)(RGB_MAX_BRIGHTNESS*rgbNewMatchRssiPercent/100);
    if (rgbNewMatchPriority) r=peak;
    else g=peak;
  }
  setAlertPixel(r,g,b);
}

static void subgReport(const char* id, uint8_t cat, const char* label, const char* method, const char* ident, const char* evidence, uint8_t tier, bool priority, int rssi, const String& extraJson) {
  const uint8_t mode = catMode(cat);
  if (mode == 0) { ++selectionFiltered; return; }
  const bool show = (mode & CAT_SHOW) != 0, rec = (mode & CAT_REC) != 0;
  AdvRecord key = {};
  strlcpy(key.mac, id, sizeof(key.mac));
  key.addrType = 2;
  key.rssi = (int8_t)constrain(rssi, -127, 0);
  key.timeMs = millis();
  TrackRecord* t = getTrack(key);
  ++t->packets;
  if (t->packets == 1) t->continuousStart = key.timeMs;
  t->minRssi = min(t->minRssi, key.rssi);
  t->maxRssi = max(t->maxRssi, key.rssi);
  const bool firstReport = (t->hits == 0);
  const bool report = firstReport || (uint32_t)(key.timeMs - t->lastReported) >= REPEAT_REPORT_MIN_MS;
  t->lastSeen = key.timeMs; t->recentRssi = key.rssi; t->priority = priority; t->shown = show;
  strlcpy(t->label, label, sizeof(t->label));
  strlcpy(t->method, method, sizeof(t->method));
  strlcpy(t->identifier, ident, sizeof(t->identifier));
  if (!report) { ++duplicateSuppressed; return; }
  ++t->hits;
  t->lastReported = key.timeMs;
  if (show) {
    ++matchedReports;
    if (tier != TIER_LOW)
      W10Audio::schedule(priority ? W10Audio::PRIORITY : (firstReport ? W10Audio::NEW_MATCH : W10Audio::REACQUIRED));
    const uint16_t color = catColor(cat);
    addHitSegment(tier == TIER_LOW ? colorBrightness(color, 40) : color);
    if (tier != TIER_LOW && (catFlashMask & (1u << cat))) startFlash(color);
    latestWasRepeat = t->hits > 1; latestWasPriority = priority; latestDeviceId = t->id;
    strlcpy(latestLabel, label, sizeof(latestLabel));
    strlcpy(latestMac, key.mac, sizeof(latestMac));
    latestRSSI = rssi;
  }
  if (!rec) return;
  ++seenReports;
  String line;
  line.reserve(600);
  line += "{\"event\":\"subg\",\"v\":1,\"watch_match\":true,\"uptime_ms\":" + String(key.timeMs);
  line += ",\"device_id\":" + String(t->id);
  line += ",\"mac\":" + asJsonString(key.mac);
  line += ",\"rssi\":" + String(rssi);
  line += ",\"rssi_min\":" + String((int)t->minRssi);
  line += ",\"rssi_max\":" + String((int)t->maxRssi);
  line += ",\"hits\":" + String(t->hits);
  line += ",\"packets\":" + String(t->packets);
  line += ",\"category\":" + asJsonString(CATS[cat].key);
  line += ",\"repeat\":" + String(t->hits > 1 ? "true" : "false");
  line += ",\"priority_match\":" + String(priority ? "true" : "false");
  line += ",\"visual_alert\":" + asJsonString(t->hits > 1 ? "watched_repeat" : "watched_new");
  line += ",\"match_method\":" + asJsonString(method);
  line += ",\"matched_signature\":" + asJsonString(label);
  line += ",\"device_guess\":" + asJsonString(label);
  line += ",\"matched_identifier\":" + asJsonString(ident);
  line += ",\"match_evidence\":" + asJsonString(evidence);
  line += ",\"confidence\":" + asJsonString(tierName(tier));
  if (extraJson.length()) { line += ','; line += extraJson; }
  appendGps(line);
  line += '}';
  emitLine(line);
}

namespace W10SubG {
static constexpr int NSS_PIN = 14;
static constexpr uint8_t EXIO_NRST = 3, EXIO_BUSY = 10, EXIO_DIO1 = 9;
static constexpr uint32_t SPI_HZ = 8000000;
static constexpr unsigned MAX_BANDS = 6, MAX_PROFILES = 16, MAX_STEPS = 96;
static constexpr float STEP_MHZ = 0.4f;
static constexpr uint32_t SETTLE_US = 1200;
static constexpr uint8_t RSSI_SAMPLES = 3;
static constexpr uint32_t CAD_TIMEOUT_MS = 250, RX_TIMEOUT_MS = 2500;
static constexpr int THRESHOLD_DEFAULT = -70;
static constexpr int FLOOR_MARGIN_DB = 20;
static constexpr uint8_t SYNC_PRIVATE_REG = 0x14;
enum Kind : uint8_t { KIND_GENERIC = 0, KIND_MESHTASTIC = 1, KIND_LORAWAN = 2 };
enum Phase : uint8_t { PH_OFF = 0, PH_RADIO_TUNE, PH_RADIO_SAMPLE, PH_LORA_TUNE, PH_LORA_CAD, PH_LORA_RX };
struct Band { float lo, hi; char name[12]; };
struct LoraProfile { float mhz; uint8_t bw, sf, cr, sync, kind; uint16_t preamble; char label[14]; };

static bool present = false, enabled = true, tcxo = false, busyFault = false;
static Band bands[MAX_BANDS]; static unsigned bandCount = 0;
static LoraProfile profiles[MAX_PROFILES]; static unsigned profileCount = 0;
static int threshold = THRESHOLD_DEFAULT;
static float stepFreq[MAX_STEPS]; static int8_t stepRssi[MAX_STEPS], stepFloor[MAX_STEPS]; static bool stepHot[MAX_STEPS];
static unsigned stepCount = 0, stepIndex = 0, profileIndex = 0;
static Phase phase = PH_OFF;
static uint32_t phaseStartUs = 0, phaseStartMs = 0;
static uint32_t sweeps = 0, bursts = 0, cadHits = 0, loraPackets = 0, loraDecoded = 0, crcErrors = 0, reinits = 0;
static int8_t samplePeak = -128; static uint8_t sampleIndex = 0;
static uint8_t packetType = 0xFF;
static float lastBurstMHz = 0; static int lastBurstDbm = -128; static uint32_t lastBurstMs = 0;
static uint8_t lastErrors = 0;
static char lastLora[40] = "";

static bool busy() { return expanderOK && expander.digitalRead(EXIO_BUSY) != 0; }
static bool waitReady(uint32_t maxUs) {
  const uint32_t start = micros();
  while (busy()) { if ((uint32_t)(micros() - start) > maxUs) { busyFault = true; return false; } }
  return true;
}
static void cmd(const uint8_t* tx, size_t n, uint8_t* rx = nullptr, size_t rxn = 0) {
  waitReady(4000);
  SPI.beginTransaction(SPISettings(SPI_HZ, MSBFIRST, SPI_MODE0));
  digitalWrite(NSS_PIN, LOW);
  delayMicroseconds(2);
  for (size_t i = 0; i < n; ++i) { const uint8_t b = SPI.transfer(tx[i]); if (rx && i < rxn) rx[i] = b; }
  for (size_t i = n; i < rxn; ++i) rx[i] = SPI.transfer(0x00);
  delayMicroseconds(2);
  digitalWrite(NSS_PIN, HIGH);
  SPI.endTransaction();
}
static void cmd1(uint8_t op) { cmd(&op, 1); }
static void cmd2(uint8_t op, uint8_t a) { const uint8_t t[2] = {op, a}; cmd(t, 2); }
static void writeReg(uint16_t reg, uint8_t v) { const uint8_t t[4] = {0x0D, (uint8_t)(reg >> 8), (uint8_t)reg, v}; cmd(t, 4); }
static uint8_t readReg(uint16_t reg) { const uint8_t t[4] = {0x1D, (uint8_t)(reg >> 8), (uint8_t)reg, 0x00}; uint8_t r[5] = {0}; cmd(t, 4, r, 5); return r[4]; }
static uint8_t getStatus() { const uint8_t t[2] = {0xC0, 0x00}; uint8_t r[2] = {0}; cmd(t, 2, r, 2); return r[1]; }
static uint16_t getIrq() { const uint8_t t[2] = {0x12, 0x00}; uint8_t r[4] = {0}; cmd(t, 2, r, 4); return (uint16_t)((r[2] << 8) | r[3]); }
static void clearIrq(uint16_t m) { const uint8_t t[3] = {0x02, (uint8_t)(m >> 8), (uint8_t)m}; cmd(t, 3); }
static uint16_t getErrors() { const uint8_t t[2] = {0x17, 0x00}; uint8_t r[4] = {0}; cmd(t, 2, r, 4); return (uint16_t)((r[2] << 8) | r[3]); }
static void clearErrors() { const uint8_t t[3] = {0x07, 0x00, 0x00}; cmd(t, 3); }
static int rssiInst() { const uint8_t t[2] = {0x15, 0x00}; uint8_t r[3] = {0}; cmd(t, 2, r, 3); return -(int)r[2] / 2; }
static void setStandbyXosc() { cmd2(0x80, 0x01); }
static void setFrequency(float mhz) {
  const uint32_t raw = (uint32_t)(((double)mhz * 1000000.0) / 32000000.0 * 33554432.0);
  const uint8_t t[5] = {0x86, (uint8_t)(raw >> 24), (uint8_t)(raw >> 16), (uint8_t)(raw >> 8), (uint8_t)raw};
  cmd(t, 5);
}
static void setRxContinuous() { const uint8_t t[4] = {0x82, 0xFF, 0xFF, 0xFF}; cmd(t, 4); }
static void calibrateImageFor(float loMHz, float hiMHz) {

  uint8_t a, b;
  if (loMHz >= 902 && hiMHz <= 928) { a = 0xE1; b = 0xE9; }
  else if (loMHz >= 863 && hiMHz <= 870) { a = 0xD7; b = 0xDB; }
  else if (loMHz >= 779 && hiMHz <= 787) { a = 0xC1; b = 0xC5; }
  else {
    a = (uint8_t)((loMHz - 1.0f) / 4.0f); b = (uint8_t)((hiMHz + 1.0f) / 4.0f);
    if (!(a & 1)) --a;
    if (!(b & 1)) ++b;
  }
  const uint8_t t[3] = {0x98, a, b}; cmd(t, 3);
}
static void setPacketTypeOnce(uint8_t type) {
  if (packetType == type) return;
  packetType = type; cmd2(0x8A, type);
}
static void configGfskSweep() {
  setStandbyXosc();
  setPacketTypeOnce(0x00);

  const uint8_t mod[9] = {0x8B, 0x00, 0x50, 0x00, 0x00, 0x09, 0x00, 0x66, 0x66}; cmd(mod, 9);
  const uint8_t pkt[10] = {0x8C, 0x00, 0x10, 0x00, 0x00, 0x00, 0x01, 0xFF, 0x01, 0x00}; cmd(pkt, 10);
}
static uint8_t bwCode(uint16_t kHz) {
  switch (kHz) { case 7: return 0x00; case 10: return 0x08; case 15: return 0x01; case 20: return 0x09; case 31: return 0x02;
    case 41: return 0x0A; case 62: return 0x03; case 125: return 0x04; case 250: return 0x05; case 500: return 0x06; }
  return 0x04;
}
static uint16_t bwKHz(uint8_t code) {
  switch (code) { case 0x00: return 7; case 0x08: return 10; case 0x01: return 15; case 0x09: return 20; case 0x02: return 31;
    case 0x0A: return 41; case 0x03: return 62; case 0x04: return 125; case 0x05: return 250; case 0x06: return 500; }
  return 125;
}
static void configLora(const LoraProfile& p) {
  setStandbyXosc();
  setPacketTypeOnce(0x01);
  const uint32_t bwHz = (uint32_t)bwKHz(p.bw) * 1000u;
  const bool ldro = ((1000000u << p.sf) / bwHz) >= 16380u;
  const uint8_t mod[5] = {0x8B, p.sf, p.bw, p.cr, (uint8_t)(ldro ? 1 : 0)}; cmd(mod, 5);
  const uint8_t pkt[7] = {0x8C, (uint8_t)(p.preamble >> 8), (uint8_t)p.preamble, 0x00, 0xFF, 0x01, 0x00}; cmd(pkt, 7);
  writeReg(0x0740, (uint8_t)((p.sync & 0xF0) | 0x04));
  writeReg(0x0741, (uint8_t)(((p.sync & 0x0F) << 4) | 0x04));
  calibrateImageFor(p.mhz - 1.0f, p.mhz + 1.0f);
  setFrequency(p.mhz);

  static const uint8_t detPeak[8] = {22, 22, 22, 22, 23, 24, 25, 28};
  const uint32_t timeout = RX_TIMEOUT_MS * 64u;
  const uint8_t cad[8] = {0x88, 0x01, detPeak[constrain((int)p.sf, 5, 12) - 5], 10, 0x01,
                          (uint8_t)(timeout >> 16), (uint8_t)(timeout >> 8), (uint8_t)timeout};
  cmd(cad, 8);
  clearIrq(0xFFFF);
  cmd1(0xC5);
}
static void rebuildSteps() {
  stepCount = 0;
  for (unsigned b = 0; b < bandCount && stepCount < MAX_STEPS; ++b)
    for (float f = bands[b].lo + STEP_MHZ / 2; f < bands[b].hi && stepCount < MAX_STEPS; f += STEP_MHZ) {
      stepFreq[stepCount] = f; stepRssi[stepCount] = -128; stepFloor[stepCount] = 0; stepHot[stepCount] = false; ++stepCount;
    }
  stepIndex = 0;
}
static void addBand(float lo, float hi, const char* name) {
  if (bandCount >= MAX_BANDS || hi <= lo || lo < 150 || hi > 960) return;
  bands[bandCount].lo = lo; bands[bandCount].hi = hi;
  strlcpy(bands[bandCount].name, name, sizeof(bands[bandCount].name));
  ++bandCount;
}
static void addProfile(float mhz, uint16_t bwk, uint8_t sf, uint8_t crDenominator, uint8_t sync, uint8_t kind, uint16_t preamble, const char* label) {
  if (profileCount >= MAX_PROFILES || mhz < 150 || mhz > 960 || sf < 5 || sf > 12) return;
  LoraProfile& p = profiles[profileCount++];
  p.mhz = mhz; p.bw = bwCode(bwk); p.sf = sf; p.cr = (uint8_t)constrain((int)crDenominator - 4, 1, 4);
  p.sync = sync; p.kind = kind; p.preamble = preamble;
  strlcpy(p.label, label, sizeof(p.label));
}
static void defaultBands() {
  bandCount = 0;
  addBand(799.5f, 805.5f, "700 mobile");
  addBand(806.0f, 813.5f, "800 mobile");
  rebuildSteps();
}
static void defaultProfiles() {
  profileCount = 0;
  addProfile(906.875f, 250, 11, 5, 0x2B, KIND_MESHTASTIC, 16, "LongFast");
  for (unsigned k = 8; k < 16; ++k) addProfile(902.3f + 0.2f * k, 125, 10, 5, 0x34, KIND_LORAWAN, 8, "US915 SF10");
  addProfile(904.6f, 500, 8, 5, 0x34, KIND_LORAWAN, 8, "US915 500k");
}
static String bandsText() {
  String s;
  for (unsigned b = 0; b < bandCount; ++b) { if (b) s += ';'; s += String(bands[b].lo, 1) + "-" + String(bands[b].hi, 1) + "," + bands[b].name; }
  return s;
}
static String profilesText() {
  String s;
  for (unsigned i = 0; i < profileCount; ++i) {
    const LoraProfile& p = profiles[i];
    if (i) s += ';';
    char hex[4]; snprintf(hex, sizeof(hex), "%02X", p.sync);
    s += String(p.mhz, 3) + "," + String(bwKHz(p.bw)) + "," + String(p.sf) + "," + String(p.cr + 4) + "," + hex + "," + String(p.kind) + "," + String(p.preamble) + "," + p.label;
  }
  return s;
}
static void parseBands(const String& text) {
  bandCount = 0;
  int pos = 0;
  while (pos <= (int)text.length() && bandCount < MAX_BANDS) {
    int end = text.indexOf(';', pos); if (end < 0) end = text.length();
    String item = text.substring(pos, end); item.trim();
    const int dash = item.indexOf('-'), comma = item.indexOf(',');
    if (dash > 0) {
      const float lo = item.substring(0, dash).toFloat();
      const float hi = (comma > dash ? item.substring(dash + 1, comma) : item.substring(dash + 1)).toFloat();
      String name = comma > dash ? item.substring(comma + 1) : String("band"); name.trim();
      addBand(lo, hi, name.c_str());
    }
    pos = end + 1;
  }
  if (!bandCount) defaultBands(); else rebuildSteps();
}
static void parseProfiles(const String& text) {
  profileCount = 0;
  int pos = 0;
  while (pos <= (int)text.length() && profileCount < MAX_PROFILES) {
    int end = text.indexOf(';', pos); if (end < 0) end = text.length();
    String item = text.substring(pos, end); item.trim();
    String f[8]; unsigned n = 0; int p2 = 0;
    while (n < 8) { int c = item.indexOf(',', p2); if (c < 0) { f[n++] = item.substring(p2); break; } f[n++] = item.substring(p2, c); p2 = c + 1; }
    if (n >= 7) addProfile(f[0].toFloat(), (uint16_t)f[1].toInt(), (uint8_t)f[2].toInt(), (uint8_t)f[3].toInt(),
                           (uint8_t)strtoul(f[4].c_str(), nullptr, 16), (uint8_t)f[5].toInt(), (uint16_t)f[6].toInt(), n >= 8 ? f[7].c_str() : "custom");
    pos = end + 1;
  }
  if (!profileCount) defaultProfiles();
}
static void loadConfig() {
  enabled = true; threshold = THRESHOLD_DEFAULT; defaultBands(); defaultProfiles();
  if (!watchPrefs.begin("w10_watch", true)) return;
  enabled = watchPrefs.getUChar("subg_on", 1) != 0;
  threshold = -(int)watchPrefs.getUChar("subg_thr", (uint8_t)(-THRESHOLD_DEFAULT));
  const String b = watchPrefs.getString("subg_bands", ""), p = watchPrefs.getString("subg_lora", "");
  watchPrefs.end();
  if (b.length()) parseBands(b);
  if (p.length()) parseProfiles(p);
}
static bool saveConfig() {
  if (!watchPrefs.begin("w10_watch", false)) return false;
  watchPrefs.putUChar("subg_on", enabled ? 1 : 0);
  watchPrefs.putUChar("subg_thr", (uint8_t)constrain(-threshold, 30, 127));
  watchPrefs.putString("subg_bands", bandsText());
  watchPrefs.putString("subg_lora", profilesText());
  watchPrefs.end();
  return true;
}

static bool begin() {
  present = false; tcxo = false; busyFault = false; packetType = 0xFF;
  if (!expanderOK) return false;
  expander.pinMode(EXIO_NRST, OUTPUT); expander.pinMode(EXIO_BUSY, INPUT); expander.pinMode(EXIO_DIO1, INPUT);
  pinMode(NSS_PIN, OUTPUT); digitalWrite(NSS_PIN, HIGH);
  expander.digitalWrite(EXIO_NRST, LOW); delay(20);
  expander.digitalWrite(EXIO_NRST, HIGH); delay(50);
  SPI.begin(SPI_SCLK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, NSS_PIN);
  waitReady(20000);
  cmd2(0x80, 0x00);
  if (readReg(0x0740) != SYNC_PRIVATE_REG) return false;
  cmd2(0x9D, 0x01);
  { const uint8_t c[2] = {0x89, 0x7F}; cmd(c, 2); }
  delay(5);
  lastErrors = (uint8_t)getErrors();
  if (lastErrors & 0x20) {
    const uint8_t t[5] = {0x97, 0x02, 0x00, 0x01, 0x40};
    cmd(t, 5); tcxo = true;
    clearErrors();
    { const uint8_t c[2] = {0x89, 0x7F}; cmd(c, 2); }
    delay(10);
    lastErrors = (uint8_t)getErrors();
  }
  cmd2(0x93, 0x30);
  { const uint8_t b[3] = {0x8F, 0x00, 0x00}; cmd(b, 3); }
  writeReg(0x08AC, 0x96);
  { const uint8_t d[9] = {0x08, 0x03, 0xE2, 0x03, 0xE2, 0x00, 0x00, 0x00, 0x00}; cmd(d, 9); }
  present = true;
  phase = PH_OFF; stepIndex = 0; profileIndex = 0;
  for (unsigned i = 0; i < stepCount; ++i) { stepRssi[i] = -128; stepFloor[i] = 0; }
  return true;
}
static void reportBurst(unsigned i, int dbm) {
  if (!stepHot[i]) { ++bursts; stepHot[i] = true; }
  lastBurstMHz = stepFreq[i]; lastBurstDbm = dbm; lastBurstMs = millis();
  const char* band = "band";
  for (unsigned b = 0; b < bandCount; ++b) if (stepFreq[i] >= bands[b].lo && stepFreq[i] <= bands[b].hi) band = bands[b].name;
  char id[20], label[40], ident[16];
  snprintf(id, sizeof(id), "RF:%.1f", (double)stepFreq[i]);
  snprintf(label, sizeof(label), "Radio keyed %.1f MHz", (double)stepFreq[i]);
  snprintf(ident, sizeof(ident), "%.3f MHz", (double)stepFreq[i]);
  String extra = "\"freq_mhz\":" + String(stepFreq[i], 3) + ",\"floor_dbm\":" + String((int)stepFloor[i]) + ",\"band\":" + asJsonString(band);
  subgReport(id, CAT_RADIO, label, "RSSI", ident, "rf_energy", TIER_MED, true, dbm, extra);
}
static String hexBytes(const uint8_t* d, unsigned n) {
  static const char* H = "0123456789ABCDEF";
  String s; s.reserve(n * 2);
  for (unsigned i = 0; i < n; ++i) { s += H[d[i] >> 4]; s += H[d[i] & 15]; }
  return s;
}
static void decodePacket(const LoraProfile& p, const uint8_t* d, unsigned n, int rssi, float snr) {
  ++loraPackets;
  String extra = "\"lora_profile\":" + asJsonString(p.label) + ",\"freq_mhz\":" + String(p.mhz, 3) + ",\"sf\":" + String(p.sf) +
                 ",\"bw_khz\":" + String(bwKHz(p.bw)) + ",\"snr_db\":" + String(snr, 1) + ",\"len\":" + String(n);
  char id[24], label[64], ident[40];
  if (p.kind == KIND_MESHTASTIC && n >= 16) {
    const uint32_t to = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
    const uint32_t from = (uint32_t)d[4] | ((uint32_t)d[5] << 8) | ((uint32_t)d[6] << 16) | ((uint32_t)d[7] << 24);
    snprintf(id, sizeof(id), "MT:%08lX", (unsigned long)from);
    snprintf(label, sizeof(label), "Meshtastic node !%08lx", (unsigned long)from);
    snprintf(ident, sizeof(ident), "!%08lx", (unsigned long)from);
    char toText[12]; snprintf(toText, sizeof(toText), "%08lX", (unsigned long)to);
    extra += ",\"mesh_to\":" + asJsonString(to == 0xFFFFFFFFu ? "broadcast" : toText) +
             ",\"hop_limit\":" + String(d[12] & 7) + ",\"channel_hash\":" + String(d[13]);
    ++loraDecoded;
    strlcpy(lastLora, label, sizeof(lastLora));
    subgReport(id, CAT_LORA, label, "LORA", ident, "meshtastic_header", TIER_LOW, false, rssi, extra);
    return;
  }
  if (p.kind == KIND_LORAWAN && n >= 12 && (d[0] & 0x03) == 0) {
    const uint8_t mtype = d[0] >> 5;
    if (mtype == 0 && n >= 23) {
      uint8_t eui[8]; for (unsigned i = 0; i < 8; ++i) eui[i] = d[16 - i];
      const String devEui = hexBytes(eui, 8);
      snprintf(id, sizeof(id), "LW:%s", devEui.c_str());
      snprintf(label, sizeof(label), "LoRaWAN join, maker %02X-%02X-%02X", eui[0], eui[1], eui[2]);
      strlcpy(ident, devEui.c_str(), sizeof(ident));
      uint8_t join[8]; for (unsigned i = 0; i < 8; ++i) join[i] = d[8 - i];
      extra += ",\"dev_eui\":" + asJsonString(devEui.c_str()) + ",\"join_eui\":" + asJsonString(hexBytes(join, 8).c_str());
      ++loraDecoded;
      strlcpy(lastLora, label, sizeof(lastLora));
      subgReport(id, CAT_LORA, label, "LORA", ident, "lorawan_join", TIER_LOW, false, rssi, extra);
      return;
    }
    if (mtype == 2 || mtype == 4 || mtype == 1 || mtype == 3 || mtype == 5) {
      const uint32_t devAddr = (uint32_t)d[1] | ((uint32_t)d[2] << 8) | ((uint32_t)d[3] << 16) | ((uint32_t)d[4] << 24);
      const bool up = (mtype == 2 || mtype == 4);
      snprintf(id, sizeof(id), "LW:%08lX", (unsigned long)devAddr);
      snprintf(label, sizeof(label), "LoRaWAN %s %08lX", up ? "uplink" : "gateway downlink", (unsigned long)devAddr);
      snprintf(ident, sizeof(ident), "%08lX", (unsigned long)devAddr);
      extra += ",\"dev_addr\":" + asJsonString(ident) + ",\"fcnt\":" + String((unsigned)(d[6] | (d[7] << 8))) + ",\"uplink\":" + String(up ? "true" : "false");
      ++loraDecoded;
      strlcpy(lastLora, label, sizeof(lastLora));
      subgReport(id, CAT_LORA, label, "LORA", ident, "lorawan_mhdr", TIER_LOW, false, rssi, extra);
      return;
    }
  }

  const String head = hexBytes(d, n < 8 ? n : 8);
  snprintf(id, sizeof(id), "LR:%s", head.substring(0, 16).c_str());
  snprintf(label, sizeof(label), "LoRa frame %s %u B", p.label, n);
  strlcpy(ident, head.c_str(), sizeof(ident));
  extra += ",\"head_hex\":" + asJsonString(head.c_str());
  strlcpy(lastLora, label, sizeof(lastLora));
  subgReport(id, CAT_LORA, label, "LORA", ident, "lora_frame", TIER_LOW, false, rssi, extra);
}
static void readPacket(const LoraProfile& p) {
  const uint8_t t[2] = {0x13, 0x00}; uint8_t r[4] = {0}; cmd(t, 2, r, 4);
  const uint8_t len = r[2], start = r[3];
  if (!len) return;
  const uint8_t ps[2] = {0x14, 0x00}; uint8_t s[5] = {0}; cmd(ps, 2, s, 5);
  const int rssi = -(int)s[2] / 2; const float snr = (float)(int8_t)s[3] / 4.0f;
  uint8_t buf[3 + 64]; const unsigned want = len > 64 ? 64 : len;
  const uint8_t rb[3] = {0x1E, start, 0x00}; cmd(rb, 3, buf, 3 + want);
  decodePacket(p, buf + 3, want, rssi, snr);
}
static void nextPhase(uint32_t now) {

  if (stepCount && (phase == PH_OFF || phase == PH_LORA_TUNE || phase == PH_LORA_CAD || phase == PH_LORA_RX)) {
    configGfskSweep();
    calibrateImageFor(bands[0].lo, bands[bandCount - 1].hi);
    stepIndex = 0; phase = PH_RADIO_TUNE;
  } else if (profileCount) {
    phase = PH_LORA_TUNE;
  } else phase = PH_OFF;
  phaseStartMs = now;
}
static void service(uint32_t now) {
  if (!present || !enabled) return;
  switch (phase) {
    case PH_OFF:
      nextPhase(now);
      break;
    case PH_RADIO_TUNE:
      setStandbyXosc();
      setFrequency(stepFreq[stepIndex]);
      setRxContinuous();
      phaseStartUs = micros(); samplePeak = -128; sampleIndex = 0;
      phase = PH_RADIO_SAMPLE;
      break;
    case PH_RADIO_SAMPLE: {
      if ((uint32_t)(micros() - phaseStartUs) < SETTLE_US) return;
      for (; sampleIndex < RSSI_SAMPLES; ++sampleIndex) { const int v = rssiInst(); if (v > samplePeak) samplePeak = (int8_t)v; }
      const unsigned i = stepIndex;
      stepRssi[i] = samplePeak;
      if (stepFloor[i] == 0 || samplePeak < stepFloor[i]) stepFloor[i] = samplePeak;
      else if ((sweeps & 7) == 7 && stepFloor[i] < -60) stepFloor[i] = (int8_t)(stepFloor[i] + 1);
      if (sweeps >= 2 && samplePeak >= threshold && samplePeak >= stepFloor[i] + FLOOR_MARGIN_DB) reportBurst(i, samplePeak);
      else stepHot[i] = false;
      if (++stepIndex >= stepCount) { ++sweeps; stepIndex = 0; if (profileCount) { phase = PH_LORA_TUNE; phaseStartMs = now; } else phase = PH_RADIO_TUNE; }
      else phase = PH_RADIO_TUNE;
      break; }
    case PH_LORA_TUNE:
      if (profileIndex >= profileCount) profileIndex = 0;
      configLora(profiles[profileIndex]);
      phaseStartMs = now; phase = PH_LORA_CAD;
      break;
    case PH_LORA_CAD: {
      const uint16_t irq = getIrq();
      if (irq & 0x0080) {
        if (irq & 0x0100) { ++cadHits; clearIrq(0x0180); phaseStartMs = now; phase = PH_LORA_RX; }
        else { clearIrq(0x0080); profileIndex = (profileIndex + 1) % profileCount; nextPhase(now); }
      } else if ((uint32_t)(now - phaseStartMs) > CAD_TIMEOUT_MS) { profileIndex = (profileIndex + 1) % profileCount; nextPhase(now); }
      break; }
    case PH_LORA_RX: {
      const uint16_t irq = getIrq();
      if (irq & 0x0002) {
        if (irq & 0x0040) ++crcErrors; else readPacket(profiles[profileIndex]);
        clearIrq(0xFFFF);
        profileIndex = (profileIndex + 1) % profileCount; nextPhase(now);
      } else if ((irq & 0x0220) || (uint32_t)(now - phaseStartMs) > RX_TIMEOUT_MS) {
        if (irq & 0x0020) ++crcErrors;
        clearIrq(0xFFFF); setStandbyXosc();
        profileIndex = (profileIndex + 1) % profileCount; nextPhase(now);
      }
      break; }
  }
}
static const char* phaseName() {
  switch (phase) { case PH_OFF: return "off"; case PH_RADIO_TUNE: case PH_RADIO_SAMPLE: return "radio_sweep";
    case PH_LORA_TUNE: return "lora_tune"; case PH_LORA_CAD: return "lora_cad"; case PH_LORA_RX: return "lora_rx"; }
  return "?";
}
static String statusJson() {
  String s = "\"subg\":{\"present\":" + String(present ? "true" : "false") + ",\"enabled\":" + String(enabled ? "true" : "false");
  s += ",\"tcxo\":" + String(tcxo ? "true" : "false") + ",\"busy_fault\":" + String(busyFault ? "true" : "false");
  s += ",\"phase\":" + asJsonString(phaseName()) + ",\"sweeps\":" + String(sweeps) + ",\"bursts\":" + String(bursts);
  s += ",\"cad_hits\":" + String(cadHits) + ",\"lora_packets\":" + String(loraPackets) + ",\"lora_decoded\":" + String(loraDecoded);
  s += ",\"crc_errors\":" + String(crcErrors) + ",\"threshold_dbm\":" + String(threshold) + ",\"steps\":" + String(stepCount);
  s += ",\"profiles\":" + String(profileCount) + ",\"chip_errors\":" + String(lastErrors);
  if (lastBurstMs) s += ",\"last_burst_mhz\":" + String(lastBurstMHz, 1) + ",\"last_burst_dbm\":" + String(lastBurstDbm);
  s += "}";
  return s;
}
static void printStatus() {
  Serial.printf("SUBG: %s, %s, tcxo %s, phase %s, sweeps %lu, bursts %lu, cad %lu, lora packets %lu (decoded %lu, crc errors %lu), busy fault %s, chip errors 0x%02X\n",
                present ? "SX1262 present" : "SX1262 NOT FOUND", enabled ? "enabled" : "disabled", tcxo ? "yes" : "no", phaseName(),
                (unsigned long)sweeps, (unsigned long)bursts, (unsigned long)cadHits, (unsigned long)loraPackets, (unsigned long)loraDecoded,
                (unsigned long)crcErrors, busyFault ? "yes" : "no", lastErrors);
  Serial.printf("SUBG: radio threshold %d dBm (plus %d dB over the per-step floor); bands: %s (%u steps of %.1f MHz)\n",
                threshold, FLOOR_MARGIN_DB, bandsText().c_str(), stepCount, (double)STEP_MHZ);
  for (unsigned i = 0; i < profileCount; ++i) {
    const LoraProfile& p = profiles[i];
    Serial.printf("SUBG LORA %u: %.3f MHz BW%u SF%u CR4/%u sync %02X preamble %u kind %u %s\n", i + 1, (double)p.mhz, bwKHz(p.bw), p.sf, p.cr + 4, p.sync, p.preamble, p.kind, p.label);
  }
  if (lastBurstMs) Serial.printf("SUBG: last radio burst %.1f MHz at %d dBm, %lu s ago\n", (double)lastBurstMHz, lastBurstDbm, (unsigned long)((millis() - lastBurstMs) / 1000));
  if (lastLora[0]) Serial.printf("SUBG: last LoRa decode: %s\n", lastLora);
}
static void printSweep() {
  Serial.printf("SUBG SCAN: %u steps, sweep %lu (RSSI / floor, dBm)\n", stepCount, (unsigned long)sweeps);
  for (unsigned i = 0; i < stepCount; ++i)
    Serial.printf("  %.1f MHz  %4d / %4d%s\n", (double)stepFreq[i], (int)stepRssi[i], (int)stepFloor[i],
                  (stepRssi[i] >= threshold && stepRssi[i] >= stepFloor[i] + FLOOR_MARGIN_DB) ? "  BURST" : "");
}
}

static String shortWatchLabel(const String& source) {
  String label = source;
  label.trim();
  if (label.startsWith("Possible ")) label.remove(0, 9);

  if (containsIgnoreCase(label.c_str(), "XUNTONG")) return "XUNTONG CID only";
  if (containsIgnoreCase(label.c_str(), "Penguin")) return "Flock? Penguin batt";
  if (containsIgnoreCase(label.c_str(), "external battery") ||
      containsIgnoreCase(label.c_str(), "FS Ext Battery")) return "Flock? ext battery";
  if (containsIgnoreCase(label.c_str(), "accessory GATT")) return "Flock? GATT service";
  if (containsIgnoreCase(label.c_str(), "Ring"))   return "Ring camera/chime";
  if (containsIgnoreCase(label.c_str(), "Axon") || containsIgnoreCase(label.c_str(), "TASER"))
    return "Axon body camera";
  if (containsIgnoreCase(label.c_str(), "Meta") || containsIgnoreCase(label.c_str(), "Ray-Ban"))
    return "Meta smart glasses";
  if (containsIgnoreCase(label.c_str(), "Flock")) return "Flock? camera OUI";
  if (containsIgnoreCase(label.c_str(), "DJI")) return "DJI drone";
  if (containsIgnoreCase(label.c_str(), "Parrot")) return "Parrot drone";
  if (containsIgnoreCase(label.c_str(), "Skydio")) return "Skydio drone";
  return label;
}
static String clippedScreenText(const String& src, unsigned maxChars=19) {
  if (src.length() <= maxChars) return src;
  return src.substring(0, maxChars);
}

static constexpr unsigned TFT_SLOTS = 12;
static constexpr unsigned TFT_COLS = 19;
struct TftLine { String text; uint16_t color = 0; uint16_t bg = 0; bool valid = false; };
static TftLine tftLines[TFT_SLOTS];
static bool tftFirstFrame = true;
static int tftRssiPixels = -1;
static uint16_t tftRssiColor = 0;

static bool tftText(unsigned slot, int x, int y, const String& text, uint16_t color, uint16_t bg = UI_BLACK, unsigned cols = TFT_COLS) {
  if (slot >= TFT_SLOTS) return false;
  String padded = clippedScreenText(text, cols);
  while (padded.length() < cols) padded += ' ';
  TftLine& line = tftLines[slot];
  if (line.valid && line.text == padded && line.color == color && line.bg == bg) return false;
  line.text = padded; line.color = color; line.bg = bg; line.valid = true;
  tft->setTextColor(color, bg);
  tft->setCursor(x, y);
  tft->print(padded);
  return true;
}
static void tftRowMarker(unsigned y, uint16_t ink) { tft->fillRect(0, y, 3, 16, ink); }
static uint32_t tftHitBarVersion = 0xFFFFFFFFu;
static bool tftHitBarAlive = true;
static void renderHitBar(uint32_t now) {
  const bool alive = scanAlive(now);
  if (hitBarVersion == tftHitBarVersion && alive == tftHitBarAlive) return;
  tftHitBarVersion = hitBarVersion; tftHitBarAlive = alive;
  tft->drawRect(HIT_BAR_X - 1, HIT_BAR_Y - 1, HIT_BAR_W + 2, HIT_BAR_H + 2, alive ? UI_DIM : UI_RED);
  tft->fillRect(HIT_BAR_X, HIT_BAR_Y, HIT_BAR_W, HIT_BAR_H, UI_BLACK);
  if (!hitBarCount) return;
  const unsigned base = HIT_BAR_W / hitBarCount, extra = HIT_BAR_W % hitBarCount;
  int x = HIT_BAR_X;
  for (unsigned i = 0; i < hitBarCount; ++i) {
    const unsigned w = base + (i < extra ? 1 : 0);

    const unsigned fillW = (base >= 4 && i + 1 < hitBarCount) ? w - 1 : w;
    if (fillW) tft->fillRect(x, HIT_BAR_Y, (int)fillW, HIT_BAR_H, hitBarSegment(i));
    x += (int)w;
  }
}

struct Ticker { String full; int x = 0, y = 0; uint16_t color = 0, bg = 0; unsigned cols = TFT_COLS, offset = 0; uint32_t nextStepMs = 0; bool active = false; };
static Ticker tickers[TFT_SLOTS];
static String tickerWindow(unsigned slot) {
  const Ticker& k = tickers[slot];
  const String loop = k.full + "   ";
  String s; s.reserve(k.cols);
  for (unsigned i = 0; i < k.cols; ++i) s += loop[(k.offset + i) % loop.length()];
  return s;
}
static bool tftTicker(unsigned slot, int x, int y, const String& text, uint16_t color, uint16_t bg, unsigned cols) {
  if (slot >= TFT_SLOTS) return false;
  Ticker& k = tickers[slot];
  if (text.length() <= cols) { k.active = false; return tftText(slot, x, y, text, color, bg, cols); }
  if (!k.active || k.full != text || k.cols != cols) {
    k.full = text; k.cols = cols; k.offset = 0; k.active = true; k.nextStepMs = millis() + TICKER_HOLD_MS;
  }
  k.x = x; k.y = y; k.color = color; k.bg = bg;
  return tftText(slot, x, y, tickerWindow(slot), color, bg, cols);
}
static void tickersReset() { for (unsigned s = 0; s < TFT_SLOTS; ++s) tickers[s].active = false; }
static void tickerService(uint32_t now) {
  if (!tftOK || flashActive) return;
  for (unsigned s = 0; s < TFT_SLOTS; ++s) {
    Ticker& k = tickers[s];
    if (!k.active || (int32_t)(now - k.nextStepMs) < 0) continue;
    k.offset = (k.offset + 1) % (k.full.length() + 3);
    k.nextStepMs = now + (k.offset == 0 ? TICKER_HOLD_MS : TICKER_STEP_MS);
    tft->setTextWrap(false); tft->setTextSize(2);
    tftText(s, k.x, k.y, tickerWindow(s), k.color, k.bg, k.cols);
  }
}

static void renderScreen();
static void startFlash(uint16_t color) {
  if (!tftOK) return;
  flashColor = color; flashStartMs = millis(); flashActive = true; flashPhase = -1; ++flashEvents;
}
static void flashService(uint32_t now) {
  if (!flashActive) return;
  const uint32_t elapsed = now - flashStartMs;
  if (elapsed >= FLASH_TOTAL_MS) {
    flashActive = false;
    tftFirstFrame = true;
    renderScreen();
    return;
  }
  const int phase = (int)((elapsed / FLASH_PERIOD_MS) & 1u);
  if (phase != flashPhase) { flashPhase = phase; tft->fillScreen(phase == 0 ? flashColor : UI_BLACK); }
}

static void clearSessionHits() {
  resetWatchTracks();
  matchedReports = 0; seenReports = 0; duplicateSuppressed = 0; selectionFiltered = 0; prolongedEvents = 0;
  hitBarCount = 0; hitBarHead = 0; ++hitBarVersion;
  latestWasPriority = latestWasRepeat = false; latestRSSI = -127;
  rgbNewMatchId = 0;
  if (logFile) logFile.close();
  bool removed = false;
  if (flashMounted && logFilename[0] && LittleFS.exists(logFilename)) removed = LittleFS.remove(logFilename);
  ++sessionEpoch;
  Serial.printf("{\"event\":\"hits_cleared\",\"epoch\":%lu,\"log_file_removed\":%s}\n",
                (unsigned long)sessionEpoch, removed ? "true" : "false");
}

static void menuScrollToCursor() {
  if (menuCursor < menuTop) menuTop = menuCursor;
  else if (menuCursor >= menuTop + MENU_ROWS) menuTop = menuCursor + 1 - MENU_ROWS;
}
static void openMenu(uint32_t now) {
  uiMode = UI_MENU; menuCursor = 0; menuTop = 0; menuDirty = true; menuConfirmClear = false; menuClearedMs = 0;
  menuLastKeyMs = now; tftFirstFrame = true;
  renderScreen();
}
static void closeMenu() {
  uiMode = UI_SCAN; menuCountdownUntil = 0; menuConfirmClear = false; tftFirstFrame = true;
  renderScreen();
}
static void menuActivate(uint32_t now) {
  if (menuCursor < CAT_COUNT) {
    static const uint8_t NEXT_MODE[4] = {CAT_BOTH, CAT_REC, 0, CAT_SHOW};
    setCatMode((uint8_t)menuCursor, NEXT_MODE[catMode((uint8_t)menuCursor)]);
    Serial.printf("SELECT: %s %s (%s)\n", CATS[menuCursor].name, catModeName(catMode((uint8_t)menuCursor)),
                  saveSelection() ? "saved" : "NVS SAVE FAILED");
  } else if (menuCursor == CAT_COUNT) {
    if (!menuConfirmClear) menuConfirmClear = true;
    else { menuConfirmClear = false; clearSessionHits(); menuClearedMs = now; }
  } else { closeMenu(); return; }
  menuDirty = true;
}
static void uiKey(uint8_t key, uint32_t now) {
  ++keyEvents;
  if (uiMode == UI_SCAN) { if (key != KEY_PWR_LONG) openMenu(now); return; }
  menuLastKeyMs = now;
  menuCountdownUntil = 0;
  switch (key) {
    case KEY_BOOT_SHORT: menuCursor = (menuCursor + 1) % MENU_ITEMS; menuScrollToCursor(); menuConfirmClear = false; menuDirty = true; break;
    case KEY_BOOT_LONG:  menuCursor = (menuCursor + MENU_ITEMS - 1) % MENU_ITEMS; menuScrollToCursor(); menuConfirmClear = false; menuDirty = true; break;
    case KEY_PWR_SHORT:  menuActivate(now); break;
    default:             closeMenu(); break;
  }
}
static void menuDrawRow(unsigned row, uint32_t now) {
  const unsigned item = menuTop + row;
  const int y = MENU_ROW_Y0 + (int)row * MENU_ROW_H;
  if (item >= MENU_ITEMS) { tft->fillRect(0, y, 240, MENU_ROW_H, UI_BLACK); return; }
  const bool highlighted = (item == menuCursor);
  uint16_t bg = 0x2104, ink = UI_WHITE;
  String left, right;
  if (item < CAT_COUNT) {
    const uint8_t mode = catMode((uint8_t)item);
    bg = colorBrightness(catColor((uint8_t)item), mode ? 45 : 12);
    if (!mode) ink = UI_DIM;
    left = CATS[item].name; right = catModeName(mode);
  } else if (item == CAT_COUNT) {
    if (menuConfirmClear) { bg = UI_DARK_RED; left = "CLEAR? PWR=YES"; }
    else if (menuClearedMs && now - menuClearedMs < 3000) left = "CLEARED";
    else { left = "CLEAR HITS"; right = String(matchedReports); }
  } else left = "EXIT TO SCAN";
  tft->fillRect(3, y + 2, 234, MENU_ROW_H - 4, bg);
  tft->drawRect(1, y, 238, MENU_ROW_H, highlighted ? UI_WHITE : UI_BLACK);
  tft->drawRect(2, y + 1, 236, MENU_ROW_H - 2, highlighted ? UI_WHITE : UI_BLACK);
  const int textY = y + (MENU_ROW_H - 16) / 2;
  tft->setTextColor(ink, bg);
  tft->setCursor(10, textY); tft->print(left);
  if (right.length()) { tft->setCursor(230 - 12 * (int)right.length(), textY); tft->print(right); }
}
static void renderMenu(uint32_t now) {
  if (tftFirstFrame) {
    tftFirstFrame = false;
    tft->fillScreen(UI_BLACK);
    for (unsigned i = 0; i < TFT_SLOTS; ++i) tftLines[i].valid = false;
    tickersReset();
    tftRssiPixels = -1; tftHitBarVersion = 0xFFFFFFFFu;
    menuDirty = true;
  }
  String head = "SELECT";
  if (menuCountdownUntil) {
    const int32_t left = (int32_t)(menuCountdownUntil - now);
    head += "  SCAN IN " + String(left > 0 ? (unsigned)((left + 999) / 1000) : 0u) + "s";
  } else head += "  " + String(matchedReports) + " HITS";
  tftText(0, 5, 3, head, UI_CYAN);
  tftTicker(11, 5, 222, "BOOT:NEXT PWR:SET   HOLD BOOT:UP   HOLD PWR:EXIT", UI_DIM, UI_BLACK, TFT_COLS);
  if (menuDirty) { menuDirty = false; for (unsigned r = 0; r < MENU_ROWS; ++r) menuDrawRow(r, now); }
}

static void renderScreen() {
  if (!tftOK) return;
  const uint32_t now = millis();
  if (flashActive) return;
  tft->setTextWrap(false);
  tft->setTextSize(2);
  if (uiMode == UI_MENU) { renderMenu(now); return; }
  if (tftFirstFrame) {
    tftFirstFrame = false;
    tft->fillScreen(UI_BLACK);
    tft->fillRect(0, 25, 240, 22, UI_WHITE);
    tft->drawFastHLine(0, 97, 240, UI_DIM);
    tft->drawFastHLine(0, 154, 240, UI_DIM);
    for (unsigned i = 0; i < TFT_SLOTS; ++i) tftLines[i].valid = false;
    tickersReset();
    tftRssiPixels = -1;
    tftHitBarVersion = 0xFFFFFFFFu;
  }
  tftText(0, 5, 4, "OUI DETECTION", UI_CYAN);

  const bool fix = validFix();
  const int sats = gps.satellites.isValid() ? (int)gps.satellites.value() : 0;
  tftText(1, 5, 28, fix ? "GPS FIX " + String(sats) + " SAT" : String("GPS SEARCHING"), UI_BLACK, UI_WHITE);

  tftText(2, 5, 53, matchedReports > 9999 ? String("HITS:9999+") : "HITS:" + String(matchedReports), UI_WHITE, UI_BLACK, 9);
  renderHitBar(now);
  tftTicker(3, 5, 76, storageStatusLine(), UI_CYAN, UI_BLACK, TFT_COLS);

  if (latestDeviceId) {
    const uint16_t headerColor = latestWasPriority ? UI_RED : UI_GREEN;
    tftText(4, 5, 103, "LAST #" + String(latestDeviceId) + " " + String(latestRSSI) + "dBm", headerColor);
    String label = aliasFor(latestMac);
    if (!label.length()) label = shortWatchLabel(String(latestLabel));
    tftTicker(5, 5, 125, label, UI_WHITE, UI_BLACK, TFT_COLS);

    const int bounded = constrain((int)latestRSSI, -100, -30);
    const int pixels = map(bounded, -100, -30, 0, 225);
    const uint16_t barColor = rssiBarColor(latestRSSI);
    if (pixels != tftRssiPixels || barColor != tftRssiColor) {
      tftRssiPixels = pixels; tftRssiColor = barColor;
      tft->drawRect(5, 145, 229, 6, UI_DIM);
      tft->fillRect(7, 147, 225, 2, UI_BLACK);
      if (pixels > 0) tft->fillRect(7, 147, pixels, 2, barColor);
    }
  } else {
    tftText(4, 5, 103, watchCount ? "AWAITING MATCH" : "NO RULES LOADED", UI_DIM);

    tftTicker(5, 5, 125, axp2101PowerLine(), UI_DIM, UI_BLACK, TFT_COLS);
  }

  uint16_t ids[MAX_DEVICES];
  unsigned total = 0;
  for (uint16_t i=0;i<MAX_DEVICES;++i) {
    if (tracks[i].used && tracks[i].shown && tracks[i].packets >= 2 &&
        (uint32_t)(now - tracks[i].lastSeen) <= WATCH_TIMEOUT_MS) ids[total++] = i;
  }
  const unsigned rows = 3;
  const unsigned pages = max(1u, (total + rows - 1) / rows);
  const unsigned page = (now / 5000UL) % pages;
  String header = "REPEATS:" + String(total);
  if (pages > 1) header += " " + String(page+1) + "/" + String(pages);
  tftText(7, 5, 158, header, UI_CYAN);

  for (unsigned row=0;row<rows;++row) {
    const unsigned index = page*rows + row;
    const unsigned y = 179 + row*19;
    if (index >= total) {

      if (tftTicker(8 + row, 5, y, "", UI_DIM, UI_BLACK, TFT_COLS)) tftRowMarker(y, UI_BLACK);
      continue;
    }
    const TrackRecord& t = tracks[ids[index]];
    const uint8_t pct = rssiBrightness(t.recentRssi);
    const uint16_t ink = colorBrightness(t.priority ? UI_RED : UI_AMBER, pct);
    String name = aliasFor(t.mac);
    if (!name.length()) name = shortWatchLabel(String(t.label));
    const String prefix = String(t.presenceEscalated ? "!" : "#") + String(t.id) + " ";
    if (tftTicker(8 + row, 5, y, prefix + name, ink, UI_BLACK, TFT_COLS)) tftRowMarker(y, ink);
  }
}

static bool pmuKeySetup() {
  const int en = axp2101Read(AXP2101_REG_INTEN2);
  if (en < 0) return false;
  if (!axp2101Write(AXP2101_REG_INTEN2, (uint8_t)(en | 0x0C))) return false;
  axp2101Write(AXP2101_REG_INTSTS2, 0xFF);
  return true;
}
static void keysService(uint32_t now) {
  static uint32_t lastPollMs = 0;
  if (now - lastPollMs < 10) return;
  lastPollMs = now;
  const bool raw = digitalRead(BOOT_KEY_PIN) == LOW;
  if (raw != bootRaw) { bootRaw = raw; bootRawSinceMs = now; }
  else if (raw != bootDown && now - bootRawSinceMs >= KEY_DEBOUNCE_MS) {
    bootDown = raw;
    if (bootDown) { bootDownMs = now; bootLongFired = false; }
    else if (!bootLongFired) uiKey(KEY_BOOT_SHORT, now);
  }
  if (bootDown && !bootLongFired && now - bootDownMs >= KEY_LONG_MS) { bootLongFired = true; uiKey(KEY_BOOT_LONG, now); }
  if (pmuKeyOK && now - pmuKeyLastPollMs >= PMU_KEY_POLL_MS) {
    pmuKeyLastPollMs = now;
    const int st = axp2101Read(AXP2101_REG_INTSTS2);
    if (st > 0 && (st & 0x0C)) {
      axp2101Write(AXP2101_REG_INTSTS2, (uint8_t)(st & 0x0C));
      uiKey((st & 0x08) ? KEY_PWR_SHORT : KEY_PWR_LONG, now);
    }
  }
}
static void uiService(uint32_t now) {
  flashService(now);
  if (flashActive) return;
  if (uiMode == UI_MENU) {
    if (menuCountdownUntil && (int32_t)(now - menuCountdownUntil) >= 0) { closeMenu(); return; }
    if (!menuCountdownUntil && now - menuLastKeyMs >= MENU_IDLE_MS) { closeMenu(); return; }
    if (menuClearedMs && now - menuClearedMs >= 3000) { menuClearedMs = 0; menuDirty = true; }
    if (menuDirty) renderScreen();
  }
  tickerService(now);
  if (now - lastTftMs >= DISPLAY_PERIOD_MS) {
    lastTftMs = now;
    renderScreen();
  }
}

static void emitStatus() {
  String line = "{\"event\":\"status\",\"v\":1,\"uptime_ms\":" + String(millis());
  line += ",\"flash_bytes\":" + String(ESP.getFlashChipSize());
  line += ",\"psram_bytes\":" + String(ESP.getPsramSize());
  line += ",\"ble_reports\":" + String(seenReports);
  line += ",\"packets_scanned\":" + String(scanPackets);
  line += ",\"packets_discarded_unmatched\":" + String(unmatchedPackets);
  line += ",\"watch_rules\":" + String(watchCount);
  line += ",\"aliases\":" + String(aliasCount);
  line += ",\"active_repeats\":" + String(activeWatchedRepeats(millis()));
  line += ",\"prolonged_presence_events\":" + String(prolongedEvents);
  line += ",\"matched_reports\":" + String(matchedReports);
  line += ",\"track_ids\":" + String(nextId - 1);
  line += ",\"queue_dropped\":" + String(droppedQueue);
  line += ",\"scan_callbacks\":" + String(scanCallbackCount);
  line += ",\"scan_discovered\":" + String(scanDiscoveredCount);
  line += ",\"scan_rsp_queued\":" + String(scanRspQueued);
  line += ",\"scan_rsp_merged\":" + String(scanRspMerged);
  line += ",\"watch_rules_custom\":" + String(customRuleCount);
  line += ",\"scan_end_events\":" + String(scanEndEvents);
  line += ",\"scan_window_ms\":" + String(bleScanWindowMs);
  line += ",\"scan_restart_failures\":" + String(scanRestartFailures);
  line += ",\"scan_watchdog_restarts\":" + String(scanWatchdogRestarts);
  line += ",\"scan_alive\":" + String(scanAlive(millis()) ? "true" : "false");
  line += ",\"ble_quiet_for_wifi\":" + String(bleQuietActive ? "true" : "false");
  line += ",\"ble_quiet_events\":" + String(bleQuietEvents);
  { const int il = axp2101Read(AXP2101_REG_VBUS_ILIM);
    static const int ilimMa[] = {100, 500, 900, 1000, 1500, 2000, 0, 0};
    line += ",\"vbus_current_limit_ma\":" + String(il < 0 ? 0 : ilimMa[il & 7]); }
  line += ",\"free_heap\":" + String(ESP.getFreeHeap());
  line += ",\"min_free_heap\":" + String(ESP.getMinFreeHeap());
  line += ",\"ap_stations\":" + String((!wifiEnabled || wifiStaMode) ? 0u : (unsigned)WiFi.softAPgetStationNum());
  line += ",\"queue_high_water\":" + String(qaQueueHigh);
  line += ",\"queue_now\":" + String(bleQueue ? uxQueueMessagesWaiting(bleQueue) : 0);
  line += ",\"raw_malformed\":" + String(qaMalformed);
  line += ",\"ad_with_name\":" + String(qaNameFields);
  line += ",\"ad_with_company\":" + String(qaCompanyFields);
  line += ",\"ad_with_service\":" + String(qaServiceFields);
  line += ",\"duplicates_suppressed\":" + String(duplicateSuppressed);
  line += ",\"repeat_min_ms\":" + String(REPEAT_REPORT_MIN_MS);
  line += ",\"selection_filtered\":" + String(selectionFiltered);
  { String sel; for (unsigned i = 0; i < CAT_COUNT; ++i) { if (i) sel += ','; sel += String(CATS[i].key) + "=" + String(catModeName(catMode((uint8_t)i))); }
    line += ",\"selection\":" + asJsonString(sel.c_str()); }
  line += ",\"flash_mask\":" + String(catFlashMask);
  line += ",\"flash_events\":" + String(flashEvents);
  line += ",\"key_events\":" + String(keyEvents);
  line += ",\"pmu_key\":" + String(pmuKeyOK ? "true" : "false");
  line += ",\"screen\":" + asJsonString(uiMode == UI_MENU ? "menu" : "scan");
  line += ",\"epoch\":" + String(sessionEpoch);
  line += "," + W10SubG::statusJson();
  line += ",\"track_evictions\":" + String(tableEvictions);
  line += ",\"gps_chars\":" + String(gps.charsProcessed());
  line += ",\"log_flash\":" + String(flashLogging ? "true" : "false");
  line += ",\"log_flash_full\":" + String(flashFull ? "true" : "false");
  line += ",\"flash_free_bytes\":" + String((unsigned long)flashFreeBytes());
  if (flashLogging) line += ",\"log_file\":" + asJsonString(logFilename);
  line += ",\"wifi\":" + asJsonString(!wifiEnabled ? "off" : wifiUp ? (wifiStaMode ? "sta_up" : "ap_up") : (wifiStaMode ? "sta_connecting" : "ap_down"));
  line += ",\"wifi_ip\":" + asJsonString(wifiIpText);
  line += ",\"web_requests\":" + String(webRequests);
  line += ",\"audio_ready\":" + String(W10Audio::ready ? "true" : "false");
  line += ",\"audio_enabled\":" + String(W10Audio::enabled ? "true" : "false");
  appendGps(line);
  line += '}';
  Serial.println(line);
}

static void reportScanQA() {
  Serial.printf("SCAN QA: discovered=%lu result_callbacks=%lu scan_rsp_queued=%lu scan_rsp_merged=%lu processed=%lu queue_dropped=%lu queue_now=%u queue_high=%lu queue_capacity=%u\n",
    (unsigned long)scanDiscoveredCount,(unsigned long)scanCallbackCount,
    (unsigned long)scanRspQueued,(unsigned long)scanRspMerged,(unsigned long)scanPackets,
    (unsigned long)droppedQueue,(unsigned)(bleQueue?uxQueueMessagesWaiting(bleQueue):0),
    (unsigned long)qaQueueHigh,(unsigned)actualQueueDepth);
  Serial.printf("SCAN QA: rules=%u (%u built-in + %u custom) unmatched=%lu matched=%lu\n",(unsigned)watchCount,
    (unsigned)(watchCount-customRuleCount),(unsigned)customRuleCount,
    (unsigned long)unmatchedPackets,(unsigned long)matchedReports);
  Serial.printf("SCAN QA: empty=%lu malformed=%lu truncated=%lu with_name=%lu with_company=%lu with_services=%lu scan_restarts=%lu\n",
    (unsigned long)scanEmptyPackets,(unsigned long)qaMalformed,(unsigned long)qaTruncated,
    (unsigned long)qaNameFields,(unsigned long)qaCompanyFields,(unsigned long)qaServiceFields,
    (unsigned long)scanEndEvents);
  Serial.printf("SCAN QA: matched OUI=%lu MAC=%lu CID=%lu SVC=%lu NAME=%lu META=%lu UUID128=%lu\n",
    (unsigned long)qaMatchTypes[0],(unsigned long)qaMatchTypes[1],
    (unsigned long)qaMatchTypes[2],(unsigned long)qaMatchTypes[3],
    (unsigned long)qaMatchTypes[4],(unsigned long)qaMatchTypes[5],
    (unsigned long)qaMatchTypes[6]);
  Serial.println("SCAN QA: unmatched raw advertisements are never printed or saved");
}

static const char WEB_PAGE[] PROGMEM =
  "<!DOCTYPE html>\n"
  "<html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
  "<title>W10 OUI DETECTION</title>\n"
  "<link rel=\"stylesheet\" href=\"https://unpkg.com/leaflet@1.9.4/dist/leaflet.css\">\n"
  "<style>\n"
  "html,body{margin:0;height:100%;background:#111;color:#ddd;font:14px system-ui,sans-serif}\n"
  "#map{position:absolute;top:0;left:0;right:0;bottom:0;background:#1b1b1b}\n"
  "#bar{position:absolute;top:0;left:0;right:0;z-index:1000;background:rgba(0,0,0,.78);padding:6px 10px;display:flex;flex-wrap:wrap;gap:8px;align-items:center}\n"
  "#bar b{color:#5ff}\n"
  "button{background:#333;color:#eee;border:1px solid #666;border-radius:4px;padding:4px 8px}\n"
  "button.on{background:#176;border-color:#3c9}\n"
  "#list{position:absolute;bottom:0;left:0;right:0;max-height:26%;overflow:auto;z-index:1000;background:rgba(0,0,0,.8);font-size:12px}\n"
  "#list.hidden{display:none}\n"
  ".leaflet-top{top:58px}\n"
  "#list div{padding:3px 8px;border-top:1px solid #333}\n"
  "#list .p{color:#f66}\n"
  "#list .n{color:#fc6}\n"
  "canvas#fallback{position:absolute;top:0;left:0;width:100%;height:100%;display:none}\n"
  "#sel{position:absolute;top:0;left:0;right:0;bottom:0;z-index:2000;background:#111;overflow:auto;padding:10px 12px 24px}\n"
  "#sel.hidden{display:none}\n"
  "#selh{display:flex;justify-content:space-between;align-items:center;gap:8px;margin-bottom:6px}\n"
  "#selh p{margin:0;font-size:13px;color:#aaa}\n"
  ".row{display:flex;gap:8px;margin:8px 0}\n"
  ".big{flex:1;display:flex;justify-content:space-between;align-items:center;min-height:60px;font-size:20px;border-left:14px solid #888;border-radius:8px;text-align:left;padding:0 14px}\n"
  ".big .st{font-size:15px;color:#9f9}\n"
  ".big.m0{opacity:.4}\n"
  ".big.m1 .st{color:#8df}\n"
  ".big.m2 .st{color:#fc6}\n"
  ".fl{width:72px;min-height:60px;font-size:13px;border-radius:8px}\n"
  ".fl.on{background:#652;border-color:#fa4}\n"
  "#bClear{width:100%;min-height:60px;margin-top:14px;font-size:18px;background:#411;border-color:#f55;border-radius:8px}\n"
  "</style></head><body>\n"
  "<div id=\"map\"></div><canvas id=\"fallback\"></canvas>\n"
  "<div id=\"bar\"><b>W10 OUI</b><span id=\"st\">loading</span>\n"
  "<button id=\"bHeat\" class=\"on\">heat</button><button id=\"bDots\" class=\"on\">dots</button>\n"
  "<button id=\"bFollow\" class=\"on\">follow GPS</button><button id=\"bAll\">all sessions</button><button id=\"bFit\">fit</button><button id=\"bList\" class=\"on\">list</button><button id=\"bSel\">select</button></div>\n"
  "<div id=\"list\"></div>\n"
  "<div id=\"sel\" class=\"hidden\"><div id=\"selh\"><div><b>Detections</b><p>Tap a category: SHOW+REC &gt; SHOW &gt; REC &gt; OFF. SHOW = screen, sound, bar. REC = log (this map). FLASH = 2 s screen flash.</p></div><button id=\"bSelClose\">close</button></div>\n"
  "<div id=\"selList\"></div><button id=\"bClear\">CLEAR HITS recorded since power-on</button></div>\n"
  "<script src=\"https://unpkg.com/leaflet@1.9.4/dist/leaflet.js\"></script>\n"
  "<script src=\"https://unpkg.com/leaflet.heat@0.2.0/dist/leaflet-heat.js\"></script>\n"
  "<script>\n"
  "const S={pts:[],seen:0,sessions:[],cur:null,off:{},heat:true,dots:true,follow:true,all:false,map:null,hl:null,dl:null,me:null,lastFit:0,status:{}};\n"
  "const $=id=>document.getElementById(id);\n"
  "function label(o){return (o.alias||o.device_guess||o.matched_signature||'match')}\n"
  "function addPoint(o){\n"
  "  if(!((o.event==='ble'||o.event==='subg')&&o.gps_fix&&typeof o.lat==='number'&&typeof o.lon==='number'))return;\n"
  "  const p={lat:o.lat,lon:o.lon,rssi:o.rssi,pri:!!o.priority_match,lab:label(o),mac:o.mac,utc:o.utc||'',id:o.device_id,method:o.match_method,ident:o.matched_identifier,conf:o.confidence||''};\n"
  "  S.pts.push(p);\n"
  "  if(S.map){\n"
  "    if(S.hl)S.hl.addLatLng([p.lat,p.lon,1]);\n"
  "    if(S.dl){const m=L.circleMarker([p.lat,p.lon],{radius:5,color:p.pri\?'#f33':'#fc3',weight:1,fillOpacity:.8});\n"
  "      m.bindPopup(`<b>${p.lab}</b><br>${p.mac} ${p.rssi} dBm<br>${p.method} ${p.ident}<br>${p.utc}`);S.dl.addLayer(m);}\n"
  "  }\n"
  "  const d=document.createElement('div');d.className=p.pri\?'p':'n';\n"
  "  d.textContent=`#${p.id} ${p.lab} ${p.rssi}dBm ${p.conf?'['+p.conf+'] ':''}${p.utc} ${p.lat.toFixed(5)},${p.lon.toFixed(5)}`;\n"
  "  const l=$('list');l.insertBefore(d,l.firstChild);while(l.childElementCount>150)l.removeChild(l.lastChild);\n"
  "}\n"
  "function ingest(txt){for(const line of txt.split('\\n')){if(line[0]!=='{')continue;try{addPoint(JSON.parse(line))}catch(e){}}}\n"
  "async function tail(file){\n"
  "  let off=S.off[file]||0;\n"
  "  for(let i=0;i<40;i++){\n"
  "    const r=await fetch(`/api/tail\?file=${encodeURIComponent(file)}&offset=${off}`,{cache:'no-store'});\n"
  "    if(!r.ok)break;\n"
  "    const next=+r.headers.get('X-Offset');const txt=await r.text();\n"
  "    if(txt.length)ingest(txt);\n"
  "    if(!(next>off))break;off=next;\n"
  "    if(r.headers.get('X-End')==='1')break;\n"
  "  }\n"
  "  S.off[file]=off;\n"
  "}\n"
  "async function refreshStatus(){\n"
  "  try{const r=await fetch('/api/status',{cache:'no-store'});const j=await r.json();S.status=j;\n"
  "    if(S.epoch!=null&&j.epoch!==S.epoch){location.reload();return;}S.epoch=j.epoch;\n"
  "    $('st').textContent=` hits ${j.matched_reports} | gps ${j.gps_fix\?'fix '+j.sats+' sat':'searching'} | rules ${j.watch_rules} | points ${S.pts.length}${j.subg&&j.subg.present\?' | subg '+(j.subg.enabled\?j.subg.bursts+' radio/'+j.subg.lora_decoded+' lora':'off'):''}`;\n"
  "    if(j.log_file&&j.log_file!==S.cur){S.cur=j.log_file;}\n"
  "    if(S.map&&j.gps_fix){const ll=[j.lat,j.lon];if(!S.me){S.me=L.circleMarker(ll,{radius:7,color:'#3cf',weight:2,fillOpacity:.5}).addTo(S.map);}else S.me.setLatLng(ll);\n"
  "      if(S.follow)S.map.setView(ll,Math.max(S.map.getZoom(),14),{animate:false});}\n"
  "  }catch(e){$('st').textContent=' no link';}\n"
  "}\n"
  "async function loadSessions(){\n"
  "  const r=await fetch('/api/sessions',{cache:'no-store'});const j=await r.json();S.sessions=j.sessions||[];\n"
  "  const files=S.all\?S.sessions.map(s=>s.name):(S.cur\?[S.cur]:S.sessions.slice(-1).map(s=>s.name));\n"
  "  for(const f of files)await tail(f);\n"
  "  fit();\n"
  "}\n"
  "function fit(){if(!S.map||!S.pts.length)return;const b=L.latLngBounds(S.pts.map(p=>[p.lat,p.lon]));S.map.fitBounds(b.pad(.2));}\n"
  "function initMap(){\n"
  "  if(typeof L==='undefined'||!L.heatLayer){fallback();return;}\n"
  "  S.map=L.map('map',{zoomControl:false}).setView([34.05,-117.8],10);L.control.zoom({position:'topright'}).addTo(S.map);\n"
  "  L.tileLayer('https://tile.openstreetmap.org/{z}/{x}/{y}.png',{maxZoom:19,attribution:'&copy; OpenStreetMap'}).addTo(S.map);\n"
  "  S.hl=L.heatLayer(S.pts.map(p=>[p.lat,p.lon,1]),{radius:28,blur:18,maxZoom:17}).addTo(S.map);\n"
  "  S.dl=L.layerGroup().addTo(S.map);\n"
  "  for(const p of S.pts.slice(-600)){const m=L.circleMarker([p.lat,p.lon],{radius:5,color:p.pri\?'#f33':'#fc3',weight:1,fillOpacity:.8});m.bindPopup(`<b>${p.lab}</b><br>${p.mac} ${p.rssi} dBm<br>${p.utc}`);S.dl.addLayer(m);}\n"
  "  S.map.on('dragstart',()=>{S.follow=false;$('bFollow').className='';});\n"
  "}\n"
  "function fallback(){\n"
  "  const c=$('fallback');c.style.display='block';$('map').style.display='none';\n"
  "  function draw(){\n"
  "    c.width=c.clientWidth;c.height=c.clientHeight;const g=c.getContext('2d');g.fillStyle='#1b1b1b';g.fillRect(0,0,c.width,c.height);\n"
  "    g.fillStyle='#aaa';g.font='13px sans-serif';g.fillText('Offline view: no map tiles without internet (use WIFI SET to join the phone hotspot).',10,c.height*0.08);g.fillText('Points are plotted relative to each other; red = priority rule.',10,c.height*0.08+18);\n"
  "    if(!S.pts.length)return;\n"
  "    let la0=1e9,la1=-1e9,lo0=1e9,lo1=-1e9;for(const p of S.pts){la0=Math.min(la0,p.lat);la1=Math.max(la1,p.lat);lo0=Math.min(lo0,p.lon);lo1=Math.max(lo1,p.lon);}\n"
  "    const pad=0.0005;la0-=pad;la1+=pad;lo0-=pad;lo1+=pad;const kx=Math.cos((la0+la1)/2*Math.PI/180);\n"
  "    const w=(lo1-lo0)*kx,h=(la1-la0),s=Math.min((c.width-40)/w,(c.height-80)/h);\n"
  "    for(const p of S.pts){const x=20+(p.lon-lo0)*kx*s,y=60+(la1-p.lat)*s;g.beginPath();g.arc(x,y,9,0,7);g.fillStyle=p.pri\?'rgba(255,60,60,.35)':'rgba(255,200,60,.35)';g.fill();g.beginPath();g.arc(x,y,3,0,7);g.fillStyle=p.pri\?'#f33':'#fc3';g.fill();}\n"
  "  }\n"
  "  S.fallbackDraw=draw;draw();window.addEventListener('resize',draw);\n"
  "}\n"
  "$('bHeat').onclick=e=>{S.heat=!S.heat;e.target.className=S.heat\?'on':'';if(S.hl)S.heat\?S.hl.addTo(S.map):S.map.removeLayer(S.hl);};\n"
  "$('bDots').onclick=e=>{S.dots=!S.dots;e.target.className=S.dots\?'on':'';if(S.dl)S.dots\?S.dl.addTo(S.map):S.map.removeLayer(S.dl);};\n"
  "$('bFollow').onclick=e=>{S.follow=!S.follow;e.target.className=S.follow\?'on':'';};\n"
  "$('bAll').onclick=async e=>{S.all=!S.all;e.target.className=S.all\?'on':'';await loadSessions();if(S.fallbackDraw)S.fallbackDraw();};\n"
  "$('bFit').onclick=()=>{S.follow=false;$('bFollow').className='';fit();};\n"
  "$('bList').onclick=e=>{const l=$('list');l.classList.toggle('hidden');e.target.className=l.classList.contains('hidden')\?'':'on';};\n"
  "const MODE=['OFF','SHOW','REC','SHOW+REC'],NEXT=[3,2,0,1];\n"
  "function renderSel(j){const l=$('selList');l.innerHTML='';\n"
  "  for(const c of j.cats){const r=document.createElement('div');r.className='row';\n"
  "    const b=document.createElement('button');b.className='big m'+c.mode;b.style.borderLeftColor=c.color;\n"
  "    b.innerHTML=`<span>${c.name}</span><span class=\"st\">${MODE[c.mode]}</span>`;\n"
  "    b.onclick=async()=>{await fetch(`/api/select\?cat=${c.id}&mode=${NEXT[c.mode]}`,{cache:'no-store'});loadSel();};\n"
  "    const f=document.createElement('button');f.className='fl'+(c.flash\?' on':'');f.textContent=c.flash\?'FLASH on':'flash off';\n"
  "    f.onclick=async()=>{await fetch(`/api/select\?cat=${c.id}&flash=${c.flash\?0:1}`,{cache:'no-store'});loadSel();};\n"
  "    r.appendChild(b);r.appendChild(f);l.appendChild(r);}\n"
  "  $('bClear').textContent=`CLEAR HITS recorded since power-on (${j.hits} shown)`;\n"
  "}\n"
  "async function loadSel(){try{const r=await fetch('/api/select',{cache:'no-store'});renderSel(await r.json());}catch(e){}}\n"
  "$('bSel').onclick=()=>{$('sel').classList.remove('hidden');loadSel();};\n"
  "$('bSelClose').onclick=()=>$('sel').classList.add('hidden');\n"
  "$('bClear').onclick=async()=>{if(!confirm('Clear every hit recorded since power-on (count, bar, device table and this session log)\?'))return;await fetch('/api/clear\?yes=1',{cache:'no-store'});location.reload();};\n"
  "(async()=>{\n"
  "  setTimeout(initMap,300);\n"
  "  await refreshStatus();await loadSessions();if(S.fallbackDraw)S.fallbackDraw();\n"
  "  setInterval(async()=>{await refreshStatus();if(S.cur)await tail(S.cur);if(S.fallbackDraw)S.fallbackDraw();},2500);\n"
  "})();\n"
  "</script></body></html>\n"
  "\n";

static void loadWifiConfig() {
  wifiStaSsid = ""; wifiStaPass = ""; wifiApSsid = WIFI_AP_SSID_DEFAULT; wifiApPass = WIFI_AP_PASS_DEFAULT;
  wifiEnabled = WIFI_ON_DEFAULT;
  if (!watchPrefs.begin("w10_watch", true)) return;
  wifiStaSsid = watchPrefs.getString("wifi_ssid", "");
  wifiStaPass = watchPrefs.getString("wifi_pass", "");
  wifiApSsid = watchPrefs.getString("wifi_ap_ssid", WIFI_AP_SSID_DEFAULT);
  wifiApPass = watchPrefs.getString("wifi_ap_pass", WIFI_AP_PASS_DEFAULT);
  wifiEnabled = watchPrefs.getUChar("wifi_on", WIFI_ON_DEFAULT ? 1 : 0) != 0;
  watchPrefs.end();
}
static bool saveWifiKey(const char* key, const String& value) {
  if (!watchPrefs.begin("w10_watch", false)) return false;
  const bool ok = watchPrefs.putString(key, value) == value.length();
  watchPrefs.end(); return ok;
}
static String wifiScreenLine() {
  if (!wifiEnabled || !wifiUp) return "";
  return String(wifiStaMode ? "IP " : "AP ") + wifiIpText;
}
static String jsonSessions() {
  String out = "{\"sessions\":[";
  bool first = true;
  if (mountFlash(false)) {
    File dir = LittleFS.open("/logs");
    if (dir && dir.isDirectory()) {
      File entry = dir.openNextFile();
      while (entry) {
        if (!entry.isDirectory()) {
          String name = entry.name();
          int slash = name.lastIndexOf('/'); if (slash >= 0) name = name.substring(slash + 1);
          if (!first) out += ',';
          first = false;
          out += "{\"name\":" + asJsonString(name.c_str()) + ",\"size\":" + String((unsigned long)entry.size()) + "}";
        }
        entry = dir.openNextFile();
      }
    }
  }
  out += "],\"current\":" + asJsonString(flashLogging ? logFilename + 6 : "") + "}";
  return out;
}
static void webSendStatus() {
  String line = "{\"uptime_ms\":" + String(millis());
  line += ",\"watch_rules\":" + String(watchCount);
  line += ",\"matched_reports\":" + String(matchedReports);
  line += ",\"packets_scanned\":" + String(scanPackets);
  line += ",\"last_label\":" + asJsonString(latestLabel);
  line += ",\"last_rssi\":" + String(latestRSSI);
  line += ",\"log_file\":" + asJsonString(flashLogging ? logFilename + 6 : "");
  line += ",\"wifi_mode\":" + asJsonString(wifiStaMode ? "sta" : "ap");
  line += ",\"epoch\":" + String(sessionEpoch);
  line += "," + W10SubG::statusJson();
  appendGps(line);
  line += '}';
  webServer.send(200, "application/json", line);
}
static String jsonSelection() {
  String out = "{\"cats\":[";
  for (unsigned i = 0; i < CAT_COUNT; ++i) {
    const uint16_t c = CATS[i].color;
    char color[8];
    snprintf(color, sizeof(color), "#%02x%02x%02x", (unsigned)(((c >> 11) & 31) * 255 / 31), (unsigned)(((c >> 5) & 63) * 255 / 63), (unsigned)((c & 31) * 255 / 31));
    if (i) out += ',';
    out += "{\"id\":" + String(i) + ",\"name\":" + asJsonString(CATS[i].name) + ",\"mode\":" + String(catMode((uint8_t)i));
    out += ",\"flash\":" + String((catFlashMask >> i) & 1u ? "true" : "false") + ",\"color\":" + asJsonString(color) + "}";
  }
  out += "],\"repeat_min_ms\":" + String(REPEAT_REPORT_MIN_MS) + ",\"epoch\":" + String(sessionEpoch) + ",\"hits\":" + String(matchedReports) + "}";
  return out;
}

static void webSendSelect() {
  if (webServer.hasArg("cat")) {
    const int cat = webServer.arg("cat").toInt();
    if (cat < 0 || cat >= (int)CAT_COUNT) { webServer.send(400, "text/plain", "bad cat"); return; }
    if (webServer.hasArg("mode")) {
      const int m = webServer.arg("mode").toInt();
      if (m < 0 || m > 3) { webServer.send(400, "text/plain", "bad mode"); return; }
      setCatMode((uint8_t)cat, (uint8_t)m);
    }
    if (webServer.hasArg("flash")) {
      if (webServer.arg("flash") == "1") catFlashMask |= (uint16_t)(1u << cat); else catFlashMask &= (uint16_t)~(1u << cat);
    }
    const bool saved = saveSelection();
    menuDirty = true;
    Serial.printf("SELECT (web): %s %s flash %s (%s)\n", CATS[cat].name, catModeName(catMode((uint8_t)cat)),
                  (catFlashMask >> cat) & 1u ? "ON" : "OFF", saved ? "saved" : "NVS SAVE FAILED");
  }
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "application/json", jsonSelection());
}
static void webSendClear() {
  if (webServer.arg("yes") != "1") { webServer.send(400, "text/plain", "add ?yes=1"); return; }
  clearSessionHits();
  menuDirty = true;
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "application/json", "{\"cleared\":true,\"epoch\":" + String(sessionEpoch) + "}");
}
static void webSendTail() {
  String name = webServer.arg("file");
  if (name.indexOf('/') >= 0 || name.indexOf("..") >= 0 || !name.endsWith(".jsonl")) {
    webServer.send(400, "text/plain", "bad file"); return;
  }
  const String path = "/logs/" + name;
  if (!mountFlash(false) || !LittleFS.exists(path)) { webServer.send(404, "text/plain", "no such log"); return; }

  if (logFile && path.equals(logFilename)) logFile.close();
  File f = LittleFS.open(path, FILE_READ);
  if (!f) { webServer.send(500, "text/plain", "open failed"); return; }
  const size_t size = f.size();
  size_t offset = (size_t)strtoul(webServer.arg("offset").c_str(), nullptr, 10);
  if (offset > size) offset = size;
  String chunk;
  size_t next = offset;
  if (offset < size) {
    f.seek(offset);
    const size_t want = min(WEB_TAIL_CHUNK, size - offset);
    chunk.reserve(want + 1);
    for (size_t i = 0; i < want; ++i) { int c = f.read(); if (c < 0) break; chunk += (char)c; }
    next = offset + chunk.length();

    if (next < size) {
      int cut = chunk.lastIndexOf('\n');
      if (cut >= 0) { chunk.remove(cut + 1); next = offset + cut + 1; }
    }
  }
  f.close();
  webServer.sendHeader("X-Offset", String((unsigned long)next));
  webServer.sendHeader("X-End", next >= size ? "1" : "0");
  webServer.sendHeader("Cache-Control", "no-store");
  webServer.send(200, "text/plain", chunk);
}
static void startWebServer() {
  if (webServerStarted) return;
  webServer.on("/", HTTP_GET, []() { ++webRequests; webServer.send_P(200, "text/html", WEB_PAGE); });
  webServer.on("/api/status", HTTP_GET, []() { ++webRequests; webSendStatus(); });
  webServer.on("/api/sessions", HTTP_GET, []() { ++webRequests; webServer.send(200, "application/json", jsonSessions()); });
  webServer.on("/api/tail", HTTP_GET, []() { ++webRequests; webSendTail(); });
  webServer.on("/api/select", HTTP_GET, []() { ++webRequests; webSendSelect(); });
  webServer.on("/api/clear", HTTP_GET, []() { ++webRequests; webSendClear(); });
  webServer.onNotFound([]() { webServer.send(404, "text/plain", "W10: not found"); });
  webServer.begin();
  webServerStarted = true;
  if (MDNS.begin(WIFI_MDNS_NAME)) MDNS.addService("http", "tcp", 80);
}
static void wifiEventHandler(arduino_event_id_t event) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_AP_STACONNECTED:
    case ARDUINO_EVENT_WIFI_STA_START:
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      bleQuietUntilMs = millis() + BLE_QUIET_FOR_WIFI_MS; break;
    case ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED:
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      bleQuietUntilMs = millis() + 1000; break;
    default: break;
  }
}
static void wifiStart() {
  static bool eventHooked = false;
  if (!eventHooked) { WiFi.onEvent(wifiEventHandler); eventHooked = true; }
  wifiUp = false; wifiIpText[0] = 0;
  if (!wifiEnabled) { WiFi.mode(WIFI_OFF); Serial.println("WIFI: off (WIFI ON to enable)"); return; }
  if (wifiStaSsid.length()) {
    wifiStaMode = true;
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(wifiStaSsid.c_str(), wifiStaPass.c_str());
    wifiConnectStartMs = millis();
    Serial.printf("WIFI: joining \"%s\" (phone hotspot); page at http://%s.local or the IP shown on the TFT\n",
                  wifiStaSsid.c_str(), WIFI_MDNS_NAME);
  } else {
    wifiStaMode = false;
    WiFi.mode(WIFI_AP);
    if (WiFi.softAP(wifiApSsid.c_str(), wifiApPass.c_str())) {
      wifiUp = true;
      strlcpy(wifiIpText, WiFi.softAPIP().toString().c_str(), sizeof(wifiIpText));
      startWebServer();
      Serial.printf("WIFI: access point \"%s\" password \"%s\", page at http://%s/ (no map tiles without internet; WIFI SET joins your phone hotspot instead)\n",
                    wifiApSsid.c_str(), wifiApPass.c_str(), wifiIpText);
    } else Serial.println("WIFI ERROR: access point did not start");
  }
}
static void wifiStop() {
  if (webServerStarted) { webServer.stop(); MDNS.end(); webServerStarted = false; }
  WiFi.disconnect(true, true);
  WiFi.mode(WIFI_OFF);
  wifiUp = false; wifiIpText[0] = 0;
}
static void wifiService(uint32_t now) {
  if (!wifiEnabled) return;
  if (webServerStarted) webServer.handleClient();
  if (now - wifiLastCheckMs < 1000) return;
  wifiLastCheckMs = now;
  if (!wifiStaMode) {

    static unsigned lastStations = 0;
    const unsigned stations = WiFi.softAPgetStationNum();
    if (stations > lastStations) bleQuietUntilMs = now + BLE_QUIET_FOR_WIFI_MS;
    lastStations = stations;
    return;
  }
  const bool connected = WiFi.status() == WL_CONNECTED;
  if (connected && !wifiUp) {
    wifiUp = true;
    strlcpy(wifiIpText, WiFi.localIP().toString().c_str(), sizeof(wifiIpText));
    startWebServer();
    Serial.printf("WIFI: connected, page at http://%s/ (http://%s.local)\n", wifiIpText, WIFI_MDNS_NAME);
  } else if (!connected && wifiUp) {
    wifiUp = false; wifiIpText[0] = 0;
    Serial.println("WIFI: hotspot lost; reconnecting in the background");
  }
}
static void processWifiCommand(const String& command, const String& raw) {
  if (command == "WIFI STATUS") {
    Serial.printf("WIFI: %s, mode %s, ssid \"%s\", %s, ip %s, stations %u, requests %lu, ble_quiet_events %lu\n",
                  wifiEnabled ? "enabled" : "disabled", wifiStaMode ? "STA" : "AP",
                  wifiStaMode ? wifiStaSsid.c_str() : wifiApSsid.c_str(),
                  wifiUp ? "up" : "down", wifiIpText[0] ? wifiIpText : "-",
                  wifiStaMode ? 0u : (unsigned)WiFi.softAPgetStationNum(), (unsigned long)webRequests, (unsigned long)bleQuietEvents);
  } else if (command == "WIFI ON" || command == "WIFI OFF") {
    wifiEnabled = command.endsWith("ON");
    if (watchPrefs.begin("w10_watch", false)) { watchPrefs.putUChar("wifi_on", wifiEnabled ? 1 : 0); watchPrefs.end(); }
    if (wifiEnabled) wifiStart(); else wifiStop();
    applyScanWindow(wifiEnabled);
    if (bleScanner) bleScanner->start(BLE_SCAN_CYCLE_MS, false, true);
    Serial.println(wifiEnabled ? "WIFI: enabled (saved); BLE scan window 50%" : "WIFI: disabled (saved); BLE scan window 100%");
  } else if (command.startsWith("WIFI SET ") || command.startsWith("WIFI AP ")) {
    const bool ap = command.startsWith("WIFI AP ");
    String trimmedRaw = raw; trimmedRaw.trim();
    String args = trimmedRaw.substring(ap ? 8 : 9); args.trim();
    int sep = args.indexOf('|');
    String ssid = sep < 0 ? args : args.substring(0, sep); ssid.trim();
    String pass = sep < 0 ? "" : args.substring(sep + 1); pass.trim();
    if (!ssid.length() || ssid.length() > 32 || pass.length() > 63 || (ap && pass.length() && pass.length() < 8)) {
      Serial.println("WIFI ERROR: WIFI SET <ssid> | <password>   (AP password 8+ characters)"); return;
    }
    const bool ok = saveWifiKey(ap ? "wifi_ap_ssid" : "wifi_ssid", ssid) && saveWifiKey(ap ? "wifi_ap_pass" : "wifi_pass", pass);
    if (!ok) { Serial.println("WIFI ERROR: NVS save failed"); return; }
    loadWifiConfig();
    Serial.printf("WIFI: %s credentials saved for \"%s\"\n", ap ? "access point" : "hotspot", ssid.c_str());
    if (wifiEnabled) { wifiStop(); wifiStart(); applyScanWindow(true); if (bleScanner) bleScanner->start(BLE_SCAN_CYCLE_MS, false, true); }
  } else if (command == "WIFI CLEAR") {
    if (watchPrefs.begin("w10_watch", false)) { watchPrefs.remove("wifi_ssid"); watchPrefs.remove("wifi_pass"); watchPrefs.end(); }
    loadWifiConfig();
    Serial.println("WIFI: hotspot credentials cleared; access point mode");
    if (wifiEnabled) { wifiStop(); wifiStart(); applyScanWindow(true); if (bleScanner) bleScanner->start(BLE_SCAN_CYCLE_MS, false, true); }
  } else {
    Serial.println("WIFI: STATUS | ON | OFF | SET <ssid> | <password> | AP <ssid> | <password> | CLEAR");
  }
}

static bool matcherCheck(const char* what,const char* mac,const uint8_t* ad,unsigned len,
                         const char* expectedMethod,bool shouldMatch=true) {
  AdvRecord r={};strlcpy(r.mac,mac,sizeof(r.mac));r.addrType=0;
  if(len>BLE_RAW_MAX) return false;
  r.payloadLength=(uint8_t)len;
  if(len)memcpy(r.bytes,ad,len);
  AdvFields f=decodePayload(r);
  SigMatch found=matchSignature(r,f);
  const bool pass=(found.matched==shouldMatch) &&
                  (!shouldMatch || strcmp(found.method,expectedMethod)==0);
  Serial.printf("WATCH SELFTEST %s: %s (actual=%s)\n",
                what,pass?"PASS":"FAIL",found.matched?found.method:"none");
  return pass;
}

static void runMatcherSelfTest() {

  static const uint8_t axonCid[]={3,0xFF,0x4D,0x03};
  static const uint8_t axonSvc[]={3,0x03,0x81,0xFC};
  static const uint8_t axonSvcFE6B[]={3,0x03,0x6B,0xFE};
  static const uint8_t axonSvcFE6C[]={3,0x03,0x6C,0xFE};
  static const uint8_t metaBoth[]={3,0xFF,0x53,0x0D,3,0x03,0x5F,0xFD};
  static const uint8_t metaName[]={8,0x09,'R','a','y','-','B','a','n'};
  static const uint8_t irrelevant[]={3,0xFF,0x4C,0x00};
  unsigned passed=0,total=0;
  ++total;passed+=matcherCheck("Axon OUI","00:25:DF:11:22:33",nullptr,0,"OUI");
  ++total;passed+=matcherCheck("Axon CID","22:33:44:11:22:33",axonCid,sizeof(axonCid),"CID");
  ++total;passed+=matcherCheck("Axon UUID16","22:33:44:11:22:33",axonSvc,sizeof(axonSvc),"SVC");
  bool fe6bInstalled=false, fe6cInstalled=false;
  for (unsigned i=0;i<watchCount;i++) {
    if(watchRules[i].type==WT_SVC && watchRules[i].ident=="FE6B") fe6bInstalled=true;
    if(watchRules[i].type==WT_SVC && watchRules[i].ident=="FE6C") fe6cInstalled=true;
  }
  if(fe6bInstalled) { ++total;passed+=matcherCheck("Axon FE6B UUID16","22:33:44:11:22:33",axonSvcFE6B,sizeof(axonSvcFE6B),"SVC"); }
  if(fe6cInstalled) { ++total;passed+=matcherCheck("Axon FE6C UUID16","22:33:44:11:22:33",axonSvcFE6C,sizeof(axonSvcFE6C),"SVC"); }
  ++total;passed+=matcherCheck("Meta composite","22:33:44:11:22:33",metaBoth,sizeof(metaBoth),"META");
  ++total;passed+=matcherCheck("Meta name","22:33:44:11:22:33",metaName,sizeof(metaName),"META");
  ++total;passed+=matcherCheck("Unrelated BLE","22:33:44:11:22:33",irrelevant,sizeof(irrelevant),"",false);

  bool flockCidInstalled=false, flockPenguinInstalled=false, flockBatteryInstalled=false;
  for(unsigned i=0;i<watchCount;i++) {
    if(watchRules[i].type==WT_CID && watchRules[i].ident=="09C8") flockCidInstalled=true;
    if(watchRules[i].type==WT_NAME && watchRules[i].ident=="Penguin-") flockPenguinInstalled=true;
    if(watchRules[i].type==WT_NAME && watchRules[i].ident=="FS Ext Battery") flockBatteryInstalled=true;
  }
  if(flockCidInstalled) {
    static const uint8_t xuntongAd[]={3,0xFF,0xC8,0x09};
    ++total;passed+=matcherCheck("XUNTONG CID 09C8 (not Flock proof)",
          "22:33:44:11:22:33",xuntongAd,sizeof(xuntongAd),"CID");
  }
  if(flockPenguinInstalled) {
    const char* str="Penguin-1234567890";
    uint8_t ad[48] = {(uint8_t)(strlen(str)+1),0x09};
    memcpy(ad+2,str,strlen(str));
    ++total;passed+=matcherCheck("Possible Flock Penguin battery",
          "22:33:44:11:22:33",ad,strlen(str)+2,"NAME");
  }
  if(flockBatteryInstalled) {
    const char* str="FS Ext Battery";
    uint8_t ad[48] = {(uint8_t)(strlen(str)+1),0x09};
    memcpy(ad+2,str,strlen(str));
    ++total;passed+=matcherCheck("Possible Flock external battery",
          "22:33:44:11:22:33",ad,strlen(str)+2,"NAME");
  }
  bool flockUuidInstalled=false;
  for(unsigned i=0;i<watchCount;i++)
    if(watchRules[i].type==WT_UUID128 &&
       watchRules[i].ident=="E8CCBB38953246A89FE51814DF172E6F")
      flockUuidInstalled=true;
  if(flockUuidInstalled) {
    static const uint8_t flockUuid[]={17,0x07,0x6F,0x2E,0x17,0xDF,0x14,0x18,
      0xE5,0x9F,0xA8,0x46,0x32,0x95,0x38,0xBB,0xCC,0xE8};
    ++total;passed+=matcherCheck("Flock UUID128","22:33:44:11:22:33",
                                  flockUuid,sizeof(flockUuid),"UUID128");
  }

  {
    static const uint8_t tileSvc[]={3,0x03,0xED,0xFE};
    static const uint8_t smartTag[]={3,0x03,0x5A,0xFD};
    static const uint8_t raven3100[]={3,0x03,0x00,0x31};
    static const uint8_t raven3101[]={3,0x03,0x01,0x31};
    static const uint8_t flipperSvc[]={3,0x03,0x81,0x30};
    static const uint8_t chipoloCid[]={3,0xFF,0xC3,0x08};
    static const uint8_t bwcTag[]={10,0x09,'B','W','C','D','E','V','I','C','E'};
    static const uint8_t bwcRev[]={10,0x09,'E','C','I','V','E','D','C','W','B'};
    static const uint8_t fsHex[]={8,0x09,'F','S','-','1','A','2','B'};
    static const uint8_t fsBad[]={8,0x09,'F','S','-','1','G','2','B'};
    static const uint8_t tenDigits[]={11,0x09,'1','2','3','4','5','6','7','8','9','0'};
    static const uint8_t findMy[]={30,0xFF,0x4C,0x00,0x12,0x19,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24};
    static const uint8_t airpodsLike[]={30,0xFF,0x4C,0x00,0x07,0x19,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24};
    static const uint8_t fmdn[]={6,0x16,0xAA,0xFE,0x40,0x11,0x22};
    static const uint8_t eddystoneUid[]={6,0x16,0xAA,0xFE,0x00,0x11,0x22};
    static const uint8_t flockSetup[]={12,0x09,'F','l','o','c','k','_','S','e','t','u','p'};
    ++total;passed+=matcherCheck("Tile FEED","22:33:44:11:22:33",tileSvc,sizeof(tileSvc),"SVC");
    ++total;passed+=matcherCheck("SmartTag FD5A","22:33:44:11:22:33",smartTag,sizeof(smartTag),"SVC");
    ++total;passed+=matcherCheck("Raven 3100","22:33:44:11:22:33",raven3100,sizeof(raven3100),"SVC");
    ++total;passed+=matcherCheck("Raven range trap 3101","22:33:44:11:22:33",raven3101,sizeof(raven3101),"",false);
    ++total;passed+=matcherCheck("Flipper 3081","22:33:44:11:22:33",flipperSvc,sizeof(flipperSvc),"SVC");
    ++total;passed+=matcherCheck("Chipolo CID (low)","22:33:44:11:22:33",chipoloCid,sizeof(chipoloCid),"CID");
    ++total;passed+=matcherCheck("Flock_Setup name","22:33:44:11:22:33",flockSetup,sizeof(flockSetup),"NAME");
    if (validatorsEnabled) {
      ++total;passed+=matcherCheck("BWCDEVICE tag","22:33:44:11:22:33",bwcTag,sizeof(bwcTag),"VALIDATOR");
      ++total;passed+=matcherCheck("BWCDEVICE reversed","22:33:44:11:22:33",bwcRev,sizeof(bwcRev),"VALIDATOR");
      ++total;passed+=matcherCheck("FS-hex name","22:33:44:11:22:33",fsHex,sizeof(fsHex),"VALIDATOR");
      ++total;passed+=matcherCheck("FS-non-hex name rejected","22:33:44:11:22:33",fsBad,sizeof(fsBad),"",false);
      ++total;passed+=matcherCheck("Ten-digit name (low)","22:33:44:11:22:33",tenDigits,sizeof(tenDigits),"VALIDATOR");
      ++total;passed+=matcherCheck("Apple Find My 4C00-12-19","22:33:44:11:22:33",findMy,sizeof(findMy),"VALIDATOR");
      ++total;passed+=matcherCheck("Apple other type ignored","22:33:44:11:22:33",airpodsLike,sizeof(airpodsLike),"",false);
      ++total;passed+=matcherCheck("Google FMDN FEAA 0x40","22:33:44:11:22:33",fmdn,sizeof(fmdn),"VALIDATOR");
      ++total;passed+=matcherCheck("Eddystone UID ignored","22:33:44:11:22:33",eddystoneUid,sizeof(eddystoneUid),"",false);
    }

    uint8_t rid[4+2+3+50]={0}; unsigned k=0;
    rid[k++]=(uint8_t)(1+4+3+50); rid[k++]=0x16; rid[k++]=0xFA; rid[k++]=0xFF; rid[k++]=0x0D; rid[k++]=0x01;
    rid[k++]=0xF0; rid[k++]=25; rid[k++]=2;
    uint8_t* basic=rid+k; basic[0]=0x00; basic[1]=0x12; memcpy(basic+2,"1581F12345ABCD",14); k+=25;
    uint8_t* loc=rid+k; loc[0]=0x10; const int32_t lat=336846000, lon=-1178265000;
    loc[5]=lat&255; loc[6]=(lat>>8)&255; loc[7]=(lat>>16)&255; loc[8]=(lat>>24)&255;
    loc[9]=lon&255; loc[10]=(lon>>8)&255; loc[11]=(lon>>16)&255; loc[12]=(lon>>24)&255;
    const uint16_t geo=(uint16_t)((120.0f+1000.0f)*2); loc[15]=geo&255; loc[16]=geo>>8; k+=25;
    AdvRecord rr={}; strlcpy(rr.mac,"22:33:44:11:22:33",sizeof(rr.mac)); rr.payloadLength=(uint8_t)k; memcpy(rr.bytes,rid,k);
    AdvFields rf=decodePayload(rr);
    const bool ridOk = rf.ridPresent && rf.ridHasLocation && strcmp(rf.ridSerial,"1581F12345ABCD")==0 && rf.ridUaType==2 &&
                       fabs(rf.ridLat-33.6846)<1e-6 && fabs(rf.ridLon+117.8265)<1e-6 && fabs(rf.ridAltM-120.0f)<0.01f;
    ++total; passed+=ridOk; Serial.printf("WATCH SELFTEST Remote ID decode: %s\n", ridOk?"PASS":"FAIL");
    ++total;passed+=matcherCheck("Remote ID FFFA rule","22:33:44:11:22:33",rid,k,"SVC");
  }
  Serial.printf("WATCH SELFTEST SUMMARY: %u/%u PASS (synthetic input, not RF reception)\n",passed,total);
}

static void processCommand(const String& raw) {
  String command=raw;command.trim();command.toUpperCase();
  if(command=="HELP") {
    Serial.println("W10 OUI DETECTION 0.4.20 matcher: OUI MAC CID SVC NAME META UUID128 + payload validators");
    Serial.println("SELECT (show selection) | SELECT <AXON|FLOCK|RAVEN|RING|DRONE|TRACKER|GLASSES|FLIPPER|OTHER|ALL> ON|SHOW|REC|OFF (saved)");
    Serial.println("SELECT FLASH <category> ON|OFF (2 s screen flash; saved) | MENU (open the on-screen menu) | HITS CLEAR (count, bar, devices, session log)");
    Serial.println("Keys: BOOT = next row / open menu, hold BOOT = previous row, PWR short = toggle, hold PWR 1 s = exit menu (4 s = power off)");
    Serial.println("SUBG | SUBG SCAN (last 7/800 MHz sweep) | SUBG ON|OFF | SUBG THRESHOLD <dBm> | SUBG BAND ADD/CLEAR | SUBG LORA ADD/CLEAR | SUBG RESET (SX1262 receive only)");
    Serial.println("WATCH LIST | WATCH EXPORT | WATCH SAVE | WATCH ADD TYPE ID | Likely device");
    Serial.println("WATCH REMOVE <number> | WATCH CLEAR YES | WATCH PRESET AXON/META");
    Serial.printf("WATCH REPO MERGE | WATCH REPO REPLACE (all %u built-in rules)\n", BUILTIN_RULE_COUNT);
    Serial.println("WATCH FLOCK BLE MERGE (idempotently add 4 Flock BLE signatures)");
    Serial.println("WATCH AXON BLE MERGE (idempotently add FE6B, FE6C)");
    Serial.println("WATCH SELFTEST | SCAN QA (scanner diagnostics) | SCAN TOP (aggregate OUI/CID/UUID/name survey) | SCAN TOP RESET");
    Serial.println("WATCH VALIDATORS ON|OFF (BWCDEVICE, FS-hex, 10-digit, Apple Find My, Google FMDN; Remote ID decode always on)");
    Serial.println("WATCH IMPORT BEGIN [to merge] | WATCH IMPORT REPLACE [to replace]");
    Serial.println("WATCH IMPORT COMMIT | WATCH IMPORT CANCEL (add rules during import)");
    Serial.println("ALIAS LIST | ALIAS SET MAC | Nickname | ALIAS REMOVE MAC");
    Serial.println("STATUS | LOG ON (re-arm flash) | LOG OFF (USB only) | LOG LIST | LOG DUMP /logs/<name> | FORMATFS YES");
    Serial.println("Flash logging is armed at every boot; built-in rules are live at every boot.");
    Serial.println("SOUND ON | SOUND OFF | SOUND VOLUME 0..85 (saved) | SOUND TEST NEW/REPEAT/PRIORITY/SUSTAINED/READY");
    Serial.println("LED CHARGE OFF | LED CHARGE DISABLE | LED CHARGE ON (red AXP2101 charge LED; blue EN and GPS PPS LEDs are hardware)");
    Serial.println("WIFI STATUS | WIFI SET <hotspot ssid> | <password> | WIFI AP <ssid> | <password> | WIFI CLEAR | WIFI ON | WIFI OFF");
    Serial.println("Heat map: join the W10 to your phone hotspot (WIFI SET), then open the IP shown on the TFT or http://w10.local");
    Serial.printf("NOTE: %u built-in BLE rules with confidence tiers; low-confidence hits log without sound. CID 09C8 = XUNTONG, NOT proof of Flock hardware.\n", BUILTIN_RULE_COUNT);
  } else if(command=="SELECT" || command=="SELECT LIST") {
    Serial.printf("SELECT: repeat rule %lu s; SHOW = screen/sound/bar/count, REC = USB+flash JSON (web page)\n", (unsigned long)(REPEAT_REPORT_MIN_MS / 1000));
    for (unsigned i = 0; i < CAT_COUNT; ++i)
      Serial.printf("SELECT  %-8s %-9s flash %s\n", CATS[i].name, catModeName(catMode((uint8_t)i)), (catFlashMask >> i) & 1u ? "ON" : "OFF");
    Serial.printf("SELECT: %lu matches dropped by OFF categories since power-up\n", (unsigned long)selectionFiltered);
  } else if(command.startsWith("SELECT ")) {
    String args = command.substring(7); args.trim();
    const bool flash = args.startsWith("FLASH ");
    if (flash) { args = args.substring(6); args.trim(); }
    const int sp = args.indexOf(' ');
    String catName = sp < 0 ? args : args.substring(0, sp); catName.trim();
    String modeName = sp < 0 ? String("") : args.substring(sp + 1); modeName.trim();
    int cat = -1;
    for (unsigned i = 0; i < CAT_COUNT; ++i) if (catName == CATS[i].name) cat = (int)i;
    int mode = -1;
    if (flash) { if (modeName == "ON") mode = 1; else if (modeName == "OFF") mode = 0; }
    else if (modeName == "ON" || modeName == "BOTH" || modeName == "SHOW+REC") mode = CAT_BOTH;
    else if (modeName == "SHOW") mode = CAT_SHOW;
    else if (modeName == "REC" || modeName == "RECORD" || modeName == "LOG") mode = CAT_REC;
    else if (modeName == "OFF") mode = 0;
    if ((cat < 0 && catName != "ALL") || mode < 0) {
      Serial.println("SELECT ERROR: SELECT <category|ALL> ON|SHOW|REC|OFF  or  SELECT FLASH <category|ALL> ON|OFF  (SELECT lists the categories)");
      return;
    }
    for (unsigned i = 0; i < CAT_COUNT; ++i) {
      if (cat >= 0 && (int)i != cat) continue;
      if (flash) { if (mode) catFlashMask |= (uint16_t)(1u << i); else catFlashMask &= (uint16_t)~(1u << i); }
      else setCatMode((uint8_t)i, (uint8_t)mode);
    }
    menuDirty = true;
    Serial.printf("SELECT: %s %s%s (%s)\n", cat < 0 ? "ALL" : CATS[cat].name, flash ? "flash " : "",
                  flash ? (mode ? "ON" : "OFF") : catModeName((uint8_t)mode), saveSelection() ? "saved" : "NVS SAVE FAILED");
  } else if(command=="SUBG" || command=="SUBG STATUS") {
    W10SubG::printStatus();
  } else if(command=="SUBG SCAN") {
    W10SubG::printSweep();
  } else if(command=="SUBG ON" || command=="SUBG OFF") {
    W10SubG::enabled = command.endsWith("ON");
    if (!W10SubG::enabled && W10SubG::present) { W10SubG::setStandbyXosc(); W10SubG::phase = W10SubG::PH_OFF; }
    W10SubG::saveConfig();
    Serial.println(W10SubG::enabled ? "SUBG: enabled (saved)" : "SUBG: disabled (saved)");
  } else if(command=="SUBG RESET") {
    ++W10SubG::reinits;
    Serial.println(W10SubG::begin() ? "SUBG: SX1262 reset and configured" : "SUBG: SX1262 not responding");
  } else if(command.startsWith("SUBG THRESHOLD ")) {
    const int v = command.substring(15).toInt();
    if (v > -30 || v < -120) { Serial.println("SUBG ERROR: threshold -120..-30 dBm"); return; }
    W10SubG::threshold = v; W10SubG::saveConfig();
    Serial.printf("SUBG: radio burst threshold %d dBm (saved)\n", v);
  } else if(command=="SUBG BAND CLEAR") {
    W10SubG::defaultBands(); W10SubG::saveConfig(); W10SubG::phase = W10SubG::PH_OFF;
    Serial.println("SUBG: bands back to defaults (saved): " + W10SubG::bandsText());
  } else if(command.startsWith("SUBG BAND ADD ")) {
    String a = raw; a.trim(); a = a.substring(14); a.trim();
    const int s1 = a.indexOf(' '); if (s1 < 1) { Serial.println("SUBG ERROR: SUBG BAND ADD <lowMHz> <highMHz> [name]"); return; }
    const int s2 = a.indexOf(' ', s1 + 1);
    const float lo = a.substring(0, s1).toFloat(), hi = (s2 < 0 ? a.substring(s1 + 1) : a.substring(s1 + 1, s2)).toFloat();
    String name = s2 < 0 ? String("band") : a.substring(s2 + 1); name.trim();
    const unsigned before = W10SubG::bandCount;
    W10SubG::addBand(lo, hi, name.c_str());
    if (W10SubG::bandCount == before) { Serial.println("SUBG ERROR: band rejected (150-960 MHz, low < high, max 6 bands)"); return; }
    W10SubG::rebuildSteps(); W10SubG::saveConfig(); W10SubG::phase = W10SubG::PH_OFF;
    Serial.printf("SUBG: band added, %u steps (saved): %s\n", W10SubG::stepCount, W10SubG::bandsText().c_str());
  } else if(command=="SUBG LORA CLEAR") {
    W10SubG::defaultProfiles(); W10SubG::saveConfig();
    Serial.printf("SUBG: LoRa profiles back to defaults (saved), %u profiles\n", W10SubG::profileCount);
  } else if(command.startsWith("SUBG LORA ADD ")) {
    String a = raw; a.trim(); a = a.substring(14); a.trim();
    String f[7]; unsigned n = 0; int p = 0;
    while (n < 7) { int sp = a.indexOf(' ', p); if (sp < 0) { f[n++] = a.substring(p); break; } f[n++] = a.substring(p, sp); p = sp + 1; while (p < (int)a.length() && a[p] == ' ') ++p; }
    if (n < 6) { Serial.println("SUBG ERROR: SUBG LORA ADD <MHz> <bw kHz> <SF> <CR denominator 5..8> <sync hex> <kind 0 generic|1 meshtastic|2 lorawan> [label]"); return; }
    const unsigned before = W10SubG::profileCount;
    W10SubG::addProfile(f[0].toFloat(), (uint16_t)f[1].toInt(), (uint8_t)f[2].toInt(), (uint8_t)f[3].toInt(),
                        (uint8_t)strtoul(f[4].c_str(), nullptr, 16), (uint8_t)f[5].toInt(), 8, n >= 7 ? f[6].c_str() : "custom");
    if (W10SubG::profileCount == before) { Serial.println("SUBG ERROR: profile rejected (150-960 MHz, SF5-12, max 16 profiles)"); return; }
    W10SubG::saveConfig();
    Serial.printf("SUBG: LoRa profile %u added (saved)\n", W10SubG::profileCount);
  } else if(command.startsWith("SUBG")) {
    Serial.println("SUBG: STATUS | SCAN | ON | OFF | RESET | THRESHOLD <dBm> | BAND ADD <lo> <hi> [name] | BAND CLEAR | LORA ADD ... | LORA CLEAR");
  } else if(command=="HITS CLEAR") {
    clearSessionHits();
    menuDirty = true;
    Serial.println("HITS: count, bar, device table and this power-up's session log cleared");
  } else if(command=="MENU") {
    if (uiMode == UI_MENU) closeMenu(); else openMenu(millis());
    Serial.println(uiMode == UI_MENU ? "MENU: open on the screen" : "MENU: closed");
  } else if(command=="WATCH VALIDATORS ON" || command=="WATCH VALIDATORS OFF") {
    validatorsEnabled = command.endsWith("ON");
    if (watchPrefs.begin("w10_watch", false)) { watchPrefs.putUChar("validators", validatorsEnabled ? 1 : 0); watchPrefs.end(); }
    Serial.println(validatorsEnabled ? "WATCH: payload validators ON (saved)" : "WATCH: payload validators OFF (saved)");
  } else if(command=="WATCH SELFTEST") {
    runMatcherSelfTest();
  } else if(command=="SCAN TOP RESET") {
    surveyReset(); Serial.println("SCAN TOP: survey counters cleared");
  } else if(command=="SCAN TOP") {
    auto printTop = [](const char* title, SurveyEntry* table, unsigned n, int width, bool ouiFormat) {
      Serial.printf("SCAN TOP %s (%u distinct)\n", title, n);
      bool shown[SURVEY_SLOTS] = {};
      for (unsigned rank = 0; rank < 15 && rank < n; ++rank) {
        int best = -1;
        for (unsigned i = 0; i < n; ++i)
          if (!shown[i] && (best < 0 || table[i].count > table[best].count)) best = (int)i;
        if (best < 0) break;
        shown[best] = true;
        if (ouiFormat) Serial.printf("  %02X:%02X:%02X  %lu\n", (unsigned)(table[best].key >> 16) & 255,
                                     (unsigned)(table[best].key >> 8) & 255, (unsigned)table[best].key & 255,
                                     (unsigned long)table[best].count);
        else Serial.printf("  %0*lX  %lu\n", width, (unsigned long)table[best].key, (unsigned long)table[best].count);
      }
    };
    Serial.printf("SCAN TOP: advertisements public=%lu random/local=%lu (OUI rules only identify vendors on public addresses)\n",
                  (unsigned long)surveyPublicAdv, (unsigned long)surveyRandomAdv);
    printTop("OUI prefixes (public addresses)", surveyOui, surveyOuiCount, 6, true);
    printTop("company IDs (0xFF manufacturer data)", surveyCid, surveyCidCount, 4, false);
    printTop("service UUID16", surveyUuid, surveyUuidCount, 4, false);
    Serial.printf("SCAN TOP names (%u distinct)\n", surveyNameCount);
    {
      bool shown[SURVEY_NAME_SLOTS] = {};
      for (unsigned rank = 0; rank < 15 && rank < surveyNameCount; ++rank) {
        int best = -1;
        for (unsigned i = 0; i < surveyNameCount; ++i)
          if (!shown[i] && (best < 0 || surveyNames[i].count > surveyNames[best].count)) best = (int)i;
        if (best < 0) break;
        shown[best] = true;
        Serial.printf("  %-23s %lu\n", surveyNames[best].name, (unsigned long)surveyNames[best].count);
      }
    }
    if (surveyOverflow) Serial.printf("SCAN TOP: %lu entries not counted (tables full; SCAN TOP RESET)\n", (unsigned long)surveyOverflow);
    Serial.println("SCAN TOP: aggregate keys only; no addresses or payloads are stored");
  } else if(command=="SCAN QA") {
    reportScanQA();
  } else if(command=="WATCH LIST" || command=="WATCH EXPORT") {
    Serial.printf("BEGIN WATCHLIST V4 count=%u importing=%u\n",
      importingRules ? importCount : watchCount, importingRules?1:0);
    const WatchRule* rules=importingRules ? importRules : watchRules;
    unsigned n=importingRules ? importCount : watchCount;
    for(unsigned i=0;i<n;i++){
      Serial.printf("RULE\t%u\t%s\t%s\t%s\t%s\n",i+1,watchTypeName(rules[i].type),
        rules[i].ident.c_str(),rules[i].description.c_str(),tierName(rules[i].tier));
    }
    Serial.println("END WATCHLIST");
  } else if(command=="WATCH SAVE") {
    Serial.println(saveWatchlist()?"WATCH SAVED":"WATCH SAVE FAILED");
  } else if(command=="WATCH IMPORT BEGIN" || command=="WATCH IMPORT REPLACE") {
    importingRules=true;
    importCount=command.endsWith("REPLACE")?0:watchCount;
    for(unsigned i=0;i<importCount;i++)importRules[i]=watchRules[i];
    Serial.printf("WATCH IMPORT READY: staging=%u\n",(unsigned)importCount);
  } else if(command=="WATCH IMPORT CANCEL") {
    importingRules=false;importCount=0;
    Serial.println("WATCH IMPORT CANCELLED; saved watchlist unchanged");
  } else if(command=="WATCH IMPORT COMMIT") {
    if(!importingRules){Serial.println("WATCH IMPORT ERROR: no active import");return;}

    if(!saveRules(importRules,importCount)){
      Serial.println("WATCH IMPORT ERROR: commit failed, old list retained in RAM");return;
    }
    for(unsigned i=0;i<importCount;i++)watchRules[i]=importRules[i];
    watchCount=importCount;importingRules=false;importCount=0;
    resetWatchTracks();
    Serial.printf("WATCH IMPORT COMMITTED: %u rules\n",(unsigned)watchCount);
  } else if(command.startsWith("WATCH ADD ")) {
    String args=raw.substring(10);args.trim();
    int firstSpace=args.indexOf(' '),sep=args.indexOf('|');
    if(firstSpace<1 || sep<=firstSpace){
      Serial.println("WATCH REJECTED: expected WATCH ADD TYPE ID | Description");return;
    }
    String type=args.substring(0,firstSpace),id=args.substring(firstSpace+1,sep),
           label=args.substring(sep+1);
    WatchType normalizedType;
    String normalizedID=id, normalizedLabel=label;
    if(parseWatchType(type,normalizedType) && validRule(normalizedType,normalizedID,normalizedLabel)) {
      const WatchRule* rules=importingRules?importRules:watchRules;
      unsigned count=importingRules?importCount:watchCount;
      for(unsigned i=0;i<count;i++){
        if(rules[i].type==normalizedType && rules[i].ident.equalsIgnoreCase(normalizedID)){
          Serial.println("WATCH EXISTS: duplicate skipped");return;
        }
      }
    }
    if(addWatchRule(type,id,label)){
      Serial.printf("WATCH ADDED: %u%s\n",importingRules?importCount:watchCount,
        importingRules?" staged":" saved");
    }else Serial.println("WATCH REJECTED: duplicate, invalid, too long, NVS full or capacity reached");
  } else if(command.startsWith("WATCH REMOVE ")) {
    if(importingRules){Serial.println("WATCH: Finish/cancel import first");return;}
    int index=command.substring(13).toInt();
    if(index<1||index>(int)watchCount){Serial.println("WATCH: invalid index");return;}
    WatchRule backup=watchRules[index-1];
    for(unsigned i=index-1;i+1<watchCount;i++)watchRules[i]=watchRules[i+1];
    --watchCount;
    if(!saveWatchlist()){
      for(unsigned i=watchCount;i>=(unsigned)index;i--)watchRules[i]=watchRules[i-1];
      watchRules[index-1]=backup;watchCount++;
      Serial.println("WATCH REMOVE FAILED: saved list preserved");return;
    }
    resetWatchTracks();
    Serial.println(isBuiltinRule(backup.type, backup.ident) ?
      "WATCH REMOVED (built-in rule: returns at next power-up)" : "WATCH REMOVED");
  } else if(command=="WATCH CLEAR YES") {
    if(importingRules){Serial.println("WATCH: finish/cancel import first");return;}

    if(saveRules(watchRules,0)){
      watchCount=0; mergeAllRepositoryRules(watchRules,watchCount); customRuleCount=0;
      resetWatchTracks();
      Serial.printf("WATCH CLEARED: custom rules removed; %u built-in rules remain active\n",(unsigned)watchCount);
    }else Serial.println("WATCH CLEAR FAILED; old list retained");
  } else if(command=="WATCH PRESET AXON" || command=="WATCH PRESET META" || command=="WATCH DEFAULTS") {
    unsigned n=0;
    if(command.endsWith("AXON")||command=="WATCH DEFAULTS")n+=applyPreset("AXON");
    if(command.endsWith("META")||command=="WATCH DEFAULTS")n+=applyPreset("META");
    Serial.printf("WATCH PRESETS: %u added; total %u\n",n,(unsigned)watchCount);
  } else if (command=="WATCH FLOCK BLE MERGE") {
    if(importingRules){Serial.println("WATCH: finish/cancel import first");return;}
    uint16_t staged=watchCount;
    for(unsigned i=0;i<watchCount;i++)importRules[i]=watchRules[i];
    unsigned added=mergeFlockBleRules(importRules,staged);
    if(!added){Serial.printf("WATCH FLOCK BLE: all %u optional rules present\n",FLOCK_BLE_EXTRA_COUNT);}
    else if(!saveRules(importRules,staged)) {
      Serial.println("WATCH FLOCK BLE: save failed; original list unchanged");
    } else {
      for(unsigned i=0;i<staged;i++)watchRules[i]=importRules[i];
      watchCount=staged;resetWatchTracks();
      Serial.printf("WATCH FLOCK BLE: added %u rules; total %u\n",added,(unsigned)watchCount);
    }
  } else if (command=="WATCH AXON BLE MERGE") {
    if(importingRules){Serial.println("WATCH: finish/cancel import first");return;}
    uint16_t staged=watchCount;
    for(unsigned i=0;i<watchCount;i++) importRules[i]=watchRules[i];
    unsigned added=mergeAxonUpstreamRules(importRules,staged);
    if(!added) Serial.println("WATCH AXON BLE: FE6B and FE6C already present");
    else if(!saveRules(importRules,staged))
      Serial.println("WATCH AXON BLE: save failed; existing rules unchanged");
    else {
      for(unsigned i=0;i<staged;i++) watchRules[i]=importRules[i];
      watchCount=staged;resetWatchTracks();
      Serial.printf("WATCH AXON BLE: added %u rules; total %u\n",added,(unsigned)watchCount);
    }
  } else if (command=="WATCH REPO MERGE") {
    if (importingRules) { Serial.println("WATCH: finish/cancel import first"); return; }
    const uint16_t before = watchCount;
    const unsigned added = mergeAllRepositoryRules(watchRules,watchCount);
    if (!added) { Serial.printf("WATCH REPO: all %u rules already present\n",BUILTIN_RULE_COUNT); }
    else if (!saveWatchlist()) {
      watchCount=before;
      Serial.println("WATCH REPO: NVS save failed; original list retained");
    } else {
      resetWatchTracks();
      Serial.printf("WATCH REPO: added %u rules; total %u\n",added,(unsigned)watchCount);
    }
  } else if (command=="WATCH REPO REPLACE") {
    if (importingRules) { Serial.println("WATCH: finish/cancel import first"); return; }
    uint16_t staged=0;
    const unsigned added=mergeAllRepositoryRules(importRules,staged);
    if (added != BUILTIN_RULE_COUNT || !saveRules(importRules,staged)) {
      Serial.println("WATCH REPO: replacement failed; original list retained");
    } else {
      for(unsigned i=0;i<staged;i++)watchRules[i]=importRules[i];
      watchCount=staged;
      resetWatchTracks();
      Serial.printf("WATCH REPO: replaced list with %u repository rules\n",(unsigned)watchCount);
    }
  } else if(command=="ALIAS LIST") {
    Serial.printf("ALIASES: %u\n",aliasCount);
    for(unsigned i=0;i<aliasCount;i++)Serial.printf("ALIAS %s | %s\n",
      deviceAliases[i].mac.c_str(),deviceAliases[i].label.c_str());
  } else if(command.startsWith("ALIAS SET ")) {
    String args=raw.substring(10);int sep=args.indexOf('|');
    if(sep<1){Serial.println("ALIAS ERROR: ALIAS SET MAC | Nickname");return;}
    String mac=hexOnly(args.substring(0,sep)),label=safeRuleText(args.substring(sep+1));
    if(mac.length()!=12||label.length()<1||label.length()>60){Serial.println("ALIAS ERROR: bad address/label");return;}
    unsigned i=0;for(;i<aliasCount;i++)if(deviceAliases[i].mac==mac)break;
    if(i==aliasCount && aliasCount==MAX_ALIASES){Serial.println("ALIAS ERROR: capacity");return;}
    DeviceAlias old=(i<aliasCount)?deviceAliases[i]:DeviceAlias{};
    deviceAliases[i]={mac,label};if(i==aliasCount)aliasCount++;
    if(saveAliases())Serial.println("ALIAS SAVED");
    else {if(i==aliasCount-1 && old.mac.length()==0)aliasCount--;else deviceAliases[i]=old;
      Serial.println("ALIAS SAVE FAILED");}
  } else if(command.startsWith("ALIAS REMOVE ")) {
    String mac=hexOnly(command.substring(13));unsigned i=0;
    for(;i<aliasCount;i++)if(deviceAliases[i].mac==mac)break;
    if(i==aliasCount){Serial.println("ALIAS NOT FOUND");return;}
    DeviceAlias saved[MAX_ALIASES];for(unsigned j=0;j<aliasCount;j++)saved[j]=deviceAliases[j];
    for(unsigned j=i;j+1<aliasCount;j++)deviceAliases[j]=deviceAliases[j+1];
    --aliasCount;
    if(saveAliases())Serial.println("ALIAS REMOVED");
    else {aliasCount++;for(unsigned j=0;j<aliasCount;j++)deviceAliases[j]=saved[j];
      Serial.println("ALIAS REMOVE FAILED");}
  } else if (command.startsWith("WIFI")) {
    processWifiCommand(command, raw);
  } else if (command=="LED CHARGE OFF" || command=="LED CHARGE ON" || command=="LED CHARGE DISABLE") {
    const uint8_t mode = command.endsWith("ON") ? 0 : command.endsWith("DISABLE") ? 2 : 1;
    const bool ok = axp2101ChargeLed(mode);
    Serial.println(ok ? (mode == 0 ? "LED: red charge LED back under charger control"
                       : mode == 1 ? "LED: red charge LED set to manual hi-Z (off)"
                                   : "LED: CHGLED pin function disabled (off)")
                      : "LED ERROR: write not confirmed; see register line");
  } else if (command=="SOUND ON") {
    W10Audio::setEnabled(true);
    Serial.println("SOUND ON (watchlist matches only)");
  } else if (command=="SOUND OFF") {
    W10Audio::setEnabled(false);
    Serial.println("SOUND OFF");
  } else if (command.startsWith("SOUND VOLUME ")) {
    int requested=command.substring(13).toInt();
    Serial.println(W10Audio::setVolume(requested) ?
      "SOUND VOLUME UPDATED" : "SOUND VOLUME ERROR: use 0-85; check AUDIO READY");
    if (W10Audio::volume == requested && watchPrefs.begin("w10_watch", false)) {
      watchPrefs.putUChar("audio_vol", (uint8_t)requested); watchPrefs.end();
      Serial.println("SOUND VOLUME SAVED");
    }
  } else if (command.startsWith("SOUND TEST")) {
    String k=command.substring(10);k.trim();
    W10Audio::Kind sound=W10Audio::NEW_MATCH;
    if(k=="REPEAT")sound=W10Audio::REACQUIRED;
    else if(k=="PRIORITY")sound=W10Audio::PRIORITY;
    else if(k=="SUSTAINED")sound=W10Audio::SUSTAINED;
    else if(k=="READY")sound=W10Audio::READY;
    else if(k.length() && k!="NEW"){
      Serial.println("SOUND TEST: NEW, REPEAT, PRIORITY, SUSTAINED, READY");return;
    }
    Serial.println(W10Audio::schedule(sound) ?
      "SOUND TEST QUEUED" : "SOUND TEST unavailable/disabled/busy");
  } else if (command == "STATUS") {
    emitStatus();
  } else if (command == "LOG OFF") {
    stopFlashLogging();
    Serial.println("{\"event\":\"command\",\"message\":\"Flash log OFF; USB NDJSON continues\"}");
  } else if (command == "LOG ON") {
    if (flashLogging) { Serial.printf("FLASH LOG already armed: %s\n", logFilename); return; }
    if (!startFlashLogging(false))
      Serial.println("{\"event\":\"error\",\"message\":\"Flash log not started; see FLASH lines above\"}");
  } else if (command == "LOG LIST") {
    if (!mountFlash(false)) { Serial.println("Filesystem not mounted."); return; }
    Serial.printf("FLASH: %lu KB free\n", (unsigned long)(flashFreeBytes() / 1024));
    File dir = LittleFS.open("/logs");
    if (!dir || !dir.isDirectory()) { Serial.println("No log directory."); return; }
    File entry = dir.openNextFile();
    while (entry) {
      Serial.printf("FILE %s %lu bytes\n", entry.path(), (unsigned long)entry.size());
      entry = dir.openNextFile();
    }
    Serial.println("END FILE LIST");
  } else if (command.startsWith("LOG DUMP ")) {
    if (!mountFlash(false)) { Serial.println("Filesystem not mounted."); return; }
    String path = raw.substring(9);
    path.trim();
    if (!path.startsWith("/logs/") || path.indexOf("..") >= 0) {
      Serial.println("Invalid log path (use LOG LIST)."); return;
    }
    if (flashLogging && logFile) logFile.flush();
    File reader = LittleFS.open(path, FILE_READ);
    if (!reader || reader.isDirectory()) { Serial.println("Log not found."); return; }
    Serial.printf("BEGIN LOG DUMP %s\n", path.c_str());
    while (reader.available()) {
      String line = reader.readStringUntil('\n');
      line.trim();
      if (line.length()) Serial.println(line);
      delay(1);
    }
    reader.close();
    Serial.println("END LOG DUMP");
  } else if (command == "FORMATFS YES") {
    stopFlashLogging();
    if (flashMounted) { LittleFS.end(); flashMounted = false; }
    bool formatted = LittleFS.format();
    flashFull = false;
    Serial.println(formatted ?
       "{\"event\":\"command\",\"message\":\"Filesystem formatted\"}" :
       "{\"event\":\"error\",\"message\":\"Filesystem format failed\"}");
    if (formatted && FLASH_LOG_AT_BOOT) startFlashLogging(false);
  } else if (command.length()) {
    Serial.println("{\"event\":\"error\",\"message\":\"Unknown command; send HELP\"}");
  }
}

void setup() {

  setAlertPixel(0,0,0);
  Serial.begin(115200);
#if defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT && defined(ARDUINO_USB_MODE) && ARDUINO_USB_MODE

  Serial.setTxTimeoutMs(20);
#endif
  delay(800);
  pinMode(LORA_CS, OUTPUT);
  digitalWrite(LORA_CS, HIGH);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(100000);
  axp2101ConfigureInput();
  if (CHARGE_LED_OFF_AT_BOOT) axp2101ChargeLed(1);
  pinMode(BOOT_KEY_PIN, INPUT_PULLUP);
  pmuKeyOK = pmuKeySetup();
  Serial.println(pmuKeyOK ? "KEYS: BOOT + power key (AXP2101 IRQ flags) active" : "KEYS: BOOT only; AXP2101 key flags not reachable");
  expanderOK = expander.begin_I2C(0x20);
  if (expanderOK) {
    expander.pinMode(1, OUTPUT);
    expander.digitalWrite(1, HIGH);
  }
  pinMode(TFT_BL, OUTPUT);
  digitalWrite(TFT_BL, LOW);

  if (expanderOK) {
    expander.digitalWrite(1, HIGH); delay(10);
    expander.digitalWrite(1, LOW); delay(10);
    expander.digitalWrite(1, HIGH); delay(50);
  }
  tftOK = tft->begin();
  if (tftOK) tft->fillScreen(0x0000);
  digitalWrite(TFT_BL, HIGH);
  if (tftOK) renderScreen();

  if (watchPrefs.begin("w10_watch", true)) {
    W10Audio::presetVolume(watchPrefs.getUChar("audio_vol", W10Audio::VOLUME_DEFAULT));
    watchPrefs.end();
  }
  if (expanderOK) W10Audio::begin(expander);
  else Serial.println("AUDIO UNAVAILABLE: MCP23017 not detected");
  loadWatchlist();
  loadAliases();
  loadSelection();
  Serial.print("SELECT:");
  for (unsigned i = 0; i < CAT_COUNT; ++i) Serial.printf(" %s=%s", CATS[i].key, catModeName(catMode((uint8_t)i)));
  Serial.printf(" flash_mask=0x%03X (saved selection; SELECT to change, menu open for %lu s)\n", (unsigned)catFlashMask, (unsigned long)(STARTUP_MENU_MS / 1000));
  W10SubG::loadConfig();
  if (W10SubG::begin())
    Serial.printf("SUBG: SX1262 ready (%s), %s; 7/800 MHz mobile-band sweep %u steps, %u LoRa profiles, threshold %d dBm, receive only\n",
                  W10SubG::tcxo ? "TCXO" : "crystal", W10SubG::enabled ? "enabled" : "disabled (SUBG ON)",
                  W10SubG::stepCount, W10SubG::profileCount, W10SubG::threshold);
  else Serial.println("SUBG: SX1262 not found (expander or SPI); sub-GHz scanning off, BLE unaffected");
  if (FLASH_LOG_AT_BOOT) startFlashLogging(true);
  gpsSerial.setRxBufferSize(2048);
  gpsSerial.begin(GPS_BAUD, SERIAL_8N1, GPS_RX, GPS_TX);

  bleQueue = xQueueCreate(BLE_QUEUE_DEPTH, sizeof(AdvRecord));
  if(!bleQueue) {
    Serial.println("SCAN: 192-entry queue unavailable; falling back to 128");
    bleQueue = xQueueCreate(128, sizeof(AdvRecord));
  }
  if(!bleQueue) Serial.println("SCAN ERROR: BLE queue allocation failed");
  actualQueueDepth = bleQueue ? (uxQueueSpacesAvailable(bleQueue) == 128 ? 128 : BLE_QUEUE_DEPTH) : 0;
  if (bleQueue) {
    NimBLEDevice::init("");
    bleScanner = NimBLEDevice::getScan();
    bleScanner->setScanCallbacks(&advertisementCallbacks, true);
    bleScanner->setActiveScan(true);

    bleScanner->setScanResponseTimeout(250);
    bleScanner->setMaxResults(0);
    loadWifiConfig();
    applyScanWindow(wifiEnabled);
    if (!bleScanner->start(BLE_SCAN_CYCLE_MS, false, true))
      Serial.println("SCAN ERROR: NimBLE scan did not start (check STATUS scan_discovered stays 0)");
    else W10Audio::schedule(W10Audio::READY);
  } else loadWifiConfig();
  scanLastAliveMs = millis();
  wifiStart();
  if (CHARGE_LED_OFF_AT_BOOT) axp2101ChargeLed(1);
  Serial.printf("{\"event\":\"boot\",\"version\":\"W10_OUISPY_0.4.20-cloud64\",\"builtin_rules\":%u,\"storage_default\":\"flash_and_usb\",\"wifi\":\"heatmap_page_only\",\"lora_tx\":false}\n", BUILTIN_RULE_COUNT);
  Serial.println("Send HELP for serial commands. USB stream is NDJSON at 115200.");
  emitStatus();

  openMenu(millis());
  menuCountdownUntil = millis() + STARTUP_MENU_MS;
}

void loop() {
  while (gpsSerial.available() > 0) gps.encode((char)gpsSerial.read());
  if(bleQueue) {
    const uint32_t pending=uxQueueMessagesWaiting(bleQueue);
    if(pending>qaQueueHigh) qaQueueHigh=pending;
  }
  AdvRecord packet;
  uint8_t count = 0;
  while (bleQueue && count++ < 96 && xQueueReceive(bleQueue, &packet, 0) == pdTRUE) {
    processBle(packet);
    while (gpsSerial.available() > 0) gps.encode((char)gpsSerial.read());
  }
  while (Serial.available() > 0) {
    int c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (serialBuffer.length()) processCommand(serialBuffer);
      serialBuffer = "";
    } else if (serialBuffer.length() < 400) serialBuffer += (char)c;
  }
  unsigned long now = millis();

  if (now - lastRgbMs >= 50) {
    lastRgbMs = now;
    updateAlertPixel(now);
  }
  keysService(now);
  uiService(now);
  W10SubG::service(now);
  wifiService(now);
  bleQuietService(now);
  scanWatchdog(now);
  if (now - lastStatusMs >= STATUS_PERIOD_MS) {
    lastStatusMs = now;
    emitStatus();
  }
  if (flashLogging && logFile && now - lastLogFlush >= 5000) {
    lastLogFlush = now;
    logFile.flush();
    if (flashFreeBytes() < FLASH_LOG_MIN_FREE) {
      stopFlashLogging(); flashFull = true;
      Serial.println("{\"event\":\"error\",\"message\":\"Flash log partition full; logging stopped, USB NDJSON continues\"}");
    }
  }
  delay(1);
}
