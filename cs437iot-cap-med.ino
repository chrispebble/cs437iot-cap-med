#include "esp_pm.h"             // Include for Dynamic Frequency Scaling (DFS)
#include <Adafruit_NeoPixel.h>  // Control neopixel ring light
#include <NTPClient.h>          // Access Time server
#include <Preferences.h>        // To access ESP32S3 Non-Volatile Storage
#include <WiFi.h>               // Create WiFi server
#include <WiFiClient.h>         // Create WiFi client
#include <math.h>               // Used for calls to ceil()

// Define the input pins
#define TILT_PIN 9 // Ball tilt sensor
#define BTN_PIN 7  // Physical button

// Define the Neo Pixel output pins
#define NEO_PIN 8
#define NEO_LED_COUNT 12

#define WIFI_RETRIES 10
#define NTP_RETRIES 10

// Define ring light options
#define RING_OFF 0
#define RING_COUNTDOWN_INIT 1
#define RING_BRIGHT_COUNTDOWN 2
#define RING_DIM_COUNTDOWN 3
#define RING_TAKE_INIT 4
#define RING_BRIGHT_TAKE 5
#define RING_DIM_TAKE 6
#define RING_WIFI_CONNECTING 7
#define RING_WIFI_SUCCESS 8
#define RING_WIFI_FAIL 9

volatile bool tiltTriggered = false;   // Flag for tilt sensor trigger
volatile bool buttonPressed = false;   // Flag for button press
unsigned long buttonHeldStartTime = 0; // Time when the button was first pressed
const unsigned long holdTime =
    2;                           // Press-and-hold time (secs) to trigger button
const time_t wakeTime = 10;      // Amount of time to stay awake before dimming
const time_t deepSleepTime = 20; // Amount of time before going to "deep" sleep
bool needsWakeup =
    false; // Flag to ensure light is made bright when timer expires

Adafruit_NeoPixel ring(NEO_LED_COUNT, NEO_PIN, NEO_GRB + NEO_KHZ800);
time_t lastPillClockReset = 0; // Time of the last wake-up
time_t wakeStartTime = 0;      // Time when the device last woke up
time_t pillInterval = 1 * 60;  // Interval between taking pills (in seconds)
double pixelDuration =
    (double)pillInterval /
    (double)ring.numPixels(); // How much time (secs) each pixel represents

// Network credentials
const char *ssid = "ssid";
const char *password = "pw";

// Initialize non-volatile storage
Preferences preferences;
constexpr const char *prefsNamespace = "pebble-cap";

// Create server and NTP client
WiFiServer server(80);

// Forward declarations of helper functions
void connectToWiFi();
void handleClientRequest(WiFiClient &client);
void readHeaders(WiFiClient &client, bool &isPost, int &contentLength);
String readBody(WiFiClient &client, int contentLength);
void sendResponse(WiFiClient &client, bool intervalChanged);
void ringLight(uint8_t ringAction);
void ringLight(uint8_t ringAction, uint8_t numPixelsOn);

void IRAM_ATTR buttonISR() {
    // Serial.println("buttonISR");
    if (digitalRead(BTN_PIN) == HIGH) { // button pressed
        // Record when the button was first pressed
        buttonPressed = true; // Set the button press flag
        buttonHeldStartTime = time(NULL);
    } else { // button released
        // Reset the button hold state on release
        buttonPressed = false;
    }
}

void IRAM_ATTR tiltISR() {
    tiltTriggered = true; // Set the tilt sensor flag
}

// Function to configure DFS for power saving
void configureDFS() {
    esp_pm_config_esp32_t pm_config = {
        .max_freq_mhz = 240, // Maximum frequency (default for ESP32)
        .min_freq_mhz = 80,  // Minimum frequency for power saving
        .light_sleep_enable =
            false // Disable light sleep (we're using DFS instead)
    };
    esp_pm_configure(&pm_config);
}

/* ****************************************
 * SETUP
 * ****************************************/
