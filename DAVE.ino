/*
  ============================================================================
  DAVE - a glucose mood light
  ============================================================================

  A frosted orb that changes colour with blood glucose, read from a FreeStyle
  Libre 2 Plus sensor via Abbott's LibreLinkUp follower cloud, over WiFi.

  Colour map (mmol/L) - now a CONTINUOUS gradient, not hard bands. The colour
  slides smoothly between these anchor points (see the GRADIENT table below):
        ~3.0   -> red          (low)        + gentle slow pulse below 3.5
        ~4.0   -> yellow       (low-ish)
        ~6.0   -> green        (ideal, mid-range)
        ~10.0  -> blue         (high-ish)
        ~13.0  -> violet       (high)
     ...readings between anchors blend (e.g. ~8 shows teal = top of range,
        heading high; ~3.5 shows orange = slipping toward low).
        stale/error -> dim white                       + slow pulse
        T&C needed  -> dim amber                       + slow pulse
        booting     -> dim teal                        + slow pulse

  ----------------------------------------------------------------------------
  *** SAFETY: DAVE IS A "NICE TO HAVE", NOT A MEDICAL ALARM. ***
  It lags the cloud by a few minutes and can silently fail (WiFi, server,
  crash). Her Libre and Omnipod alarms remain the safety-critical layer.
  DAVE is designed to FAIL VISIBLY (white pulse) rather than show a stale,
  falsely-reassuring colour.
  ----------------------------------------------------------------------------

  HARDWARE (this build)
    - ESP32-DevKitC V4 (DUBEUYEW, with external 3 dBi U.FL antenna)
    - BTF-Lighting WS2812B strip, 1m, 100 LEDs/m, with JST connector + pigtail
      (only the first 16 LEDs are used; the rest sit dark inside the orb)
    - 400-point solderless breadboard
    - 330 ohm resistor on the data line
    - 1000 uF / 16V electrolytic capacitor across 5V and GND
    - 5V USB power into the ESP32's USB port (any phone charger >= 1A)

  WIRING (to be confirmed against your actual board's silkscreen)
    ESP32  5V  -> capacitor + -> strip red wire (5V)
    ESP32 GND  -> capacitor - -> strip white wire (GND)
    ESP32 GPIO13 --[ 330 ohm ]-> strip green wire (DIN)

    IMPORTANT: connect the antenna (U.FL clip on the module's gold square,
    other end to the SMA antenna) BEFORE powering on, or there is no WiFi.

  LIBRARIES (Arduino IDE -> Library Manager)
    - "Adafruit NeoPixel" by Adafruit
    - "ArduinoJson"       by Benoit Blanchon  (v7.x)
    (WiFi / HTTPClient / mbedTLS come with the ESP32 board package.)

  ARDUINO IDE BOARD SETTINGS
    - Tools -> Board -> ESP32 Arduino -> "ESP32 Dev Module"
    - Tools -> Upload Speed -> 921600
    - Tools -> Flash Size -> 4MB (32Mb)
    (Other defaults are fine.)

  ----------------------------------------------------------------------------
  QUICK START
    1. Leave USE_FAKE_GLUCOSE = true. Wire it up, flash, and watch DAVE cycle
       through every colour and both pulse states with NO network needed.
    2. Once your LibreLinkUp follower account is set up and your phone is
       showing her live number, fill in the WiFi + LLU credentials below,
       set USE_FAKE_GLUCOSE = false, and reflash. Done.
  ============================================================================
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include "mbedtls/md.h"

// ===========================================================================
//  CONFIG - the only section you should normally need to touch
// ===========================================================================

// --- Mode -------------------------------------------------------------------
#define USE_FAKE_GLUCOSE   false     // true  = demo with fake values, offline
                                     // false = real LibreLinkUp data over WiFi

// --- Hardware ---------------------------------------------------------------
#define NUM_LEDS           58       // drive the whole strip. NOTE: ~100 LEDs
                                     // can pull 1.5-2A on bright states - too
                                     // much for the ESP32's USB port. Power the
                                     // strip from a dedicated 5V supply (common
                                     // ground with the ESP32) for the real build.
                                     // For a quick USB bench test, lower
                                     // MAX_BRIGHTNESS below and use a 2A+ charger.
#define LED_PIN            13        // ESP32 GPIO that the strip's data wire
                                     // is connected to (via the 330R resistor)
#define MAX_BRIGHTNESS     140       // 0-255. 150 is pleasant for a frosted orb

// --- Behaviour --------------------------------------------------------------
// DAVE's colour is a continuous gradient (the GRADIENT table further down, in
// the colour section). These settings only control the gentle "breathing"
// pulse, which is reserved for readings worth noticing.
#define PULSE_ON_LOW       true      // pulse when the reading is below PULSE_BELOW
#define PULSE_BELOW         3.5      //   ...this value (mmol/L)
#define PULSE_ON_HIGH      true     // also pulse when the reading is above PULSE_ABOVE?
#define PULSE_ABOVE        18.0      //   ...this value (mmol/L)

// --- Polling & freshness ----------------------------------------------------
#define POLL_INTERVAL_MS   (45UL * 1000UL)   // 60s. Community says <45s is risky.
                                             // (REAL mode only.)

// --- Fake/test-mode timing (only used when USE_FAKE_GLUCOSE = true) ---------
// With a continuous scale, the demo SWEEPS smoothly one way up the range, then
// shows the stale and T&Cs status states, then SWEEPS smoothly back down. Each
// sweep is one-directional and takes FAKE_SWEEP_MS. The sequence loops forever:
//   sweep up (low->high)  ->  stale  ->  T&Cs  ->  sweep down (high->low)  -> ...
#define FAKE_SWEEP_MS      (20UL * 1000UL)   // one one-way glide (up OR down)
#define FAKE_SPECIAL_MS    ( 3UL * 1000UL)   // dwell on each of stale / T&Cs
#define FAKE_LOW_MMOL       2.5              // bottom of the demo sweep
#define FAKE_HIGH_MMOL     14.0              // top of the demo sweep

// --- WiFi (only used when USE_FAKE_GLUCOSE = false) -------------------------
const char* WIFI_SSID     = "ENTER-WIFI-NAME";
const char* WIFI_PASSWORD = "ENTER-WIFI-PASSWORD";

// --- LibreLinkUp follower account (the account YOU created as a follower) ---
const char* LLU_EMAIL     = "ENTER-LIBRE-ACCOUNT-NAME";
const char* LLU_PASSWORD  = "ENTER-LIBRE-ACCOUNT-PASSWORD";

// --- Region -----------------------------------------------------------------
// UK / Europe = "eu". Others: "us", "ae", "au", "de", "fr", "jp", "la", etc.
const char* LLU_REGION    = "eu";

// ===========================================================================
//  END CONFIG
// ===========================================================================


// --- Colour definitions (R, G, B 0-255) ------------------------------------
struct RGB { uint8_t r, g, b; };

const RGB C_LOW       = {255,   0,   0};   // red       (low)
const RGB C_LOW_ISH   = {255, 230,   0};   // yellow    (low-ish)
const RGB C_IN_RANGE  = {  0, 200,  4};   // green     (in range)
const RGB C_HIGH_ISH  = {  0,  110, 255};   // blue      (high-ish)
const RGB C_HIGH      = {150,  40, 220};   // violet    (high)
const RGB C_STALE     = {120, 120, 120};   // dim white (pulses) - no fresh data
const RGB C_TOS       = {200, 120,   0};   // amber     (pulses) - accept T&Cs
const RGB C_BOOTING   = {  0, 100, 110};   // dim teal  (pulses) - just woke up

// --- Colour gradient --------------------------------------------------------
// The heart of the "sliding scale". Each stop pins a pure colour at a glucose
// value; colourForGlucose() linearly blends between the two nearest stops.
// Below the first stop it holds the first colour; above the last, the last.
// Tune the whole behaviour just by editing these rows (keep them in ascending
// mmol order). The anchors below reproduce the old bands as their *centres*,
// so the old band edges naturally become blend zones.
struct ColourStop { float mmol; RGB colour; };
const ColourStop GRADIENT[] = {
  {  3.8, C_LOW      },   // red    - low (kept snappy; a low should grab attention)
  {  4.1, C_IN_RANGE },   // green  - in-range, lower edge
  {  10.0, C_IN_RANGE },   // green  - in-range, upper edge (solid green 4-8)
  { 12.2, C_HIGH_ISH },   // blue   - fade green->blue spread across all of 8-12
  { 15.0, C_HIGH     },   // violet - fade blue->violet spread across 12-15
};
const int GRADIENT_N = sizeof(GRADIENT) / sizeof(GRADIENT[0]);

Adafruit_NeoPixel strip(NUM_LEDS, LED_PIN, NEO_GRB + NEO_KHZ800);

// --- State machine ----------------------------------------------------------
// Glucose is now a continuous colour (ST_GLUCOSE), so the old per-band states
// are gone; what remains are the non-numeric status states.
enum DaveState {
  ST_BOOTING,    // we've only just powered on; haven't talked to the cloud yet
  ST_GLUCOSE,    // showing a live reading, blended along the gradient
  ST_STALE,      // network/cloud problem; reading can't be trusted
  ST_TOS         // Abbott pushed new T&Cs; open LibreLinkUp app to accept
};

// Result of a cloud fetch. Declared up here (not down in the LibreLinkUp
// section) so updateGlucoseState() below can see these names. The Arduino IDE
// auto-generates function prototypes but NOT enum declarations, so this must
// appear before the first function that uses it.
enum FetchResult { FETCH_OK, FETCH_FAIL, FETCH_TOS_REQUIRED };

DaveState currentState   = ST_BOOTING;
RGB       targetColour   = C_BOOTING;
RGB       shownColour    = {0, 0, 0};   // what's actually on the strip now
bool      shouldPulse    = true;

// --- LibreLinkUp session ----------------------------------------------------
String        jwtToken       = "";
String        accountIdHash  = "";
String        patientId      = "";
unsigned long lastPollMs     = 0;
bool          hasEverPolled  = false;   // for distinguishing "booting" from "stale"

// ===========================================================================
//  SETUP
// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println(F("=== DAVE booting ==="));
  Serial.printf("Mode: %s\n", USE_FAKE_GLUCOSE ? "FAKE (offline demo)" : "REAL (LibreLinkUp)");
  Serial.printf("Strip: %d LEDs on GPIO %d, max brightness %d/255\n",
                NUM_LEDS, LED_PIN, MAX_BRIGHTNESS);

  strip.begin();
  strip.setBrightness(MAX_BRIGHTNESS);
  strip.clear();
  strip.show();

  setState(ST_BOOTING);

  if (!USE_FAKE_GLUCOSE) {
    connectWiFi();
  }
}

// ===========================================================================
//  MAIN LOOP
// ===========================================================================
void loop() {
  if (USE_FAKE_GLUCOSE) {
    // Demo mode: a self-timed tour through every state (see fakeGlucoseTick).
    fakeGlucoseTick();
  } else {
    // Real mode: poll the cloud on an interval. The first poll is immediate.
    if (lastPollMs == 0 || millis() - lastPollMs >= POLL_INTERVAL_MS) {
      lastPollMs = millis();
      updateGlucoseState();
      hasEverPolled = true;
    }
  }

  // Animate the strip smoothly every frame, regardless of polling cadence.
  animate();
  delay(20);   // ~50 fps
}

// ===========================================================================
//  GLUCOSE  ->  STATE
// ===========================================================================
void updateGlucoseState() {
  // --- Real LibreLinkUp path (fake mode is handled in loop()) ---
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println(F("[poll] WiFi down, reconnecting..."));
    connectWiFi();
    if (WiFi.status() != WL_CONNECTED) { setState(ST_STALE); return; }
  }

  float mmol;
  int   result = fetchLatestGlucose(mmol);

  if (result == FETCH_TOS_REQUIRED) {
    Serial.println(F("[poll] *** Terms of Service must be accepted ***"));
    Serial.println(F("       Open the official LibreLinkUp app, tap through"));
    Serial.println(F("       the new terms, and DAVE recovers automatically."));
    setState(ST_TOS);
    return;
  }
  if (result != FETCH_OK) {
    Serial.println(F("[poll] fetch failed -> stale"));
    setState(ST_STALE);
    return;
  }

  Serial.printf("[poll] glucose = %.1f mmol/L\n", mmol);
  showGlucose(mmol);
}

// Linearly blend the GRADIENT table to a colour for this reading.
RGB colourForGlucose(float mmol) {
  if (mmol <= GRADIENT[0].mmol)              return GRADIENT[0].colour;
  if (mmol >= GRADIENT[GRADIENT_N - 1].mmol) return GRADIENT[GRADIENT_N - 1].colour;

  for (int i = 0; i < GRADIENT_N - 1; i++) {
    const ColourStop& a = GRADIENT[i];
    const ColourStop& b = GRADIENT[i + 1];
    if (mmol >= a.mmol && mmol <= b.mmol) {
      float t = (mmol - a.mmol) / (b.mmol - a.mmol);    // 0..1 between the stops
      RGB out;
      out.r = (uint8_t)(a.colour.r + t * ((int)b.colour.r - (int)a.colour.r));
      out.g = (uint8_t)(a.colour.g + t * ((int)b.colour.g - (int)a.colour.g));
      out.b = (uint8_t)(a.colour.b + t * ((int)b.colour.b - (int)a.colour.b));
      return out;
    }
  }
  return GRADIENT[GRADIENT_N - 1].colour;   // unreachable; satisfies the compiler
}

// Show a live reading: colour from the gradient, pulse only at the extremes.
void showGlucose(float mmol) {
  currentState = ST_GLUCOSE;
  targetColour = colourForGlucose(mmol);
  shouldPulse  = (PULSE_ON_LOW  && mmol <  PULSE_BELOW) ||
                 (PULSE_ON_HIGH && mmol >= PULSE_ABOVE);
}

void setState(DaveState s) {
  currentState = s;
  switch (s) {
    case ST_BOOTING:  targetColour = C_BOOTING;  shouldPulse = true;  break;
    case ST_STALE:    targetColour = C_STALE;    shouldPulse = true;  break;
    case ST_TOS:      targetColour = C_TOS;      shouldPulse = true;  break;
    case ST_GLUCOSE:  /* colour/pulse are set by showGlucose() */     break;
  }
}

