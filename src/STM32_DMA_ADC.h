#pragma
#include "Arduino.h"
#include <stdlib.h>
#include <stdint.h>
#include "hal_conf_extra.h"
#include "stm32f4xx_it.h"

#undef Error_Handler
#define ADC_MAX_CHANNELS 8

typedef void (*TcallbackADC)(uint8_t*data, int byteCount);
uint8_t* adc_buffer = nullptr;
uint16_t adc_buffer_size = 0;
volatile int16_t *adc_result = nullptr; 
ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;
TcallbackADC adc_callback = nullptr;

enum ErrorLevelSTM32 {Error, Info, Warning};
void Error_Handler(void);
void STM32_LOG(ErrorLevelSTM32, const char *fmt,...);

/**
 * @brief fast ADC using the DMA. We have 8 channels available 
10	PA0-WKUP	ADC1_IN0	Channel 0
11	PA1	ADC1_IN1	Channel 1
13	PA3	ADC1_IN3	Channel 2
14	PA4	ADC1_IN4	Channel 3
15	PA5	ADC1_IN5	Channel 4
16	PA6	ADC1_IN6	Channel 5
17	PA7	ADC1_IN7	Channel 6
18	PB0	ADC1_IN8	Channe 7
 */
class STM32_DMA_ADC {
  public:
    STM32_DMA_ADC() = default;

    ~STM32_DMA_ADC() {
        if (is_active) end();
        if (p_timer!=nullptr) delete p_timer;
        if (adc_buffer!=nullptr) delete adc_buffer;
    }

    /// Starts the ADC Processing
    bool begin(int sampleRate, int channels, TcallbackADC adcCallback=nullptr, uint32_t bufferSize=0){
        channel_count = channels;
        adc_callback = adcCallback;
        adc_buffer_size = getBufferSize(bufferSize);
        // calculate offset of last frame in result half buffer
        int samplesBuffer = adc_buffer_size/2;
        int samplesHalfBuffer = samplesBuffer/2;
        lastFrameStartIdx = samplesHalfBuffer - channels;

        STM32_LOG(Info,"channels: %d ", channel_count);
        STM32_LOG(Info,"total bufferSize: %d bytes", adc_buffer_size);
        STM32_LOG(Info,"total bufferSize: %d samples", samplesBuffer);
        STM32_LOG(Info,"half bufferSize: %d samples", samplesHalfBuffer);
        STM32_LOG(Info,"lastFrameStartIdx: %d samples", lastFrameStartIdx);


        // allocate buffer
        if (adc_buffer==nullptr){
            adc_buffer = new uint8_t[adc_buffer_size];
        }

        MX_GPIO_Init();
        MX_DMA_Init();
        MX_ADC1_Init();

        if (HAL_ADC_Start_DMA(&hadc1, (uint32_t*) adc_buffer, samplesBuffer)!=HAL_OK){
            Error_Handler();
            return false;
        }

        if (p_timer==nullptr){
            p_timer = new HardwareTimer(TIM3);
            p_timer->setPrescaleFactor(30-1); // 30 mhz -> 1mhz
            int overflowHz = correction_factor * sampleRate / channels * 10;
            STM32_LOG(Info, "overflowHz: %d", overflowHz);
    
            p_timer->setOverflow(overflowHz, HERTZ_FORMAT); // 10000 microseconds = 10 milliseconds
            //p_timer->attachInterrupt(adcDmaTimerCallback);
        }

        // Activate trigger for DMA - instead of p_timer callback
        TIM_MasterConfigTypeDef sMasterConfig = {0};
        sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
        sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_ENABLE;
        if (HAL_TIMEx_MasterConfigSynchronization(p_timer->getHandle(), &sMasterConfig) != HAL_OK) {
            Error_Handler();
        }

        // start the p_timer
        p_timer->resume();
        delay(100);

        is_active = true;
        return is_active;
    }

    operator bool() {
        return is_active;
    }

    int channels() {
        return channel_count;
    }

    /// Stops the ADC processing
    void end() {
        p_timer->pause();

        HAL_ADC_Stop_DMA(&hadc1);
        HAL_ADC_MspDeInit(&hadc1);
        is_active = false;
    }