void setup() {
    Serial.begin(115200);
    delay(100);
    Serial.println("Device is awake...");

    // Configure input pins
    pinMode(TILT_PIN, INPUT_PULLUP);
    pinMode(BTN_PIN, INPUT);
    attachInterrupt(BTN_PIN, buttonISR,
                    CHANGE); // Detect button press or release
    attachInterrupt(TILT_PIN, tiltISR,
                    CHANGE); // Tilt sensor triggers on state change

    // Configure neopixel
    ring.begin();
    ring.show();
    ring.setBrightness(10);

    // Connect to WiFi
    connectToWiFi();
    server.begin();
    Serial.println("Server started.");

    // Configure time to ntp servers, use 0, 0 to get GMT time.
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    int retries = NTP_RETRIES;
    while (time(NULL) < 1577836800) { // 1577836800 == January 1, 2020
        delay(1000);                  // Wait until time is obtained
        if (--retries == 0)
            break; // Wait until time is set, give it 10 seconds
    }
    // Check if time sync successful
    if (time(NULL) >= 1577836800) {
        Serial.println("NTP sync successful!");
    } else {
        Serial.println("NTP sync failed.");
    }
    time_t now = time(NULL);
    Serial.println((long)now);

    // Access flash memory on ESP32S3
    // The second parameter (false) means we want read/write mode.
    preferences.begin(prefsNamespace, false);
    // Check if we have a stored time variable, if not use the current time
    lastPillClockReset = (time_t)preferences.getLong64("lastpill", time(NULL));
    preferences.end();
    Serial.printf("Loading saved lastPillClockReset: %lu\n",
                  lastPillClockReset);

    // Start wake timer
    wakeStartTime = time(NULL);

    // Start power management
    configureDFS();
}

/* ****************************************
 * LOOP
 * ****************************************/
void loop() {

    // Handle button held condition
    if (buttonPressed && (digitalRead(BTN_PIN) == HIGH)) {
        if ((time(NULL) - buttonHeldStartTime) >= holdTime) {
            buttonPressed = false; // Clear flag to prevent re-triggering
            needsWakeup =
                true; // If we are dim when countdown ends, wake up to alert!
            // Update our last pill taken timer
            lastPillClockReset = time(NULL);
            // Save to non-volatile memory
            preferences.begin(prefsNamespace, false);
            preferences.putLong64("lastpill",
                                  (unsigned long)lastPillClockReset);
            preferences.end();
            // Update our wake timer
            wakeStartTime = time(NULL);

            // Special ring light action showing we are starting the countdown
            ringLight(RING_COUNTDOWN_INIT);

            Serial.println("Button held for 1 second, action triggered.");
            Serial.print("lastPillClockReset = ");
            Serial.println(lastPillClockReset);
        }
    }

    if (tiltTriggered) {
        tiltTriggered = false;
        wakeStartTime = time(NULL);
    }

    // Update Ring Light
    time_t elapsed = time(NULL) - lastPillClockReset;
    bool inCountdown = (elapsed < pillInterval);
    bool awake = ((time(NULL) - wakeStartTime) <= wakeTime);
    bool lightSleep = ((time(NULL) - wakeStartTime) <= deepSleepTime);
    // COUNTDOWN
    if (inCountdown) {
        double timeRemaining = pillInterval - elapsed;
        uint8_t fracRemaining = (uint8_t)ceil(timeRemaining / pixelDuration);
        if (awake) {
            ringLight(RING_BRIGHT_COUNTDOWN, fracRemaining);
        } else {
            if (lightSleep) {
                ringLight(RING_DIM_COUNTDOWN, fracRemaining);
            } else {
                ringLight(RING_OFF);
            }
        }
    }
    // TAKE MED
    else {
        if (awake) {
            ringLight(RING_BRIGHT_TAKE);
            needsWakeup = false;
        } else {
            if (needsWakeup) {
                wakeStartTime = time(NULL);
                needsWakeup = false;
                // on next cycle, we'll be awake
            } else {
                ringLight(RING_DIM_TAKE);
            }
        }
    }

    WiFiClient client = server.available(); // Listen for incoming clients
    if (client) {
        Serial.println("New Client connected.");
        handleClientRequest(client);
        client.stop();
        Serial.println("Client disconnected.");
    }

    // Allow DFS to enter low power mode
    delay(100);
}

