#include "STM32_DMA_ADC.h"
#include "AudioTools.h"

STM32_DMA_ADC adc;
int sample_rate = 8000;
int channels = 2;
int buffer_size = 1024;
NBuffer<uint8_t> buffer(buffer_size, 50);

// data callback: we just fill a buffer
void writeData(uint8_t *rec, int byteCount){
  buffer.writeBytes(rec, byteCount);
}

void setup() {
  Serial.begin(115200);
  while(!Serial);

  adc.begin(sample_rate, channels, writeData, buffer_size);  
}

void loop() {
  static int ch = 0;
  // print all data in buffer
  int16_t data[buffer_size/2];
  int bytesRead = buffer.readBytes(data, buffer_size);
  int samples = bytesRead / 2;
  for (int j=0;j<samples;j++){
    Serial.print(data[j]);
    Serial.print(" ");
    if (++ch==channels){
      ch = 0;
      Serial.println();
    }
  }
}