// ===========================================================================
//  FAKE GLUCOSE - exercises the light with no network / follower account.
//
//  Because the scale is continuous, the demo SWEEPS the reading smoothly UP
//  the range (red -> violet) over FAKE_SWEEP_MS, then shows the two status
//  states, then SWEEPS smoothly back DOWN (violet -> red) over FAKE_SWEEP_MS.
//  It loops forever:
//    [sweep up low->high] -> [stale] -> [T&Cs] -> [sweep down high->low] -> ...
//  The low end of each sweep dips below PULSE_BELOW, so you also see the pulse
//  switch on and off as it passes through.
// ===========================================================================
void fakeGlucoseTick() {
  static int           phase       = 0;   // 0 sweep up, 1 stale, 2 T&Cs, 3 sweep down
  static unsigned long phaseStart  = 0;
  static int           loggedPhase = -1;
  static unsigned long lastPrint   = 0;

  unsigned long now     = millis();
  if (phaseStart == 0) phaseStart = now;
  unsigned long elapsed = now - phaseStart;

  if (phase != loggedPhase) {
    const char* names[] = { "sweep up (red->violet)", "STALE demo",
                            "T&Cs demo", "sweep down (violet->red)" };
    Serial.printf("[fake] phase: %s\n", names[phase]);
    loggedPhase = phase;
  }

  switch (phase) {
    case 0:    // sweep up:   low -> high
    case 3: {  // sweep down: high -> low
      float frac = (float)elapsed / (float)FAKE_SWEEP_MS;   // 0..1 across the sweep
      if (frac > 1.0) frac = 1.0;
      float ramp = (phase == 0) ? frac : (1.0 - frac);      // up on 0, down on 3
      float mmol = FAKE_LOW_MMOL + ramp * (FAKE_HIGH_MMOL - FAKE_LOW_MMOL);
      showGlucose(mmol);
      if (now - lastPrint > 1000) {                         // log ~once a second
        Serial.printf("[fake] glucose = %.1f mmol/L\n", mmol);
        lastPrint = now;
      }
      if (elapsed >= FAKE_SWEEP_MS) {
        phase = (phase == 0) ? 1 : 0;                       // up->stale, down->wrap
        phaseStart = now;
      }
      break;
    }
    case 1:    // stale
      setState(ST_STALE);
      if (elapsed >= FAKE_SPECIAL_MS) { phase++; phaseStart = now; }
      break;
    case 2:    // T&Cs
      setState(ST_TOS);
      if (elapsed >= FAKE_SPECIAL_MS) { phase = 3; phaseStart = now; }
      break;
  }
}