void changeDoseInterval(time_t newInterval) {
    // convert hours to seconds
    pillInterval = newInterval;
    // How much time (in seconds) each of the 12 pixels represents
    pixelDuration = (double)pillInterval / (double)ring.numPixels();

    Serial.print("Dose interval changed to: ");
    Serial.print(pillInterval);
    Serial.println(" seconds");
}

void connectToWiFi() {
    Serial.print("Connecting to WiFi");
    WiFi.begin(ssid, password);
    int retries = WIFI_RETRIES;
    while (WiFi.status() != WL_CONNECTED) {
        ringLight(RING_WIFI_CONNECTING);
        Serial.print(".");
        delay(1000);
        if (--retries == 0)
            break; // Wait until time is set, give it 10 seconds
    }

    if (WiFi.status() != WL_CONNECTED) {
        ringLight(RING_WIFI_FAIL);
    } else {
        ringLight(RING_WIFI_SUCCESS);
    }

    Serial.println("\nWiFi connected.");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
}

// // Handles the full request/response lifecycle for a given client.
void handleClientRequest(WiFiClient &client) {
    bool isPost = false;
    int contentLength = 0;

    readHeaders(client, isPost, contentLength);

    bool intervalChanged = false;

    if (isPost && contentLength > 0) {
        String postData = readBody(client, contentLength);
        Serial.print("POST Body: ");
        Serial.println(postData);

        // Extract days
        int daysStart = postData.indexOf("days=");
        int hoursStart = postData.indexOf("hours=");
        int minutesStart = postData.indexOf("minutes=");

        if (daysStart != -1 && hoursStart != -1 && minutesStart != -1) {
            // Parse days
            daysStart += 5; // skip "days="
            int daysEnd = postData.indexOf('&', daysStart);
            if (daysEnd == -1)
                daysEnd = postData.length();
            String daysStr = postData.substring(daysStart, daysEnd);
            uint32_t days = daysStr.toInt();

            // Parse hours
            hoursStart += 6; // skip "hours="
            int hoursEnd = postData.indexOf('&', hoursStart);
            if (hoursEnd == -1)
                hoursEnd = postData.length();
            String hoursStr = postData.substring(hoursStart, hoursEnd);
            uint32_t hours = hoursStr.toInt();

            // Parse minutes
            minutesStart += 8; // skip "minutes="
            int minutesEnd = postData.indexOf('&', minutesStart);
            if (minutesEnd == -1)
                minutesEnd = postData.length();
            String minutesStr = postData.substring(minutesStart, minutesEnd);
            uint32_t minutes = minutesStr.toInt();

            // Convert to seconds
            time_t newIntervalInSeconds =
                (days * 24UL * 3600UL) + (hours * 3600UL) + (minutes * 60UL);
            Serial.print("Parsed Interval: ");
            Serial.print(days);
            Serial.print("d ");
            Serial.print(hours);
            Serial.print("h ");
            Serial.print(minutes);
            Serial.println("m");
            Serial.print("In seconds: ");
            Serial.println(newIntervalInSeconds);

            if (newIntervalInSeconds > 0) {
                changeDoseInterval(newIntervalInSeconds);
                intervalChanged = true;
            }
        }
    }

    sendResponse(client, intervalChanged);
}

// Reads request headers from the client.
// Updates isPost and contentLength based on the headers.
void readHeaders(WiFiClient &client, bool &isPost, int &contentLength) {
    String currentLine = "";
    while (client.connected()) {
        if (client.available()) {
            char c = client.read();
            if (c == '\n') {
                if (currentLine.length() == 0) {
                    return; // Headers are complete
                } else {
                    if (currentLine.startsWith("POST ")) {
                        isPost = true;
                    }
                    int clIndex = currentLine.indexOf("Content-Length: ");
                    if (clIndex != -1) {
                        contentLength = currentLine.substring(clIndex + 16).toInt();
                    }
                    currentLine = "";
                }
            } else if (c != '\r') {
                currentLine += c;
            }
        }
    }
}


