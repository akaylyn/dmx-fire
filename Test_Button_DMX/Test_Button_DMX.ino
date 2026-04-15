/*
 * @Hardwares: M5AtomS3 Lite + Unit DMX
 * @Platform Version: Arduino M5Stack Board Manager v2.1.3
 * @Dependent Library:
 * M5Unified@^0.2.11: https://github.com/m5stack/M5Unified
 * FastLED@^3.9.10: https://github.com/FastLED/FastLED
 * SparkFunDMX@^2.0.1: https://github.com/sparkfun/SparkFunDMX 
 */

#include <M5Unified.h>
#include <FastLED.h>
#include <SparkFunDMX.h>

// Handle DMX calls for light and fire
SparkFunDMX dmxDevice;
const uint8_t enPin = -1;
const uint8_t rxPin = 1;
const uint8_t txPin = 2;
const uint16_t numChannels = 64;  // Number of DMX channels, can be up to 512
uint16_t dmxSerialBufferSize = 0;

#define KEY_INPUT_PIN 39  // simple switch
m5::Button_Class keyButton;

#define ATOM_RGB_PIN 35  // one-wire data line
CRGB ATOM_LED[1];

void setup() {
  delay(500);

  auto cfg = M5.config();
  cfg.serial_baudrate = 115200;
  M5.begin(cfg);
  M5.Ex_I2C.release();  // Free up PORTA so we can use it for Serial1

  Serial.print("\nStartup.\n");

  // define pin modes after M5.begin(), as it might (kinda randomly) yoink a pin for... stuff.
  pinMode(KEY_INPUT_PIN, INPUT_PULLUP);
  pinMode(ATOM_RGB_PIN, OUTPUT);

  /* Init RGB led */
  FastLED.addLeds<WS2812, ATOM_RGB_PIN, GRB>(ATOM_LED, 1);
  ATOM_LED[0] = CRGB::Red;
  FastLED.setBrightness(255);
  FastLED.show();

  // Begin DMX serial port
  Serial1.begin(DMX_BAUD, DMX_FORMAT, rxPin, txPin);
  Serial1.setTxBufferSize(512);  // dmx universe size.
  Serial1.flush();
  // how big is the buffer?
  dmxSerialBufferSize = Serial1.availableForWrite();

  // Begin DMX driver
  dmxDevice.begin(Serial1, enPin, numChannels);

  // Set communication direction, which can be changed on the fly as needed
  dmxDevice.setComDir(DMX_WRITE_DIR);
}

// Work with palettes as an easy way to have dynamic colors
// https://github.com/FastLED/FastLED/wiki/Gradient-color-palettes
DEFINE_GRADIENT_PALETTE(firepal){
  // Remember that we're rotating around the palette with a 0-255 index, so you want the 0 value and the 255 value to align or the color change is jerky.
  30, 255, 255, 0,    // yellow (middle of flames)
  65, 255, 0, 0,      // red (base of flames)
  225, 255, 255, 0,   // yellow (middle of flames)
  255, 255, 255, 255  // white (hottest part/tips of flames)
};

DEFINE_GRADIENT_PALETTE(electricGreenFirePal){
  // Green fire palette - for a toxic/alien look
  0, 0, 32, 0,        // dark dark green.
  32, 0, 70, 0,       // dark green (base)
  190, 57, 255, 20,   // electric neon green (middle)
  255, 255, 255, 255  // white (hottest part)
};

DEFINE_GRADIENT_PALETTE(electricBlueFirePal){
  // Blue fire palette - for a cold/ice fire look
  0, 0, 0, 0,         // Black (bottom)
  32, 0, 0, 70,       // Dark blue (base)
  128, 20, 57, 255,   // Electric blue (middle)
  255, 255, 255, 255  // White (hottest part)
};

void loop() {
  M5.update();
  FastLED.show();

  // This is shite: the constructor can't map to a hardware pin.
  // So, map GPIO → button
  bool pressed = (digitalRead(KEY_INPUT_PIN) == LOW);
  keyButton.setRawState(millis(), pressed);

  // some DMX devices will time out without a periodic update
  EVERY_N_MILLISECONDS(1000) {
    // current outbound buffer size
    uint16_t sendingBufferSize = dmxSerialBufferSize - Serial1.availableForWrite();
    // if we're currently sending, bail out.
    if (sendingBufferSize == 0) dmxDevice.update();
  }

  // store the current color palettes
  static CRGBPalette256 currPal = electricGreenFirePal;  // green?
  static byte currBright = 16;                           // low

  // check the Big Button status and do stuff on a state change
  if (keyButton.wasPressed()) {
    Serial.println("External Pressed");

    // Signal that we're firing
    ATOM_LED[0] = CRGB::White;

    currPal = firepal;  // red
    currBright = 255;   // bright
  }

  if (keyButton.wasReleased()) {
    Serial.println("External Released");

    currPal = electricGreenFirePal;  // back to green
    currBright = 16;                 // dim
  }

  // returns true while held.
  if (keyButton.isPressed()) {
  }

  // don't bomb the DMX channel with outputs
  EVERY_N_MILLISECONDS(20) {
    // track color index
    static uint8_t currIndex = 0;
    // Get the actual RGB color from the palette
    CRGB c = ColorFromPalette(currPal, currIndex++, currBright, LINEARBLEND);

    // NOTE: DMX addresses start a _1_ NOT 0.  Writing to address 0 is a super-bad idea; appears to crash the DMX system.
    // Ask me how I know.
    dmxDevice.writeByte(c.r, 1);
    dmxDevice.writeByte(c.g, 2);
    dmxDevice.writeByte(c.b, 3);

    // white strobes while button pressed.
    static byte whiteLevel = 0;
    if (keyButton.isPressed()) {
      EVERY_N_MILLISECONDS(100) {
        if (whiteLevel == 0) whiteLevel = 255;
        else whiteLevel = 0;
      }
    } else {
      whiteLevel = 0;
    }
    dmxDevice.writeByte(whiteLevel, 4);

    dmxDevice.update();
  }

  // as an alternative to palettes, could use a rotating HSV object to cycle colors.
  if (!keyButton.isPressed()) {
    // otherwise, track hue around the colorwheel
    EVERY_N_MILLISECONDS(20) {

      static uint8_t currHue = 0;
      // Get the actual RGB color from the palette
      CHSV chsv = CHSV(currHue++, 255, 255);

      ATOM_LED[0] = chsv;

      CRGB crgb = chsv;  // in case you want to extract RGB levels from it, like the DMX stuff above.
    }
  }

  if (M5.BtnA.isPressed()) {
    Serial.println("Atom Pressed");
    ATOM_LED[0] = CRGB::White;
  }

  if (M5.BtnA.wasReleased()) {
    Serial.println("Atom Released");
  }
}