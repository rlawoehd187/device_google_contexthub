/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <atomic.h>
#include <gpio.h>
#include <nanohubPacket.h>
#include <plat/exti.h>
#include <plat/gpio.h>
#include <platform.h>
#include <plat/syscfg.h>
#include <sensors.h>
#include <seos.h>
#include <i2c.h>
#include <timer.h>
#include <stdlib.h>
#include <string.h>
#include <variant/variant.h>

#define LPS22HB_APP_ID              APP_ID_MAKE(NANOHUB_VENDOR_STMICRO, 1)

/* Sensor defs */
#define LPS22HB_INT_CFG_REG_ADDR        0x0B
#define LPS22HB_LIR_BIT                 0x04

#define LPS22HB_WAI_REG_ADDR            0x0F
#define LPS22HB_WAI_REG_VAL             0xB1

#define LPS22HB_SOFT_RESET_REG_ADDR     0x11
#define LPS22HB_SOFT_RESET_BIT          0x04
#define LPS22HB_I2C_DIS                 0x08
#define LPS22HB_IF_ADD_INC              0x10

#define LPS22HB_ODR_REG_ADDR            0x10
#define LPS22HB_ODR_ONE_SHOT            0x00
#define LPS22HB_ODR_1_HZ                0x10
#define LPS22HB_ODR_10_HZ               0x20
#define LPS22HB_ODR_25_HZ               0x30
#define LPS22HB_ODR_50_HZ               0x40
#define LPS22HB_ODR_75_HZ               0x50

#define LPS22HB_PRESS_OUTXL_REG_ADDR    0x28
#define LPS22HB_TEMP_OUTL_REG_ADDR      0x2B

#define LPS22HB_INT1_REG_ADDR           0x23
#define LPS22HB_INT2_REG_ADDR           0x24

#define LPS22HB_INT1_PIN                GPIO_PA(4)
#define LPS22HB_INT2_PIN                GPIO_PB(0)

#define LPS22HB_HECTO_PASCAL(baro_val)  (baro_val/4096)
#define LPS22HB_CENTIGRADES(temp_val)   (temp_val/100)

