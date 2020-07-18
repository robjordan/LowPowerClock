#include <Arduino.h>

/*
 * LowPowerClock (based on Paul Stoffegren Time lib and his examples)
 * Requirement: Update an e-paper display with current time once each minute.
 * Desires: Maximise battery life by using deep sleep between each update and 
 * minimise use of WiFi to access NTP, because WiFi has huge current draw. 
 * Complication: ESP8266 RTC is not accurate during deep sleep.
 * Approach: Use NTP to initialise the clock. Then use NTP at initially frequent
 * intervals to calibrate the drift. As drift becomes well-calibrated the NTP 
 * interval can be increased.
 * This sketch uses the ESP8266WiFi library
 */

#include <TimeLib.h>
#include <Timezone.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <GxEPD2.h>
#include <GxGDEP015OC1/GxGDEP015OC1.cpp>  // 1.54" b/w
// FreeFonts from Adafruit_GFX
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeMonoBold12pt7b.h>
#include <Fonts/FreeMonoBold18pt7b.h>
#include <Fonts/FreeMonoBold24pt7b.h>
#include <GxIO/GxIO_SPI/GxIO_SPI.h>
#include <GxIO/GxIO.h>

#include "config.h"
extern "C" {
#include "user_interface.h"
}

#define ONEMINUTE (60 * 1E6)
#define RTCMEMORYSTART (65)
// Calibrate drift after 10 minutes and then every 9 hours
#define NTP_INTERVAL (rtcMem.iterations<12 ? 600 : 28800)
#define DIFF19001970 (2208988800UL)

// NTP Servers:
static const char ntpServerName[] = "uk.pool.ntp.org";
//static const char ntpServerName[] = "time.nist.gov";
//static const char ntpServerName[] = "time-a.timefreq.bldrdoc.gov";
//static const char ntpServerName[] = "time-b.timefreq.bldrdoc.gov";
//static const char ntpServerName[] = "time-c.timefreq.bldrdoc.gov";

// Removed timezone so we know we are working exclusive in CUT

WiFiUDP Udp;
unsigned int localPort = 8888;  // local port to listen for UDP packets

// RTC memory that persists across sleeps
typedef enum {NTP=0, Estimate=1} Sync;
typedef struct {
  time_t wakeTime;
  time_t lastNTP;
  Sync syncType;
  unsigned iterations;
  int64_t driftPerMinute;   // usecs, +ve: running slow, -ve: running fast
} rtcStore;

rtcStore rtcMem;

