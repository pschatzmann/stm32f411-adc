#include "STM32_DMA_ADC.h"

STM32_DMA_ADC adc;

int sample_rate = 8000;
int channels = 2;
int sample_no = 0;
int frame = 0;

void writeData(uint8_t *buffer, int byteCount){
  int16_t *data = (int16_t*)buffer;
  int sample_count = byteCount/2;
  for (int j=0;j<sample_count;j++){
    if (sample_no++>=ADC_MAX_CHANNELS){
       sample_no = 0;
       frame++;
    }
  }
}

void setup() {
  Serial.begin(115200);
  while(!Serial);

  adc.begin(sample_rate, channels, writeData, 1024);  
}

void loop() {
  delay(1000);
  Serial.println(frame);
  frame = 0;
}