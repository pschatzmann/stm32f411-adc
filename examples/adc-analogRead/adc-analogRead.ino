#include "AnalogReaderDMA.h"

// define continuous mode adc w/o timer
const int channels = 2;
AnalogReaderDMA adc(channels);

void setup() {
  Serial.begin(115200);
  while(!Serial);

  adc.begin();  

}

void loop() {
  for (int j=0; j<channels; j++){
    Serial.print(adc.analogRead(j));
    Serial.print(" ");
  }
  Serial.println();
}