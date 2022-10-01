#include "STM32_DMA_ADC.h"

STM32_DMA_ADC adc;
int channels = 2;

void setup() {
  Serial.begin(115200);
  while(!Serial);

  adc.begin(8000, channels);  

}

void loop() {
  for (int j=0; j<channels; j++){
    Serial.print(adc.analogRead(j));
    Serial.print(" ");
  }
  Serial.println();
}