// --- INCLUDES ---
#include "driver/i2s.h"
#include "driver/adc.h"
#include "SD.h"
#include "SPI.h"
#include <Wire.h>
#include "Adafruit_TPA2016.h"
#include <WiFi.h>

// --- HARDWARE & PIN DEFINITIONS ---
#define I2C_SDA_PIN 22
#define I2C_SCL_PIN 26
#define SD_CS_PIN     5
#define SPI_MOSI_PIN 23
#define SPI_MISO_PIN 19
#define SPI_SCK_PIN  18

// Dial control pins
#define DIAL_LEFT_PIN 4   // Safe input pin
#define DIAL_RIGHT_PIN 15 // Strapping pin but works as input with pull-up
#define LASER_CONTROL_PIN 13
#define PUMP_PIN 2  // Strapping pin - HIGH=pump ON (good for boot)

const int PHOTODIODE_PINS[5] = {34, 35, 32, 33, 36};
const adc1_channel_t ADC_CHANNELS[5] = {
    ADC1_CHANNEL_6, // GPIO 34
    ADC1_CHANNEL_7, // GPIO 35
    ADC1_CHANNEL_4, // GPIO 32
    ADC1_CHANNEL_5, // GPIO 33
    ADC1_CHANNEL_0  // GPIO 36
};

// --- WIFI CONFIGURATION ---
const char* ap_ssid = "LaserHarp-AP";

// --- GLOBAL CONSTANTS ---
const int NUM_SENSORS = 5;
const int WAV_HEADER_SIZE = 44;
const size_t MIXER_BUFFER_SAMPLES = 128; 

// --- THRESHOLD & DEBOUNCING ---
int stringThresholds[5] = {3200, 3200, 3200, 3200, 3200}; // Individual thresholds
const int DEBOUNCE_DELAY = 20;
bool previousStates[NUM_SENSORS];
bool pendingStates[NUM_SENSORS];
unsigned long lastChangeTime[NUM_SENSORS];

// --- SYSTEM STATE CONTROL ---
enum SystemState {
    SYSTEM_OFF,        // State 1: All off (LEFT: HIGH, RIGHT: LOW)
    FOUNTAIN_MODE,     // State 2: Fountain mode (LEFT: LOW, RIGHT: LOW) - WiFi control available
    HARP_MODE          // State 3: Full harp mode (LEFT: LOW, RIGHT: HIGH)
};

SystemState currentSystemState = SYSTEM_OFF;
SystemState previousSystemState = SYSTEM_OFF;

// --- WEB CONTROL VARIABLES (only active in fountain mode) ---
bool webPumpState = false;
bool webLaserState = false;
int webGain = 0;

// --- AUDIO FILE CONFIGURATION ---
const char* noteFiles[NUM_SENSORS] = {
    "/HNotes/C.wav", "/HNotes/D.wav", "/HNotes/E.wav",
    "/HNotes/G.wav", "/HNotes/A.wav"
};

const char* noteNames[5] = {"1", "2", "3", "4", "5"};

// --- AUDIO DATA STRUCTURES ---
struct WavHeader {
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t num_channels;
};

struct AudioNote {
    uint8_t* buffer = NULL;
    size_t size = 0;
    WavHeader header;
    volatile bool isPlaying = false;
    volatile size_t current_position = 0;
};

volatile AudioNote notes[NUM_SENSORS];

// --- RTOS & TASK MANAGEMENT ---
TaskHandle_t audioMixerTaskHandle = NULL;
TaskHandle_t webServerTaskHandle = NULL;
bool audioSystemInitialized = false;
bool sdCardInitialized = false;
bool wifiInitialized = false;

// --- HARDWARE OBJECTS ---
Adafruit_TPA2016 audioamp = Adafruit_TPA2016();

// --- FUNCTION PROTOTYPES ---
bool loadNoteToPsram(const char* path, AudioNote& note);
void audioMixerTask(void *pvParameters);
void webServerTask(void *pvParameters);
bool initializeI2S(const WavHeader& header);
SystemState readDialState();
void handleSystemStateChange();
bool initializeAudioSystem();
void shutdownAudioSystem();
bool initializeSDCard();
void initializeWiFi();
void shutdownWiFi();

