// Copyright Microsoft Corporation
// Implementation for STM32F4: Copyright Oberon microsystems, Inc
// Copyright 2017 GHI Electronics, LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "STM32F4.h"


#if SYSTEM_APB1_CLOCK_HZ == SYSTEM_CYCLE_CLOCK_HZ
#define PWM1_CLK_HZ (SYSTEM_APB1_CLOCK_HZ)
#else
#define PWM1_CLK_HZ (SYSTEM_APB1_CLOCK_HZ * 2)
#endif
#define PWM1_CLK_MHZ (PWM1_CLK_HZ / ONE_MHZ)

#if SYSTEM_APB2_CLOCK_HZ == SYSTEM_CYCLE_CLOCK_HZ
#define PWM2_CLK_HZ (SYSTEM_APB2_CLOCK_HZ)
#else
#define PWM2_CLK_HZ (SYSTEM_APB2_CLOCK_HZ * 2)
#endif
#define PWM2_CLK_MHZ (PWM2_CLK_HZ / ONE_MHZ)

#if PWM2_CLK_MHZ > PWM1_CLK_MHZ
#define PWM_MAX_CLK_MHZ PWM2_CLK_MHZ
#else
#define PWM_MAX_CLK_MHZ PWM1_CLK_MHZ
#endif

#define PWM_MILLISECONDS  1000
#define PWM_MICROSECONDS  1000000
#define PWM_NANOSECONDS   1000000000

#define STM32F4_MIN_PWM_FREQUENCY 1

#define pwmController(x)  g_PwmController[x - 1]

static uint8_t pwmProviderDefs[TOTAL_PWM_CONTROLLER * sizeof(TinyCLR_Pwm_Provider)];
static TinyCLR_Pwm_Provider* pwmProviders[TOTAL_PWM_CONTROLLER];
static TinyCLR_Api_Info pwmApi;

const TinyCLR_Api_Info* STM32F4_Pwm_GetApi() {
    for (int i = 0; i < TOTAL_PWM_CONTROLLER; i++) {
        pwmProviders[i] = (TinyCLR_Pwm_Provider*)(pwmProviderDefs + (i * sizeof(TinyCLR_Pwm_Provider)));
        pwmProviders[i]->Parent = &pwmApi;
        pwmProviders[i]->Index = i;
        pwmProviders[i]->SetDesiredFrequency = &STM32F4_Pwm_SetDesiredFrequency;
        pwmProviders[i]->AcquirePin = &STM32F4_Pwm_AcquirePin;
        pwmProviders[i]->ReleasePin = &STM32F4_Pwm_ReleasePin;
        pwmProviders[i]->EnablePin = &STM32F4_Pwm_EnablePin;
        pwmProviders[i]->DisablePin = &STM32F4_Pwm_DisablePin;
        pwmProviders[i]->SetPulseParameters = &STM32F4_Pwm_SetPulseParameters;
        pwmProviders[i]->GetMinFrequency = &STM32F4_Pwm_GetMinFrequency;
        pwmProviders[i]->GetMaxFrequency = &STM32F4_Pwm_GetMaxFrequency;
        pwmProviders[i]->GetActualFrequency = &STM32F4_Pwm_GetActualFrequency;
        pwmProviders[i]->GetPinCount = &STM32F4_Pwm_GetPinCount;
    }

    pwmApi.Author = "GHI Electronics, LLC";
    pwmApi.Name = "GHIElectronics.TinyCLR.NativeApis.STM32F4.PwmProvider";
    pwmApi.Type = TinyCLR_Api_Type::PwmProvider;
    pwmApi.Version = 0;
    pwmApi.Count = TOTAL_PWM_CONTROLLER;
    pwmApi.Implementation = pwmProviders;

    return &pwmApi;
}


typedef  TIM_TypeDef* ptr_TIM_TypeDef;

struct PwmController {
    ptr_TIM_TypeDef     port;
    uint32_t            gpioAlternateFunction;
    int32_t             gpioPin[MAX_PWM_PER_CONTROLLER];

    bool                invert[MAX_PWM_PER_CONTROLLER];
    double              actualFreq;
    double              theoryFreq;
    double              dutyCycle[MAX_PWM_PER_CONTROLLER];

