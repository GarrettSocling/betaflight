/*
 * This file is part of Cleanflight.
 *
 * Cleanflight is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Cleanflight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Cleanflight.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#include "platform.h"

#include "build/debug.h"

#include "common/axis.h"
#include "common/maths.h"
#include "common/filter.h"

#include "config/parameter_group.h"
#include "config/parameter_group_ids.h"

#include "drivers/accgyro.h"
#include "drivers/accgyro_adxl345.h"
#include "drivers/accgyro_bma280.h"
#include "drivers/accgyro_fake.h"
#include "drivers/accgyro_l3g4200d.h"
#include "drivers/accgyro_l3gd20.h"
#include "drivers/accgyro_lsm303dlhc.h"
#include "drivers/accgyro_mma845x.h"
#include "drivers/accgyro_mpu.h"
#include "drivers/accgyro_mpu3050.h"
#include "drivers/accgyro_mpu6050.h"
#include "drivers/accgyro_mpu6500.h"
#include "drivers/accgyro_spi_icm20689.h"
#include "drivers/accgyro_spi_mpu6000.h"
#include "drivers/accgyro_spi_mpu6500.h"
#include "drivers/accgyro_spi_mpu9250.h"
#include "drivers/bus_spi.h"
#include "drivers/gyro_sync.h"
#include "drivers/io.h"
#include "drivers/system.h"

#include "fc/runtime_config.h"

#include "io/beeper.h"
#include "io/statusindicator.h"

#include "scheduler/scheduler.h"

#include "sensors/boardalignment.h"
#include "sensors/gyro.h"
#include "sensors/sensors.h"

#ifdef USE_HARDWARE_REVISION_DETECTION
#include "hardware_revision.h"
#endif

gyro_t gyro;

STATIC_UNIT_TESTED gyroDev_t gyroDev0;
static int16_t gyroTemperature0;

static int32_t gyroADC[XYZ_AXIS_COUNT];

STATIC_UNIT_TESTED int32_t gyroZero[XYZ_AXIS_COUNT] = { 0, 0, 0 };
static uint16_t calibratingG = 0;

static filterApplyFnPtr softLpfFilterApplyFn;
static void *softLpfFilter[3];
static filterApplyFnPtr notchFilter1ApplyFn;
static void *notchFilter1[3];
static filterApplyFnPtr notchFilter2ApplyFn;
static void *notchFilter2[3];

#define DEBUG_GYRO_CALIBRATION 3

#ifdef STM32F10X
#define GYRO_SYNC_DENOM_DEFAULT 8
#elif defined(USE_GYRO_SPI_MPU6000) || defined(USE_GYRO_SPI_MPU6500)  || defined(USE_GYRO_SPI_ICM20689)
#define GYRO_SYNC_DENOM_DEFAULT 1
#else
#define GYRO_SYNC_DENOM_DEFAULT 4
#endif

PG_REGISTER_WITH_RESET_TEMPLATE(gyroConfig_t, gyroConfig, PG_GYRO_CONFIG, 0);

PG_RESET_TEMPLATE(gyroConfig_t, gyroConfig,
    .gyro_lpf = GYRO_LPF_256HZ,
    .gyro_sync_denom = GYRO_SYNC_DENOM_DEFAULT,
    .gyro_soft_lpf_type = FILTER_PT1,
    .gyro_soft_lpf_hz = 90,
    .gyro_soft_notch_hz_1 = 400,
    .gyro_soft_notch_cutoff_1 = 300,
    .gyro_soft_notch_hz_2 = 200,
    .gyro_soft_notch_cutoff_2 = 100,
    .gyro_align = ALIGN_DEFAULT,
    .gyroMovementCalibrationThreshold = 32
);

#if defined(USE_GYRO_MPU6050) || defined(USE_GYRO_MPU3050) || defined(USE_GYRO_MPU6500) || defined(USE_GYRO_SPI_MPU6500) || defined(USE_GYRO_SPI_MPU6000) || defined(USE_ACC_MPU6050) || defined(USE_GYRO_SPI_MPU9250) || defined(USE_GYRO_SPI_ICM20689)
static const extiConfig_t *selectMPUIntExtiConfig(void)
{
#if defined(MPU_INT_EXTI)
    static const extiConfig_t mpuIntExtiConfig = { .tag = IO_TAG(MPU_INT_EXTI) };
    return &mpuIntExtiConfig;
#elif defined(USE_HARDWARE_REVISION_DETECTION)
    return selectMPUIntExtiConfigByHardwareRevision();
#else
    return NULL;
#endif
}
#endif

const mpuConfiguration_t *gyroMpuConfiguration(void)
{
    return &gyroDev0.mpuConfiguration;
}
const mpuDetectionResult_t *gyroMpuDetectionResult(void)
{
    return &gyroDev0.mpuDetectionResult;
}

STATIC_UNIT_TESTED gyroSensor_e gyroDetect(gyroDev_t *dev)
{
    gyroSensor_e gyroHardware = GYRO_DEFAULT;

    dev->gyroAlign = ALIGN_DEFAULT;

    switch(gyroHardware) {
    case GYRO_DEFAULT:
#ifdef USE_GYRO_MPU6050
    case GYRO_MPU6050:
        if (mpu6050GyroDetect(dev)) {
            gyroHardware = GYRO_MPU6050;
#ifdef GYRO_MPU6050_ALIGN
            dev->gyroAlign = GYRO_MPU6050_ALIGN;
#endif
            break;
        }
#endif

#ifdef USE_GYRO_L3G4200D
    case GYRO_L3G4200D:
        if (l3g4200dDetect(dev)) {
            gyroHardware = GYRO_L3G4200D;
#ifdef GYRO_L3G4200D_ALIGN
            dev->gyroAlign = GYRO_L3G4200D_ALIGN;
#endif
            break;
        }
#endif

#ifdef USE_GYRO_MPU3050
    case GYRO_MPU3050:
        if (mpu3050Detect(dev)) {
            gyroHardware = GYRO_MPU3050;
#ifdef GYRO_MPU3050_ALIGN
            dev->gyroAlign = GYRO_MPU3050_ALIGN;
#endif
            break;
        }
#endif

#ifdef USE_GYRO_L3GD20
    case GYRO_L3GD20:
        if (l3gd20Detect(dev)) {
            gyroHardware = GYRO_L3GD20;
#ifdef GYRO_L3GD20_ALIGN
            dev->gyroAlign = GYRO_L3GD20_ALIGN;
#endif
            break;
        }
#endif

#ifdef USE_GYRO_SPI_MPU6000
    case GYRO_MPU6000:
        if (mpu6000SpiGyroDetect(dev)) {
            gyroHardware = GYRO_MPU6000;
#ifdef GYRO_MPU6000_ALIGN
            dev->gyroAlign = GYRO_MPU6000_ALIGN;
#endif
            break;
        }
#endif

#if defined(USE_GYRO_MPU6500) || defined(USE_GYRO_SPI_MPU6500)
    case GYRO_MPU6500:
    case GYRO_ICM20608G:
    case GYRO_ICM20602:
#ifdef USE_GYRO_SPI_MPU6500
        if (mpu6500GyroDetect(dev) || mpu6500SpiGyroDetect(dev)) {
#else
        if (mpu6500GyroDetect(dev)) {
#endif
            switch(dev->mpuDetectionResult.sensor) {
            case MPU_9250_SPI:
                gyroHardware = GYRO_MPU9250;
                break;
            case ICM_20608_SPI:
                gyroHardware = GYRO_ICM20608G;
                break;
            case ICM_20602_SPI:
                gyroHardware = GYRO_ICM20602;
                break;
            default:
                gyroHardware = GYRO_MPU6500;
            }
#ifdef GYRO_MPU6500_ALIGN
            dev->gyroAlign = GYRO_MPU6500_ALIGN;
#endif
            break;
        }
#endif

#ifdef USE_GYRO_SPI_MPU9250
    case GYRO_MPU9250:

        if (mpu9250SpiGyroDetect(dev))
        {
            gyroHardware = GYRO_MPU9250;
#ifdef GYRO_MPU9250_ALIGN
            dev->gyroAlign = GYRO_MPU9250_ALIGN;
#endif
        break;
    }
#endif

#ifdef USE_GYRO_SPI_ICM20689
    case GYRO_ICM20689:
        if (icm20689SpiGyroDetect(dev))
        {
            gyroHardware = GYRO_ICM20689;
#ifdef GYRO_ICM20689_ALIGN
            dev->gyroAlign = GYRO_ICM20689_ALIGN;
#endif
            break;
        }
#endif

#ifdef USE_FAKE_GYRO
    case GYRO_FAKE:
        if (fakeGyroDetect(dev)) {
            gyroHardware = GYRO_FAKE;
            break;
        }
#endif
    default:
        gyroHardware = GYRO_NONE;
    }

    if (gyroHardware != GYRO_NONE) {
        detectedSensors[SENSOR_INDEX_GYRO] = gyroHardware;
        sensorsSet(SENSOR_GYRO);
    }


    return gyroHardware;
}

bool gyroInit(void)
{
    memset(&gyro, 0, sizeof(gyro));
#if defined(USE_GYRO_MPU6050) || defined(USE_GYRO_MPU3050) || defined(USE_GYRO_MPU6500) || defined(USE_GYRO_SPI_MPU6500) || defined(USE_GYRO_SPI_MPU6000) || defined(USE_ACC_MPU6050) || defined(USE_GYRO_SPI_MPU9250) || defined(USE_GYRO_SPI_ICM20689)
    gyroDev0.mpuIntExtiConfig = selectMPUIntExtiConfig();
    mpuDetect(&gyroDev0);
    mpuResetFn = gyroDev0.mpuConfiguration.resetFn;
#endif
    const gyroSensor_e gyroHardware = gyroDetect(&gyroDev0);
    if (gyroHardware == GYRO_NONE) {
        return false;
    }

    switch (gyroHardware) {
    case GYRO_MPU6500:
    case GYRO_MPU9250:
    case GYRO_ICM20689:
    case GYRO_ICM20608G:
    case GYRO_ICM20602:
        // do nothing, as gyro supports 32kHz
        break;
    default:
        // gyro does not support 32kHz
        gyroConfigMutable()->gyro_use_32khz = false;
        break;
    }

    // Must set gyro sample rate before initialisation
    gyro.targetLooptime = gyroSetSampleRate(&gyroDev0, gyroConfig()->gyro_lpf, gyroConfig()->gyro_sync_denom, gyroConfig()->gyro_use_32khz);
    gyroDev0.lpf = gyroConfig()->gyro_lpf;
    gyroDev0.init(&gyroDev0);
    if (gyroConfig()->gyro_align != ALIGN_DEFAULT) {
        gyroDev0.gyroAlign = gyroConfig()->gyro_align;
    }
    gyroInitFilters();
    return true;
}

void gyroInitFilters(void)
{
    static biquadFilter_t gyroFilterLPF[XYZ_AXIS_COUNT];
    static pt1Filter_t gyroFilterPt1[XYZ_AXIS_COUNT];
    static firFilterDenoise_t gyroDenoiseState[XYZ_AXIS_COUNT];
    static biquadFilter_t gyroFilterNotch_1[XYZ_AXIS_COUNT];
    static biquadFilter_t gyroFilterNotch_2[XYZ_AXIS_COUNT];

    softLpfFilterApplyFn = nullFilterApply;
    notchFilter1ApplyFn = nullFilterApply;
    notchFilter2ApplyFn = nullFilterApply;

    uint32_t gyroFrequencyNyquist = (1.0f / (gyro.targetLooptime * 0.000001f)) / 2; // No rounding needed

    if (gyroConfig()->gyro_soft_lpf_hz && gyroConfig()->gyro_soft_lpf_hz <= gyroFrequencyNyquist) {  // Initialisation needs to happen once samplingrate is known
        if (gyroConfig()->gyro_soft_lpf_type == FILTER_BIQUAD) {
            softLpfFilterApplyFn = (filterApplyFnPtr)biquadFilterApply;
            for (int axis = 0; axis < 3; axis++) {
                softLpfFilter[axis] = &gyroFilterLPF[axis];
                biquadFilterInitLPF(softLpfFilter[axis], gyroConfig()->gyro_soft_lpf_hz, gyro.targetLooptime);
            }
        } else if (gyroConfig()->gyro_soft_lpf_type == FILTER_PT1) {
            softLpfFilterApplyFn = (filterApplyFnPtr)pt1FilterApply;
            const float gyroDt = (float) gyro.targetLooptime * 0.000001f;
            for (int axis = 0; axis < 3; axis++) {
                softLpfFilter[axis] = &gyroFilterPt1[axis];
                pt1FilterInit(softLpfFilter[axis], gyroConfig()->gyro_soft_lpf_hz, gyroDt);
            }
        } else {
            softLpfFilterApplyFn = (filterApplyFnPtr)firFilterDenoiseUpdate;
            for (int axis = 0; axis < 3; axis++) {
                softLpfFilter[axis] = &gyroDenoiseState[axis];
                firFilterDenoiseInit(softLpfFilter[axis], gyroConfig()->gyro_soft_lpf_hz, gyro.targetLooptime);
            }
        }
    }

    if (gyroConfig()->gyro_soft_notch_hz_1 && gyroConfig()->gyro_soft_notch_hz_1 <= gyroFrequencyNyquist) {
        notchFilter1ApplyFn = (filterApplyFnPtr)biquadFilterApply;
        const float gyroSoftNotchQ1 = filterGetNotchQ(gyroConfig()->gyro_soft_notch_hz_1, gyroConfig()->gyro_soft_notch_cutoff_1);
        for (int axis = 0; axis < 3; axis++) {
            notchFilter1[axis] = &gyroFilterNotch_1[axis];
            biquadFilterInit(notchFilter1[axis], gyroConfig()->gyro_soft_notch_hz_1, gyro.targetLooptime, gyroSoftNotchQ1, FILTER_NOTCH);
        }
    }
    if (gyroConfig()->gyro_soft_notch_hz_2 && gyroConfig()->gyro_soft_notch_hz_2 <= gyroFrequencyNyquist) {
        notchFilter2ApplyFn = (filterApplyFnPtr)biquadFilterApply;
        const float gyroSoftNotchQ2 = filterGetNotchQ(gyroConfig()->gyro_soft_notch_hz_2, gyroConfig()->gyro_soft_notch_cutoff_2);
        for (int axis = 0; axis < 3; axis++) {
            notchFilter2[axis] = &gyroFilterNotch_2[axis];
            biquadFilterInit(notchFilter2[axis], gyroConfig()->gyro_soft_notch_hz_2, gyro.targetLooptime, gyroSoftNotchQ2, FILTER_NOTCH);
        }
    }
}

bool isGyroCalibrationComplete(void)
{
    return calibratingG == 0;
}

static bool isOnFinalGyroCalibrationCycle(void)
{
    return calibratingG == 1;
}

static uint16_t gyroCalculateCalibratingCycles(void)
{
    return (CALIBRATING_GYRO_CYCLES / gyro.targetLooptime) * CALIBRATING_GYRO_CYCLES;
}

static bool isOnFirstGyroCalibrationCycle(void)
{
    return calibratingG == gyroCalculateCalibratingCycles();
}

void gyroSetCalibrationCycles(void)
{
    calibratingG = gyroCalculateCalibratingCycles();
}

STATIC_UNIT_TESTED void performGyroCalibration(uint8_t gyroMovementCalibrationThreshold)
{
    static int32_t g[3];
    static stdev_t var[3];

    for (int axis = 0; axis < 3; axis++) {

        // Reset g[axis] at start of calibration
        if (isOnFirstGyroCalibrationCycle()) {
            g[axis] = 0;
            devClear(&var[axis]);
        }

        // Sum up CALIBRATING_GYRO_CYCLES readings
        g[axis] += gyroADC[axis];
        devPush(&var[axis], gyroADC[axis]);

        // Reset global variables to prevent other code from using un-calibrated data
        gyroADC[axis] = 0;
        gyroZero[axis] = 0;

        if (isOnFinalGyroCalibrationCycle()) {
            float dev = devStandardDeviation(&var[axis]);

            DEBUG_SET(DEBUG_GYRO, DEBUG_GYRO_CALIBRATION, lrintf(dev));

            // check deviation and startover in case the model was moved
            if (gyroMovementCalibrationThreshold && dev > gyroMovementCalibrationThreshold) {
                gyroSetCalibrationCycles();
                return;
            }
            gyroZero[axis] = (g[axis] + (gyroCalculateCalibratingCycles() / 2)) / gyroCalculateCalibratingCycles();
        }
    }

    if (isOnFinalGyroCalibrationCycle()) {
        schedulerResetTaskStatistics(TASK_SELF); // so calibration cycles do not pollute tasks statistics
        beeper(BEEPER_GYRO_CALIBRATED);
    }
    calibratingG--;

}

#if defined(GYRO_USES_SPI) && defined(USE_MPU_DATA_READY_SIGNAL)
static bool gyroUpdateISR(gyroDev_t* gyroDev)
{
    if (!gyroDev->dataReady || !gyroDev->read(gyroDev)) {
        return false;
    }
#ifdef DEBUG_MPU_DATA_READY_INTERRUPT
    debug[2] = (uint16_t)(micros() & 0xffff);
#endif
    gyroDev->dataReady = false;
    // move gyro data into 32-bit variables to avoid overflows in calculations
    gyroADC[X] = gyroDev->gyroADCRaw[X];
    gyroADC[Y] = gyroDev->gyroADCRaw[Y];
    gyroADC[Z] = gyroDev->gyroADCRaw[Z];

    alignSensors(gyroADC, gyroDev->gyroAlign);

    for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
        gyroADC[axis] -= gyroZero[axis];
        // scale gyro output to degrees per second
        float gyroADCf = (float)gyroADC[axis] * gyroDev->scale;
        gyroADCf = softLpfFilterApplyFn(softLpfFilter[axis], gyroADCf);
        gyroADCf = notchFilter1ApplyFn(notchFilter1[axis], gyroADCf);
        gyroADCf = notchFilter2ApplyFn(notchFilter2[axis], gyroADCf);
        gyro.gyroADCf[axis] = gyroADCf;
    }
    return true;
}
#endif

void gyroUpdate(void)
{
    // range: +/- 8192; +/- 2000 deg/sec
    if (gyroDev0.update) {
        // if the gyro update function is set then return, since the gyro is read in gyroUpdateISR
        return;
    }
    if (!gyroDev0.read(&gyroDev0)) {
        return;
    }
    gyroDev0.dataReady = false;
    // move gyro data into 32-bit variables to avoid overflows in calculations
    gyroADC[X] = gyroDev0.gyroADCRaw[X];
    gyroADC[Y] = gyroDev0.gyroADCRaw[Y];
    gyroADC[Z] = gyroDev0.gyroADCRaw[Z];

    alignSensors(gyroADC, gyroDev0.gyroAlign);

    const bool calibrationComplete = isGyroCalibrationComplete();
    if (calibrationComplete) {
#if defined(GYRO_USES_SPI) && defined(USE_MPU_DATA_READY_SIGNAL)
        // SPI-based gyro so can read and update in ISR
        if (gyroConfig()->gyro_isr_update) {
            mpuGyroSetIsrUpdate(&gyroDev0, gyroUpdateISR);
            return;
        }
#endif
#ifdef DEBUG_MPU_DATA_READY_INTERRUPT
        debug[3] = (uint16_t)(micros() & 0xffff);
#endif
    } else {
        performGyroCalibration(gyroConfig()->gyroMovementCalibrationThreshold);
    }

    for (int axis = 0; axis < XYZ_AXIS_COUNT; axis++) {
        gyroADC[axis] -= gyroZero[axis];
        // scale gyro output to degrees per second
        float gyroADCf = (float)gyroADC[axis] * gyroDev0.scale;

        // Apply LPF
        DEBUG_SET(DEBUG_GYRO, axis, lrintf(gyroADCf));
        gyroADCf = softLpfFilterApplyFn(softLpfFilter[axis], gyroADCf);

        // Apply Notch filtering
        DEBUG_SET(DEBUG_NOTCH, axis, lrintf(gyroADCf));
        gyroADCf = notchFilter1ApplyFn(notchFilter1[axis], gyroADCf);
        gyroADCf = notchFilter2ApplyFn(notchFilter2[axis], gyroADCf);
        gyro.gyroADCf[axis] = gyroADCf;
    }

    if (!calibrationComplete) {
        gyroADC[X] = lrintf(gyro.gyroADCf[X] / gyroDev0.scale);
        gyroADC[Y] = lrintf(gyro.gyroADCf[Y] / gyroDev0.scale);
        gyroADC[Z] = lrintf(gyro.gyroADCf[Z] / gyroDev0.scale);
    }
}

void gyroReadTemperature(void)
{
    if (gyroDev0.temperature) {
        gyroDev0.temperature(&gyroDev0, &gyroTemperature0);
    }
}

int16_t gyroGetTemperature(void)
{
    return gyroTemperature0;
}

int16_t gyroRateDps(int axis)
{
    return lrintf(gyro.gyroADCf[axis] / gyroDev0.scale);
}
