DAVE — Project Handover
A handover document for the next instance of Claude picking up this project. Read this before doing anything; it'll save you and the user a lot of time.
What DAVE is
DAVE is a personal project: an ambient frosted orb that glows different colours based on the user's girlfriend's blood glucose level. She's a type 1 diabetic on an Omnipod 5 pump with a FreeStyle Libre 2 Plus sensor (the standard NHS hybrid closed-loop setup in the UK). The orb is meant to be a "lovely to have" — a way for both of them to know her state at a glance without picking up a controller — not a medical alarm.
The user is based in Edinburgh, UK, and is comfortable with electronics and code but explicitly does not own a soldering iron and isn't keen on soldering. The whole build has been designed around this constraint.
Colour map
mmol/LMeaningBehaviourbelow 4LowRed, slow pulse4 – 8In rangeGreen, steady8 – 12High-ishBlue, steadyabove 12HighViolet, steadystale / network errorCan't trust the dataDim white, slow pulseT&Cs need acceptingAbbott pushed new termsDim amber, slow pulsebootingJust powered onDim teal, slow pulse
The user's original brief said red above 4, but he had it backwards — corrected to red below 4. Don't reintroduce that bug.
Architecture (decided, do not relitigate)
The data route is LibreLinkUp, Abbott's official "follower" cloud. This works for Omnipod 5 / Libre 2 Plus users in the UK because Insulet and Abbott built an integration: the Omnipod controller uploads sensor data to Abbott's LibreView cloud, which then serves it to LibreLinkUp followers. We confirmed this is officially supported — not a hack — provided two prerequisites are in place (see "Open setup steps" below).
The firmware architecture is all-on-ESP32 (no Pi or home server). We considered a helper-machine architecture and rejected it: the user has no always-on machine, an ESP32 can comfortably handle the workload (a Pi Zero in the community does the same job polling every minute, flawlessly, for over a year), and it matches the "single USB cable, orb on a shelf" vision.
Polling cadence: every 60 seconds. Community wisdom says <45s is risky for API rate limiting.
The data field DAVE reads is data.connection.glucoseMeasurement.Value, not graphData. This is important — graphData is downsampled into 15-minute averages and can lag 15–30 minutes behind. Several developers in the open-source diabetes community got this wrong and got bad behaviour from it. Don't switch to graphData.
Hardware (purchased, on the way from Amazon UK)

ESP32-DevKitC V4 (DUBEUYEW brand), with external U.FL antenna (the -U variant). The user picked the external-antenna version specifically so the orb can move around the house. The U.FL connector is fiddly the first time — needs to be clipped on with a fingernail; if it's missing, no WiFi.
BTF-Lighting WS2812B strip, 1m, 100 LEDs (60 LEDs/m). Ships with a JST connector and "gift" male-connector pigtail with bare wires on the other end — perfect for breadboard use, no soldering.
400-point solderless breadboard.
ELEGOO 120-piece jumper wire kit (M-M, M-F, F-F).
Resistor assortment kit (will pull a 330Ω from this).
1000μF / 16V electrolytic capacitors (one needed; 15 in the pack).

Only the first 16 LEDs of the 100-LED strip are used. The rest sit dark inside the orb. This was a deliberate substitution: rings come bare (require soldering), strips ship with wires already attached.
A frosted lamp/orb enclosure has NOT been bought yet — the user wants to involve his girlfriend in picking it. Anything frosted, hollow, ~8–15cm across, with an opening big enough to fit the strip + ESP32 will work.
What state the firmware is in
A complete Arduino sketch (DAVE.ino) has been written and handed off. Key things to know about it:

Has a USE_FAKE_GLUCOSE flag at the top. Defaults to true. In this mode, DAVE ignores the network entirely and cycles through every state (low/in-range/high-ish/high/stale/tos) once per minute. This lets the user test the entire hardware build before the LibreLinkUp account is even set up.
The LLU section implements: login with token caching (~6 month JWT), SHA-256 hash of userId into the Account-Id header (using mbedTLS on-chip), status-4 detection for the T&C trap, automatic re-login on 401/403, and reads the live glucoseMeasurement field (not graphData).
Smooth colour fades between states (~700ms transitions, verified by simulation), gentle 3.5s breathing pulse with a 35% floor so it never blinks fully off.
LLU_REGION = "eu" for UK accounts.
The LLU_VERSION constant ("4.16.0" at time of writing) tracks the LibreLinkUp Android app version; if logins start failing months from now, this is almost always the fix — bump it to whatever the current app version is.

