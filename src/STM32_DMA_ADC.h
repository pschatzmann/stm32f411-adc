#pragma
#include "Arduino.h"
#include "hal_conf_extra.h"
#include "stm32f4xx_it.h"
#include <stdlib.h>
#include <stdint.h>
#include <forward_list>
#include <cassert>
#include <functional>
#include <vector>

#undef Error_Handler
#define ADC_MAX_CHANNELS 8

// Callback handler vectors
std::vector<std::function<void(ADC_HandleTypeDef*)>> list_HAL_ADC_MspInit;
std::vector<std::function<void(ADC_HandleTypeDef*)>> list_HAL_ADC_MspDeInit;
std::vector<std::function<void(ADC_HandleTypeDef*)>> list_HAL_ADC_ConvCpltCallback;
std::vector<std::function<void(ADC_HandleTypeDef*)>> list_HAL_ADC_ConvHalfCpltCallback;
std::vector<std::function<void()>> list_DMA2_Stream0_IRQHandler;

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

   enum ErrorLevelSTM32 {Error, Info, Warning};
   typedef void (*TcallbackADC)(int16_t*data, int sampleCount);

    /**
     * @brief Class which is used to calculate the actual avg value of each channel
     * Audio data is centered around 0. We can use this calss to calculate the offset
     */
    class ADCAverageCalculator {
    public:
        ADCAverageCalculator(int channels, int maxCount){
            channel_cnt = channels;
            max_cnt = maxCount;
            p_avg = new float[channel_cnt]();
            is_relevant = max_cnt>0;
            is_ready = false;
            idx = 0;
            cnt = 0;
        }

        ~ADCAverageCalculator(){
            if (p_avg!=nullptr) delete p_avg;
        }

        void add(int16_t* data, int n){
            for (int j=0;j<n;j++){
                p_avg[idx]+=data[j];
                if (++idx>=channel_cnt){
                    idx = 0;
                    if (++cnt>=max_cnt){
                        is_ready=true;  
                        for (int ch=0;ch<channel_cnt;ch++){
                            p_avg[ch] = p_avg[ch] / max_cnt;
                        }
                        return;
                    }
                }
            }
        }

        void update(int16_t* data, int n){
            for (int j=0;j<n;j++){
                data[j]-=avg(idx);
                if (++idx>=channel_cnt){
                    idx = 0;
                }
            }
        }

        int16_t avg(int idx){
            if (p_avg==nullptr || idx>=channel_cnt) return 0;
            return p_avg[idx];
        }

        void reset(){
            for (int j=0;j<channel_cnt;j++){
                p_avg[j]=0;
            }
            is_ready = false;
            cnt = 0;
        }

        bool isReady() {
            return is_ready;
        }

        bool isRelevant() {
            return is_relevant;
        }

    protected:
        int idx = 0;
        int channel_cnt;
        float* p_avg=nullptr;
        int cnt;
        int max_cnt=0;
        volatile bool is_ready = false;
        volatile bool is_relevant = false;
    };

  public:

    /**
     * @brief Construct a new stm32 dma adc object w/o timer in ContinuousConvMode
     * 
     * @param channels 
     */
    STM32_DMA_ADC(int channels) {
        channel_cnt = channels;
        adc_buffer_size = getBufferSize(0);
        is_continuous_conv_mode = true;
        // make sure that we have enough time to process the callbacks
        sampling_time = ADC_SAMPLETIME_56CYCLES;  //ADC_SAMPLETIME_3CYCLES ADC_SAMPLETIME_15CYCLES ADC_SAMPLETIME_28CYCLES ADC_SAMPLETIME_144CYCLES ADC_SAMPLETIME_480CYCLES
    };

    /**
     * @brief Construct a new stm32 dma adc object with defined sample rate (using a timer)
     * 
     * @param timerNum 
     * @param sampleRate 
     * @param bufferSize 
     */
    STM32_DMA_ADC(int channels, TIM_TypeDef *timerNum, int sampleRate, TcallbackADC adcCallback=nullptr, uint32_t bufferSize=0) {
        channel_cnt = channels;
        timer_num = timerNum;
        sample_rate = sampleRate;
        adc_callback = adcCallback;
        adc_buffer_size = getBufferSize(bufferSize);
        is_continuous_conv_mode = false;
        sampling_time = sampleRate < 50000 ? ADC_SAMPLETIME_15CYCLES : ADC_SAMPLETIME_3CYCLES;
        correction_factor = sampleRate <= 50000 ? 0.89 : 0.84;
    };

    /// Destructor
    ~STM32_DMA_ADC() {
        if (is_active) end();
        if (p_timer!=nullptr) delete p_timer;
        if (adc_buffer!=nullptr) delete adc_buffer;
    }

    /// Starts the ADC Processing
    bool begin(){
        // SystemClock_Config();

        // calculate offset of last frame in result half buffer
        int samplesBuffer = adc_buffer_size/2;
        int samplesHalfBuffer = samplesBuffer/2;
        lastFrameStartIdx = samplesHalfBuffer - channel_cnt;

        // add handlers
        addHandlers();

        // log some relevant information
        STM32_LOG(Info,"sample_rate: %d ", sample_rate);
        STM32_LOG(Info,"channels: %d ", channel_cnt);
        STM32_LOG(Info,"total bufferSize: %d bytes", adc_buffer_size);
        STM32_LOG(Info,"total bufferSize: %d samples", samplesBuffer);
        STM32_LOG(Info,"half bufferSize: %d samples", samplesHalfBuffer);
        STM32_LOG(Info,"lastFrameStartIdx: %d samples", lastFrameStartIdx);

        // allocate buffer
        if (adc_buffer==nullptr){
            adc_buffer = new uint8_t[adc_buffer_size];
        }

        if (p_avg==nullptr){
            p_avg = new ADCAverageCalculator(channel_cnt, is_center_zero?500:0);
        }

        MX_GPIO_Init();
        MX_DMA_Init();
        MX_ADC1_Init();

        // Start ADC
        if (HAL_ADC_Start_DMA(&hadc1, (uint32_t*) adc_buffer, samplesBuffer)!=HAL_OK){
            Error_Handler();
            return false;
        }

        // if DMA is driven by timer we start it now
        if (!is_continuous_conv_mode){
            // allocate the timer
            p_timer = new HardwareTimer(timer_num);
            int32_t hz = correction_factor * 10 * sample_rate / channel_cnt;
            int32_t micro_sec = 1000000 / hz;
            p_timer->setOverflow(micro_sec, MICROSEC_FORMAT); 
            STM32_LOG(Info, "overflowHz: %d", hz);
            STM32_LOG(Info, "overflowUs: %d", micro_sec);

            // Activate trigger for DMA - instead of p_timer callback
            TIM_MasterConfigTypeDef sMasterConfig = {0};
            sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
            sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_ENABLE;
            if (HAL_TIMEx_MasterConfigSynchronization(p_timer->getHandle(), &sMasterConfig) != HAL_OK) {
                Error_Handler();
            }

            // start the p_timer
            p_timer->resume();

        }

        delay(100);

        // // we might be able to use the buffer information from hdma
        // STM32_LOG(Info, "hdma_adc1 check: %d",&hdma_adc1==hadc1.DMA_Handle);
        // STM32_LOG(Info, "adc_buffer_size check: %d - %d",hdma_adc1.Instance->NDTR, adc_buffer_size);
        // STM32_LOG(Info, "buffer_check: %x %x", (uint8_t*)hdma_adc1.Instance->PAR, adc_buffer);

        is_active = true;
        return is_active;
    }

    /// Provides the avg calculated over the initial samples for the indicated channel. Values are only available with normalization active: Call setCenterZero(true) before begin()!
    int16_t avg(int ch){
        if (ch>=channel_cnt) return 0;
        return p_avg->avg(ch);
    }

    /// Returns true if it has been started  
    bool isActive() {
        return is_active;
    }

    /// Provides the number of active channels
    int channels() {
        return channel_cnt;
    }

    /// Stops the ADC processing
    void end() {
        p_timer->pause();
        removeHandlers();

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
            channel = getChannelForPin(in);
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

    /// Define the sampling time e.g. ADC_SAMPLETIME_15CYCLES
    void setSamplingTime(uint32_t st){
        sampling_time = st;
    }

    /// Provides the actually defined sampleing time
    uint32_t samplingTime() {
        return sampling_time;
    }

    /// Audio data is usually centered at 0 with negative and positive values: activate value normalization by setting active to true!
    void setCenterZero(bool active){
        is_center_zero = active;
    }

    /// Returns true if the values are normlized around 0
    bool isCenterZero() {
        return is_center_zero;
    }

protected:
    HardwareTimer *p_timer=nullptr;
    TIM_TypeDef *timer_num;
    float correction_factor =  1.0;
    bool is_active = false;
    bool is_center_zero = false;
    bool is_center_zero_in_progress;
    bool is_continuous_conv_mode;
    int channel_cnt=0;
    int sample_rate=0;
    int lastFrameStartIdx=0;
    uint32_t sampling_time = ADC_SAMPLETIME_28CYCLES; // ADC_SAMPLETIME_3CYCLES ADC_SAMPLETIME_15CYCLES ADC_SAMPLETIME_28CYCLES ADC_SAMPLETIME_144CYCLES;
    uint8_t* adc_buffer = nullptr;
    uint16_t adc_buffer_size = 0;
    volatile int16_t *adc_result = nullptr; 
    ADC_HandleTypeDef hadc1;
    DMA_HandleTypeDef hdma_adc1;
    TcallbackADC adc_callback = nullptr;
    ADCAverageCalculator *p_avg = nullptr;

    const std::function<void(ADC_HandleTypeDef*)> f_HAL_ADC_MspInit=std::bind(&STM32_DMA_ADC::HAL_ADC_MspInit, this, std::placeholders::_1);
    const std::function<void(ADC_HandleTypeDef*)> f_HAL_ADC_MspDeInit=std::bind(&STM32_DMA_ADC::HAL_ADC_MspDeInit, this, std::placeholders::_1);
    const std::function<void(ADC_HandleTypeDef*)> f_HAL_ADC_ConvCpltCallback=std::bind(&STM32_DMA_ADC::HAL_ADC_ConvCpltCallback, this, std::placeholders::_1);
    const std::function<void(ADC_HandleTypeDef*)> f_HAL_ADC_ConvHalfCpltCallback=std::bind(&STM32_DMA_ADC::HAL_ADC_ConvHalfCpltCallback, this, std::placeholders::_1);
    const std::function<void()> f_DMA2_Stream0_IRQHandler=std::bind(&STM32_DMA_ADC::DMA2_Stream0_IRQHandler, this);


    /// determines the "correct" buffer size based on the requested size
    int getBufferSize(int bufferSize) {
        int result = bufferSize;
        int frameSize = channel_cnt*sizeof(int16_t);
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

    /// Determines the channel for the indicated pin  
    int getChannelForPin(int pin){
        int channel = -1;
        switch(pin){
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
                STM32_LOG(Error, "Invalid pin: %d", pin);
                channel = -1;
                break;
        }
        return channel;
    }

    // register local handlers
    void addHandlers() {
        list_HAL_ADC_MspInit.push_back(f_HAL_ADC_MspInit); 
        list_HAL_ADC_MspDeInit.push_back(f_HAL_ADC_MspDeInit); 
        list_HAL_ADC_ConvCpltCallback.push_back(f_HAL_ADC_ConvCpltCallback); 
        list_HAL_ADC_ConvHalfCpltCallback.push_back(f_HAL_ADC_ConvHalfCpltCallback); 
        list_DMA2_Stream0_IRQHandler.push_back(f_DMA2_Stream0_IRQHandler);
    }

    // deregister local handlers
    void removeHandlers() {
        // list_HAL_ADC_MspInit.remove(f_HAL_ADC_MspInit); 
        // list_HAL_ADC_MspDeInit.remove(f_HAL_ADC_MspDeInit); 
        // list_HAL_ADC_ConvCpltCallback.remove(f_HAL_ADC_ConvCpltCallback); 
        // list_HAL_ADC_ConvHalfCpltCallback.remove(f_HAL_ADC_ConvHalfCpltCallback); 
        // list_DMA2_Stream0_IRQHandler.remove(f_DMA2_Stream0_IRQHandler);
        list_HAL_ADC_MspInit.clear();
        list_HAL_ADC_MspDeInit.clear(); 
        list_HAL_ADC_ConvCpltCallback.clear(); 
        list_HAL_ADC_ConvHalfCpltCallback.clear(); 
        list_DMA2_Stream0_IRQHandler.clear();
    }

    // void SystemClock_Config(void) {
    //     RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    //     RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    //     /** Configure the main internal regulator output voltage
    //      */
    //     __HAL_RCC_PWR_CLK_ENABLE();
    //     __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    //     /** Initializes the RCC Oscillators according to the specified parameters
    //      * in the RCC_OscInitTypeDef structure.
    //      */
    //     RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    //     RCC_OscInitStruct.HSEState = RCC_HSE_ON;
    //     RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    //     RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    //     RCC_OscInitStruct.PLL.PLLM = 12;
    //     RCC_OscInitStruct.PLL.PLLN = 96;
    //     RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
    //     RCC_OscInitStruct.PLL.PLLQ = 5;
    //     if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
    //     {
    //         Error_Handler();
    //     }

    //     /** Initializes the CPU, AHB and APB buses clocks
    //      */
    //     RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
    //                                 |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
    //     RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    //     RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    //     RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
    //     RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

    //     if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK)
    //     {
    //         Error_Handler();
    //     }
    // }

    /**
     * @brief ADC1 Initialization Function
     */
    void MX_ADC1_Init(void){

        ADC_ChannelConfTypeDef sConfig = {0};

        // Configure the global features of the ADC (Clock, Resolution, Data Alignment and number of conversion)
        hadc1.Instance = ADC1;
        hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4; // ADC_CLOCK_SYNC_PCLK_DIV4
        hadc1.Init.Resolution = ADC_RESOLUTION_12B;
        hadc1.Init.ScanConvMode = channel_cnt>1 ? ENABLE : DISABLE;
        hadc1.Init.DiscontinuousConvMode = DISABLE;
        hadc1.Init.NbrOfDiscConversion = 1;
        hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
        hadc1.Init.NbrOfConversion = channel_cnt;
        hadc1.Init.DMAContinuousRequests = ENABLE;
        hadc1.Init.EOCSelection = ADC_EOC_SEQ_CONV; //ADC_EOC_SINGLE_CONV; // ADC_EOC_SEQ_CONV

        if (is_continuous_conv_mode){
            hadc1.Init.ContinuousConvMode = ENABLE;
            hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_NONE;
            hadc1.Init.ExternalTrigConv = ADC_SOFTWARE_START;
        } else {
            hadc1.Init.ContinuousConvMode =  DISABLE;
            hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
            hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T3_TRGO;
        }

        if (HAL_ADC_Init(&hadc1) != HAL_OK){
            Error_Handler();
        }

        int adc_channels[] = {ADC_CHANNEL_0, ADC_CHANNEL_1, ADC_CHANNEL_3, ADC_CHANNEL_4, ADC_CHANNEL_5, ADC_CHANNEL_6, ADC_CHANNEL_6, ADC_CHANNEL_7, ADC_CHANNEL_8};
        // Configure for the selected ADC regular channel its corresponding rank in the sequencer and its sample time.
        for (int ch=0; ch < channels(); ch++){
            sConfig.Channel = adc_channels[ch];
            sConfig.Rank = ch+1;
            sConfig.SamplingTime = sampling_time; 
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

    /// DMA Callback
    void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc){
        // guard
        if (hadc!=&hadc1) return;

        int16_t *start = (int16_t *) &(adc_buffer[adc_buffer_size/2]);
        int len_bytes = adc_buffer_size/2;
        int len_samples = len_bytes / 2;
        if (p_avg->isRelevant() && !p_avg->isReady()){
            p_avg->add(start,len_samples);
        } 
        adc_result = start;
        if (adc_callback!=nullptr) {
            p_avg->update(start,len_samples);
            adc_callback(start, len_samples);
        }
    }

    /// DMA Callback
    void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef* hadc){
        // guard
        if (hadc!=&hadc1) return;

        int16_t *start = (int16_t *) adc_buffer;
        int len_bytes = adc_buffer_size/2;
        int len_samples = len_bytes / 2;
        if (p_avg->isRelevant() && !p_avg->isReady()){
            p_avg->add(start,len_samples);
        } 
        adc_result = start;
        if (adc_callback!=nullptr){
            p_avg->update(start,len_samples);
            adc_callback(start, len_samples);
        } 
    }

    /**
    * @brief ADC MSP Initialization
    * This function configures the hardware resources used in this example
    */
    void HAL_ADC_MspInit(ADC_HandleTypeDef* hadc) {
        // guard
        if (hadc!=&hadc1) return;

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
            hdma_adc1.Init.Priority = DMA_PRIORITY_LOW; //DMA_PRIORITY_HIGH
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
    void HAL_ADC_MspDeInit(ADC_HandleTypeDef* hadc) {
        // guard
        if (hadc!=&hadc1) return;

        if(hadc->Instance==ADC1){
            __HAL_RCC_ADC1_CLK_DISABLE();
            HAL_GPIO_DeInit(GPIOA, GPIO_PIN_0|GPIO_PIN_1|GPIO_PIN_3|GPIO_PIN_4
                                |GPIO_PIN_5|GPIO_PIN_6|GPIO_PIN_7);

            HAL_GPIO_DeInit(GPIOB, GPIO_PIN_0);

            HAL_DMA_DeInit(hadc->DMA_Handle);
        }
    }

    void DMA2_Stream0_IRQHandler(void){
        HAL_DMA_IRQHandler(&hdma_adc1);
    }

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
        STM32_LOG(Error, "adc error");
    }

};

/// DMA IRQ Handler  
extern "C" void DMA2_Stream0_IRQHandler(void){
//    HAL_DMA_IRQHandler(&(self_STM32_DMA_ADC->hdma_adc1));
    for (auto f: list_DMA2_Stream0_IRQHandler){
        f();
    }
}

/// DMA Callback
extern "C" void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc){
//    if (self_STM32_DMA_ADC) self_STM32_DMA_ADC->HAL_ADC_ConvCpltCallback(hadc);
    for (auto f: list_HAL_ADC_ConvCpltCallback){
        f(hadc);
    }
}

/// DMA Callback
extern "C"  void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef* hadc){
//    if (self_STM32_DMA_ADC) self_STM32_DMA_ADC->HAL_ADC_ConvHalfCpltCallback(hadc);
    for (auto f: list_HAL_ADC_ConvHalfCpltCallback){
        f(hadc);
    }
}

/**
* @brief ADC MSP Initialization
* This function configures the hardware resources used in this example
*/
extern "C" void HAL_ADC_MspInit(ADC_HandleTypeDef* hadc) {
    //if (self_STM32_DMA_ADC) self_STM32_DMA_ADC->HAL_ADC_MspInit(hadc);
    for (auto f: list_HAL_ADC_MspInit){
        f(hadc);
    }
}

/**
* @brief ADC MSP De-Initialization
* This function freeze the hardware resources used in this example
*/
extern "C" void HAL_ADC_MspDeInit(ADC_HandleTypeDef* hadc) {
 //   if (self_STM32_DMA_ADC) self_STM32_DMA_ADC->HAL_ADC_MspDeInit(hadc);
    for (auto f: list_HAL_ADC_MspDeInit){
        f(hadc);
    }

}