    /// Returns the actual ADC value for the indicated channel (0 - 7) or pin PA0 to PB0
    int16_t analogRead(int in){
        int channel = in;
        if (adc_result==nullptr) {
            STM32_LOG(Error, "adc_result is null");
            return 0;
        }
        if (in>ADC_MAX_CHANNELS){
            // chanenl contains pin
            switch(channel){
                case PA0: channel = 0;
                    break; 
                case PA1: channel = 1;
                    break; 
                case PA3: channel = 2;
                    break; 
                case PA4: channel = 3;
                    break; 
                case PA5: channel = 4;
                    break; 
                case PA6: channel = 5;
                    break; 
                case PA7: channel = 6;
                    break; 
                case PB0: channel = 7;
                    break; 
                default:
                    STM32_LOG(Error, "Invalid pin/channel: %d", in);
                    return 0;
            }
        }
        // check that we have a valid index
        if (channel > channels()-1) {
            STM32_LOG(Error, "requested channel %d not valid", channel);
            return 0;
        }
        // frames in a half buffer
        return adc_result[lastFrameStartIdx+channel];
    }

    /// We can correct the sampling rate if the effective data input does not match
    void setRateCorrectionFactor(float factor){
        correction_factor = factor;
    }

    /// Actually defined correction factor which is applied to the sample rate
    float rateCorrectionFactor(){
        return correction_factor; 
    }

protected:
    HardwareTimer *p_timer=nullptr;
    float correction_factor = 0.9;
    bool is_active = false;
    int channel_count=0;
    int lastFrameStartIdx=0;

    int getBufferSize(int bufferSize) {
        int result = bufferSize;
        int frameSize = channel_count*sizeof(int16_t);
        // min size for double buffer
        int minBufferSize = frameSize * 2;
        
        if (result<=minBufferSize){
            result = minBufferSize;
        }
        
        if (result % minBufferSize!=0){
            result = result / minBufferSize * minBufferSize; 
            STM32_LOG(Warning, "bufferSize set to %d",result);
        }

        return result;
    }

    /**
     * @brief ADC1 Initialization Function
     */
    void MX_ADC1_Init(void){

        ADC_ChannelConfTypeDef sConfig = {0};

        // Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
        hadc1.Instance = ADC1;
        hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
        hadc1.Init.Resolution = ADC_RESOLUTION_12B;
        hadc1.Init.ScanConvMode = channel_count>1 ? ENABLE : DISABLE;
        hadc1.Init.ContinuousConvMode = DISABLE;
        hadc1.Init.DiscontinuousConvMode = DISABLE;
        //hadc1.Init.DiscontinuousConvMode = ENABLE;
        //hadc1.Init.NbrOfDiscConversion = 1;
        hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
        hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T3_TRGO;
        //hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
        //hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;

        hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
        hadc1.Init.NbrOfConversion = channel_count;
        hadc1.Init.DMAContinuousRequests = ENABLE;
        hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV; //ADC_EOC_SINGLE_CONV; // ADC_EOC_SEQ_CONV
        if (HAL_ADC_Init(&hadc1) != HAL_OK){
            Error_Handler();
        }

        int adc_channels[] = {ADC_CHANNEL_0,ADC_CHANNEL_1,ADC_CHANNEL_3,ADC_CHANNEL_4,ADC_CHANNEL_5,ADC_CHANNEL_6,ADC_CHANNEL_6,ADC_CHANNEL_7,ADC_CHANNEL_8};
        // Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
        for (int ch=0; ch < channels(); ch++){
            sConfig.Channel = adc_channels[ch];
            sConfig.Rank = ch+1;
            sConfig.SamplingTime = ADC_SAMPLETIME_15CYCLES; // ADC_SAMPLETIME_3CYCLES ADC_SAMPLETIME_15CYCLES ADC_SAMPLETIME_144CYCLES;
            if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK){
                Error_Handler();
            }
        }
    }

    /**
     * Enable DMA controller clock
     */
    void MX_DMA_Init(void){
        /* DMA controller clock enable */
        __HAL_RCC_DMA2_CLK_ENABLE();

        /* DMA interrupt init */
        /* DMA2_Stream0_IRQn interrupt configuration */
        HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 0, 0);
        HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
    }

    /**
     * @brief GPIO Initialization Function
     */
    void MX_GPIO_Init(void){
        /* GPIO Ports Clock Enable */
        __HAL_RCC_GPIOH_CLK_ENABLE();
        __HAL_RCC_GPIOA_CLK_ENABLE();
        __HAL_RCC_GPIOB_CLK_ENABLE();
    }
    /**
    * @brief Timer callback which starts a new the ADC DMA conversion
    */
    static  void adcDmaTimerCallback() {
        if (HAL_ADC_Start_DMA(&hadc1, (uint32_t*) adc_buffer, adc_buffer_size/2)!=HAL_OK){
            Error_Handler();
        }
    }

};

