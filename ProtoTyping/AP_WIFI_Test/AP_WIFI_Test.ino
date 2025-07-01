#include <WiFi.h>

// --- Network Configuration ---
const char* ssid     = "MyESP32_AP";
const char* password = "123456789";

// --- GPIO Configuration ---
const int outputPin = 2; // Use GPIO 2
bool isGpioOn = false;     // Use boolean for state tracking

// --- Web Server Task ---
void webServerTask(void *pvParameters) {
  WiFiServer server(80);
  String header;

  Serial.println("Setting up the Access Point...");
  WiFi.softAP(ssid, password);

  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);
  
  server.begin();

  for(;;) {
    WiFiClient client = server.accept();

    if (client) {
      Serial.println("\n>> New Client Connected.");
      String currentLine = "";
      while (client.connected()) {
        if (client.available()) {
          char c = client.read();
          header += c;
          if (c == '\n') {
            if (currentLine.length() == 0) {
              
              // --- !! IMPORTANT DIAGNOSTIC STEP !! ---
              Serial.println("--- RECEIVED HEADER ---");
              Serial.print(header);
              Serial.println("-----------------------");

              // Handle the favicon request gracefully
              if (header.indexOf("GET /favicon.ico") >= 0) {
                  Serial.println("-> Ignoring favicon request.");
                  client.println("HTTP/1.1 204 No Content");
                  client.println();
                  break;
              }

              // Control GPIO based on the request
              if (header.indexOf("GET /on") >= 0) {
                Serial.println("-> GPIO ON command received.");
                isGpioOn = true;
                digitalWrite(outputPin, HIGH);
              } else if (header.indexOf("GET /off") >= 0) {
                Serial.println("-> GPIO OFF command received.");
                isGpioOn = false;
                digitalWrite(outputPin, LOW);
              }

              // --- Send the HTTP Response ---
              client.println("HTTP/1.1 200 OK");
              client.println("Content-type:text/html");
              client.println("Connection: close");
              client.println();

              // --- Display the HTML web page ---
              client.println("<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
              client.println("<link rel=\"icon\" href=\"data:,\">");
              client.println("<style>html{font-family:Helvetica;display:inline-block;margin:0 auto;text-align:center;}");
              client.println(".button{background-color:#008CBA;border:none;color:white;padding:16px 40px;text-decoration:none;font-size:30px;margin:2px;cursor:pointer;}");
              client.println(".button2{background-color:#555555;}</style></head>");
              client.println("<body><h1>ESP32 Web Server</h1><h2>GPIO 2 Control</h2>");
              
              if (isGpioOn) {
                client.println("<p>GPIO 2 State: ON</p><p><a href=\"/off\"><button class=\"button button2\">TURN OFF</button></a></p>");
              } else {
                client.println("<p>GPIO 2 State: OFF</p><p><a href=\"/on\"><button class=\"button\">TURN ON</button></a></p>");
              }
              
              client.println("</body></html>");
              client.println();
              break; 
            } else {
              currentLine = "";
            }
          } else if (c != '\r') {
            currentLine += c;
          }
        }
      }
      header = "";
      client.stop();
      Serial.println(">> Client Disconnected.");
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void setup() {
  delay(250); // Add a short delay
  Serial.begin(115200);
  
  pinMode(outputPin, OUTPUT);
  
  // --- Blink Test ---
  // This will confirm your LED is working on the correct pin before Wi-Fi starts.
  Serial.println("Performing blink test on GPIO 2...");
  digitalWrite(outputPin, HIGH);
  delay(500);
  digitalWrite(outputPin, LOW);
  delay(500);
  Serial.println("Blink test complete.");

  // Create the Web Server Task on Core 1
  xTaskCreatePinnedToCore(
    webServerTask, "WebServer", 8192, NULL, 1, NULL, 1
  );

  Serial.println("Main setup() finished. Web server task is running.");
}

void loop() {
  // Main loop is idle.
  vTaskDelay(1000 / portTICK_PERIOD_MS);
}