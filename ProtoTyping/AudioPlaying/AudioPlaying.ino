#include "driver/i2s.h"
#include "SD.h"
#include "SPI.h"
#include <Wire.h>
#include "Adafruit_TPA2016.h"

// --- SD Card Pin Definitions ---
#define SD_CS       5
#define SPI_MOSI    23
#define SPI_MISO    19
#define SPI_SCK     18

// --- Amplifier Object ---
Adafruit_TPA2016 audioamp = Adafruit_TPA2016();

// Pointer for our PSRAM buffer
uint8_t *wavBuffer = NULL;
size_t bufferSize = 0;
const int WAV_HEADER_SIZE = 44; // Standard WAV header size

// --- Struct to hold WAV file properties ---
struct WavHeader {
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint16_t num_channels;
};

// --- Function Prototypes ---
void getAndPlayFile();
bool loadFileToPsram(const char* path);
bool configureI2sFromWav(WavHeader* header);
void playWavFile(const WavHeader& header);

void setup() {
    Serial.begin(115200);

    // --- Initialize the I2C Bus on Custom Pins FIRST ---
    Wire.begin(22, 26); // Use GPIO 22 for SDA, GPIO 26 for SCL

    // --- Initialize the Amplifier ---
    // Now call begin() without arguments. It will use the bus we just set up.
    if (!audioamp.begin()) { 
        Serial.println("Could not find TPA2016 amplifier, check wiring!");
        while (1);
    }
    Serial.println("TPA2016 amplifier found!");

    // --- Set the Gain ---
    // Gain can be from -28dB to +30dB. A good starting point is 24dB.
    audioamp.setGain(-10);
    Serial.printf("Amplifier gain set to: %d dB\n", audioamp.getGain());

    // --- Check for PSRAM ---
    if (!psramInit()) {
        Serial.println("PSRAM not found or failed to initialize!");
        while (true);
    }
    
    // --- Initialize SD Card ---
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
    if (!SD.begin(SD_CS)) {
        Serial.println("SD Card mount failed!");
        while (true);
    }
    
    Serial.println("SD Card initialized.");
    Serial.println("\nReady to play a file.");
    Serial.println("Enter the folder and file name (e.g., /sounds/track1.wav)");
}

/**
 * @brief Loads a specified .wav file into the PSRAM buffer.
 * It will first free any existing buffer before loading the new file.
 * @param path The full path to the .wav file on the SD card.
 * @return true if the file was loaded successfully, false otherwise.
 */
bool loadFileToPsram(const char* path) {
    // If a buffer is already allocated, free it first.
    if (wavBuffer != NULL) {
        free(wavBuffer);
        wavBuffer = NULL;
        bufferSize = 0;
        Serial.println("Cleared previous audio from PSRAM.");
    }

    File file = SD.open(path, "r");
    if (!file) {
        Serial.printf("Failed to open %s\n", path);
        return false;
    }
    
    bufferSize = file.size();
    if (bufferSize <= WAV_HEADER_SIZE) {
        Serial.println("File is empty or too small to be a valid WAV.");
        file.close();
        return false;
    }

    // Allocate memory from PSRAM
    wavBuffer = (uint8_t *)ps_malloc(bufferSize);
    if (wavBuffer == NULL) {
        Serial.println("Failed to allocate PSRAM!");
        file.close();
        while(true); // Critical memory failure
    }
    
    // Read the file into the buffer
    size_t bytesRead = file.read(wavBuffer, bufferSize);
    file.close();

    if(bytesRead == bufferSize) {
        Serial.printf("Successfully loaded %s into PSRAM (%d bytes).\n", path, bufferSize);
        return true;
    } else {
        Serial.printf("Error reading file. Expected %d bytes, got %d.\n", bufferSize, bytesRead);
        free(wavBuffer);
        wavBuffer = NULL;
        bufferSize = 0;
        return false;
    }
}

/**
 * @brief Parses the WAV header and configures the I2S driver.
 * @param header Pointer to a WavHeader struct to store the parsed info.
 * @return true on success, false on failure.
 */
bool configureI2sFromWav(WavHeader* header) {
    // --- Parse Header ---
    if (wavBuffer[0] != 'R' || wavBuffer[1] != 'I' || wavBuffer[2] != 'F' || wavBuffer[3] != 'F' ||
        wavBuffer[8] != 'W' || wavBuffer[9] != 'A' || wavBuffer[10] != 'V' || wavBuffer[11] != 'E') {
        Serial.println("Invalid WAV file header");
        return false;
    }
    header->num_channels = *(uint16_t*)(wavBuffer + 22);
    header->sample_rate = *(uint32_t*)(wavBuffer + 24);
    header->bits_per_sample = *(uint16_t*)(wavBuffer + 34);

    Serial.printf("WAV Info: %d Channel(s), %d Hz, %d Bits/Sample\n", 
                  header->num_channels, header->sample_rate, header->bits_per_sample);

    if (header->num_channels != 1) {
        Serial.println("Error: This player only supports mono WAV files.");
        return false;
    }
    if (header->bits_per_sample != 8 && header->bits_per_sample != 16) {
        Serial.printf("Error: Unsupported bit depth: %d. Only 8 or 16 bit is supported.\n", header->bits_per_sample);
        return false;
    }

    // --- Configure I2S ---
    i2s_driver_uninstall(I2S_NUM_0); 

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
        .sample_rate = header->sample_rate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_MSB,
        .intr_alloc_flags = 0,
        .dma_buf_count = 8,
        .dma_buf_len = 1024,
        .use_apll = false
    };

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, NULL);

    Serial.println("I2S driver configured from WAV header.");
    return true;
}