String readBody(WiFiClient &client, int contentLength) {
    String postData = "";
    while ((int)postData.length() < contentLength) {
        if (client.available()) {
            char bodyChar = client.read();
            postData += bodyChar;
        }
    }
    Serial.print("POST Body: ");
    Serial.println(postData);
    return postData;
}

void sendResponse(WiFiClient &client, bool intervalChanged) {
    // Recalculate times based on current states
    time_t timeSinceLastDose = time(NULL) - lastPillClockReset;
    uint32_t currentIntervalSeconds =
        pillInterval; // Current interval in seconds
    uint32_t currentIntervalDays = currentIntervalSeconds / (3600 * 24);
    uint32_t remainder = currentIntervalSeconds % (3600 * 24);
    uint32_t currentIntervalHours = remainder / 3600;
    remainder = remainder % 3600;
    uint32_t currentIntervalMinutes = remainder / 60;

    uint32_t hours = timeSinceLastDose / 3600;
    uint32_t minutes = (timeSinceLastDose % 3600) / 60;
    uint32_t seconds = timeSinceLastDose % 60;

    // Send HTTP header
    client.println("HTTP/1.1 200 OK");
    client.println("Content-Type: text/html; charset=UTF-8");
    client.println("Connection: close");
    client.println();

    // Begin HTML with Bootstrap and custom styling
    client.println("<!DOCTYPE html>");
    client.println("<html lang='en'>");
    client.println("<head>");
    client.println("<meta charset='UTF-8'>");
    client.println("<meta name='viewport' content='width=device-width, "
                   "initial-scale=1.0'>");
    client.println(
        "<link "
        "href='https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/"
        "bootstrap.min.css' rel='stylesheet' "
        "integrity='sha384-9ndCyUa1m1F8hU4heJu7r7vx0cZlrr63c6lu6zWJe2eJwn/"
        "kMp13hZfp+Bk1JYh7' crossorigin='anonymous'>");

    // Custom CSS to give some margin, serif font, and uniform input sizes
    client.println("<style>");
    client.println("body { font-family: Verdana, sans-serif; padding: 2rem; "
                   "background-color: #f8f9fa; }");
    client.println(".form-control { max-width: 80px; text-align: center; }");
    client.println(".input-label { font-weight: 600; margin-bottom: 0.7rem; "
                   "display: inline-block; }");
    client.println("input { width: 50px; }");
    client.println("button { height: 2rem; width: 150px; margin-top: 1rem;}");
    client.println("h1 { font-size: 2rem; }");
    client.println(".card { border: none; margin: 10px; }");
    client.println("</style>");

    client.println("<title>IOT Medication Cap Settings</title>");
    client.println("</head>");
    client.println("<body>");

    client.println("<div class='container'>");
    client.println("<div class='card'>");
    client.println("<div class='card-body'>");
    client.println("<h1 class='card-title mb-4'>Medication Dose Interval</h1>");

    if (intervalChanged) {
        client.println("<div class='alert alert-success' role='alert'>");
        client.println("The interval was successfully changed!");
        client.println("</div>");
    }

    // Show the current interval in days, hours, minutes
    client.println("<p><strong>Current Interval:</strong> ");
    client.print(currentIntervalDays);
    client.print("d ");
    client.print(currentIntervalHours);
    client.print("h ");
    client.print(currentIntervalMinutes);
    client.println("m</p>");

    client.println("<p><strong>Time Since Last Dose:</strong> ");
    client.print(hours);
    client.print("h ");
    client.print(minutes);
    client.print("m ");
    client.print(seconds);
    client.println("s</p>");

    // Form with three input boxes: days, hours, minutes
    client.println("<form method='POST'>");
    client.println("<div class='mb-3'>");
    client.println(
        "<label class='form-label d-block fw-bold'>New Dose Interval:</label>");

    // We'll use a row of labeled inputs
    client.println("<div class='row g-3 align-items-end'>");

    // Days input
    client.println("<div class='col-auto'>");
    client.println("<label for='days' class='input-label'>Days</label>");
    client.print("<input type='number' class='form-control' id='days' "
                 "name='days' placeholder='0' min='0' size='10' value='");
    client.print(currentIntervalDays);
    client.println("' required>");
    client.println("</div>");

    // Hours input
    client.println("<div class='col-auto'>");
    client.println("<label for='hours' class='input-label'>Hours</label>");
    client.print(
        "<input type='number' class='form-control' id='hours' name='hours' "
        "placeholder='0' min='0' max='23' size='10' value='");
    client.print(currentIntervalHours);
    client.println("' required>");
    client.println("</div>");

    // Minutes input
    client.println("<div class='col-auto'>");
    client.println("<label for='minutes' class='input-label'>Minutes</label>");
    client.print(
        "<input type='number' class='form-control' id='minutes' name='minutes' "
        "placeholder='0' min='0' max='59' size='10' value='");
    client.print(currentIntervalMinutes);
    client.println("' required>");
    client.println("</div>");

    client.println("</div>"); // end row
    client.println("</div>");

    client.println(
        "<button type='submit' class='btn btn-primary'>Submit</button>");
    client.println("</form>");

    client.println("</div>");
    client.println("</div>");
    client.println("</div>");

    client.println("</body>");
    client.println("</html>");
}

