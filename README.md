# Arduino STM32F411 Analog Input DMA Library

I wanted to use the __ADC__ with DMA in Arduino with my __STM32F411 Black Pill__ processor together with my [Arduino Audio Tools](https://github.com/pschatzmann/arduino-audio-tools)! 

![stm32f411](https://pschatzmann.github.io/stm32f411-i2s/stm32f411.jpeg)

Unfortunately [STMDuino](https://github.com/stm32duino) does not provide this functionality.

My first trials failed miserably using the DMA versions of the HAL API, so I decided to generate a working solution using the __STM Cube IDE__ and then convert this to Arduino library, that provides the following functionality:

- The DMA is used to transfer the data
- The API is using __Callbacks__ to transfer the data.
- 8 input channels
- Only __16bit__ data is supported
- I also incuded the __codec drivers__ that are part of some stm32 evaluation boards. 

## Pins for I2S3

PINs  |	FUNCTIONs 
------|------------	
PA0   |	Channel0	
PA1	  | Channel1
PA3	  | Channel2	
PA4	  | Channel3
PA5	  | Channel4
PA6	  | Channel5
PA7	  | Channel6
PB0	  | Channel7


## API

Below I demonstrate the basic API provided by this library. However, I recommend that you use the I2SStream class from the [Arduino Audio Tools](https://github.com/pschatzmann/arduino-audio-tools) library which uses this functionality.


### Receiving Data

```
#include "stm32-adc.h"

int channels = 8;
int sample_n o =0;


void writeData(uint8_t *buffer, int byteCount){
  //out.write(buffer, byteCount);
  int16_t *data = (int16_t*)buffer;
  int sample_count = byteCount/2;
  for (int j=0;j<sample_count;j++){
    Serial.print(data[j]);
    if (sample_no++==8){
       Serial.println();
    }
  }
}

void setup() {
  Serial.begin(115200);
  while(!Serial);
  
  Serial.printf("HCLK=%d\n", HAL_RCC_GetHCLKFreq());
  Serial.printf("APB1=%d\n", HAL_RCC_GetPCLK1Freq());
  Serial.printf("APB2=%d\n", HAL_RCC_GetPCLK2Freq());

  if (!fastAdcBegin(8000, writeData, 1024)){
    Serial.println("ADC Error");
  }

}

void loop() {

}
```

## Documentation

Here is the link to the [actual documentation](https://pschatzmann.github.io/stm32f411-i2s/html/modules.html).

You might also find further information in [my Blogs](https://www.pschatzmann.ch/tags/stm32)


## Installation in Arduino

You can download the library as zip and call include Library -> zip library. Or you can git clone this project into the Arduino libraries folder e.g. with

```
cd  ~/Documents/Arduino/libraries
git clone https://github.com/pschatzmann/stm32f411-adc
```

I recommend to use git because you can easily update to the latest version just by executing the ```git pull``` command in the project folder.


## Copyright

__Copyright © 2022 Phil Schatzmann__

[GNU General Public License](License.txt)


__Copyright © 2015 STMicroelectronics__
  
Redistribution and use in source and binary forms, with or without modification,
are permitted provided that the following conditions are met:
	1. Redistributions of source code must retain the above copyright notice,
	this list of conditions and the following disclaimer.
	2. Redistributions in binary form must reproduce the above copyright notice,
	this list of conditions and the following disclaimer in the documentation
	and/or other materials provided with the distribution.
	3. Neither the name of STMicroelectronics nor the names of its contributors
	may be used to endorse or promote products derived from this software
	without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
  