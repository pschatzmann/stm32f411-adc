#include "STM32_DMA_ADC.h"

const int sample_rate = 44200;
const int channels = 1;
int sample_no = 0;
volatile int frame_count = 0;
void writeData(int16_t *data, int sampleCount);
// DMA with timer and defined sample rate
STM32_DMA_ADC adc(channels, TIM3, sample_rate, writeData, 1024);

void writeData(int16_t *data, int sampleCount){
  for (int j=0;j<sampleCount;j++){
    if (sample_no++>=ADC_MAX_CHANNELS){
       sample_no = 0;
       frame_count++;
    }
  }
}

void setup() {
  Serial.begin(115200);
  while(!Serial);

  adc.begin();  
}

void loop() {
  // count frames every second
  delay(1000);
  Serial.println(frame_count);
  frame_count = 0;
}