void ringLight(uint8_t ringAction, uint8_t numPixelsOn) {
    switch (ringAction) {
    case RING_OFF:
        setRingColor(ring.Color(0, 0, 0, 0), ring.numPixels());
        break;
    case RING_COUNTDOWN_INIT:
        dazzle(100, ring.numPixels());
        colorWipe(ring.Color(255, 0, 0, 0), 100, ring.numPixels());
        break;
    case RING_BRIGHT_COUNTDOWN:
        colorWipe(ring.Color(255, 0, 0, 0), 100, numPixelsOn);
        break;
    case RING_DIM_COUNTDOWN:
        setRingColor(ring.Color(30, 0, 0, 0), numPixelsOn);
        break;
    case RING_BRIGHT_TAKE:
        colorWipe(ring.Color(0, 255, 0, 0), 50, ring.numPixels());
        break;
    case RING_DIM_TAKE:
        colorWipe(ring.Color(0, 30, 0, 0), 100, ring.numPixels());
        break;
    case RING_WIFI_CONNECTING:
        theaterChase(ring.Color(128, 128, 128, 0), 100, ring.numPixels());
        break;
    case RING_WIFI_SUCCESS:
        theaterChase(ring.Color(0, 0, 255, 0), 100, ring.numPixels());
        break;
    case RING_WIFI_FAIL:
        theaterChase(ring.Color(255, 0, 0, 0), 100, ring.numPixels());
        break;
    default:
        setRingColor(ring.Color(255, 255, 255, 0), ring.numPixels());
        break;
    }
}

// If only ringAction is passed, default to all pixels ON.
void ringLight(uint8_t ringAction) { ringLight(ringAction, ring.numPixels()); }


// ********************************************
// Ring color functions, many taken from strandtest.ino:
// https://github.com/adafruit/Adafruit_NeoPixel/blob/master/examples/strandtest_wheel/strandtest_wheel.ino
// ********************************************

// Just set all pixels to one color
void setRingColor(uint32_t c, uint8_t numPixelsOn) {
    for (uint8_t i = numPixelsOn; i < ring.numPixels(); i++) {
        ring.setPixelColor(i, ring.Color(0, 0, 0, 0));
    }
    for (uint8_t i = 0; i < numPixelsOn; i++) {
        ring.setPixelColor(i, c);
    }
    ring.show();
}

