// --- INCLUDES ---
#include "driver/i2s.h"
#include "SD.h"
#include "SPI.h"
#include <Wire.h>
#include "Adafruit_TPA2016.h"
#include "freertos/queue.h"

// --- HARDWARE & PIN DEFINITIONS ---
#define I2C_SDA_PIN 22
#define I2C_SCL_PIN 26
#define SD_CS_PIN    5
#define SPI_MOSI_PIN 23
#define SPI_MISO_PIN 19
#define SPI_SCK_PIN  18
const int PHOTODIODE_PINS[5] = {34, 35, 32, 33, 27};
const int LASER_POWER_PIN = 13;

// --- GLOBAL CONSTANTS ---
const int NUM_SENSORS = 5;
const int WAV_HEADER_SIZE = 44;
const size_t I2S_WRITE_BUFFER_SIZE = 4096; // Play audio in 4KB chunks

// --- SENSOR CALIBRATION & DEBOUNCING ---
const int CALIBRATION_SAMPLES = 50;
const int DEBOUNCE_DELAY = 50;
int thresholds[NUM_SENSORS];
bool previousStates[NUM_SENSORS];
bool pendingStates[NUM_SENSORS];
unsigned long lastChangeTime[NUM_SENSORS];

// --- AUDIO FILE CONFIGURATION ---
const char* noteFiles[NUM_SENSORS] = {
    "/HNotes/C.wav", "/HNotes/D.wav", "/HNotes/E.wav",
    "/HNotes/G.wav", "/HNotes/A.wav"
};

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
};

AudioNote notes[NUM_SENSORS];

// --- RTOS & TASK MANAGEMENT ---
QueueHandle_t noteQueue;
TaskHandle_t audioTaskHandle = NULL;

// --- HARDWARE OBJECTS ---
Adafruit_TPA2016 audioamp = Adafruit_TPA2016();

// --- FUNCTION PROTOTYPES ---
void calibrateSensors();
bool loadNoteToPsram(const char* path, AudioNote& note);
void audioPlayerTask(void *pvParameters);
void playNote(int noteIndex);
bool initializeI2S(const WavHeader& header);

// =================================================================
// ---                           SETUP                           ---
// =================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n--- Laser Harp System Initializing (Chunked Streaming) ---");

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    if (!audioamp.begin()) {
        Serial.println("FATAL: TPA2016 amplifier not found. Halting.");
        while (1);
    }
    audioamp.setGain(-28);
    Serial.printf("TPA2016 amplifier found! Gain: %d dB.\n", audioamp.getGain());

    if (!psramInit()) {
        Serial.println("FATAL: PSRAM failed!"); while (true);
    }
    Serial.println("PSRAM OK.");
    SPI.begin(SPI_SCK_PIN, SPI_MISO_PIN, SPI_MOSI_PIN);
    if (!SD.begin(SD_CS_PIN)) {
        Serial.println("FATAL: SD Card mount failed!"); while (true);
    }
    Serial.println("SD Card OK.");

    Serial.println("Loading WAV files...");
    for (int i = 0; i < NUM_SENSORS; i++) {
        if (!loadNoteToPsram(noteFiles[i], notes[i])) {
            Serial.printf("FATAL: Failed to load %s. Halting.\n", noteFiles[i]);
            while(true);
        }
    }
    Serial.println("All WAV files loaded successfully.");

    Serial.println("Initializing I2S audio driver...");
    if (!initializeI2S(notes[0].header)) {
        Serial.println("FATAL: Failed to initialize I2S driver. Halting.");
        while(true);
    }
    Serial.println("I2S driver OK.");

    for (int i = 0; i < NUM_SENSORS; i++) {
        previousStates[i] = false; pendingStates[i] = false; lastChangeTime[i] = 0;
    }
    pinMode(LASER_POWER_PIN, OUTPUT);
    digitalWrite(LASER_POWER_PIN, LOW);
    calibrateSensors();

    noteQueue = xQueueCreate(5, sizeof(int));
    if(noteQueue == NULL){
        Serial.println("FATAL: Could not create note queue. Halting.");
        while(true);
    }

    Serial.println("Creating background audio playback task...");
    xTaskCreatePinnedToCore(
        audioPlayerTask, "AudioPlayer", 16384, NULL, 5, &audioTaskHandle, 1);

    digitalWrite(LASER_POWER_PIN, HIGH);
    Serial.println("\n--- System Ready! Monitoring for broken beams... ---");
}

// =================================================================
// ---                         MAIN LOOP                         ---
// =================================================================
void loop() {
    for (int i = 0; i < NUM_SENSORS; i++) {
        int photodiodeValue = analogRead(PHOTODIODE_PINS[i]);
        bool laserBroken = photodiodeValue < thresholds[i];

        if (laserBroken != pendingStates[i]) {
            lastChangeTime[i] = millis();
            pendingStates[i] = laserBroken;
        }

        if ((millis() - lastChangeTime[i]) > DEBOUNCE_DELAY) {
            if (pendingStates[i] != previousStates[i]) {
                previousStates[i] = pendingStates[i];
                if (pendingStates[i]) {
                    Serial.printf("Sensor %d: ---> LASER BROKEN <---\n", i + 1);
                    // Send the note index to the queue. The audio task will handle
                    // the logic of whether to play it or not.
                    xQueueSend(noteQueue, &i, (TickType_t)0);
                } else {
                    Serial.printf("Sensor %d: Laser Reconnected\n", i + 1);
                }
            }
        }
    }
    delay(5);
}

// =================================================================
// ---                     AUDIO FUNCTIONS                     ---
// =================================================================

