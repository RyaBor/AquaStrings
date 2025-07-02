#include "driver/i2s.h"
#include "SD.h"
#include "SPI.h"

// --- SD Card Pin Definitions ---
#define SD_CS       5
#define SPI_MOSI    23
#define SPI_MISO    19
#define SPI_SCK     18

// Pointer for our PSRAM buffer
uint8_t *wavBuffer = NULL;
size_t bufferSize = 0;
const int WAV_HEADER_SIZE = 44;

void setup() {
  Serial.begin(115200);

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
  
  // --- Load File to PSRAM ---
  File file = SD.open("/Scale2.wav", "r");
  if (!file) {
    Serial.println("Failed to open /Scale2.wav");
    while(true);
  }
  
  bufferSize = file.size();
  wavBuffer = (uint8_t *)ps_malloc(bufferSize);
  if (wavBuffer == NULL) {
    Serial.println("Failed to allocate PSRAM!");
    while(true);
  }
  
  file.read(wavBuffer, bufferSize);
  file.close();
  SD.end(); 
  Serial.println("Loaded WAV file into PSRAM.");

  // --- Configure and Start I2S ---
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
    .sample_rate = 96000,
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT, // Set to stereo
    .communication_format = I2S_COMM_FORMAT_STAND_MSB,
    .intr_alloc_flags = 0,
    .dma_buf_count = 8,
    .dma_buf_len = 1024,
    .use_apll = false
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, NULL);
  i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN); // Enable both DAC channels

  Serial.println("Starting playback...");
}

void play_sound_from_psram() {
  // A buffer to hold the stereo-formatted data
  uint16_t processing_buffer[1024];
  
  size_t bytes_written = 0;
  size_t bytes_read_from_psram = 0;

  // Set the starting point of our audio data (skipping the header)
  uint8_t *audio_data_start = wavBuffer + WAV_HEADER_SIZE;
  size_t audio_data_size = bufferSize - WAV_HEADER_SIZE;

  // Loop through the PSRAM buffer, processing it in chunks
  while (bytes_read_from_psram < audio_data_size) {
    
    // Determine how many 8-bit samples to read in this chunk
    int samples_to_process = sizeof(processing_buffer) / sizeof(processing_buffer[0]) / 2; // Process half as many since we are duplicating them
    if (bytes_read_from_psram + samples_to_process > audio_data_size) {
      samples_to_process = audio_data_size - bytes_read_from_psram;
    }
    
    // Expand the 8-bit mono data to 16-bit stereo data
    for (int i=0; i < samples_to_process; i++) {
      uint16_t sample = (uint16_t)(audio_data_start[bytes_read_from_psram + i]) << 8;
      processing_buffer[i*2]     = sample; // Left channel
      processing_buffer[i*2 + 1] = sample; // Right channel
    }
    
    // Write the correctly formatted stereo data to the I2S driver
    i2s_write(I2S_NUM_0, processing_buffer, samples_to_process * sizeof(uint16_t) * 2, &bytes_written, portMAX_DELAY);
    
    bytes_read_from_psram += samples_to_process;
  }
}


void loop() {
  play_sound_from_psram();

  Serial.println("Playback complete.");
  
  // Cleanly stop the I2S driver to prevent popping
  i2s_zero_dma_buffer(I2S_NUM_0);
  
  delay(2000); // Wait 2 seconds and play again
}