// =================================================================
// ---                           SETUP                           ---
// =================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n--- Integrated Laser Harp System Initializing ---");

    // --- DIAL CONTROL INITIALIZATION ---
    pinMode(DIAL_LEFT_PIN, INPUT_PULLUP);
    pinMode(DIAL_RIGHT_PIN, INPUT_PULLUP);
    pinMode(LASER_CONTROL_PIN, OUTPUT);
    pinMode(PUMP_PIN, OUTPUT);
    
    // Start with everything off
    digitalWrite(LASER_CONTROL_PIN, LOW);
    digitalWrite(PUMP_PIN, LOW);
    Serial.println("Hardware pins initialized - everything OFF");

    // --- ADC INITIALIZATION ---
    Serial.println("Configuring ADC channels...");
    adc1_config_width(ADC_WIDTH_BIT_12);
    for (int i = 0; i < NUM_SENSORS; i++) {
        adc1_config_channel_atten(ADC_CHANNELS[i], ADC_ATTEN_DB_11);
    }
    Serial.println("ADC OK.");

    // --- PSRAM INITIALIZATION ---
    if (!psramInit()) {
        Serial.println("FATAL: PSRAM failed!"); 
        while (true);
    }
    Serial.println("PSRAM OK.");

    // Initialize sensor states
    for (int i = 0; i < NUM_SENSORS; i++) {
        previousStates[i] = false; 
        pendingStates[i] = false; 
        lastChangeTime[i] = 0;
    }

    // --- SD CARD AND AUDIO LOADING ---
    Serial.println("Loading audio files from SD Card into PSRAM...");
    if (!initializeSDCard()) {
        Serial.println("WARNING: SD Card initialization failed!"); 
        Serial.println("System will continue - audio disabled");
    } else {
        // Load all WAV files into PSRAM at startup
        Serial.println("Loading WAV files into PSRAM...");
        for (int i = 0; i < NUM_SENSORS; i++) {
            if (!loadNoteToPsram(noteFiles[i], (AudioNote&)notes[i])) {
                Serial.printf("WARNING: Failed to load %s\n", noteFiles[i]);
            }
        }
        Serial.println("Audio files loaded into PSRAM successfully.");
    }

    // --- INITIALIZE WIFI ACCESS POINT ---
    Serial.println("Starting WiFi Access Point...");
    initializeWiFi();

    // --- READ INITIAL DIAL STATE AND CONFIGURE SYSTEM ---
    currentSystemState = readDialState();
    previousSystemState = currentSystemState;
    handleSystemStateChange(); // Configure initial state
    
    Serial.println("\n--- System Ready! ---");
    Serial.printf("Current mode: ");
    switch(currentSystemState) {
        case SYSTEM_OFF: Serial.println("OFF - All controls disabled"); break;
        case FOUNTAIN_MODE: Serial.println("FOUNTAIN - Web controls available"); break;
        case HARP_MODE: Serial.println("HARP - Full system active, web controls disabled"); break;
    }
    
    Serial.println("Mode descriptions:");
    Serial.println("- OFF: Everything disabled, web interface shows status only");
    Serial.println("- FOUNTAIN: Pump ON, DAC OFF, Lasers controllable via web");
    Serial.println("- HARP: Pump ON, DAC ON, Lasers ON, web controls disabled");
}

// =================================================================
// ---                         MAIN LOOP                         ---
// =================================================================
void loop() {
    // Read current dial state
    currentSystemState = readDialState();
    
    // Handle state changes
    if (currentSystemState != previousSystemState) {
        handleSystemStateChange();
        previousSystemState = currentSystemState;
    }

    // Only process laser sensors if in HARP_MODE
    if (currentSystemState == HARP_MODE && audioSystemInitialized) {
        for (int i = 0; i < NUM_SENSORS; i++) {
            int photodiodeValue = adc1_get_raw(ADC_CHANNELS[i]);
            bool laserBroken = photodiodeValue < stringThresholds[i];

            if (laserBroken != pendingStates[i]) {
                lastChangeTime[i] = millis();
                pendingStates[i] = laserBroken;
            }

            if ((millis() - lastChangeTime[i]) > DEBOUNCE_DELAY) {
                if (pendingStates[i] != previousStates[i]) {
                    previousStates[i] = pendingStates[i];
                    if (pendingStates[i]) {
                        // Re-trigger the note by resetting its state
                        notes[i].current_position = WAV_HEADER_SIZE;
                        notes[i].isPlaying = true;
                    }
                }
            }
        }
    }
    
    delay(10); // Small delay to prevent excessive polling
}