Known limitation
Freshness check is half-wired. The approximateAgeSeconds function currently returns 0 (always treats data as fresh). This means connection failures correctly trigger the stale-state, but a working connection serving an old number won't. The fix is NTP + parsing the response Timestamp field. This is the planned v1.1 improvement once the live data path is confirmed working. Do not try to fix this proactively unless the user explicitly asks — there's no point adding untested code paths until we know the basic data flow works.
Open setup steps (gating real-data testing)
The user can do hardware testing immediately when parts arrive (fake-glucose mode). Going live requires two account-setup steps on the human side, which were paused because his girlfriend was busy:

Link her PodderCentral (omnipod.com) account to a LibreView account. This is the Omnipod 5 / Libre 2 Plus account-linking step Insulet rolled out for the UK. Some UK users have reported delays getting Insulet to enable the feature on their account — may need chasing Insulet support if it isn't available yet.
Set up LibreLinkUp follower chain. From her LibreView account, invite a follower email he controls. He installs the official LibreLinkUp app on his phone, registers, accepts the invite, and taps through every T&C screen until his phone displays her live glucose number. Reaching that "I can see her number on my phone" point is the proof the data route works, and clears the T&C trap so DAVE's first automated login succeeds. The same email/password go into DAVE's config.

Verification step we agreed on: before any code goes live, his phone should show her live readings via LibreLinkUp for a day or two. If that works, DAVE will work. If it doesn't, no amount of ESP32 cleverness fixes it.
WiFi confirmation (resolved)
Her Omnipod 5 Controller is the locked-down NHS-issued Android device that uploads to the LibreView cloud. The user initially feared it wasn't on WiFi, but confirmed WiFi is available and just hasn't been connected yet. She needs to connect it to home WiFi as part of the LibreLinkUp setup above. The whole project hinges on the controller being online, so if anything goes wrong with that step, everything else stops mattering.
What's likely to come next
When the parts arrive, the user will send a photo of the ESP32 (especially the pin-label silkscreen). Things to do at that point:

Confirm GPIO pin choice. The sketch assumes GPIO 5 for the LED data line. ESP32-DevKitC V4 silkscreens label this clearly, but worth checking. GPIO 5 is generally safe (no boot-strapping conflicts), but if the user's board labels are different from the standard reference, point this out.
Walk through the U.FL antenna clip-on. Small gold square on the ESP32-WROOM-32U module; press the U.FL plug onto it with a fingernail until it clicks. Then screw the SMA antenna onto the cable's other end.
Draw the actual wiring diagram. ESP32 5V → cap+ → strip red. ESP32 GND → cap– → strip white. ESP32 GPIO5 → 330Ω resistor → strip green. Capacitor sits close to the strip end of the power lines.
Walk through flashing the sketch (Arduino IDE, board = "ESP32 Dev Module", install Adafruit NeoPixel and ArduinoJson libraries, plug in USB, select port, upload).
Watch the fake-glucose cycle confirm everything works visually.
Only then do the LibreLinkUp credentials and the flag flip.
After it's stable on live data, look at v1.1: NTP + real freshness checking.

Tone the user appreciates
The user is technically sharp and likes:

Being told when something is genuinely uncertain vs. just being guessed at. Don't pretend to know things you don't.
Being told why a decision is being made, not just what.
Honest flagging of known limitations (e.g. the half-wired freshness check, the API being community-reverse-engineered rather than official, the Insulet account-linking sometimes being delayed).
Concrete next steps rather than open-ended "let me know what you think."
Being asked questions when answers genuinely shape the work, but not being made to make pointless choices.

He's doing this for his girlfriend, who is a real person with type 1 diabetes. The safety framing matters: DAVE is ambient, not medical. Her Libre and Omnipod alarms remain the safety-critical layer. The "fail visibly" design (white pulse when DAVE can't see fresh data) is a deliberate ethical choice, not just a UX nicety — don't strip it out for "simplicity."
One last thing
If anything in the firmware or this document conflicts with what the user actually wants, the user wins. This is his project. These notes are scaffolding, not gospel.
Good luck. Have fun. DAVE's a nice project.