void STM32_LOG(ErrorLevelSTM32 level, const char *fmt,...) {
    char log_buffer[200];
    strcpy(log_buffer,"STM32 ");
    switch(level){
        case Error:
            strcat(log_buffer, "Error:");
            break;
        case Warning:
            strcat(log_buffer, "Warning:");
            break;
        case Info:
            strcat(log_buffer, "Info:");
            break;
    }  
    int len = strlen(log_buffer);
    va_list arg;
    va_start(arg, fmt);
    len = vsnprintf(log_buffer + len,200-len, fmt, arg);
    va_end(arg);
    Serial.println(log_buffer);
}

void Error_Handler(void) {
    STM32_LOG(Error, "adc dma error");
}

/// DMA Callback
extern "C" void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc){
    uint8_t *start = &(adc_buffer[adc_buffer_size/2]);
    int len = adc_buffer_size/2;
    adc_result = (int16_t*)start;
    if (adc_callback!=nullptr) adc_callback(start, len);
}

/// DMA Callback
extern "C"  void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef* hadc){
    uint8_t *start = adc_buffer;
    int len = adc_buffer_size/2;
    adc_result = (int16_t*)start;
    if (adc_callback!=nullptr) adc_callback(start, len);
}

/// DMA IRQ Handler  
extern "C" void DMA2_Stream0_IRQHandler(void){
    HAL_DMA_IRQHandler(&hdma_adc1);
}

/**
* @brief ADC MSP Initialization
* This function configures the hardware resources used in this example
*/
extern "C" void HAL_ADC_MspInit(ADC_HandleTypeDef* hadc) {
    GPIO_InitTypeDef GPIO_InitStruct = {0};
    if(hadc->Instance==ADC1) {
        __HAL_RCC_ADC1_CLK_ENABLE();

        __HAL_RCC_GPIOA_CLK_ENABLE();
        __HAL_RCC_GPIOB_CLK_ENABLE();
        /**ADC1 GPIO Configuration
        PA0     ------> ADC1_IN0
        PA1     ------> ADC1_IN1
        PA3     ------> ADC1_IN3
        PA4     ------> ADC1_IN4
        PA5     ------> ADC1_IN5
        PA6     ------> ADC1_IN6
        PA7     ------> ADC1_IN7
        PB0     ------> ADC1_IN8
        */
        GPIO_InitStruct.Pin = GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_3|GPIO_PIN_4
                            |GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7;
        GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

        GPIO_InitStruct.Pin = GPIO_PIN_0;
        GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
        GPIO_InitStruct.Pull = GPIO_NOPULL;
        HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

        /* ADC1 DMA Init */
        /* ADC1 Init */
        hdma_adc1.Instance = DMA2_Stream0;
        hdma_adc1.Init.Channel = DMA_CHANNEL_0;
        hdma_adc1.Init.Direction = DMA_PERIPH_TO_MEMORY;
        hdma_adc1.Init.PeriphInc = DMA_PINC_DISABLE;
        hdma_adc1.Init.MemInc = DMA_MINC_ENABLE;
        hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
        hdma_adc1.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
        hdma_adc1.Init.Mode = DMA_CIRCULAR;
        hdma_adc1.Init.Priority = DMA_PRIORITY_HIGH;
        //hdma_adc1.Init.FIFOMode = DMA_FIFOMODE_DISABLE;  
        hdma_adc1.Init.FIFOMode = DMA_FIFOMODE_ENABLE; 
        hdma_adc1.Init.FIFOThreshold = DMA_FIFO_THRESHOLD_FULL;
        hdma_adc1.Init.MemBurst = DMA_MBURST_SINGLE;
        hdma_adc1.Init.PeriphBurst = DMA_MBURST_SINGLE;
        if (HAL_DMA_Init(&hdma_adc1) != HAL_OK){
            Error_Handler();
        }

        __HAL_LINKDMA(hadc,DMA_Handle,hdma_adc1);

    }
}

/**
* @brief ADC MSP De-Initialization
* This function freeze the hardware resources used in this example
*/
extern "C" void HAL_ADC_MspDeInit(ADC_HandleTypeDef* hadc) {
    if(hadc->Instance==ADC1){
        __HAL_RCC_ADC1_CLK_DISABLE();
        HAL_GPIO_DeInit(GPIOA, GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_3|GPIO_PIN_4
                            |GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7);

        HAL_GPIO_DeInit(GPIOB, GPIO_PIN_0);

        HAL_DMA_DeInit(hadc->DMA_Handle);
    }
}
