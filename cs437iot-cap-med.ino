#include <Adafruit_NeoPixel.h>

#define NEO_PIN   8
#define NEO_LED_COUNT 12

#define TILT_PIN 9
#define BTN_PIN 7

// Variables for the tilt switch status (tilted or not) and button (pressed or not)
int tiltState = 0;
int btnState = 0;

Adafruit_NeoPixel ring(NEO_LED_COUNT, NEO_PIN, NEO_GRB + NEO_KHZ800);

void setup() {
  // Set up Built in LED
  pinMode(LED_BUILTIN, OUTPUT);

  // Set up Tilt switch pins
  pinMode(TILT_PIN, INPUT_PULLUP);

  // Set up Button pins
  pinMode(BTN_PIN, INPUT);

  // Initialize Neo Pixel Ring Light
  ring.begin();
  ring.show();
  ring.setBrightness(10);
}


void loop() {

  btnState = digitalRead(BTN_PIN);
  if (btnState == HIGH){
    dazzle(50);
  } else{

    // If we are not pressing the button, check for movement.
    tiltState = digitalRead(TILT_PIN);
    if (tiltState == HIGH){
      blink(1, 200, 150, 255, 100, 50);
      digitalWrite(LED_BUILTIN, LOW);
    } else{
      digitalWrite(LED_BUILTIN, HIGH);
    }

  }
}

void dazzle(uint8_t wait){
  for(int i = 0; i < ring.numPixels(); i++){
    uint32_t color = ring.Color(random(200,255), random(127,255), random(127,255), 0);
    ring.setPixelColor(i, color);
    ring.show();
    delay(wait);
  }
  for(int i = 0; i <= ring.numPixels(); i++){
    ring.setPixelColor(i, 0, 0, 0, 0);
    ring.show();
    delay(wait);
  }
}

void blink(uint8_t count, uint8_t on, int off, int r, int g, int b){
  uint32_t color = ring.Color(r, g, b, 0); // Assuming white component is 0
  for(int iter = 0; iter < count; iter++){
    for(int i = 0; i < ring.numPixels(); i++){
      ring.setPixelColor(i, color);
    }
    ring.show();
    delay(on);
    for(int i = 0; i < ring.numPixels(); i++){
      ring.setPixelColor(i, 0);
    }
    ring.show();
    delay(off);
  }
}