    uint32_t            period;
    uint32_t            presc;

};

static PwmController g_PwmController[TOTAL_PWM_CONTROLLER] = STM32F4_PWM;

//--//

TinyCLR_Result STM32F4_Pwm_AcquirePin(const TinyCLR_Pwm_Provider* self, int32_t pin) {
    int32_t controller = (self->Index + 1);

    ptr_TIM_TypeDef treg = pwmController(controller).port;

    int32_t actualPin = STM32F4_Pwm_GetGpioPinForChannel(self, pin);

    TinyCLR_Result acquirePin = STM32F4_Gpio_AcquirePin(nullptr, actualPin);

    if (acquirePin != TinyCLR_Result::Success)
        return acquirePin;

    // relevant RCC register & bit
    __IO uint32_t* enReg = &RCC->APB1ENR;
    if ((uint32_t)treg & 0x10000) enReg = &RCC->APB2ENR;
    int enBit = 1 << (((uint32_t)treg >> 10) & 0x1F);

    if (!(*enReg & enBit)) { // not yet initialized
        *enReg |= enBit; // enable controller clock
        treg->CR1 = TIM_CR1_URS | TIM_CR1_ARPE; // double buffered update
        treg->EGR = TIM_EGR_UG; // enforce first update
        if (controller == 1 || controller == 8) {
            treg->BDTR |= TIM_BDTR_MOE; // main output enable (controller 1 & 8 only)
        }
    }

    *(__IO uint16_t*)&((uint32_t*)&treg->CCR1)[pin] = 0; // reset compare register

    // enable PWM channel
    uint32_t mode = TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1PE; // PWM1 mode, double buffered
    if (pin & 1) mode <<= 8; // 1 or 3
    __IO uint16_t* reg = &treg->CCMR1;
    if (pin & 2) reg = &treg->CCMR2; // 2 or 3
    *reg |= mode;

    return acquirePin;
}

TinyCLR_Result STM32F4_Pwm_ReleasePin(const TinyCLR_Pwm_Provider* self, int32_t pin) {
    int32_t controller = (self->Index + 1);

    ptr_TIM_TypeDef treg = pwmController(controller).port;

    uint32_t mask = 0xFF; // disable PWM channel
    if (pin & 1) mask = 0xFF00; // 1 or 3
    __IO uint16_t* reg = &treg->CCMR1;
    if (pin & 2) reg = &treg->CCMR2; // 2 or 3
    *reg &= ~mask;

    if ((treg->CCMR1 | treg->CCMR2) == 0) { // no channel active
        __IO uint32_t* enReg = &RCC->APB1ENR;
        if ((uint32_t)treg & 0x10000) enReg = &RCC->APB2ENR;
        int enBit = 1 << (((uint32_t)treg >> 10) & 0x1F);
        *enReg &= ~enBit; // disable controller clock
    }

    return TinyCLR_Result::Success;
}

void STM32F4_Pwm_GetScaleFactor(double frequency, uint32_t& period, uint32_t& scale) {
    if (frequency >= 1000.0) {
        period = (uint32_t)(((uint32_t)PWM_NANOSECONDS / frequency) + 0.5);
        scale = PWM_NANOSECONDS;
    }
    else if (frequency >= 1.0) {
        period = (uint32_t)(((uint32_t)PWM_MICROSECONDS / frequency) + 0.5);
        scale = PWM_MICROSECONDS;
    }
    else {
        period = (uint32_t)(((uint32_t)PWM_MILLISECONDS / frequency) + 0.5);
        scale = PWM_MILLISECONDS;
    }
}