// Timezone info for GMT0BST
//United Kingdom (London, Belfast)
TimeChangeRule BST = {"BST", Last, Sun, Mar, 1, 60};  //British Summer Time
TimeChangeRule GMT = {"GMT", Last, Sun, Oct, 2, 0};   //Standard Time
Timezone UK(BST, GMT);
const char *daynames[] = {"", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
const char *monthnames[] = {"", "Jan", "Feb", "Mar", "Apr", "May", "Jun", 
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

// e-Paper Display constructor for ESP8266, copy from GxEPD_Example
GxIO_Class io(SPI, SS, D3, D4); 
GxEPD_Class display(io);

time_t LPgetNtpTime();
void digitalClockDisplay();
void printDigits(int digits);
void sendNTPpacket(IPAddress &address);
void connect_to_wifi();
void readFromRTCMemory();
void writeToRTCMemory();

void setup()
{
  Serial.begin(115200);

  Serial.println("LowPowerClock: ");
  Serial.println("Waking up, reset reason");
  Serial.println(ESP.getResetReason());

  if (ESP.getResetReason() == "Deep-Sleep Wake") {
    // Deep-sleep wake, RTC memory contains valid state
    Serial.print("Reading RTC memory");
    readFromRTCMemory();
    rtcMem.iterations++;
  } else {
    // Any other wakeup reason, re-initialise
    rtcMem.wakeTime = 0;
    rtcMem.lastNTP = 0;
    rtcMem.syncType = NTP;
    rtcMem.iterations = 0;
    rtcMem.driftPerMinute = 0;

    // Setup e-Paper display
    display.init();
    display.eraseDisplay();
  }

  setSyncProvider(LPgetNtpTime);

}

void loop()
{
  WakeMode mode; // will WiFi be enabled at next wakeup?

  if (timeStatus() != timeNotSet) {

    digitalClockDisplay();

    // will next time be an estimate or an NTP request? 
    if ((now() - rtcMem.lastNTP) > NTP_INTERVAL) {
      // It's too long snce we checked NTP; schedule a re-sync next wakeup
      rtcMem.syncType = NTP;
      mode = WAKE_RF_DEFAULT;
    }
    else {
      // No need to check NTP
      rtcMem.syncType = Estimate;
      mode = WAKE_RF_DISABLED;
    }

    // with the clock updated, it's time to go to sleep.
    // wake time is determined by rounding up current time to next whole minute

    time_t sleepSeconds = 60 - second(now());
    rtcMem.wakeTime = now() + sleepSeconds;
    writeToRTCMemory();

    // add adjustment for drift, which we've calibrated
    uint64_t USeconds = sleepSeconds * 1E6 - rtcMem.driftPerMinute;

    Serial.print("About to sleep for this many seconds: ");
    Serial.println((double)USeconds / 1E6);
    ESP.deepSleep(USeconds, mode);
    
  }
}

void digitalClockDisplay()
{
  // convert to local time
  time_t uktime = UK.toLocal(now());

  char t[6], d[11], dbg[128];
  sprintf(t, "%02d:%02d", hour(uktime), minute(uktime));
  sprintf(d, "%s %d %s", daynames[weekday()], day(), monthnames[month()]);
  sprintf(dbg, "s:%02d i:%d d(ms):%d", second(), rtcMem.iterations, (int)rtcMem.driftPerMinute/1000);

  // digital clock display of the time
  Serial.println(t);
  Serial.println(d);
  // Additional debugging info
  Serial.print("Seconds: ");
  Serial.println(second());

  display.init();
  display.setRotation(1);
  // display.fillScreen(GxEPD_WHITE);
  display.setTextColor(GxEPD_BLACK);
  display.setFont(&FreeMonoBold24pt7b);
  display.setCursor(31, 50);
  display.println(t);
  display.setFont(&FreeSans9pt7b);
  display.setCursor(56, 100);
  display.println(d);
  display.setCursor(5, 190);
  display.println(dbg);
  display.update();
}

void printDigits(int digits)
{
  // utility for digital clock display: prints preceding colon and leading 0
  Serial.print(":");
  if (digits < 10)
    Serial.print('0');
  Serial.print(digits);
}

void connect_to_wifi() {
  Serial.print("Connecting to ");
  Serial.println(ssid);
  WiFi.begin(ssid, pass);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.print("IP number assigned by DHCP is ");
  Serial.println(WiFi.localIP());
  Serial.println("Starting UDP");
  Udp.begin(localPort);
  Serial.print("Local port: ");
  Serial.println(Udp.localPort());
}

/*-------- NTP code ----------*/

const int NTP_PACKET_SIZE = 48; // NTP time is in the first 48 bytes of message
byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming & outgoing packets

time_t LPgetNtpTime() // Low-Power version of Get NTP Time example from Time
{
  // The method of setting time depends whether this iteration os a low-power 
  // estimate or a full NTP request
  if (rtcMem.syncType == Estimate) {
    Serial.println("Time set by estimate.");
    return rtcMem.wakeTime;
  } else {
    IPAddress ntpServerIP; // NTP server's ip address

    if (WiFi.status() != WL_CONNECTED) {
      connect_to_wifi();
    }

    while (Udp.parsePacket() > 0) ; // discard any previously received packets
    Serial.println("Transmit NTP Request");
    // get a random server from the pool
    WiFi.hostByName(ntpServerName, ntpServerIP);
    Serial.print(ntpServerName);
    Serial.print(": ");
    Serial.println(ntpServerIP);
    sendNTPpacket(ntpServerIP);
    uint32_t beginWait = millis();
    while (millis() - beginWait < 1500) {
      int size = Udp.parsePacket();
      if (size >= NTP_PACKET_SIZE) {
        Serial.println("Receive NTP Response");
        Udp.read(packetBuffer, NTP_PACKET_SIZE);  // read packet into the buffer
        unsigned long secsSince1900;
        // convert four bytes starting at location 40 to a long integer
        secsSince1900 =  (unsigned long)packetBuffer[40] << 24;
        secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
        secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
        secsSince1900 |= (unsigned long)packetBuffer[43];
        time_t ntpTime = secsSince1900 - DIFF19001970;

        // if it's our second or subsequent NTP query
        if (rtcMem.lastNTP > 0) {
          Serial.println(ntpTime);
          // work out the drift in microseconds per second (careful about overflow)
          long drift = ntpTime - rtcMem.wakeTime; // seconds of drift
          drift = (drift * 1000) - millis(); // milliseconds
          time_t interval = ntpTime - rtcMem.lastNTP; // seconds
          // the drift since last NTP query included a compensation, stored in 
          // driftPerMinute. So we need to add to the existing, not replace it.
          rtcMem.driftPerMinute = rtcMem.driftPerMinute + 
            1000 * ((60 * drift) / interval); // us per min
          Serial.print("Drift since last NTP (ms/min): ");
          Serial.println((long)(rtcMem.driftPerMinute/1000));
        }
        rtcMem.lastNTP = ntpTime;
        return ntpTime;
      }
    }
    Serial.println("No NTP Response :-(");
    return 0; // return 0 if unable to get the time
  }
}

// send an NTP request to the time server at the given address
void sendNTPpacket(IPAddress &address)
{
  // set all bytes in the buffer to 0
  memset(packetBuffer, 0, NTP_PACKET_SIZE);
  // Initialize values needed to form NTP request
  // (see URL above for details on the packets)
  packetBuffer[0] = 0b11100011;   // LI, Version, Mode
  packetBuffer[1] = 0;     // Stratum, or type of clock
  packetBuffer[2] = 6;     // Polling Interval
  packetBuffer[3] = 0xEC;  // Peer Clock Precision
  // 8 bytes of zero for Root Delay & Root Dispersion
  packetBuffer[12] = 49;
  packetBuffer[13] = 0x4E;
  packetBuffer[14] = 49;
  packetBuffer[15] = 52;
  // all NTP fields have been given values, now
  // you can send a packet requesting a timestamp:
  Udp.beginPacket(address, 123); //NTP requests are to port 123
  Udp.write(packetBuffer, NTP_PACKET_SIZE);
  Udp.endPacket();
}

void readFromRTCMemory() {
  system_rtc_mem_read(RTCMEMORYSTART, &rtcMem, sizeof(rtcMem));

  Serial.print("wakeTime = ");
  Serial.println(rtcMem.wakeTime);
  yield();
}

void writeToRTCMemory() {
  system_rtc_mem_write(RTCMEMORYSTART, &rtcMem, sizeof(rtcMem));

  Serial.print("wakeTime = ");
  Serial.println(rtcMem.wakeTime);
  yield();
}