#define INFO_PRINT(fmt, ...) \
    do { \
        osLog(LOG_INFO, "%s " fmt, "[LPS22HB]", ##__VA_ARGS__); \
    } while (0);

#define DEBUG_PRINT(fmt, ...) \
    do { \
        if (LPS22HB_DBG_ENABLED) { \
            osLog(LOG_DEBUG, "%s " fmt, "[LPS22HB]", ##__VA_ARGS__); \
        } \
    } while (0);

#define ERROR_PRINT(fmt, ...) \
    do { \
        osLog(LOG_ERROR, "%s " fmt, "[LPS22HB]", ##__VA_ARGS__); \
    } while (0);

/* DO NOT MODIFY, just to avoid compiler error if not defined using FLAGS */
#ifndef LPS22HB_DBG_ENABLED
#define LPS22HB_DBG_ENABLED                           0
#endif /* LPS22HB_DBG_ENABLED */

enum lps22hbSensorEvents
{
    EVT_COMM_DONE = EVT_APP_START + 1,
    EVT_INT1_RAISED,
    EVT_SENSOR_BARO_TIMER,
    EVT_SENSOR_TEMP_TIMER,
    EVT_TEST,
};

enum lps22hbSensorState {
    SENSOR_BOOT,
    SENSOR_VERIFY_ID,
    SENSOR_INIT,
    SENSOR_BARO_POWER_UP,
    SENSOR_BARO_POWER_DOWN,
    SENSOR_TEMP_POWER_UP,
    SENSOR_TEMP_POWER_DOWN,
    SENSOR_READ_SAMPLES,
    SENSOR_DO_NOTHING,
};

#ifndef LPS22HB_I2C_BUS_ID
#error "LPS22HB_I2C_BUS_ID is not defined; please define in variant.h"
#endif

#ifndef LPS22HB_I2C_SPEED
#error "LPS22HB_I2C_SPEED is not defined; please define in variant.h"
#endif

#ifndef LPS22HB_I2C_ADDR
#error "LPS22HB_I2C_ADDR is not defined; please define in variant.h"
#endif

enum lps22hbSensorIndex {
    BARO = 0,
    TEMP,
    NUM_OF_SENSOR,
};

//#define NUM_OF_SENSOR 1

struct lps22hbSensor {
    uint32_t handle;
};

#define LPS22HB_MAX_PENDING_I2C_REQUESTS   4
#define LPS22HB_MAX_I2C_TRANSFER_SIZE      6

struct I2cTransfer
{
    size_t tx;
    size_t rx;
    int err;
    uint8_t txrxBuf[LPS22HB_MAX_I2C_TRANSFER_SIZE];
    uint8_t state;
    bool inUse;
};

/* Task structure */
struct lps22hbTask {
    uint32_t tid;

    /* timer */
    uint32_t baroTimerHandle;
    uint32_t tempTimerHandle;

    /* sensor flags */
    bool baroOn;
    bool baroReading;
    bool baroWantRead;
    bool tempOn;
    bool tempReading;
    bool tempWantRead;

    //int sensLastRead;

    struct I2cTransfer transfers[LPS22HB_MAX_PENDING_I2C_REQUESTS];

    /* Communication functions */
    void (*comm_tx)(uint8_t addr, uint8_t data, uint32_t delay, uint8_t state);
    void (*comm_rx)(uint8_t addr, uint16_t len, uint32_t delay, uint8_t state);

    /* sensors */
    struct lps22hbSensor sensors[NUM_OF_SENSOR];
};

static struct lps22hbTask mTask;

// Allocate a buffer and mark it as in use with the given state, or return NULL
// if no buffers available. Must *not* be called from interrupt context.
static struct I2cTransfer *allocXfer(uint8_t state)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(mTask.transfers); i++) {
        if (!mTask.transfers[i].inUse) {
            mTask.transfers[i].inUse = true;
            mTask.transfers[i].state = state;
            return &mTask.transfers[i];
        }
    }

    ERROR_PRINT("Ran out of i2c buffers!");
    return NULL;
}

static void i2cCallback(void *cookie, size_t tx, size_t rx, int err)
{
    struct I2cTransfer *xfer = cookie;

    xfer->tx = tx;
    xfer->rx = rx;
    xfer->err = err;

    osEnqueuePrivateEvt(EVT_COMM_DONE, cookie, NULL, mTask.tid);
    if (err != 0)
        ERROR_PRINT("i2c error (tx: %d, rx: %d, err: %d)\n", tx, rx, err);
}

static void i2c_read(uint8_t addr, uint16_t len, uint32_t delay, uint8_t state)
{
    struct I2cTransfer *xfer = allocXfer(state);
    //int ret = -1;

    if (xfer != NULL) {
        xfer->txrxBuf[0] = 0x80 | addr;
        i2cMasterTxRx(LPS22HB_I2C_BUS_ID, LPS22HB_I2C_ADDR, xfer->txrxBuf, 1, xfer->txrxBuf, len, i2cCallback, xfer);
    }
}

static void i2c_write(uint8_t addr, uint8_t data, uint32_t delay, uint8_t state)
{
    struct I2cTransfer *xfer = allocXfer(state);
    //int ret = -1;

    if (xfer != NULL) {
        xfer->txrxBuf[0] = addr;
        xfer->txrxBuf[1] = data;
        i2cMasterTx(LPS22HB_I2C_BUS_ID, LPS22HB_I2C_ADDR, xfer->txrxBuf, 2, i2cCallback, xfer);
    }

    //return (ret == 0);
}

/* Sensor Info */
static void sensorBaroTimerCallback(uint32_t timerId, void *data)
{
    osEnqueuePrivateEvt(EVT_SENSOR_BARO_TIMER, data, NULL, mTask.tid);
}

static void sensorTempTimerCallback(uint32_t timerId, void *data)
{
    osEnqueuePrivateEvt(EVT_SENSOR_TEMP_TIMER, data, NULL, mTask.tid);
}

#define DEC_INFO(name, type, axis, inter, samples, rates, raw, scale, bias) \
    .sensorName = name, \
    .sensorType = type, \
    .numAxis = axis, \
    .interrupt = inter, \
    .minSamples = samples, \
    .supportedRates = rates, \
    .rawType = raw, \
    .rawScale = scale, \
    .biasType = bias

static uint32_t lps22hbRates[] = {
    SENSOR_HZ(1.0f),
    SENSOR_HZ(10.0f),
    SENSOR_HZ(25.0f),
    SENSOR_HZ(50.0f),
    SENSOR_HZ(75.0f),
    0
};

// should match "supported rates in length" and be the timer length for that rate in nanosecs
static const uint64_t lps22hbRatesRateVals[] =
{
    1 * 1000000000ULL,
    1000000000ULL / 10,
    1000000000ULL / 25,
    1000000000ULL / 50,
    1000000000ULL / 75,
};


static const struct SensorInfo lps22hbSensorInfo[NUM_OF_SENSOR] =
{
    { DEC_INFO("Pressure", SENS_TYPE_BARO, NUM_AXIS_EMBEDDED, NANOHUB_INT_NONWAKEUP,
        300, lps22hbRates, 0, 0, 0) },
    { DEC_INFO("Temperature", SENS_TYPE_TEMP, NUM_AXIS_EMBEDDED, NANOHUB_INT_NONWAKEUP,
        20, lps22hbRates, 0, 0, 0) },
};

/* Sensor Operations */
static bool baroPower(bool on, void *cookie)
{
    bool oldMode = mTask.baroOn || mTask.tempOn;
    bool newMode = on || mTask.tempOn;
    uint32_t state = on ? SENSOR_BARO_POWER_UP : SENSOR_BARO_POWER_DOWN;

    DEBUG_PRINT("baroPower %s\n", on ? "enable" : "disable");
    if (!on && mTask.baroTimerHandle) {
        timTimerCancel(mTask.baroTimerHandle);
        mTask.baroTimerHandle = 0;
        mTask.baroReading = false;
    }

    if (oldMode != newMode) {
        if (on)
            mTask.comm_tx(LPS22HB_ODR_REG_ADDR, LPS22HB_ODR_10_HZ, 0, state);
        else
            mTask.comm_tx(LPS22HB_ODR_REG_ADDR, LPS22HB_ODR_ONE_SHOT, 0, state);
    } else
        sensorSignalInternalEvt(mTask.sensors[BARO].handle,
                    SENSOR_INTERNAL_EVT_POWER_STATE_CHG, on, 0);

    mTask.baroReading = false;
    mTask.baroOn = on;
    return true;
}

static bool baroFwUpload(void *cookie)
{
    return sensorSignalInternalEvt(mTask.sensors[BARO].handle, SENSOR_INTERNAL_EVT_FW_STATE_CHG, 1, 0);
}

static bool baroSetRate(uint32_t rate, uint64_t latency, void *cookie)
{
    DEBUG_PRINT("baroSetRate %ld (%lld)\n", rate, latency);
    if (mTask.baroTimerHandle)
        timTimerCancel(mTask.baroTimerHandle);

    mTask.baroTimerHandle = timTimerSet(sensorTimerLookupCommon(lps22hbRates,
                lps22hbRatesRateVals, rate), 0, 50, sensorBaroTimerCallback, NULL, false);

    return sensorSignalInternalEvt(mTask.sensors[BARO].handle,
                SENSOR_INTERNAL_EVT_RATE_CHG, rate, latency);
}

static bool baroFlush(void *cookie)
{
    return osEnqueueEvt(sensorGetMyEventType(SENS_TYPE_BARO), SENSOR_DATA_EVENT_FLUSH, NULL);
}

static bool tempPower(bool on, void *cookie)
{
    bool oldMode = mTask.baroOn || mTask.tempOn;
    bool newMode = on || mTask.baroOn;
    uint32_t state = on ? SENSOR_TEMP_POWER_UP : SENSOR_TEMP_POWER_DOWN;

    DEBUG_PRINT("tempPower %s\n", on ? "enable" : "disable");
    if (!on && mTask.tempTimerHandle) {
        timTimerCancel(mTask.tempTimerHandle);
        mTask.tempTimerHandle = 0;
        mTask.tempReading = false;
    }

    if (oldMode != newMode) {
        if (on)
            mTask.comm_tx(LPS22HB_ODR_REG_ADDR, LPS22HB_ODR_10_HZ, 0, state);
        else
            mTask.comm_tx(LPS22HB_ODR_REG_ADDR, LPS22HB_ODR_ONE_SHOT, 0, state);
    } else
        sensorSignalInternalEvt(mTask.sensors[TEMP].handle,
                    SENSOR_INTERNAL_EVT_POWER_STATE_CHG, on, 0);

    mTask.tempReading = false;
    mTask.tempOn = on;
    return true;
}

static bool tempFwUpload(void *cookie)
{
    return sensorSignalInternalEvt(mTask.sensors[TEMP].handle, SENSOR_INTERNAL_EVT_FW_STATE_CHG, 1, 0);
}

static bool tempSetRate(uint32_t rate, uint64_t latency, void *cookie)
{
    if (mTask.tempTimerHandle)
        timTimerCancel(mTask.tempTimerHandle);

    DEBUG_PRINT("tempSetRate %ld (%lld)\n", rate, latency);
    mTask.tempTimerHandle = timTimerSet(sensorTimerLookupCommon(lps22hbRates,
                lps22hbRatesRateVals, rate), 0, 50, sensorTempTimerCallback, NULL, false);

    return sensorSignalInternalEvt(mTask.sensors[TEMP].handle,
                SENSOR_INTERNAL_EVT_RATE_CHG, rate, latency);
}

static bool tempFlush(void *cookie)
{
    return osEnqueueEvt(sensorGetMyEventType(SENS_TYPE_BARO), SENSOR_DATA_EVENT_FLUSH, NULL);
}

#define DEC_OPS(power, firmware, rate, flush, cal, cfg) \
    .sensorPower = power, \
    .sensorFirmwareUpload = firmware, \
    .sensorSetRate = rate, \
    .sensorFlush = flush, \
    .sensorCalibrate = cal, \
    .sensorCfgData = cfg

static const struct SensorOps lps22hbSensorOps[NUM_OF_SENSOR] =
{
    { DEC_OPS(baroPower, baroFwUpload, baroSetRate, baroFlush, NULL, NULL) },
    { DEC_OPS(tempPower, tempFwUpload, tempSetRate, tempFlush, NULL, NULL) },
};

static uint8_t *baro_samples;
static uint8_t *temp_samples;
static void handleCommDoneEvt(const void* evtData)
{
    uint8_t i;
    int baro_val;
    short temp_val;
    //uint32_t state = (uint32_t)evtData;
    union EmbeddedDataPoint sample;
    struct I2cTransfer *xfer = (struct I2cTransfer *)evtData;

    switch (xfer->state) {
    case SENSOR_BOOT:
        mTask.comm_rx(LPS22HB_WAI_REG_ADDR, 1, 1, SENSOR_VERIFY_ID);
        break;

    case SENSOR_VERIFY_ID:
        /* Check the sensor ID */
        if (xfer->err != 0 || xfer->txrxBuf[0] != LPS22HB_WAI_REG_VAL) {
            INFO_PRINT("WAI returned is: %02x\n", xfer->txrxBuf[0]);
            break;
        }


        INFO_PRINT("Device ID is correct! (%02x)\n", xfer->txrxBuf[0]);
        for (i = 0; i < NUM_OF_SENSOR; i++)
            sensorRegisterInitComplete(mTask.sensors[i].handle);

        /* TEST the environment in standalone mode */
        //osEnqueuePrivateEvt(EVT_TEST, NULL, NULL, mTask.tid);
        break;

    case SENSOR_INIT:
        for (i = 0; i < NUM_OF_SENSOR; i++)
            sensorRegisterInitComplete(mTask.sensors[i].handle);
        break;

    case SENSOR_BARO_POWER_UP:
        sensorSignalInternalEvt(mTask.sensors[BARO].handle,
                    SENSOR_INTERNAL_EVT_POWER_STATE_CHG, true, 0);
        break;

    case SENSOR_BARO_POWER_DOWN:
        sensorSignalInternalEvt(mTask.sensors[BARO].handle,
                    SENSOR_INTERNAL_EVT_POWER_STATE_CHG, false, 0);
        break;

    case SENSOR_TEMP_POWER_UP:
        sensorSignalInternalEvt(mTask.sensors[TEMP].handle,
                    SENSOR_INTERNAL_EVT_POWER_STATE_CHG, true, 0);
        break;

    case SENSOR_TEMP_POWER_DOWN:
        sensorSignalInternalEvt(mTask.sensors[TEMP].handle,
                    SENSOR_INTERNAL_EVT_POWER_STATE_CHG, false, 0);
        break;

    case SENSOR_READ_SAMPLES:
        if (mTask.baroOn && mTask.baroWantRead) {
            mTask.baroWantRead = false;
            baro_samples = xfer->txrxBuf;

            baro_val = ((baro_samples[2] << 16) & 0xff0000) |
                    ((baro_samples[1] << 8) & 0xff00) |
                    (baro_samples[0]);

            mTask.baroReading = false;
            sample.fdata = LPS22HB_HECTO_PASCAL((float)baro_val);
            //osLog(LOG_INFO, "baro: %p\n", sample.vptr);
            osEnqueueEvt(sensorGetMyEventType(SENS_TYPE_BARO), sample.vptr, NULL);
        }

        if (mTask.tempOn && mTask.tempWantRead) {
            mTask.tempWantRead = false;
            temp_samples = &xfer->txrxBuf[3];

            temp_val  = ((temp_samples[1] << 8) & 0xff00) |
                    (temp_samples[0]);

            mTask.tempReading = false;
            sample.fdata = LPS22HB_CENTIGRADES((float)temp_val);
            //osLog(LOG_INFO, "temp: %p\n", sample.vptr);
            osEnqueueEvt(sensorGetMyEventType(SENS_TYPE_TEMP), sample.vptr, NULL);
        }

        break;

    case SENSOR_DO_NOTHING:
    default:
        break;
    }

    xfer->inUse = false;
}

static void handleEvent(uint32_t evtType, const void* evtData)
{
    switch (evtType) {
    case EVT_APP_START:
        INFO_PRINT("EVT_APP_START\n");
        osEventUnsubscribe(mTask.tid, EVT_APP_START);

        mTask.comm_tx(LPS22HB_SOFT_RESET_REG_ADDR,
                    LPS22HB_SOFT_RESET_BIT, 0, SENSOR_BOOT);
        break;

    case EVT_COMM_DONE:
        //INFO_PRINT("EVT_COMM_DONE %d\n", (int)evtData);
        handleCommDoneEvt(evtData);
        break;

    case EVT_SENSOR_BARO_TIMER:
        //INFO_PRINT("EVT_SENSOR_BARO_TIMER\n");

        mTask.baroWantRead = true;

        /* Start sampling for a value */
        if (!mTask.baroReading && !mTask.tempReading) {
            mTask.baroReading = true;

            mTask.comm_rx(LPS22HB_PRESS_OUTXL_REG_ADDR, 5, 1, SENSOR_READ_SAMPLES);
        }

        break;

    case EVT_SENSOR_TEMP_TIMER:
        //INFO_PRINT("EVT_SENSOR_TEMP_TIMER\n");

        mTask.tempWantRead = true;

        /* Start sampling for a value */
        if (!mTask.baroReading && !mTask.tempReading) {
            mTask.tempReading = true;

            mTask.comm_rx(LPS22HB_PRESS_OUTXL_REG_ADDR, 5, 1, SENSOR_READ_SAMPLES);
        }

        break;

    case EVT_INT1_RAISED:
        INFO_PRINT("EVT_INT1_RAISED\n");
        break;

    case EVT_TEST:
        INFO_PRINT("EVT_TEST\n");

        baroPower(true, NULL);
        tempPower(true, NULL);
        baroSetRate(SENSOR_HZ(1), 0, NULL);
        tempSetRate(SENSOR_HZ(1), 0, NULL);
        break;

    default:
        break;
    }

}

static bool startTask(uint32_t task_id)
{
    uint8_t i;

    mTask.tid = task_id;

    INFO_PRINT("task started\n");

    mTask.baroOn = mTask.tempOn = false;
    mTask.baroReading = mTask.tempReading = false;

    /* Init the communication part */
    i2cMasterRequest(LPS22HB_I2C_BUS_ID, LPS22HB_I2C_SPEED);

    mTask.comm_tx = i2c_write;
    mTask.comm_rx = i2c_read;

    for (i = 0; i < NUM_OF_SENSOR; i++) {
        mTask.sensors[i].handle =
            sensorRegister(&lps22hbSensorInfo[i], &lps22hbSensorOps[i], NULL, false);
    }

    osEventSubscribe(mTask.tid, EVT_APP_START);

    return true;
}

static void endTask(void)
{
    INFO_PRINT("task ended\n");
}

INTERNAL_APP_INIT(LPS22HB_APP_ID, 0, startTask, endTask, handleEvent);