double STM32F4_Pwm_GetActualFrequency(const TinyCLR_Pwm_Provider* self) {
    uint32_t period = 0;
    uint32_t scale = 0;

    int32_t controller = (self->Index + 1);

    double freq = pwmController(controller).theoryFreq;

    ptr_TIM_TypeDef treg = pwmController(controller).port;

    STM32F4_Pwm_GetScaleFactor(freq, period, scale);

    uint32_t p = period, s = scale;

    // set pre, p, & d such that:
    // pre * p = PWM_CLK * period / scale
    // pre * d = PWM_CLK * duration / scale

    uint32_t clk = PWM1_CLK_HZ;

    if ((uint32_t)treg & 0x10000) clk = PWM2_CLK_HZ; // APB2

    uint32_t pre = clk / s; // prescaler

    if (pre == 0) { // s > PWM_CLK
        uint32_t sm = s / ONE_MHZ; // scale in MHz
        clk = PWM1_CLK_MHZ;      // clock in MHz
        if ((uint32_t)treg & 0x10000) clk = PWM2_CLK_MHZ; // APB2
        if (p > 0xFFFFFFFF / PWM_MAX_CLK_MHZ) { // avoid overflow
            pre = clk;
            p /= sm;
        }
        else {
            pre = 1;
            p = p * clk / sm;
        }
    }
    else {
        while (pre > 0x10000) { // prescaler too large
            if (p >= 0x80000000) return 0;
            pre >>= 1;
            p <<= 1;
        }
    }

    if (controller != 2 && controller != 5) { // 16 bit controller
        while (p >= 0x10000) { // period too large
            if (pre > 0x8000) return 0;
            pre <<= 1;
            p >>= 1;
        }
    }

    clk = PWM1_CLK_HZ;

    if ((uint32_t)treg & 0x10000)
        clk = PWM2_CLK_HZ; // APB2

    if (p == 0 || pre == 0)
        return 0;

    uint32_t period2 = scale / ((clk / pre) / p);
    double actualFreq = (double)(scale / period2);

    while (period2 < period || actualFreq > freq) {
        p++;

        period2 = scale / ((clk / pre) / p);

        if (period2 == 0)
            return 0;

        actualFreq = (double)(scale / period2);
    }

    pwmController(controller).actualFreq = actualFreq;
    pwmController(controller).period = p;
    pwmController(controller).presc = pre;

    return actualFreq;
}

TinyCLR_Result STM32F4_Pwm_EnablePin(const TinyCLR_Pwm_Provider* self, int32_t pin) {
    int32_t controller = (self->Index + 1);

    ptr_TIM_TypeDef treg = pwmController(controller).port;

    int32_t actualPin = STM32F4_Pwm_GetGpioPinForChannel(self, pin);

    STM32F4_Gpio_EnableAlternatePin(actualPin, TinyCLR_Gpio_PinDriveMode::Input, 1, (uint32_t)pwmController(controller).gpioAlternateFunction);

    uint16_t enBit = TIM_CCER_CC1E << (4 * pin);

    treg->CCER |= enBit; // enable output

    uint16_t cr1 = treg->CR1;

    if ((cr1 & TIM_CR1_CEN) == 0) { // controller stopped
        treg->EGR = TIM_EGR_UG; // enforce register update
        treg->CR1 = cr1 | TIM_CR1_CEN; // start controller
    }

    return TinyCLR_Result::Success;
}

TinyCLR_Result STM32F4_Pwm_DisablePin(const TinyCLR_Pwm_Provider* self, int32_t pin) {
    int32_t controller = (self->Index + 1);

    ptr_TIM_TypeDef treg = pwmController(controller).port;

    int32_t actualPin = STM32F4_Pwm_GetGpioPinForChannel(self, pin);

    uint16_t ccer = treg->CCER;

    ccer &= ~(TIM_CCER_CC1E << (4 * pin));
    treg->CCER = ccer; // disable output

    STM32F4_Gpio_EnableAlternatePin(actualPin, TinyCLR_Gpio_PinDriveMode::Input, 0, GPIO_ALT_MODE(0));

    if ((ccer & (TIM_CCER_CC1E | TIM_CCER_CC2E | TIM_CCER_CC3E | TIM_CCER_CC4E)) == 0) { // idle
        treg->CR1 &= ~TIM_CR1_CEN; // stop controller
    }

    return TinyCLR_Result::Success;
}

int32_t STM32F4_Pwm_GetPinCount(const TinyCLR_Pwm_Provider* self) {
    int chnlCnt = 0;

    int32_t controller = (self->Index + 1);

    for (int p = 0; p < MAX_PWM_PER_CONTROLLER; p++) {
        if (pwmController(controller).gpioPin[p] != GPIO_PIN_NONE) {
            chnlCnt++;
        }
    }

    return chnlCnt;
}