/**
 * @brief Plays the .wav file with a precise start/stop sequence to prevent pops.
 * @param header The WavHeader struct with the file's properties.
 */
void playWavFile(const WavHeader& header) {
    if (wavBuffer == NULL) return;

    // --- NEW START SEQUENCE ---
    // 1. Enable the DAC just before we start writing data
    i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN);

    Serial.println("Starting playback...");
    
    // 2. Ramp-up: Write a buffer of silence to prime the DAC output
    uint16_t silence_buffer[512];
    for (int i = 0; i < 512; i++) {
        silence_buffer[i] = 0x8000; // Unsigned 16-bit silence
    }
    size_t bytes_written = 0;
    i2s_write(I2S_NUM_0, silence_buffer, sizeof(silence_buffer), &bytes_written, portMAX_DELAY);

    // --- 3. Main Playback Loop ---
    uint8_t processing_buffer[2048];
    size_t bytes_read_from_psram = 0;
    uint8_t *audio_data_start = wavBuffer + WAV_HEADER_SIZE;
    size_t audio_data_size = bufferSize - WAV_HEADER_SIZE;

    while (bytes_read_from_psram < audio_data_size) {
        int bytes_to_process = sizeof(processing_buffer);
        int samples_in_buffer = 0;

        // Logic for 8-bit mono WAV
        if (header.bits_per_sample == 8) {
            samples_in_buffer = sizeof(processing_buffer) / 4; 
            if (bytes_read_from_psram + samples_in_buffer > audio_data_size) {
                samples_in_buffer = audio_data_size - bytes_read_from_psram;
            }
            bytes_to_process = samples_in_buffer * 4;
            uint16_t* stereo_buffer_ptr = (uint16_t*)processing_buffer;
            for (int i = 0; i < samples_in_buffer; i++) {
                uint16_t sample = ((uint16_t)(audio_data_start[bytes_read_from_psram + i])) << 8;
                *stereo_buffer_ptr++ = sample;
                *stereo_buffer_ptr++ = sample;
            }
            bytes_read_from_psram += samples_in_buffer;
        } 
        // Logic for 16-bit mono WAV
        else if (header.bits_per_sample == 16) {
            samples_in_buffer = sizeof(processing_buffer) / 4;
            size_t source_bytes_to_read = samples_in_buffer * 2;
            if (bytes_read_from_psram + source_bytes_to_read > audio_data_size) {
                source_bytes_to_read = audio_data_size - bytes_read_from_psram;
                samples_in_buffer = source_bytes_to_read / 2;
            }
            bytes_to_process = samples_in_buffer * 4;
            uint16_t* stereo_buffer_ptr = (uint16_t*)processing_buffer;
            int16_t* source_ptr = (int16_t*)(audio_data_start + bytes_read_from_psram);
            for (int i = 0; i < samples_in_buffer; i++) {
                // Correctly convert signed 16-bit sample to unsigned for DAC
                uint16_t sample = (*source_ptr++) + 32768; 
                *stereo_buffer_ptr++ = sample;
                *stereo_buffer_ptr++ = sample;
            }
            bytes_read_from_psram += source_bytes_to_read;
        }
        i2s_write(I2S_NUM_0, processing_buffer, bytes_to_process, &bytes_written, portMAX_DELAY);
    }

    // --- NEW STOP SEQUENCE ---
    // 4. Ramp-down: Write silence to bring the output level back to the midpoint.
    i2s_write(I2S_NUM_0, silence_buffer, sizeof(silence_buffer), &bytes_written, portMAX_DELAY);
    
    // 5. Stop the I2S peripheral. This halts the clocks and DMA.
    i2s_stop(I2S_NUM_0);
    
    // 6. Disable the DAC, completely turning off the output.
    i2s_set_dac_mode(I2S_DAC_CHANNEL_DISABLE);
    
    Serial.println("Playback complete.");
}

/**
 * @brief Waits for user input for a file path, then loads and plays it.
 */
void getAndPlayFile() {
    if (Serial.available() > 0) {
        String filePath = Serial.readStringUntil('\n');
        filePath.trim();

        if (filePath.length() > 0) {
            Serial.printf("Received request for: %s\n", filePath.c_str());
            
            if (loadFileToPsram(filePath.c_str())) {
                WavHeader header;
                if (configureI2sFromWav(&header)) {
                    playWavFile(header);
                }
            }

            Serial.println("\nReady for next file.");
            Serial.println("Enter the folder and file name (e.g., /sounds/track1.wav)");
        }
    }
}

void loop() {
    getAndPlayFile();
    delay(100); 
}