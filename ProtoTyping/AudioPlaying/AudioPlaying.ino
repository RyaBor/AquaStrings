#include "driver/i2s.h"
#include "SD.h"
#include "SPI.h"
#include <Wire.h>
#include "Adafruit_TPA2016.h"

Adafruit_TPA2016 audioamp = Adafruit_TPA2016();

#define SD_CS       5
// ... other SD pins

uint8_t *wavBuffer = NULL;
size_t bufferSize = 0;
const int WAV_HEADER_SIZE = 44;

// --- Digital Volume Control ---
float digital_volume = 0.1; // Start at 10% volume. Range is 0.0 to 1.0

void setup() {
  Serial.begin(115200);

  // --- TPA2016 Initialization ---
  Wire.begin(22, 26); // SDA on pin 22, SCL on pin 26
  if (!audioamp.begin()) {
    Serial.println("Could not find TPA2016!");
    while (1);
  }
  Serial.println("TPA2016 found");
  // Set a fixed, medium gain on the amplifier. We will adjust the digital volume instead.
  audioamp.setGain(20); 
  
  // --- PSRAM & SD Card Setup ---
  if (!psramInit()) { while(true); }
  SPI.begin(18, 19, 23); // SCK, MISO, MOSI
  if (!SD.begin(SD_CS)) { while(true); }
  
  // --- Load File to PSRAM ---
  File file = SD.open("/Scale2.wav", "r");
  if (!file) { while(true); }
  bufferSize = file.size();
  wavBuffer = (uint8_t *)ps_malloc(bufferSize);
  if (wavBuffer == NULL) { while(true); }
  file.read(wavBuffer, bufferSize);
  file.close();
  SD.end(); 
  Serial.println("Loaded WAV file into PSRAM.");

  // --- Configure and Start I2S ---
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN),
    .sample_rate = 96000,
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
  i2s_set_dac_mode(I2S_DAC_CHANNEL_BOTH_EN);

  Serial.println("Starting playback...");
  Serial.println("Send 'u' to increase digital volume, 'd' to decrease.");
}

void play_sound_from_psram() {
  uint16_t processing_buffer[1024];
  size_t bytes_written = 0;
  size_t bytes_read_from_psram = 0;

  uint8_t *audio_data_start = wavBuffer + WAV_HEADER_SIZE;
  size_t audio_data_size = bufferSize - WAV_HEADER_SIZE;

  while (bytes_read_from_psram < audio_data_size) {
    int samples_to_process = sizeof(processing_buffer) / 2 / 2;
    if (bytes_read_from_psram + samples_to_process > audio_data_size) {
      samples_to_process = audio_data_size - bytes_read_from_psram;
    }
    
    for (int i=0; i < samples_to_process; i++) {
      // --- This is the new Digital Volume logic ---
      int8_t original_sample = audio_data_start[bytes_read_from_psram + i] - 128; // Get AC component (-128 to 127)
      int8_t scaled_sample = original_sample * digital_volume; // Scale it
      uint8_t final_sample = scaled_sample + 128; // Add DC offset back
      
      uint16_t output_sample = (uint16_t)final_sample << 8;
      // --- End of new logic ---
      
      processing_buffer[i*2]     = output_sample; // Left channel
      processing_buffer[i*2 + 1] = output_sample; // Right channel
    }
    
    i2s_write(I2S_NUM_0, processing_buffer, samples_to_process * sizeof(uint16_t) * 2, &bytes_written, portMAX_DELAY);
    
    bytes_read_from_psram += samples_to_process;

    // Check for volume change commands during playback
    if (Serial.available()) { return; }
  }
}


void loop() {
  // Check for serial commands to change digital volume
  if (Serial.available()) {
    char c = Serial.read();
    if (c == 'u' && digital_volume < 1.0) {
      digital_volume += 0.05; // Increase by 5%
      Serial.printf("Digital volume set to: %.2f\n", digital_volume);
    }
    if (c == 'd' && digital_volume > 0.0) {
      digital_volume -= 0.05; // Decrease by 5%
      if (digital_volume < 0) digital_volume = 0;
      Serial.printf("Digital volume set to: %.2f\n", digital_volume);
    }
  }

  play_sound_from_psram();
  i2s_zero_dma_buffer(I2S_NUM_0);
}