// ===========================================================================
//  WIFI
// ===========================================================================
void connectWiFi() {
  Serial.printf("[wifi] connecting to %s ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print(F("[wifi] connected, IP = "));
    Serial.println(WiFi.localIP());
    Serial.printf("[wifi] signal = %d dBm\n", WiFi.RSSI());
  } else {
    Serial.println(F("[wifi] FAILED (will retry next poll)"));
    Serial.println(F("[wifi] (is the antenna plugged in?)"));
  }
}

// ===========================================================================
//  LIBRELINKUP API
// ===========================================================================
// Required headers reverse-engineered by the open-source diabetes community.
// "version" tracks the LibreLinkUp Android app version; bump it if logins
// start failing.
#define LLU_PRODUCT  "llu.android"
#define LLU_VERSION  "4.16.0"

// The region actually in use. Starts at your configured LLU_REGION, but if the
// login server replies with a redirect (account lives in another data centre),
// login() updates this and all later requests follow automatically.
String activeRegion = LLU_REGION;

String baseUrl() {
  return String("https://api-") + activeRegion + ".libreview.io";
}

// SHA-256(userId) -> lowercase hex. This goes in the "Account-Id" header.
String sha256Hex(const String& input) {
  byte hash[32];
  mbedtls_md_context_t ctx;
  mbedtls_md_init(&ctx);
  mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0);
  mbedtls_md_starts(&ctx);
  mbedtls_md_update(&ctx, (const unsigned char*)input.c_str(), input.length());
  mbedtls_md_finish(&ctx, hash);
  mbedtls_md_free(&ctx);

  char out[65];
  for (int i = 0; i < 32; i++) sprintf(out + i * 2, "%02x", hash[i]);
  out[64] = '\0';
  return String(out);
}

