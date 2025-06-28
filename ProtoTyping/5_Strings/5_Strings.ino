const int PHOTODIODE_PINS[5] = {34, 35, 32, 33, 27}; // Photodiode input pins
const int LED_PINS[5] = {16, 26, 4, 25, 13};           // LED output pins
const int NUM_SENSORS = 5;

// Threshold value to determine if laser is broken
// Adjust this based on your photodiode readings
const int THRESHOLD = 2000; // Mid-point for 10-bit ADC (0-1023)

// Variables to store previous states for change detection
bool previousStates[NUM_SENSORS];

void setup() {
  // Initialize serial communication for debugging
  Serial.begin(115200);
  
  // Configure LED pins as outputs
  for (int i = 0; i < NUM_SENSORS; i++) {
    pinMode(LED_PINS[i], OUTPUT);
    digitalWrite(LED_PINS[i], LOW); // Start with LEDs off
    previousStates[i] = false;
  }
  
  // Photodiode pins are analog inputs by default on ESP32
  // No need to set pinMode for analog pins
  
  Serial.println("Laser Break Detection System Initialized");
  Serial.println("Photodiode readings will be displayed...");
}

void loop() {
  bool stateChanged = false;
  
  for (int i = 0; i < NUM_SENSORS; i++) {
    // Read photodiode value (0-1023 for 10-bit ADC)
    int photodiodeValue = analogRead(PHOTODIODE_PINS[i]);
    
    // Determine if laser is broken (photodiode reading below threshold)
    bool laserBroken = photodiodeValue < THRESHOLD;
    
    // Control LED based on laser state
    if (laserBroken) {
      digitalWrite(LED_PINS[i], HIGH); // Turn on LED when laser is broken
    } else {
      digitalWrite(LED_PINS[i], LOW);  // Turn off LED when laser is connected
    }
    
    // Check if state changed for debugging output
    if (laserBroken != previousStates[i]) {
      stateChanged = true;
      previousStates[i] = laserBroken;
      
      Serial.print("Sensor ");
      Serial.print(i + 1);
      Serial.print(": ");
      if (laserBroken) {
        Serial.println("LASER BROKEN - LED ON");
      } else {
        Serial.println("Laser connected - LED off");
      }
    }
  }
  
  // Print readings for each string periodically
  static unsigned long lastPrint = 0;
  if (millis() - lastPrint > 20) { // Print every 500ms
    Serial.println("=== LASER STRING STATUS ===");
    
    for (int i = 0; i < NUM_SENSORS; i++) {
      int reading = analogRead(PHOTODIODE_PINS[i]);
      bool isBroken = reading < THRESHOLD;
      
      Serial.print("String ");
      Serial.print(i + 1);
      Serial.print(": Reading = ");
      Serial.print(reading);
      Serial.print(" | Status: ");
      
      if (isBroken) {
        Serial.print("BROKEN (below ");
        Serial.print(THRESHOLD);
        Serial.println(")");
      } else {
        Serial.print("Connected (above ");
        Serial.print(THRESHOLD);
        Serial.println(")");
      }
    }
    
    // Summary of broken strings
    Serial.print("Broken strings: ");
    bool anyBroken = false;
    for (int i = 0; i < NUM_SENSORS; i++) {
      if (analogRead(PHOTODIODE_PINS[i]) < THRESHOLD) {
        if (anyBroken) Serial.print(", ");
        Serial.print(i + 1);
        anyBroken = true;
      }
    }
    if (!anyBroken) {
      Serial.print("None");
    }
    Serial.println();
    Serial.println(""); // Empty line for readability
    
    lastPrint = millis();
  }
  
  delay(20); // Small delay to prevent excessive readings
}