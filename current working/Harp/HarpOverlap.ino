// --- INCLUDES ---
#include "driver/i2s.h"
#include "driver/adc.h"
#include "SD.h"
#include "SPI.h"
#include <Wire.h>
#include "Adafruit_TPA2016.h"

// --- HARDWARE & PIN DEFINITIONS ---
#define I2C_SDA_PIN 22
#define I2C_SCL_PIN 26
#define SD_CS_PIN     5
#define SPI_MOSI_PIN 23
#define SPI_MISO_PIN 19
#define SPI_SCK_PIN  18
#define CONSTANT_HIGH_PIN 17

const int PHOTODIODE_PINS[5] = {34, 35, 32, 33, 36};
const adc1_channel_t ADC_CHANNELS[5] = {
    ADC1_CHANNEL_6, // GPIO 34
    ADC1_CHANNEL_7, // GPIO 35
    ADC1_CHANNEL_4, // GPIO 32
    ADC1_CHANNEL_5, // GPIO 33
    ADC1_CHANNEL_0  // GPIO 36
};

// --- GLOBAL CONSTANTS ---
const int NUM_SENSORS = 5;
const int WAV_HEADER_SIZE = 44;
// MODIFIED: Smaller software buffer for lower latency
const size_t MIXER_BUFFER_SAMPLES = 128; 

// --- MANUAL THRESHOLD & DEBOUNCING ---
const int TRIGGER_THRESHOLD = 3000;
// MODIFIED: Reduced debounce delay for faster response
const int DEBOUNCE_DELAY = 20;
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
    volatile size_t current_position = 0;
};

volatile AudioNote notes[NUM_SENSORS];

// --- RTOS & TASK MANAGEMENT ---
TaskHandle_t audioMixerTaskHandle = NULL;

// --- HARDWARE OBJECTS ---
Adafruit_TPA2016 audioamp = Adafruit_TPA2016();

// --- FUNCTION PROTOTYPES ---
bool loadNoteToPsram(const char* path, AudioNote& note);
void audioMixerTask(void *pvParameters);
bool initializeI2S(const WavHeader& header);

// =================================================================
// ---                           SETUP                           ---
// =================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n--- Laser Harp System Initializing (Low Latency) ---");

    // --- ADC INITIALIZATION ---
    Serial.println("Configuring ADC channels...");
    adc1_config_width(ADC_WIDTH_BIT_12);
    for (int i = 0; i < NUM_SENSORS; i++) {
        adc1_config_channel_atten(ADC_CHANNELS[i], ADC_ATTEN_DB_11);
    }
    Serial.println("ADC OK.");

    // --- Set Pin 17 HIGH ---
    pinMode(CONSTANT_HIGH_PIN, OUTPUT);
    digitalWrite(CONSTANT_HIGH_PIN, HIGH);
    Serial.printf("Pin %d has been set HIGH.\n", CONSTANT_HIGH_PIN);

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    if (!audioamp.begin()) {
        Serial.println("FATAL: TPA2016 amplifier not found. Halting.");
        while (1);
    }
    audioamp.setGain(0);
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
        if (!loadNoteToPsram(noteFiles[i], (AudioNote&)notes[i])) {
            Serial.printf("FATAL: Failed to load %s. Halting.\n", noteFiles[i]);
            while(true);
        }
    }
    Serial.println("All WAV files loaded successfully.");

    Serial.println("Initializing I2S audio driver for low latency...");
    if (!initializeI2S(((AudioNote&)notes[0]).header)) { 
        Serial.println("FATAL: Failed to initialize I2S driver. Halting.");
        while(true);
    }
    Serial.println("I2S driver OK.");

    for (int i = 0; i < NUM_SENSORS; i++) {
        previousStates[i] = false; pendingStates[i] = false; lastChangeTime[i] = 0;
    }

    Serial.println("Creating background audio mixer task...");
    xTaskCreatePinnedToCore(
        audioMixerTask, "AudioMixer", 8192, NULL, 5, &audioMixerTaskHandle, 1);

    Serial.println("\n--- System Ready! Monitoring for broken beams... ---");
    Serial.printf("Trigger threshold set to: %d\n", TRIGGER_THRESHOLD);
}

// =================================================================
// ---                         MAIN LOOP                         ---
// =================================================================
void loop() {
    for (int i = 0; i < NUM_SENSORS; i++) {
        int photodiodeValue = adc1_get_raw(ADC_CHANNELS[i]);
        bool laserBroken = photodiodeValue < TRIGGER_THRESHOLD;

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
    // MODIFIED: Removed delay for faster sensor scanning.
}

// =================================================================
// ---                      AUDIO FUNCTIONS                      ---
// =================================================================

/**
 * @brief Main audio mixer task with corrected 8-bit logic for the internal DAC.
 */
void audioMixerTask(void *pvParameters) {
    uint16_t i2s_output_buffer[MIXER_BUFFER_SAMPLES * 2];

    while (true) {
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
        // MODIFIED: Smaller DMA buffers for lower audio pipeline latency
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
        file.close(); return false;
    }

    note.buffer = (uint8_t *)ps_malloc(note.size);
    if (note.buffer == NULL) {
        Serial.println("    -> FATAL: Failed to allocate PSRAM!");
        file.close(); return false;
    }
    
    if(file.read(note.buffer, note.size) != note.size) {
        Serial.println("    -> ERROR: File read failed.");
        free(note.buffer); note.buffer = NULL; file.close(); return false;
    }
    file.close();

    note.header.num_channels = *(uint16_t*)(note.buffer + 22);
    note.header.sample_rate = *(uint32_t*)(note.buffer + 24);
    note.header.bits_per_sample = *(uint16_t*)(note.buffer + 34);

    Serial.printf("    -> Loaded %s (%d Hz, %d-bit, %d chan)\n", 
        path, note.header.sample_rate, note.header.bits_per_sample, note.header.num_channels);

    if (note.header.num_channels != 1 || note.header.bits_per_sample != 8 || note.header.sample_rate != 96000) {
        Serial.println("    -> ERROR: WAV must be 8-bit, Mono, and 96kHz for mixing.");
        free(note.buffer); note.buffer = NULL; return false;
    }
    return true;
}