void addCommonHeaders(HTTPClient& http) {
  http.addHeader("product", LLU_PRODUCT);
  http.addHeader("version", LLU_VERSION);
  http.addHeader("content-type", "application/json");
  http.addHeader("cache-control", "no-cache");
  if (jwtToken.length())      http.addHeader("Authorization", "Bearer " + jwtToken);
  if (accountIdHash.length()) http.addHeader("Account-Id", accountIdHash);
}

// Logs in, fills jwtToken + accountIdHash. Returns a FetchResult.
// Follows LibreLinkUp region redirects automatically: if the server replies
// status=0 with {data:{redirect:true,region:"xx"}}, it means this account lives
// on a different data centre, so we switch activeRegion and retry.
int login() {
  for (int attempt = 0; attempt < 3; attempt++) {
    Serial.printf("[llu] logging in (region '%s')...\n", activeRegion.c_str());
    WiFiClientSecure client;
    client.setInsecure();   // skip cert pinning; fine for this hobby use

    HTTPClient http;
    String url = baseUrl() + "/llu/auth/login";
    if (!http.begin(client, url)) {
      Serial.println(F("[llu] http.begin() failed"));
      return FETCH_FAIL;
    }

    http.addHeader("product", LLU_PRODUCT);
    http.addHeader("version", LLU_VERSION);
    http.addHeader("content-type", "application/json");

    String body = String("{\"email\":\"") + LLU_EMAIL +
                  "\",\"password\":\"" + LLU_PASSWORD + "\"}";

    int code = http.POST(body);
    if (code != HTTP_CODE_OK) {
      Serial.printf("[llu] login HTTP %d\n", code);
      http.end();
      return FETCH_FAIL;
    }

    String payload = http.getString();
    http.end();

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      Serial.println(F("[llu] login JSON parse error"));
      return FETCH_FAIL;
    }

    int status = doc["status"] | -1;
    // status 4 means: terms/T&Cs must be accepted in the official app first.
    if (status == 4) return FETCH_TOS_REQUIRED;

    // Region redirect: correct credentials, wrong data centre. The reply is
    // {status:0, data:{redirect:true, region:"xx"}}. Switch region and retry.
    if (doc["data"]["redirect"].as<bool>()) {
      const char* newRegion = doc["data"]["region"] | "";
      if (strlen(newRegion) > 0 && activeRegion != newRegion) {
        Serial.printf("[llu] server redirect: account is in region '%s' (was '%s')\n",
                      newRegion, activeRegion.c_str());
        activeRegion = String(newRegion);
        continue;   // retry the login against the correct regional server
      }
      Serial.println(F("[llu] redirect with no usable region; giving up"));
      return FETCH_FAIL;
    }

    const char* token  = doc["data"]["authTicket"]["token"];
    const char* userId = doc["data"]["user"]["id"];
    if (!token || !userId) {
      Serial.printf("[llu] login status=%d but no token/userId\n", status);
      Serial.println(F("[llu] (not a redirect - check email/password)"));
      return FETCH_FAIL;
    }

    jwtToken      = String(token);
    accountIdHash = sha256Hex(String(userId));
    Serial.printf("[llu] login OK (region '%s')\n", activeRegion.c_str());
    return FETCH_OK;
  }

  Serial.println(F("[llu] too many region redirects; giving up"));
  return FETCH_FAIL;
}

