# LowPowerClock
Display time to 1 minute precision, using ESP8266 with an e-Paper display, deep sleeping to conserve battery.

Initialise clock with a request to NTP. Then attempt to maintain time to 1 minute accuracy by deep-sleeping ESP8266 for most of every minute. On the minute boundary, wake up and refresh the e-Paper display. 

The snag is that ESP8266 RTC is known to be inaccurate. It's not locked to a reliable crystal and is known to be temperature sensitive. So we need occasional NTP requests to correct the time. It should not be allowed to stray more than 30 seconds form true time, otherwise it will display an inaccurate 'minute' time. 

The code attempts to overcome this by tracking drift, and hence delaying the necessity of a repeated NTP call. Each time NTP is consulted, the drift from previous update is calculated and turned into a per-minute error, which is added to the sleep time.

To date, I have found NTP needs to be requested every 8 hours to maintain 1-minute accuracy.

Factors in power consumtion:
* Deep-sleeps 58 seconds of every minute, current is low uA, therefore negligible.
* Refreshes screen for ~2 seconds of every minute, no WiFi, current 10-20mA, the dominant factor in power consumption.
* Consults NTP for ~5 seconds every 8 hours, WiFi on, current ~100mA. Accounts for under 3% of power consumption. 

Estimated life of a 500mAh battery: ~30 days.

The biggest opportunity for power-saving is to reduce the screen update time by partial refresh. Potentially reduces update time from 2s to 0.5s, quadrupling battery life.

Kudos:
* Main time-management code based on example from [Paul Stoffegren Time](https://github.com/PaulStoffregen/Time) library.
* e-Paper support and guidance from [Jean-Marc Zingg GxEPD2](https://github.com/ZinggJM/GxEPD2) library.
* Useful videos from [David Watts](https://www.youtube.com/watch?v=OPaCF-XJhqc) and [David Bird](https://www.youtube.com/watch?v=AeYbX0zaJTY).