// =================================================================
// ---                    DIAL CONTROL FUNCTIONS                 ---
// =================================================================
SystemState readDialState() {
    int stateLeft = digitalRead(DIAL_LEFT_PIN);
    int stateRight = digitalRead(DIAL_RIGHT_PIN);

    if (stateLeft == HIGH && stateRight == LOW) {
        return SYSTEM_OFF;
    } else if (stateLeft == LOW && stateRight == LOW) {
        return FOUNTAIN_MODE;
    } else if (stateLeft == LOW && stateRight == HIGH) {
        return HARP_MODE;
    }
    
    // Default to off for any undefined state
    return SYSTEM_OFF;
}

void handleSystemStateChange() {
    Serial.printf("State change: %d -> %d\n", previousSystemState, currentSystemState);
    
    switch (currentSystemState) {
        case SYSTEM_OFF:
            Serial.println("Switching to: SYSTEM OFF");
            // Turn everything OFF
            digitalWrite(PUMP_PIN, LOW);
            digitalWrite(LASER_CONTROL_PIN, LOW);
            
            // Shutdown audio system (DAC OFF)
            if (audioSystemInitialized) {
                shutdownAudioSystem();
            }
            
            // Reset web control states
            webPumpState = false;
            webLaserState = false;
            
            Serial.println("- Pump: OFF");
            Serial.println("- Lasers: OFF");
            Serial.println("- DAC: OFF");
            Serial.println("- Web controls: DISABLED (display only)");
            break;
            
        case FOUNTAIN_MODE:
            Serial.println("Switching to: FOUNTAIN MODE");
            // Turn pump ON, keep lasers controllable via web
            digitalWrite(PUMP_PIN, HIGH);
            webPumpState = true; // Update web state to reflect reality
            
            // Lasers controlled by web interface
            digitalWrite(LASER_CONTROL_PIN, webLaserState ? HIGH : LOW);
            
            // Keep audio system OFF (DAC OFF)
            if (audioSystemInitialized) {
                shutdownAudioSystem();
            }
            
            Serial.println("- Pump: ON (controllable via web)");
            Serial.println("- Lasers: Controllable via web");
            Serial.println("- DAC: OFF");
            Serial.println("- Web controls: ENABLED");
            break;
            
        case HARP_MODE:
            Serial.println("Switching to: HARP MODE");
            // Force everything ON
            digitalWrite(PUMP_PIN, HIGH);
            digitalWrite(LASER_CONTROL_PIN, HIGH);
            
            // Update web states to reflect hardware reality
            webPumpState = true;
            webLaserState = true;
            
            // Initialize audio system (DAC ON)
            if (!audioSystemInitialized && sdCardInitialized) {
                delay(500); // Give system time to stabilize
                if (initializeAudioSystem()) {
                    Serial.println("Audio system (DAC) initialized successfully");
                } else {
                    Serial.println("Failed to initialize audio system");
                }
            } else if (!sdCardInitialized) {
                Serial.println("Audio disabled - no SD card available");
            }
            
            Serial.println("- Pump: ON (forced)");
            Serial.println("- Lasers: ON (forced)");
            Serial.println("- DAC: ON");
            Serial.println("- Web controls: DISABLED (display only)");
            break;
    }
}