// Gets the first connection's patientId.
int fetchPatientId() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = baseUrl() + "/llu/connections";
  if (!http.begin(client, url)) return FETCH_FAIL;
  addCommonHeaders(http);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[llu] connections HTTP %d\n", code);
    http.end();
    return FETCH_FAIL;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) return FETCH_FAIL;

  JsonArray data = doc["data"].as<JsonArray>();
  if (data.isNull() || data.size() == 0) {
    Serial.println(F("[llu] no connections - is sharing set up?"));
    Serial.println(F("       Is her sensor active? Did she accept the invite?"));
    return FETCH_FAIL;
  }
  const char* pid = data[0]["patientId"];
  if (!pid) return FETCH_FAIL;
  patientId = String(pid);
  Serial.print(F("[llu] patientId = ")); Serial.println(patientId);
  return FETCH_OK;
}

// Reads the LIVE latest measurement (NOT graphData, which lags 15-30 min).
// Fills mmol. Handles token expiry by re-logging in once.
int fetchLatestGlucose(float& mmol) {
  // Ensure we have a session.
  if (jwtToken.length() == 0) {
    int r = login();
    if (r != FETCH_OK) return r;
  }
  if (patientId.length() == 0) {
    if (fetchPatientId() != FETCH_OK) return FETCH_FAIL;
  }

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = baseUrl() + "/llu/connections/" + patientId + "/graph";
  if (!http.begin(client, url)) return FETCH_FAIL;
  addCommonHeaders(http);

  int code = http.GET();

  // Token expired or invalid -> re-login once and retry.
  if (code == HTTP_CODE_UNAUTHORIZED || code == HTTP_CODE_FORBIDDEN) {
    Serial.println(F("[llu] token rejected, re-logging in..."));
    http.end();
    jwtToken = "";
    int r = login();
    if (r != FETCH_OK) return r;
    if (!http.begin(client, url)) return FETCH_FAIL;
    addCommonHeaders(http);
    code = http.GET();
  }

  if (code != HTTP_CODE_OK) {
    Serial.printf("[llu] graph HTTP %d\n", code);
    http.end();
    return FETCH_FAIL;
  }

  String payload = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, payload)) {
    Serial.println(F("[llu] graph JSON parse error"));
    return FETCH_FAIL;
  }

  // The LIVE reading lives here, NOT in graphData (which is 15-min averages).
  JsonObject gm = doc["data"]["connection"]["glucoseMeasurement"];
  if (gm.isNull()) {
    Serial.println(F("[llu] no glucoseMeasurement in response"));
    return FETCH_FAIL;
  }

  float value      = gm["Value"]          | -1.0;
  int   units      = gm["GlucoseUnits"]   | 1;     // 1 = mmol/L, 0 = mg/dL
  long  valMgdl    = gm["ValueInMgPerDl"] | -1;

  if (units == 1) {                // mmol/L
    mmol = value;
  } else {                          // mg/dL reported -> convert
    mmol = (valMgdl > 0 ? valMgdl : value) / 18.0;
  }

  // NOTE: a proper freshness check (using NTP and the response Timestamp)
  // is a planned v1.1 improvement. Today, connection failures correctly
  // trigger ST_STALE, but a working connection serving an old number won't.
  // Her real Libre alarms remain the primary safety layer.

  return FETCH_OK;
}

// ===========================================================================
//  ANIMATION
// ===========================================================================
void animate() {
  // Ease shownColour toward targetColour, per channel.
  shownColour.r = ease(shownColour.r, targetColour.r);
  shownColour.g = ease(shownColour.g, targetColour.g);
  shownColour.b = ease(shownColour.b, targetColour.b);

  float pulse = 1.0;
  if (shouldPulse) {
    // Slow breathing: ~3.5s period, floor at 0.35 so it never goes fully off.
    float phase = (millis() % 3500) / 3500.0 * TWO_PI;
    pulse = 0.675 + 0.325 * sinf(phase);   // ranges ~0.35 .. 1.0
  }

  uint8_t r = (uint8_t)(shownColour.r * pulse);
  uint8_t g = (uint8_t)(shownColour.g * pulse);
  uint8_t b = (uint8_t)(shownColour.b * pulse);

  for (int i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, strip.Color(r, g, b));
  strip.show();
}

uint8_t ease(uint8_t cur, uint8_t target) {
  if (cur == target) return cur;
  int diff = (int)target - (int)cur;
  int step = diff / 8;                 // move ~1/8 of the way each frame
  if (step == 0) step = (diff > 0) ? 1 : -1;
  return (uint8_t)(cur + step);
}