int32_t STM32F4_Pwm_GetGpioPinForChannel(const TinyCLR_Pwm_Provider* self, int32_t pin) {
    int32_t controller = (self->Index + 1);
    return (int32_t)pwmController(controller).gpioPin[pin];
}

double STM32F4_Pwm_GetMaxFrequency(const TinyCLR_Pwm_Provider* self) {
    int32_t controller = (self->Index + 1);

    ptr_TIM_TypeDef treg = pwmController(controller).port;

    if ((uint32_t)treg & 0x10000)
        return SYSTEM_APB2_CLOCK_HZ; // max can be Systemclock / 2 MHz on some PWM

    return SYSTEM_APB1_CLOCK_HZ; //max can be Systemclock / 4 MHz on some PWM
}

double STM32F4_Pwm_GetMinFrequency(const TinyCLR_Pwm_Provider* self) {
    return STM32F4_MIN_PWM_FREQUENCY;
}

TinyCLR_Result STM32F4_Pwm_SetPulseParameters(const TinyCLR_Pwm_Provider* self, int32_t pin, double dutyCycle, bool invertPolarity) {

    int32_t controller = (self->Index + 1);

    ptr_TIM_TypeDef treg = pwmController(controller).port;

    uint32_t duration = (uint32_t)(dutyCycle * pwmController(controller).period);

    if (duration > pwmController(controller).period)
        duration = pwmController(controller).period;

    treg->PSC = pwmController(controller).presc - 1;
    treg->ARR = pwmController(controller).period - 1;

    if (controller == 2) {
        if (pin == 0)
            treg->CCR1 = duration;
        else if (pin == 1)
            treg->CCR2 = duration;
        else if (pin == 2)
            treg->CCR3 = duration;
        else if (pin == 3)
            treg->CCR4 = duration;
    }
    else {
        *(__IO uint16_t*)&((uint32_t*)&treg->CCR1)[pin] = duration;
    }

    uint32_t invBit = TIM_CCER_CC1P << (4 * pin);

    if (invertPolarity) {
        treg->CCER |= invBit;
    }
    else {
        treg->CCER &= ~invBit;
    }

    if (duration != (uint32_t)(pwmController(controller).dutyCycle[pin] * pwmController(controller).period))
        treg->EGR = TIM_EGR_UG; // enforce register update - update immidiately any changes

    pwmController(controller).invert[pin] = invertPolarity;
    pwmController(controller).dutyCycle[pin] = dutyCycle;

    return TinyCLR_Result::Success;

}

TinyCLR_Result STM32F4_Pwm_SetDesiredFrequency(const TinyCLR_Pwm_Provider* self, double& frequency) {
    int32_t controller = (self->Index + 1);

    // If current frequency is same with desired frequency, no need to re-calculate
    if (pwmController(controller).theoryFreq == frequency) {
        return TinyCLR_Result::Success;
    }

    // If detected a different, save desired frequency
    pwmController(controller).theoryFreq = frequency;

    // Calculate actual frequency base on desired frequency
    frequency = STM32F4_Pwm_GetActualFrequency(self);

    // Update channel if frequency had different
    for (int p = 0; p < MAX_PWM_PER_CONTROLLER; p++)
        if (pwmController(controller).gpioPin[p] != GPIO_PIN_NONE)
            if (STM32F4_Pwm_SetPulseParameters(self, p, pwmController(controller).dutyCycle[p], pwmController(controller).invert[p]) != TinyCLR_Result::Success)
                return TinyCLR_Result::InvalidOperation;

    return TinyCLR_Result::Success;
}

void STM32F4_Pwm_Reset() {
    for (auto i = 0; i < TOTAL_PWM_CONTROLLER; i++) {
        int32_t controller = (i + 1);

        for (int p = 0; p < MAX_PWM_PER_CONTROLLER; p++) {
            if (pwmController(controller).gpioPin[p] != GPIO_PIN_NONE) {
                STM32F4_Pwm_DisablePin(pwmProviders[i], p);
                STM32F4_Pwm_ReleasePin(pwmProviders[i], p);

                pwmController(controller).dutyCycle[p] = 0;
                pwmController(controller).invert[p] = false;
            }
        }

        pwmController(controller).theoryFreq = 0;
        pwmController(controller).actualFreq = 0;
        pwmController(controller).period = 0;
        pwmController(controller).presc = 0;
    }
}