void audioPlayerTask(void *pvParameters) {
    int noteIndexToPlay;
    while (true) {
        if (xQueueReceive(noteQueue, &noteIndexToPlay, portMAX_DELAY) == pdPASS) {
            // Only play the note if it's not already playing.
            // This prevents re-triggering if the queue receives multiple requests.
            if (!notes[noteIndexToPlay].isPlaying) {
                playNote(noteIndexToPlay);
            }
        }
    }
}

/**
 * @brief Plays an audio note by streaming it in chunks.
 * This is much more stable than writing the entire file at once.
 */
void playNote(int noteIndex) {
    if (noteIndex < 0 || noteIndex >= NUM_SENSORS) return;

    notes[noteIndex].isPlaying = true;
    Serial.printf("Audio Task: Playing note %d\n", noteIndex + 1);

    i2s_set_sample_rates(I2S_NUM_0, notes[noteIndex].header.sample_rate);

    uint8_t *audio_data_start = notes[noteIndex].buffer + WAV_HEADER_SIZE;
    size_t audio_data_size = notes[noteIndex].size - WAV_HEADER_SIZE;
    size_t bytes_sent = 0;
    
    // --- CHUNKED STREAMING LOOP ---
    while (bytes_sent < audio_data_size) {
        size_t bytes_to_send = audio_data_size - bytes_sent;
        // Don't send more than our defined buffer size at a time
        if (bytes_to_send > I2S_WRITE_BUFFER_SIZE) {
            bytes_to_send = I2S_WRITE_BUFFER_SIZE;
        }

        size_t bytes_written = 0;
        // Write the next chunk of data to the I2S driver
        i2s_write(I2S_NUM_0, audio_data_start + bytes_sent, bytes_to_send, &bytes_written, portMAX_DELAY);
        
        if (bytes_written > 0) {
            bytes_sent += bytes_written;
        } else {
            // Write error, break out of loop
            Serial.println("Error writing to I2S, stopping playback.");
            break;
        }
    }
    
    // Wait for the DMA buffer to finish playing everything, then clear it for silence.
    i2s_zero_dma_buffer(I2S_NUM_0);

    Serial.printf("Audio Task: Finished note %d\n", noteIndex + 1);
    notes[noteIndex].isPlaying = false;
}


bool initializeI2S(const WavHeader& header) {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
        .sample_rate = header.sample_rate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_MSB,
        .intr_alloc_flags = 0,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = false,
        .tx_desc_auto_clear = true
    };

    if (i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL) != ESP_OK) return false;
    if (i2s_set_pin(I2S_NUM_0, NULL) != ESP_OK) return false;
    if (i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN) != ESP_OK) return false;
    
    return true;
}

bool loadNoteToPsram(const char* path, AudioNote& note) {
    File file = SD.open(path, "r");
    if (!file) {
        Serial.printf("  -> ERROR: Failed to open %s\n", path);
        return false;
    }
    
    note.size = file.size();
    if (note.size <= WAV_HEADER_SIZE) {
        Serial.println("  -> ERROR: File is too small.");
        file.close(); return false;
    }

    note.buffer = (uint8_t *)ps_malloc(note.size);
    if (note.buffer == NULL) {
        Serial.println("  -> FATAL: Failed to allocate PSRAM!");
        file.close(); return false;
    }
    
    if(file.read(note.buffer, note.size) != note.size) {
        Serial.println("  -> ERROR: File read failed.");
        free(note.buffer); note.buffer = NULL; file.close(); return false;
    }
    file.close();

    note.header.num_channels = *(uint16_t*)(note.buffer + 22);
    note.header.sample_rate = *(uint32_t*)(note.buffer + 24);
    note.header.bits_per_sample = *(uint16_t*)(note.buffer + 34);

    Serial.printf("  -> Loaded %s (%d Hz, %d-bit, %d chan)\n", 
        path, note.header.sample_rate, note.header.bits_per_sample, note.header.num_channels);

    if (note.header.num_channels != 1 || note.header.bits_per_sample != 16) {
        Serial.println("  -> ERROR: WAV must be 16-bit, Mono.");
        free(note.buffer); note.buffer = NULL; return false;
    }
    return true;
}

void calibrateSensors() {
    long laserOnReadings[NUM_SENSORS] = {0};
    long ambientReadings[NUM_SENSORS] = {0};
    Serial.println("\n--- Starting Sensor Calibration ---");

    digitalWrite(LASER_POWER_PIN, HIGH); delay(500);
    for (int i = 0; i < NUM_SENSORS; i++) {
        long sum = 0;
        for (int j = 0; j < CALIBRATION_SAMPLES; j++) { sum += analogRead(PHOTODIODE_PINS[i]); delay(2); }
        laserOnReadings[i] = sum / CALIBRATION_SAMPLES;
    }
    
    digitalWrite(LASER_POWER_PIN, LOW); delay(500);
    for (int i = 0; i < NUM_SENSORS; i++) {
        long sum = 0;
        for (int j = 0; j < CALIBRATION_SAMPLES; j++) { sum += analogRead(PHOTODIODE_PINS[i]); delay(2); }
        ambientReadings[i] = sum / CALIBRATION_SAMPLES;
    }

    Serial.println("\n--- Calibration Results ---");
    for (int i = 0; i < NUM_SENSORS; i++) {
        long range = laserOnReadings[i] - ambientReadings[i];
        if (range < 100) {
            Serial.printf("WARNING: Sensor %d has low signal range (%ld). Check alignment.\n", i+1, range);
        }
        thresholds[i] = ambientReadings[i] + (range * 0.75);
        Serial.printf("Sensor %d: ON=%ld, AMBIENT=%ld -> Threshold=%d\n",
            i + 1, laserOnReadings[i], ambientReadings[i], thresholds[i]);
    }
    Serial.println("--- Calibration Complete ---");
}