// Fill the dots one after the other with a color
void colorWipe(uint32_t c, uint8_t wait, uint8_t numPixelsOn) {
    for (uint8_t i = numPixelsOn; i < ring.numPixels(); i++) {
        ring.setPixelColor(i, ring.Color(0, 0, 0, 0));
    }
    for (uint8_t i = 0; i < numPixelsOn; i++) {
        ring.setPixelColor(i, c);
        ring.show();
        delay(wait);
    }
}

// Theatre-style crawling lights.
void theaterChase(uint32_t c, uint8_t wait, uint8_t numPixelsOn) {
    for (uint8_t i = numPixelsOn; i < ring.numPixels(); i++) {
        ring.setPixelColor(i, ring.Color(0, 0, 0, 0));
    }
    for (int j = 0; j < 10; j++) { // do 10 cycles of chasing
        for (int q = 0; q < 3; q++) {
            for (uint8_t i = 0; i < numPixelsOn; i = i + 3) {
                ring.setPixelColor(i + q, c); // turn every third pixel on
            }
            ring.show();
            delay(wait);
            for (uint8_t i = 0; i < numPixelsOn; i = i + 3) {
                ring.setPixelColor(i + q, 0); // turn every third pixel off
            }
        }
    }
}

// Theatre-style crawling lights with rainbow effect
void theaterChaseRainbow(uint8_t wait, uint8_t numPixelsOn) {
    for (uint8_t i = numPixelsOn; i < ring.numPixels(); i++) {
        ring.setPixelColor(i, ring.Color(0, 0, 0, 0));
    }
    for (int j = 0; j < 256; j++) { // cycle all 256 colors in the wheel
        for (int q = 0; q < 3; q++) {
            for (uint8_t i = 0; i < numPixelsOn; i = i + 3) {
                ring.setPixelColor(
                    i + q, Wheel((i + j) % 255)); // turn every third pixel on
            }
            ring.show();
            delay(wait);
            for (uint8_t i = 0; i < numPixelsOn; i = i + 3) {
                ring.setPixelColor(i + q, 0); // turn every third pixel off
            }
        }
    }
}

// Razzle Dazzle
void dazzle(uint8_t wait, uint8_t numPixelsOn) {
    for (uint8_t i = numPixelsOn; i < ring.numPixels(); i++) {
        ring.setPixelColor(i, ring.Color(0, 0, 0, 0)); // Turn off pixels
    }

    for (int i = 0; i < numPixelsOn; i++) {
        // Generate random values for R, G, and B
        uint8_t r = random(0, 256);
        uint8_t g = random(0, 256);
        uint8_t b = random(0, 256);

        // Ensure at least one value is 50 or greater
        if (r < 50 && g < 50 && b < 50) {
            int ensureBrightChannel = random(0, 3); // Randomly pick R, G, or B
            switch (ensureBrightChannel) {
            case 0:
                r = random(50, 256);
                break; // Ensure R is >= 50
            case 1:
                g = random(50, 256);
                break; // Ensure G is >= 50
            case 2:
                b = random(50, 256);
                break; // Ensure B is >= 50
            }
        }

        uint32_t color = ring.Color(r, g, b, 0); // Create the color
        ring.setPixelColor(i, color);            // Set pixel color
        ring.show();
        delay(wait);
    }

    for (int i = 0; i <= numPixelsOn; i++) {
        ring.setPixelColor(i, 0, 0, 0, 0); // Turn off pixels
        ring.show();
        delay(wait);
    }
}

// Input a value 0 to 255 to get a color value.
// The colours are a transition r - g - b - back to r.
uint32_t Wheel(byte WheelPos) {
    WheelPos = 255 - WheelPos;
    if (WheelPos < 85) {
        return ring.Color(255 - WheelPos * 3, 0, WheelPos * 3);
    }
    if (WheelPos < 170) {
        WheelPos -= 85;
        return ring.Color(0, WheelPos * 3, 255 - WheelPos * 3);
    }
    WheelPos -= 170;
    return ring.Color(WheelPos * 3, 255 - WheelPos * 3, 0);
}
