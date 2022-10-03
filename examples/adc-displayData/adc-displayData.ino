#include "STM32_DMA_ADC.h"
#include "AudioTools.h"

const int sample_rate = 8000;
const int channels = 2;
const int buffer_size = 1024;
void writeData(int16_t *data, int sampleCount);
// DMA with timer and defined sample rate
STM32_DMA_ADC adc(channels, TIM3, sample_rate, writeData, 1024);
NBuffer<uint8_t> buffer(buffer_size, 50);

// data callback: we just fill a buffer
void writeData(int16_t *rec, int sampleCount){
  buffer.writeArray((uint8_t*)rec, sampleCount*2);
}

void setup() {
  Serial.begin(115200);
  while(!Serial);

  adc.setCenterZero(true);
  adc.begin();  
}

void loop() {
  static int ch = 0;
  // print all data in buffer
  int16_t data[buffer_size/2];
  int bytesRead = buffer.readArray((uint8_t*)data, buffer_size);
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