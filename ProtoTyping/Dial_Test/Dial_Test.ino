const int pinLeft = 4;
const int pinRight = 16;

void setup() {
  // Set both pins as inputs with internal pull-up resistors enabled
  pinMode(pinLeft, INPUT_PULLUP);
  pinMode(pinRight, INPUT_PULLUP);
  pinMode(17, OUTPUT);
  pinMode(13, OUTPUT);

  // Start the serial monitor to display the results
  Serial.begin(115200);
  Serial.println("Ready to read pin states...");
  Serial.println("Note: Unconnected pins will read HIGH.");
  digitalWrite(13, HIGH);
}

void loop() {
  

  // Read the current logical state of each pin
  int stateLeft = digitalRead(pinLeft);
  int stateRight = digitalRead(pinRight);

  // Check which of the three defined states is active
  if (stateLeft == HIGH && stateRight == LOW) {
    // State 1: (HIGH, LOW)
    Serial.println("State 1 detected: (Left: HIGH, Right: LOW)");
    digitalWrite(17, LOW);

  } else if (stateLeft == LOW && stateRight == LOW) {
    // State 2: (LOW, LOW)
    Serial.println("State 2 detected: (Left: LOW, Right: LOW)");
        digitalWrite(17, HIGH);


  } else if (stateLeft == LOW && stateRight == HIGH) {
    // State 3: (LOW, HIGH)
    Serial.println("State 3 detected: (Left: LOW, Right: HIGH)");
    digitalWrite(17, LOW);
  }
  
  // A small delay to keep the serial monitor from flooding
  delay(100);
}