// =================================================================
// ---                      WIFI FUNCTIONS                       ---
// =================================================================
void initializeWiFi() {
    Serial.println("Initializing WiFi Access Point...");
    
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid);
    
    IPAddress IP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(IP);
    Serial.printf("Connect to: %s (No password)\n", ap_ssid);
    Serial.printf("Then open: http://%s\n", IP.toString().c_str());
    
    // Create Web Server Task on Core 1
    xTaskCreatePinnedToCore(
        webServerTask, "WebServer", 16384, NULL, 1, &webServerTaskHandle, 1
    );
    
    wifiInitialized = true;
    Serial.println("WiFi Access Point ready!");
}

void shutdownWiFi() {
    if (wifiInitialized) {
        Serial.println("Shutting down WiFi...");
        
        // Stop web server task
        if (webServerTaskHandle != NULL) {
            vTaskDelete(webServerTaskHandle);
            webServerTaskHandle = NULL;
        }
        
        WiFi.softAPdisconnect(true);
        WiFi.mode(WIFI_OFF);
        wifiInitialized = false;
        Serial.println("WiFi shutdown complete.");
    }
}

// =================================================================
// ---                      WEB SERVER TASK                      ---
// =================================================================
void webServerTask(void *pvParameters) {
    WiFiServer server(80);
    String header;

    server.begin();

    for(;;) {
        // Check if we should still be running
        if (!wifiInitialized) {
            Serial.println("WiFi task stopping...");
            vTaskDelete(NULL);
        }

        WiFiClient client = server.accept();

        if (client) {
            Serial.println("New Client Connected");
            String currentLine = "";
            // Read current system mode
            SystemState currentMode = readDialState();
            bool controlsEnabled = (currentMode == FOUNTAIN_MODE);
            
            while (client.connected()) {
                if (client.available()) {
                    char c = client.read();
                    header += c;
                    if (c == '\n') {
                        if (currentLine.length() == 0) {
                            
                            // Handle the favicon request gracefully
                            if (header.indexOf("GET /favicon.ico") >= 0) {
                                client.println("HTTP/1.1 204 No Content");
                                client.println();
                                break;
                            }

                            // Handle API endpoint for live data
                            if (header.indexOf("GET /api/status") >= 0) {
                                client.println("HTTP/1.1 200 OK");
                                client.println("Content-type:application/json");
                                client.println("Connection: close");
                                client.println();
                                
                                // Send JSON response with current status
                                SystemState apiMode = readDialState();
                                client.print("{");
                                client.print("\"pump\":" + String(webPumpState ? "true" : "false") + ",");
                                client.print("\"laser\":" + String(webLaserState ? "true" : "false") + ",");
                                client.print("\"gain\":" + String(webGain) + ",");
                                client.print("\"controlsEnabled\":" + String(controlsEnabled ? "true" : "false") + ",");
                                client.print("\"mode\":\"");
                                if (apiMode == SYSTEM_OFF) client.print("SYSTEM OFF");
                                else if (apiMode == FOUNTAIN_MODE) client.print("FOUNTAIN MODE");
                                else if (apiMode == HARP_MODE) client.print("HARP MODE");
                                client.print("\",");
                                client.print("\"sensors\":[");
                                for (int i = 0; i < 5; i++) {
                                  client.print(adc1_get_raw(ADC_CHANNELS[i]));
                                  if (i < 4) client.print(",");
                                }
                                client.print("],");
                                client.print("\"thresholds\":[");
                                for (int i = 0; i < 5; i++) {
                                  client.print(stringThresholds[i]);
                                  if (i < 4) client.print(",");
                                }
                                client.print("]}");
                                client.println();
                                break;
                            }

                            // Handle control commands (ONLY process in fountain mode)
                            if (controlsEnabled) {
                                if (header.indexOf("GET /pump_on") >= 0) {
                                    webPumpState = true;
                                    digitalWrite(PUMP_PIN, HIGH);
                                    Serial.println("Web: Pump ON");
                                } else if (header.indexOf("GET /pump_off") >= 0) {
                                    webPumpState = false;
                                    digitalWrite(PUMP_PIN, LOW);
                                    Serial.println("Web: Pump OFF");
                                } else if (header.indexOf("GET /laser_on") >= 0) {
                                    webLaserState = true;
                                    digitalWrite(LASER_CONTROL_PIN, HIGH);
                                    Serial.println("Web: Laser ON");
                                } else if (header.indexOf("GET /laser_off") >= 0) {
                                    webLaserState = false;
                                    digitalWrite(LASER_CONTROL_PIN, LOW);
                                    Serial.println("Web: Laser OFF");
                                }

                                // Handle slider updates
                                if (header.indexOf("GET /setgain?value=") >= 0) {
                                    int valueStart = header.indexOf("value=") + 6;
                                    int valueEnd = header.indexOf(" ", valueStart);
                                    if (valueEnd == -1) valueEnd = header.indexOf("&", valueStart);
                                    if (valueEnd == -1) valueEnd = header.length();
                                    
                                    int newGain = header.substring(valueStart, valueEnd).toInt();
                                    if (newGain >= -28 && newGain <= 30) {
                                        webGain = newGain;
                                        // Note: gain setting saved but DAC is off in fountain mode
                                        Serial.printf("Web: Gain set to %d dB (will apply when DAC active)\n", webGain);
                                    }
                                }
                                
                                // Handle threshold updates
                                for (int i = 0; i < 5; i++) {
                                    String thresholdCmd = "/setthresh" + String(i) + "?value=";
                                    if (header.indexOf("GET " + thresholdCmd) >= 0) {
                                        int valueStart = header.indexOf("value=") + 6;
                                        int valueEnd = header.indexOf(" ", valueStart);
                                        if (valueEnd == -1) valueEnd = header.indexOf("&", valueStart);
                                        if (valueEnd == -1) valueEnd = header.length();
                                        
                                        int newThreshold = header.substring(valueStart, valueEnd).toInt();
                                        if (newThreshold >= 0 && newThreshold <= 4095) {
                                            stringThresholds[i] = newThreshold;
                                            Serial.printf("Web: String %s threshold set to %d\n", noteNames[i], newThreshold);
                                        }
                                    }
                                }
                            }

                            // Send HTML page
                            client.println("HTTP/1.1 200 OK");
                            client.println("Content-type:text/html");
                            client.println("Connection: close");
                            client.println();

                            // HTML page with enhanced styling for disabled state
                            client.println("<!DOCTYPE html><html><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">");
                            client.println("<style>body{font-family:Arial;text-align:center;margin:20px;background-color:#f5f5f5;}");
                            client.println(".container{max-width:800px;margin:0 auto;background:white;padding:20px;border-radius:8px;box-shadow:0 2px 10px rgba(0,0,0,0.1);}");
                            client.println(".button{background-color:#008CBA;border:none;color:white;padding:12px 24px;margin:5px;cursor:pointer;border-radius:4px;font-size:16px;}");
                            client.println(".button2{background-color:#555555;}.button3{background-color:#4CAF50;}");
                            client.println(".disabled{background-color:#ccc !important;color:#999 !important;cursor:not-allowed !important;opacity:0.6;}");
                            client.println(".slider{width:200px;} .slider:disabled{opacity:0.5;}");
                            client.println(".status{padding:10px;border-radius:4px;margin:10px 0;}");
                            client.println(".status-off{background-color:#ffebee;color:#c62828;}");
                            client.println(".status-fountain{background-color:#e8f5e8;color:#2e7d32;}");
                            client.println(".status-harp{background-color:#e3f2fd;color:#1565c0;}");
                            client.println("table{border-collapse:collapse;margin:20px auto;}th,td{padding:8px;border:1px solid #ddd;}");
                            client.println("</style></head>");
                            client.println("<body><div class='container'>");
                            client.println("<h1>🎵 Laser Harp Control Panel</h1>");
                            
                            // Mode display with color coding
                            client.println("<h2>Current Mode: <span id='currentMode'>");
                            String statusClass = "";
                            if (currentMode == SYSTEM_OFF) {
                                client.println("SYSTEM OFF");
                                statusClass = "status-off";
                            } else if (currentMode == FOUNTAIN_MODE) {
                                client.println("FOUNTAIN MODE");
                                statusClass = "status-fountain";
                            } else if (currentMode == HARP_MODE) {
                                client.println("HARP MODE");
                                statusClass = "status-harp";
                            }
                            client.println("</span></h2>");

                            // Status description
                            client.println("<div class='status " + statusClass + "'>");
                            if (currentMode == SYSTEM_OFF) {
                                client.println("🔴 All systems disabled - Status display only<br>");
                                client.println("Pump: OFF | Lasers: OFF | DAC: OFF");
                            } else if (currentMode == FOUNTAIN_MODE) {
                                client.println("🟢 Fountain mode active - Full web control available<br>");
                                client.println("Pump: Controllable | Lasers: Controllable | DAC: OFF");
                            } else if (currentMode == HARP_MODE) {
                                client.println("🔵 Full harp system active - Hardware controlled<br>");
                                client.println("Pump: ON | Lasers: ON | DAC: ON | Web controls disabled");
                            }
                            client.println("</div>");

                            // System Controls
                            client.println("<h3>💧 System Controls</h3>");
                            
                            // Pump control
                            if (webPumpState) {
                                if (controlsEnabled) {
                                    client.println("<a href=\"/pump_off\"><button class=\"button button3\">PUMP ON - CLICK TO TURN OFF</button></a>");
                                } else {
                                    client.println("<button class=\"button button3 disabled\">PUMP ON (Hardware Controlled)</button>");
                                }
                            } else {
                                if (controlsEnabled) {
                                    client.println("<a href=\"/pump_on\"><button class=\"button button2\">PUMP OFF - CLICK TO TURN ON</button></a>");
                                } else {
                                    client.println("<button class=\"button button2 disabled\">PUMP OFF (Hardware Controlled)</button>");
                                }
                            }
                            
                            client.println("<br>");
                            
                            // Laser control
                            if (webLaserState) {
                                if (controlsEnabled) {
                                    client.println("<a href=\"/laser_off\"><button class=\"button button3\">LASER ON - CLICK TO TURN OFF</button></a>");
                                } else {
                                    client.println("<button class=\"button button3 disabled\">LASER ON (Hardware Controlled)</button>");
                                }
                            } else {
                                if (controlsEnabled) {
                                    client.println("<a href=\"/laser_on\"><button class=\"button button2\">LASER OFF - CLICK TO TURN ON</button></a>");
                                } else {
                                    client.println("<button class=\"button button2 disabled\">LASER OFF (Hardware Controlled)</button>");
                                }
                            }

                            // Gain Control
                            client.println("<h3>🔊 Audio Gain: <span id='gainValue'>" + String(webGain) + "</span> dB</h3>");
                            if (controlsEnabled) {
                                client.println("<input type='range' class='slider' id='gainSlider' min='-28' max='30' value='" + String(webGain) + "' onchange='setGain(this.value)'>");
                                client.println("<br><small>Note: DAC is OFF in fountain mode - gain will apply when switched to harp mode</small>");
                            } else {
                                client.println("<input type='range' class='slider' disabled min='-28' max='30' value='" + String(webGain) + "'>");
                                if (currentMode == SYSTEM_OFF) {
                                    client.println("<br><small>Controls disabled in OFF mode</small>");
                                } else {
                                    client.println("<br><small>Controls disabled in HARP mode - hardware controlled</small>");
                                }
                            }

                            // String Thresholds
                            client.println("<h3>🎯 Laser Detection Thresholds</h3>");
                            if (!controlsEnabled) {
                                if (currentMode == SYSTEM_OFF) {
                                    client.println("<p><small>All controls disabled in OFF mode</small></p>");
                                } else {
                                    client.println("<p><small>Threshold adjustment disabled in HARP mode</small></p>");
                                }
                            }
                            client.println("<table>");
                            client.println("<tr><th>String</th><th>Threshold</th><th>Current Sensor</th><th>Status</th></tr>");
                            
                            for (int i = 0; i < 5; i++) {
                                client.println("<tr><td>" + String(noteNames[i]) + "</td>");
                                client.println("<td>");
                                if (controlsEnabled) {
                                    client.println("<input type='range' min='0' max='4095' value='" + String(stringThresholds[i]) + "' onchange='setThreshold(" + String(i) + ", this.value)'>");
                                } else {
                                    client.println("<input type='range' class='disabled' disabled min='0' max='4095' value='" + String(stringThresholds[i]) + "'>");
                                }
                                client.println("<br><span id='thresh" + String(i) + "'>" + String(stringThresholds[i]) + "</span>");
                                client.println("</td><td><span id='sensor" + String(i) + "'>0</span></td>");
                                client.println("<td><span id='status" + String(i) + "'>-</span></td></tr>");
                            }
                            client.println("</table>");

                            // JavaScript
                            client.println("<script>");
                            client.println("function setGain(value) { document.getElementById('gainValue').textContent = value; fetch('/setgain?value=' + value); }");
                            client.println("function setThreshold(index, value) { document.getElementById('thresh' + index).textContent = value; fetch('/setthresh' + index + '?value=' + value); }");
                            client.println("function updateStatus() {");
                            client.println("  fetch('/api/status').then(r => r.json()).then(data => {");
                            client.println("    document.getElementById('currentMode').textContent = data.mode;");
                            client.println("    for(let i = 0; i < 5; i++) {");
                            client.println("      const sensorVal = data.sensors[i];");
                            client.println("      const threshold = data.thresholds[i];");
                            client.println("      document.getElementById('sensor' + i).textContent = sensorVal;");
                            client.println("      const isTriggered = sensorVal < threshold;");
                            client.println("      document.getElementById('status' + i).textContent = isTriggered ? '🔴 TRIGGERED' : '🟢 CLEAR';");
                            client.println("      document.getElementById('status' + i).style.color = isTriggered ? '#c62828' : '#2e7d32';");
                            client.println("    }");
                            client.println("  }).catch(e => console.log('Update failed:', e));");
                            client.println("}");
                            client.println("setInterval(updateStatus, 500); updateStatus();");
                            client.println("</script></div></body></html>");

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
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

// =================================================================
// ---                      AUDIO FUNCTIONS                      ---
// =================================================================
bool initializeAudioSystem() {
    Serial.println("Initializing audio hardware...");
    
    // Initialize I2C and amplifier
    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    if (!audioamp.begin()) {
        Serial.println("ERROR: TPA2016 amplifier not found.");
        return false;
    }
    audioamp.setGain(webGain);
    Serial.printf("TPA2016 amplifier found! Gain: %d dB.\n", audioamp.getGain());

    // Initialize I2S (audio files are already loaded)
    Serial.println("Initializing I2S audio driver...");
    if (!initializeI2S(((AudioNote&)notes[0]).header)) { 
        Serial.println("ERROR: Failed to initialize I2S driver.");
        return false;
    }
    Serial.println("I2S driver OK.");

    // Create audio mixer task
    if (audioMixerTaskHandle == NULL) {
        Serial.println("Creating background audio mixer task...");
        xTaskCreatePinnedToCore(
            audioMixerTask, "AudioMixer", 8192, NULL, 5, &audioMixerTaskHandle, 1);
    }

    audioSystemInitialized = true;
    Serial.println("Audio system ready!");
    return true;
}

bool initializeSDCard() {
    Serial.println("Initializing SD Card...");
    
    // Initialize SPI
    SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN, SD_CS_PIN);
    delay(200); // Give SPI time to stabilize
    
    // Try multiple times with different settings
    for (int attempt = 0; attempt < 3; attempt++) {
        Serial.printf("SD Card init attempt %d...\n", attempt + 1);
        
        // Try different frequencies
        uint32_t freq = (attempt == 0) ? 1000000 : (attempt == 1) ? 4000000 : 8000000;
        
        if (SD.begin(SD_CS_PIN, SPI, freq)) {
            Serial.printf("SD Card OK (freq: %d Hz)\n", freq);
            sdCardInitialized = true;
            return true;
        }
        
        delay(500);
    }
    
    Serial.println("ERROR: All SD card initialization attempts failed");
    return false;
}

void shutdownAudioSystem() {
    Serial.println("Shutting down audio system...");
    
    // Stop all notes first
    for (int i = 0; i < NUM_SENSORS; i++) {
        notes[i].isPlaying = false;
        notes[i].current_position = 0;
    }
    
    // Stop audio mixer task
    if (audioMixerTaskHandle != NULL) {
        vTaskDelete(audioMixerTaskHandle);
        audioMixerTaskHandle = NULL;
        delay(100); // Give task time to stop
    }
    
    // Stop I2S driver
    if (audioSystemInitialized) {
        i2s_stop(I2S_NUM_0);
        i2s_driver_uninstall(I2S_NUM_0);
    }
    
    audioSystemInitialized = false;
    Serial.println("Audio system shutdown complete.");
}

void audioMixerTask(void *pvParameters) {
    uint16_t i2s_output_buffer[MIXER_BUFFER_SAMPLES * 2];

    while (true) {
        // Only process audio if system is in harp mode
        if (currentSystemState != HARP_MODE) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        
        for (int i = 0; i < MIXER_BUFFER_SAMPLES; i++) {
            int32_t mixed_sample = 0;
            for (int j = 0; j < NUM_SENSORS; j++) {
                if (notes[j].isPlaying) {
                    if (notes[j].current_position < notes[j].size) {
                        uint8_t sample8bit = notes[j].buffer[notes[j].current_position];
                        mixed_sample += (int16_t)(sample8bit - 128);
                        notes[j].current_position++;
                    } else {
                        notes[j].isPlaying = false;
                    }
                }
            }

            if (mixed_sample > 127) mixed_sample = 127;
            if (mixed_sample < -128) mixed_sample = -128;

            uint8_t final_8bit_sample = (uint8_t)(mixed_sample + 128);
            uint16_t i2s_sample = ((uint16_t)final_8bit_sample) << 8;

            i2s_output_buffer[i * 2]     = i2s_sample;
            i2s_output_buffer[i * 2 + 1] = i2s_sample;
        }

        size_t bytes_written = 0;
        i2s_write(I2S_NUM_0, i2s_output_buffer, sizeof(i2s_output_buffer), &bytes_written, portMAX_DELAY);
    }
}

bool initializeI2S(const WavHeader& header) {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
        .sample_rate = 96000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = (i2s_comm_format_t)(I2S_COMM_FORMAT_STAND_MSB),
        .intr_alloc_flags = 0,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true
    };

    if (i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL) != ESP_OK) return false;
    if (i2s_set_pin(I2S_NUM_0, NULL) != ESP_OK) return false;
    if (i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN) != ESP_OK) return false;
    i2s_zero_dma_buffer(I2S_NUM_0);
    
    return true;
}

bool loadNoteToPsram(const char* path, AudioNote& note) {
    File file = SD.open(path, "r");
    if (!file) {
        Serial.printf("    -> ERROR: Failed to open %s\n", path);
        return false;
    }
    
    note.size = file.size();
    if (note.size <= WAV_HEADER_SIZE) {
        Serial.println("    -> ERROR: File is too small.");
        file.close(); 
        return false;
    }

    note.buffer = (uint8_t *)ps_malloc(note.size);
    if (note.buffer == NULL) {
        Serial.println("    -> FATAL: Failed to allocate PSRAM!");
        file.close(); 
        return false;
    }
    
    if(file.read(note.buffer, note.size) != note.size) {
        Serial.println("    -> ERROR: File read failed.");
        free(note.buffer); 
        note.buffer = NULL; 
        file.close(); 
        return false;
    }
    file.close();

    note.header.num_channels = *(uint16_t*)(note.buffer + 22);
    note.header.sample_rate = *(uint32_t*)(note.buffer + 24);
    note.header.bits_per_sample = *(uint16_t*)(note.buffer + 34);

    Serial.printf("    -> Loaded %s (%d Hz, %d-bit, %d chan)\n", 
        path, note.header.sample_rate, note.header.bits_per_sample, note.header.num_channels);

    if (note.header.num_channels != 1 || note.header.bits_per_sample != 8 || note.header.sample_rate != 96000) {
        Serial.println("    -> WARNING: WAV should be 8-bit, Mono, and 96kHz for optimal mixing.");
    }
    return true;
}