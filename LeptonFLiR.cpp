/*  Arduino Library for the Lepton FLiR Thermal Camera Module.
    Copyright (c) 2016 NachtRaveVL      <nachtravevl@gmail.com>

    Permission is hereby granted, free of charge, to any person
    obtaining a copy of this software and associated documentation
    files (the "Software"), to deal in the Software without
    restriction, including without limitation the rights to use,
    copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the
    Software is furnished to do so, subject to the following
    conditions:

    This permission notice shall be included in all copies or
    substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
    EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
    OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
    NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
    HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
    WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
    OTHER DEALINGS IN THE SOFTWARE.

    Lepton-FLiR-Arduino - Version 0.9
*/

#include "LeptonFLiR.h"
#if (defined(ARDUINO_ARCH_SAM) || defined(ARDUINO_ARCH_SAMD)) && !defined(LEPFLIR_DISABLE_SCHEDULER)
#include "Scheduler.h"
#define LEPFLIR_USE_SCHEDULER           1
#endif

#define LEPFLIR_GEN_CMD_TIMEOUT                 5000 // Timeout for commands to be processed
#define LEPFLIR_SPI_MAX_SPEED                   20000000 // Maximum SPI speed for FLiR module
#define LEPFLIR_SPI_MIN_SPEED                   2200000 // Minimum SPI speed for FLiR module
#define LEPFLIR_SPI_FRAME_PACKET_SIZE           164 // 2B ID + 2B CRC + 160B for 80x1 14bpp/8bppAGC thermal image data, otherwise if telemetry row 2B revision + 162B telemetry data
#define LEPFLIR_SPI_FRAME_PACKET_HEADER_SIZE16  2
#define LEPFLIR_SPI_FRAME_PACKET_DATA_SIZE16    80

#ifndef LEPFLIR_DISABLE_ALIGNED_MALLOC
static inline int roundUpVal16(int val) { return ((val + 15) & -16); }
static inline uint8_t *roundUpPtr16(uint8_t *ptr) { return (uint8_t *)(((uintptr_t)ptr + 15) & 0xF); }
static inline uint8_t *roundUpMalloc16(int size) { return (uint8_t *)malloc((size_t)(size + 15)); }
static inline uint8_t *roundUpSpiFrame16(uint8_t *spiFrame) { return roundUpPtr16(spiFrame) + 16 - 4; }
#else
static inline int roundUpVal16(int val) { return val; }
static inline uint8_t *roundUpPtr16(uint8_t *ptr) { return ptr; }
static inline uint8_t *roundUpMalloc16(int size) { return (uint8_t *)malloc((size_t)size); }
static inline uint8_t *roundUpSpiFrame16(uint8_t *spiFrame) { return spiFrame; }
#endif

#ifndef LEPFLIR_USE_SOFTWARE_I2C
LeptonFLiR::LeptonFLiR(TwoWire& i2cWire, uint8_t spiCSPin) {
    _i2cWire = &i2cWire;
#else
LeptonFLiR::LeptonFLiR(uint8_t spiCSPin) {
#endif
    _spiCSPin = spiCSPin;
    _spiSettings = SPISettings(LEPFLIR_SPI_MAX_SPEED, MSBFIRST, SPI_MODE3);
    _storageMode = LeptonFLiR_ImageStorageMode_Count;
    _imageData = _spiFrameData = _telemetryData = NULL;
    _isReadingNextFrame = false;
    _lastI2CError = _lastErrorCode = 0;
}

LeptonFLiR::~LeptonFLiR() {
    if (_imageData) free(_imageData);
    if (_spiFrameData) free(_spiFrameData);
    if (_telemetryData) free(_telemetryData);
}

void LeptonFLiR::init(LeptonFLiR_ImageStorageMode storageMode, LeptonFLiR_TemperatureMode tempMode) {
    _storageMode = (LeptonFLiR_ImageStorageMode)constrain((int)storageMode, 0, (int)LeptonFLiR_ImageStorageMode_Count - 1);
    _tempMode = (LeptonFLiR_TemperatureMode)constrain((int)tempMode, 0, (int)LeptonFLiR_TemperatureMode_Count - 1);

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.print("LeptonFLiR::init spiCSPin: ");
    Serial.print(_spiCSPin);
    Serial.print(", storageMode: ");
    Serial.println(storageMode);
#endif

    pinMode(_spiCSPin, OUTPUT);
    digitalWrite(_spiCSPin, HIGH);

    _imageData = roundUpMalloc16(getImageTotalBytes());
    _spiFrameData = roundUpMalloc16(getSPIFrameTotalBytes());

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    int mallocOffset = 0;
#ifndef LEPFLIR_DISABLE_ALIGNED_MALLOC
    mallocOffset = 15;
#endif
    Serial.print("  LeptonFLiR::init imageData: ");
    Serial.print(getImageTotalBytes() + mallocOffset);
    Serial.print("B, spiFrameData: ");
    Serial.print(getSPIFrameTotalBytes() + mallocOffset);
    Serial.print("B, total: ");
    Serial.print(getImageTotalBytes() + mallocOffset + getSPIFrameTotalBytes() + mallocOffset);
    Serial.println("B");
    Serial.print("  LeptonFLiR::init SPIPortSpeed: ");
    for (int divisor = 2; divisor <= 128; divisor *= 2) {
        if (F_CPU / (float)divisor <= LEPFLIR_SPI_MAX_SPEED + 0.00001f || divisor == 128) {
            Serial.print(roundf((F_CPU / (float)divisor) / 1000.0f) / 1000.0f);
            Serial.print("MHz (SPI_CLOCK_DIV");
            Serial.print(divisor);
            Serial.print(")");
            break;
        }
    }
    if (F_CPU / 2.0f < LEPFLIR_SPI_MIN_SPEED - 0.00001f)
        Serial.println(" <speed too low>");
    else if (F_CPU / 128.0f > LEPFLIR_SPI_MAX_SPEED + 0.00001f)
        Serial.println(" <speed too high>");
    else
        Serial.println("");
#endif
}

int LeptonFLiR::getImageWidth() {
    switch (_storageMode) {
    case LeptonFLiR_ImageStorageMode_80x60_16bpp:
    case LeptonFLiR_ImageStorageMode_80x60_8bpp:
        return 80;
    case LeptonFLiR_ImageStorageMode_40x30_16bpp:
    case LeptonFLiR_ImageStorageMode_40x30_8bpp:
        return 40;
    case LeptonFLiR_ImageStorageMode_20x15_16bpp:
    case LeptonFLiR_ImageStorageMode_20x15_8bpp:
        return 20;
    default:
        return 0;
    }
}

int LeptonFLiR::getImageHeight() {
    switch (_storageMode) {
    case LeptonFLiR_ImageStorageMode_80x60_16bpp:
    case LeptonFLiR_ImageStorageMode_80x60_8bpp:
        return 60;
    case LeptonFLiR_ImageStorageMode_40x30_16bpp:
    case LeptonFLiR_ImageStorageMode_40x30_8bpp:
        return 30;
    case LeptonFLiR_ImageStorageMode_20x15_16bpp:
    case LeptonFLiR_ImageStorageMode_20x15_8bpp:
        return 15;
    default:
        return 0;
    }
}

int LeptonFLiR::getImageBpp() {
    switch (_storageMode) {
    case LeptonFLiR_ImageStorageMode_80x60_16bpp:
    case LeptonFLiR_ImageStorageMode_40x30_16bpp:
    case LeptonFLiR_ImageStorageMode_20x15_16bpp:
        return 2;
    case LeptonFLiR_ImageStorageMode_80x60_8bpp:
    case LeptonFLiR_ImageStorageMode_40x30_8bpp:
    case LeptonFLiR_ImageStorageMode_20x15_8bpp:
        return 1;
    default:
        return 0;
    }
}

int LeptonFLiR::getImagePitch() {
    switch (_storageMode) {
    case LeptonFLiR_ImageStorageMode_80x60_16bpp:
        return roundUpVal16(80 * 2);
    case LeptonFLiR_ImageStorageMode_80x60_8bpp:
        return roundUpVal16(80 * 1);
    case LeptonFLiR_ImageStorageMode_40x30_16bpp:
        return roundUpVal16(40 * 2);
    case LeptonFLiR_ImageStorageMode_40x30_8bpp:
        return roundUpVal16(40 * 1);
    case LeptonFLiR_ImageStorageMode_20x15_16bpp:
        return roundUpVal16(20 * 2);
    case LeptonFLiR_ImageStorageMode_20x15_8bpp:
        return roundUpVal16(20 * 1);
    default:
        return 0;
    }
}

int LeptonFLiR::getImageTotalBytes() {
    return ((getImageHeight() - 1) * getImagePitch()) + (getImageWidth() * getImageBpp());
}

uint8_t *LeptonFLiR::getImageData() {
    return !_isReadingNextFrame ? roundUpPtr16(_imageData) : NULL;
}

uint8_t *LeptonFLiR::getImageDataRow(int row) {
    return !_isReadingNextFrame ? (roundUpPtr16(_imageData) + (getImagePitch() * row)) : NULL;
}

uint8_t *LeptonFLiR::_getImageDataRow(int row) {
    return roundUpPtr16(_imageData) + (getImagePitch() * row);
}

uint8_t *LeptonFLiR::getTelemetryData() {
    return !_isReadingNextFrame && _telemetryData && _telemetryData[0] != 0x0F ? _telemetryData : NULL;
}

void LeptonFLiR::getTelemetryData(TelemetryData *telemetry) {
    if (_isReadingNextFrame || !_telemetryData || !telemetry) return;
    uint16_t *telemetryData = (uint16_t *)_telemetryData;

    telemetry->revisionMajor = lowByte(telemetryData[0]);
    telemetry->revisionMinor = highByte(telemetryData[0]);

    telemetry->cameraUptime = (uint32_t)telemetryData[1] << 16 | (uint32_t)telemetryData[2];

    telemetry->ffcDesired = telemetryData[4] & 0x0004;
    uint_fast8_t ffcState = (telemetryData[4] & 0x0018) >> 3;
    if (telemetry->revisionMajor >= 9 && ffcState >= 1)
        ffcState -= 1;
    telemetry->ffcState = (TelemetryData_FFCState)ffcState;
    telemetry->agcEnabled = telemetryData[4] & 0x0800;
    telemetry->shutdownImminent = telemetryData[3] & 0x0010;

    wordsToHexString(&telemetryData[5], 8, telemetry->serialNumber, 24);
    wordsToHexString(&telemetryData[13], 4, telemetry->softwareRevision, 12);

    telemetry->frameCounter = (uint32_t)telemetryData[20] << 16 | (uint32_t)telemetryData[21];
    telemetry->frameMean = telemetryData[22];

    telemetry->fpaTemperature = kelvin100ToTemperature(telemetryData[24]);
    telemetry->housingTemperature = kelvin100ToTemperature(telemetryData[26]);

    telemetry->lastFFCTime = (uint32_t)telemetryData[30] << 16 | (uint32_t)telemetryData[31];
    telemetry->fpaTempAtLastFFC = kelvin100ToTemperature(telemetryData[29]);
    telemetry->housingTempAtLastFFC = kelvin100ToTemperature(telemetryData[32]);

    telemetry->agcRegion.startRow = telemetryData[34];
    telemetry->agcRegion.startCol = telemetryData[35];
    telemetry->agcRegion.endCol = telemetryData[36];
    telemetry->agcRegion.endRow = telemetryData[37];

    telemetry->agcClipHigh = telemetryData[38];
    telemetry->agcClipLow = telemetryData[39];

    telemetry->log2FFCFrames = telemetryData[74];
}

int LeptonFLiR::getSPIFrameLines() {
    switch (_storageMode) {
    case LeptonFLiR_ImageStorageMode_80x60_16bpp:
    case LeptonFLiR_ImageStorageMode_80x60_8bpp:
        return 1;
    case LeptonFLiR_ImageStorageMode_40x30_16bpp:
    case LeptonFLiR_ImageStorageMode_40x30_8bpp:
        return 2;
    case LeptonFLiR_ImageStorageMode_20x15_16bpp:
    case LeptonFLiR_ImageStorageMode_20x15_8bpp:
        return 4;
    default:
        return 0;
    }
}

int LeptonFLiR::getSPIFrameTotalBytes() {
    return getSPIFrameLines() * roundUpVal16(LEPFLIR_SPI_FRAME_PACKET_SIZE);
}

uint8_t *LeptonFLiR::getSPIFrameDataRow(int row) {
    return roundUpSpiFrame16(_spiFrameData) + (roundUpVal16(LEPFLIR_SPI_FRAME_PACKET_SIZE) * row);
}

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT

static void printSPIFrame(uint8_t *spiFrame, uint8_t *pxlData) {
    if (spiFrame) {
        Serial.print("ID: 0x");
        Serial.print(((uint16_t *)spiFrame)[0], HEX);
        Serial.print(" CRC: 0x");
        Serial.print(((uint16_t *)spiFrame)[1], HEX);
        if (!pxlData) Serial.println("");
    }

    if (pxlData) {
        if (spiFrame) Serial.print(" ");
        Serial.print("Data: ");
        for (int i = 0; i < 5; ++i) {
            Serial.print(i > 0 ? "-0x" : "0x");
            Serial.print(((uint16_t *)pxlData)[i], HEX);
        }
        Serial.print("...");
        for (int i = 75; i < 80; ++i) {
            Serial.print(i > 75 ? "-0x" : "0x");
            Serial.print(((uint16_t *)pxlData)[i], HEX);
        }
        Serial.println("");
    }
}

#endif

static void delayTimeout(int timeout) {
    unsigned long endTime = millis() + (unsigned long)timeout;

    while (millis() < endTime) {
#ifdef LEPFLIR_USE_SCHEDULER
        Scheduler.yield();
#else
        delay(1);
#endif
    }
}

static inline void SPI_transfer16(uint16_t *buffer, int count) {
    while (count-- > 0)
        *buffer++ = SPI.transfer16((uint16_t)0);
}

bool LeptonFLiR::readNextFrame() {
    if (!_isReadingNextFrame) {
        _isReadingNextFrame = true;

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
        Serial.println("LeptonFLiR::readNextFrame");
#endif

        bool agc8Enabled, telemetryEnabled, cameraBooted; LEP_SYS_TELEMETRY_LOCATION telemetryLocation;
        {   uint16_t value;

        receiveCommand(commandCode(LEP_CID_AGC_ENABLE_STATE, LEP_I2C_COMMAND_TYPE_GET), &value);
        agc8Enabled = value;

        if (agc8Enabled) {
            receiveCommand(commandCode(LEP_CID_AGC_HEQ_SCALE_FACTOR, LEP_I2C_COMMAND_TYPE_GET), &value);
            agc8Enabled = (value == (uint16_t)LEP_AGC_SCALE_TO_8_BITS);
        }

        receiveCommand(commandCode(LEP_CID_SYS_TELEMETRY_ENABLE_STATE, LEP_I2C_COMMAND_TYPE_GET), &value);
        telemetryEnabled = value;

        receiveCommand(commandCode(LEP_CID_SYS_TELEMETRY_LOCATION, LEP_I2C_COMMAND_TYPE_GET), &value);
        telemetryLocation = (LEP_SYS_TELEMETRY_LOCATION)value;

        readRegister(LEP_I2C_STATUS_REG, &value, 1, 1);
        cameraBooted = (value & LEP_I2C_STATUS_BOOT_MODE_BIT_MASK) && (value & LEP_I2C_STATUS_BOOT_STATUS_BIT_MASK);
        }

        if (!cameraBooted) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
            Serial.println("  LeptonFLiR::readNextFrame Camera has not yet booted. Aborting.");
#endif
            _isReadingNextFrame = false;
            return false;
        }

        if (telemetryEnabled && !_telemetryData) {
            _telemetryData = (uint8_t *)malloc(LEPFLIR_SPI_FRAME_PACKET_SIZE);
            _telemetryData[0] = 0x0F; // initialize as discard packet
        }
        else if (!telemetryEnabled && _telemetryData) {
            free(_telemetryData);
            _telemetryData = NULL;
        }

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
        Serial.print("  LeptonFLiR::readNextFrame AGC-8bit: ");
        Serial.print(agc8Enabled ? "enabled" : "disabled");
        Serial.print(", Telemetry: ");
        if (telemetryEnabled) {
            Serial.print("enabled, Location: ");
            Serial.println(telemetryLocation == LEP_TELEMETRY_LOCATION_HEADER ? "header" : "footer");
        }
        else
            Serial.println("disabled");
#endif

        uint_fast8_t readLines = 0;
        uint_fast8_t imgRows = getImageHeight();
        uint_fast8_t currImgRow = 0;
        uint_fast8_t spiRows = getSPIFrameLines();
        uint_fast8_t currSpiRow = 0;
        uint_fast8_t teleRows = (telemetryEnabled * 3);
        uint_fast8_t currTeleRow = 0;
        uint_fast8_t framesSkipped = 0;
        uint_fast16_t packetsRead = 0;
        uint8_t *spiFrame = getSPIFrameDataRow(currSpiRow);
        bool packetHeaderRead = true;
        bool discardFrame = false;

        SPI.beginTransaction(_spiSettings);

        digitalWrite(_spiCSPin, LOW);
        digitalWrite(_spiCSPin, HIGH);
        delayTimeout(185);
        digitalWrite(_spiCSPin, LOW);
        SPI_transfer16((uint16_t *)spiFrame, LEPFLIR_SPI_FRAME_PACKET_HEADER_SIZE16);

        while (currImgRow < imgRows || currTeleRow < teleRows) {
            ++packetsRead;
            if (!packetHeaderRead) {
                spiFrame = getSPIFrameDataRow(currSpiRow);
                digitalWrite(_spiCSPin, LOW);
                SPI_transfer16((uint16_t *)spiFrame, LEPFLIR_SPI_FRAME_PACKET_HEADER_SIZE16);
            }
            else
                packetHeaderRead = false;

            if (spiFrame[0] == 0x00 && spiFrame[1] == readLines) { // Image packet
                uint8_t *pxlData = (_storageMode == LeptonFLiR_ImageStorageMode_80x60_16bpp ? _getImageDataRow(readLines) : &spiFrame[4]);

                SPI_transfer16((uint16_t *)pxlData, LEPFLIR_SPI_FRAME_PACKET_DATA_SIZE16);
                digitalWrite(_spiCSPin, HIGH);

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
                Serial.println("    LeptonFLiR::readNextFrame VoSPI Image Packet:");
                Serial.print("      ");  printSPIFrame(spiFrame, pxlData);
#endif

                ++readLines; ++currSpiRow;
            }
            else if ((spiFrame[0] & 0x0F != 0x0F) && teleRows && currTeleRow < 3 &&
                ((telemetryLocation == LEP_TELEMETRY_LOCATION_HEADER && readLines == 0) ||
                (telemetryLocation == LEP_TELEMETRY_LOCATION_FOOTER && readLines == 60))) { // Telemetry packet
                if (currTeleRow == 0) {
                    SPI_transfer16((uint16_t *)&_telemetryData[4], LEPFLIR_SPI_FRAME_PACKET_DATA_SIZE16);
                    digitalWrite(_spiCSPin, HIGH);
                    memcpy(_telemetryData, spiFrame, 4);
                    //wroteTeleData = true;

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
                    Serial.println("    LeptonFLiR::readNextFrame VoSPI Telemetry(A) Packet:");
                    Serial.print("      ");  printSPIFrame(_telemetryData, &_telemetryData[4]);
#endif
                }
                else {
                    SPI_transfer16((uint16_t *)&spiFrame[4], LEPFLIR_SPI_FRAME_PACKET_DATA_SIZE16);
                    digitalWrite(_spiCSPin, HIGH);

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
                    Serial.print("    LeptonFLiR::readNextFrame VoSPI Telemetry(");
                    Serial.print(currTeleRow == 1 ? "B" : "C");
                    Serial.println(") Packet:");
                    Serial.print("      ");  printSPIFrame(spiFrame, &spiFrame[4]);
#endif
                }

                ++currTeleRow;
            }
            else { // Discard packet
                SPI_transfer16((uint16_t *)&spiFrame[4], LEPFLIR_SPI_FRAME_PACKET_DATA_SIZE16);
                digitalWrite(_spiCSPin, HIGH);

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
                Serial.println("    LeptonFLiR::readNextFrame VoSPI Discard Packet:");
                Serial.print("      ");  printSPIFrame(spiFrame, &spiFrame[4]);
#endif

                if (packetsRead > 0 && (spiFrame[0] & 0x0F) == 0x0F)
                    delayTimeout(185);

                uint_fast8_t triesLeft = 120;

                while (triesLeft > 0) {
                    digitalWrite(_spiCSPin, LOW);
                    SPI_transfer16((uint16_t *)spiFrame, LEPFLIR_SPI_FRAME_PACKET_HEADER_SIZE16);

                    if ((spiFrame[0] & 0x0F) != 0x0F) {
                        if ((spiFrame[0] == 0x00 && spiFrame[1] == readLines) ||
                            (teleRows && readLines == 60 && spiFrame[0] > 0x00 && telemetryLocation == LEP_TELEMETRY_LOCATION_FOOTER)) {
                            // Reestablished sync at position we're next expecting
                            break;
                        }
                        else if ((spiFrame[0] == 0x00 && spiFrame[1] == 0) ||
                            (teleRows && spiFrame[0] > 0x00 && telemetryLocation == LEP_TELEMETRY_LOCATION_HEADER)) {
                            // Reestablished sync at next frame position

                            if (packetsRead > 0 && ++framesSkipped >= 5) {
                                SPI_transfer16((uint16_t *)&spiFrame[4], LEPFLIR_SPI_FRAME_PACKET_DATA_SIZE16);
                                digitalWrite(_spiCSPin, HIGH);
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
                                Serial.println("  LeptonFLiR::readNextFrame Maximum frame skip reached. Aborting.");
#endif
                                SPI.endTransaction();
                                _isReadingNextFrame = false;
                                return false;
                            }
                            else {
                                packetHeaderRead = true;
                                readLines = currImgRow = currSpiRow = currTeleRow = 0;

                                uint8_t* prevSPIFrame = spiFrame;
                                spiFrame = getSPIFrameDataRow(currSpiRow);
                                if (spiFrame != prevSPIFrame)
                                    memcpy(spiFrame, prevSPIFrame, LEPFLIR_SPI_FRAME_PACKET_HEADER_SIZE16 * 2);

                                break;
                            }
                        }
                    }

                    SPI_transfer16((uint16_t *)&spiFrame[4], LEPFLIR_SPI_FRAME_PACKET_DATA_SIZE16);
                    digitalWrite(_spiCSPin, HIGH);
                    --triesLeft;
                }

                if (triesLeft == 0) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
                    Serial.println("  LeptonFLiR::readNextFrame Maximum resync retries reached. Aborting.");
#endif
                    SPI.endTransaction();
                    _isReadingNextFrame = false;
                    return false;
                }
            }

            // Write out to frame
            if (currSpiRow == spiRows && (!teleRows || currTeleRow > 1)) {
                if (_storageMode != LeptonFLiR_ImageStorageMode_80x60_16bpp) {
                    spiFrame = getSPIFrameDataRow(0) + 4;
                    uint8_t *pxlData = _getImageDataRow(currImgRow);

                    uint_fast8_t imgWidth = getImageWidth();
                    uint_fast8_t imgBpp = getImageBpp();

                    uint_fast32_t divisor = (spiRows * spiRows) * (!agc8Enabled && imgBpp == 1 ? 64 : 1);
                    uint_fast32_t clamp = (!agc8Enabled && imgBpp == 2 ? 0x3FFF : 0x00FF);

                    while (imgWidth-- > 0) {
                        uint_fast32_t total = 0;

                        uint_fast8_t y = spiRows;
                        uint8_t *spiYFrame = spiFrame;
                        while (y-- > 0) {

                            uint_fast8_t x = spiRows;
                            uint16_t *spiXFrame = (uint16_t *)spiYFrame;
                            while (x-- > 0)
                                total += *spiXFrame++;

                            spiYFrame += roundUpVal16(LEPFLIR_SPI_FRAME_PACKET_SIZE);
                        }

                        if (imgBpp == 2)
                            *((uint16_t *)pxlData) = (uint16_t)constrain(total / divisor, 0, clamp);
                        else
                            *((uint8_t *)pxlData) = (uint8_t)constrain(total / divisor, 0, clamp);
                        pxlData += imgBpp;
                        spiFrame += 2 * spiRows;
                    }
                }

                ++currImgRow; currSpiRow = 0;
            }
        }

        SPI.endTransaction();

        _isReadingNextFrame = false;
    }

    return true;
}

void LeptonFLiR::setAGCEnabled(bool enabled) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setAGCEnabled");
#endif

    sendCommand(commandCode(LEP_CID_AGC_ENABLE_STATE, LEP_I2C_COMMAND_TYPE_SET), (uint32_t)enabled);
}

bool LeptonFLiR::getAGCEnabled() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getAGCEnabled");
#endif

    uint16_t enabled;
    receiveCommand(commandCode(LEP_CID_AGC_ENABLE_STATE, LEP_I2C_COMMAND_TYPE_GET), &enabled);
    return enabled;
}

void LeptonFLiR::setAGCPolicy(LEP_AGC_POLICY policy) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setAGCPolicy");
#endif

    sendCommand(commandCode(LEP_CID_AGC_POLICY, LEP_I2C_COMMAND_TYPE_SET), (uint32_t)policy);
}

LEP_AGC_POLICY LeptonFLiR::getAGCPolicy() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getAGCPolicy");
#endif

    uint16_t policy;
    receiveCommand(commandCode(LEP_CID_AGC_POLICY, LEP_I2C_COMMAND_TYPE_GET), &policy);
    return (LEP_AGC_POLICY)policy;
}

void LeptonFLiR::setAGCHEQScaleFactor(LEP_AGC_HEQ_SCALE_FACTOR factor) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setAGCHEQScaleFactor");
#endif

    sendCommand(commandCode(LEP_CID_AGC_HEQ_SCALE_FACTOR, LEP_I2C_COMMAND_TYPE_SET), (uint32_t)factor);
}

LEP_AGC_HEQ_SCALE_FACTOR LeptonFLiR::getAGCHEQScaleFactor() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getAGCHEQScaleFactor");
#endif

    uint16_t factor;
    receiveCommand(commandCode(LEP_CID_AGC_HEQ_SCALE_FACTOR, LEP_I2C_COMMAND_TYPE_GET), &factor);
    return (LEP_AGC_HEQ_SCALE_FACTOR)factor;
}

void LeptonFLiR::setAGCCalcEnabled(bool enabled) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setAGCCalcEnabled");
#endif

    sendCommand(commandCode(LEP_CID_AGC_CALC_ENABLE_STATE, LEP_I2C_COMMAND_TYPE_SET), (uint32_t)enabled);
}

bool LeptonFLiR::getAGCCalcEnabled() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getAGCCalcEnabled");
#endif

    uint16_t enabled;
    receiveCommand(commandCode(LEP_CID_AGC_CALC_ENABLE_STATE, LEP_I2C_COMMAND_TYPE_GET), &enabled);
    return enabled;
}

void LeptonFLiR::getSysCameraStatus(LEP_SYS_CAM_STATUS *status) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getSysCameraStatus");
#endif

    receiveCommand(commandCode(LEP_CID_SYS_CAM_STATUS, LEP_I2C_COMMAND_TYPE_GET), (uint16_t *)status, sizeof(LEP_SYS_CAM_STATUS) / 2);
}

void LeptonFLiR::getSysFlirSerialNumber(char *buffer, int maxLength) {
    if (!buffer || maxLength < 16) return;

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getSysFlirSerialNumber");
#endif

    uint16_t innerBuffer[4];
    receiveCommand(commandCode(LEP_CID_SYS_FLIR_SERIAL_NUMBER, LEP_I2C_COMMAND_TYPE_GET), innerBuffer, 4);
    wordsToHexString(innerBuffer, 4, buffer, maxLength);
}

void LeptonFLiR::getSysCustomerSerialNumber(char *buffer, int maxLength) {
    if (!buffer || maxLength < 64) return;

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getSysCustomerSerialNumber");
#endif

    uint16_t innerBuffer[16];
    receiveCommand(commandCode(LEP_CID_SYS_CUST_SERIAL_NUMBER, LEP_I2C_COMMAND_TYPE_GET), innerBuffer, 16);
    wordsToHexString(innerBuffer, 16, buffer, maxLength);
}

uint32_t LeptonFLiR::getSysCameraUptime() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getSysCameraUptime");
#endif

    uint32_t uptime;
    receiveCommand(commandCode(LEP_CID_SYS_CAM_UPTIME, LEP_I2C_COMMAND_TYPE_GET), &uptime);
    return uptime;
}

float LeptonFLiR::getSysAuxTemperature() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getSysAuxTemperature");
#endif

    uint16_t kelvin100;
    receiveCommand(commandCode(LEP_CID_SYS_AUX_TEMPERATURE_KELVIN, LEP_I2C_COMMAND_TYPE_GET), &kelvin100);
    return kelvin100ToTemperature(kelvin100);
}

float LeptonFLiR::getSysFPATemperature() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getSysFPATemperature");
#endif

    uint16_t kelvin100;
    receiveCommand(commandCode(LEP_CID_SYS_FPA_TEMPERATURE_KELVIN, LEP_I2C_COMMAND_TYPE_GET), &kelvin100);
    return kelvin100ToTemperature(kelvin100);
}

void LeptonFLiR::setSysTelemetryEnabled(bool enabled) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setSysTelemetryEnabled");
#endif

    sendCommand(commandCode(LEP_CID_SYS_TELEMETRY_ENABLE_STATE, LEP_I2C_COMMAND_TYPE_SET), (uint32_t)enabled);

    if (!_lastI2CError && !_lastErrorCode) {
        if (enabled && !_telemetryData) {
            _telemetryData = (uint8_t *)malloc(LEPFLIR_SPI_FRAME_PACKET_SIZE);
            _telemetryData[0] = 0x0F; // initialize as discard packet
        }
        else if (!enabled && _telemetryData) {
            free(_telemetryData);
            _telemetryData = NULL;
        }
    }
}

bool LeptonFLiR::getSysTelemetryEnabled() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getSysTelemetryEnabled");
#endif

    uint16_t enabled;
    receiveCommand(commandCode(LEP_CID_SYS_TELEMETRY_ENABLE_STATE, LEP_I2C_COMMAND_TYPE_GET), &enabled);

    if (!_lastI2CError && !_lastErrorCode) {
        if (enabled && !_telemetryData) {
            _telemetryData = (uint8_t *)malloc(LEPFLIR_SPI_FRAME_PACKET_SIZE);
            _telemetryData[0] = 0x0F; // initialize as discard packet
        }
        else if (!enabled && _telemetryData) {
            free(_telemetryData);
            _telemetryData = NULL;
        }
    }

    return enabled;
}

void LeptonFLiR::setVidPolarity(LEP_VID_POLARITY polarity) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setVidPolarity");
#endif

    sendCommand(commandCode(LEP_CID_VID_POLARITY_SELECT, LEP_I2C_COMMAND_TYPE_SET), (uint32_t)polarity);
}

LEP_VID_POLARITY LeptonFLiR::getVidPolarity() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getVidPolarity");
#endif

    uint16_t polarity;
    receiveCommand(commandCode(LEP_CID_VID_POLARITY_SELECT, LEP_I2C_COMMAND_TYPE_GET), &polarity);
    return (LEP_VID_POLARITY)polarity;
}

void LeptonFLiR::setVidPseudoColorLUT(LEP_VID_PCOLOR_LUT table) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setVidPseudoColorLUT");
#endif

    sendCommand(commandCode(LEP_CID_VID_LUT_SELECT, LEP_I2C_COMMAND_TYPE_SET), (uint32_t)table);
}

LEP_VID_PCOLOR_LUT LeptonFLiR::getVidPseudoColorLUT() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getVidPseudoColorLUT");
#endif

    uint16_t table;
    receiveCommand(commandCode(LEP_CID_VID_LUT_SELECT, LEP_I2C_COMMAND_TYPE_GET), &table);
    return (LEP_VID_PCOLOR_LUT)table;
}

void LeptonFLiR::setVidFocusCalcEnabled(bool enabled) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setVidFocusCalcEnabled");
#endif

    sendCommand(commandCode(LEP_CID_VID_FOCUS_CALC_ENABLE, LEP_I2C_COMMAND_TYPE_SET), (uint32_t)enabled);
}

bool LeptonFLiR::getVidFocusCalcEnabled() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getVidFocusCalcEnabled");
#endif

    uint16_t enabled;
    receiveCommand(commandCode(LEP_CID_VID_FOCUS_CALC_ENABLE, LEP_I2C_COMMAND_TYPE_GET), &enabled);
    return enabled;
}

void LeptonFLiR::setVidFreezeEnabled(bool enabled) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setVidFreezeEnabled");
#endif

    sendCommand(commandCode(LEP_CID_VID_FREEZE_ENABLE, LEP_I2C_COMMAND_TYPE_SET), (uint32_t)enabled);
}

bool LeptonFLiR::getVidFreezeEnabled() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getVidFreezeEnabled");
#endif

    uint16_t enabled;
    receiveCommand(commandCode(LEP_CID_VID_FREEZE_ENABLE, LEP_I2C_COMMAND_TYPE_GET), &enabled);
    return enabled;
}

#ifndef LEPFLIR_EXCLUDE_EXT_I2C_FUNCS

void LeptonFLiR::setAGCHistogramRegion(LEP_AGC_HISTOGRAM_ROI *region) {
    if (!region) return;

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setAGCHistogramRegion");
#endif

    sendCommand(commandCode(LEP_CID_AGC_ROI, LEP_I2C_COMMAND_TYPE_SET), (uint16_t *)region, sizeof(LEP_AGC_HISTOGRAM_ROI) / 2);
}

void LeptonFLiR::getAGCHistogramRegion(LEP_AGC_HISTOGRAM_ROI *region) {
    if (!region) return;

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getAGCHistogramRegion");
#endif

    receiveCommand(commandCode(LEP_CID_AGC_ROI, LEP_I2C_COMMAND_TYPE_GET), (uint16_t *)region, sizeof(LEP_AGC_HISTOGRAM_ROI) / 2);
}

void LeptonFLiR::getAGCHistogramStatistics(LEP_AGC_HISTOGRAM_STATISTICS *statistics) {
    if (!statistics) return;

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getAGCHistogramStatistics");
#endif

    receiveCommand(commandCode(LEP_CID_AGC_STATISTICS, LEP_I2C_COMMAND_TYPE_GET), (uint16_t *)statistics, sizeof(LEP_AGC_HISTOGRAM_STATISTICS) / 2);
}

void LeptonFLiR::setAGCHistogramClipPercent(uint16_t percent) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setAGCHistogramClipPercent");
#endif

    sendCommand(commandCode(LEP_CID_AGC_HISTOGRAM_CLIP_PERCENT, LEP_I2C_COMMAND_TYPE_SET), percent);
}

uint16_t LeptonFLiR::getAGCHistogramClipPercent() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getAGCHistogramClipPercent");
#endif

    uint16_t percent;
    receiveCommand(commandCode(LEP_CID_AGC_HISTOGRAM_CLIP_PERCENT, LEP_I2C_COMMAND_TYPE_GET), &percent);
    return percent;
}

void LeptonFLiR::setAGCHistogramTailSize(uint16_t size) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setAGCHistogramTailSize");
#endif

    sendCommand(commandCode(LEP_CID_AGC_HISTOGRAM_TAIL_SIZE, LEP_I2C_COMMAND_TYPE_SET), size);
}

uint16_t LeptonFLiR::getAGCHistogramTailSize() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getAGCHistogramTailSize");
#endif

    uint16_t size;
    receiveCommand(commandCode(LEP_CID_AGC_HISTOGRAM_TAIL_SIZE, LEP_I2C_COMMAND_TYPE_GET), &size);
    return size;
}

void LeptonFLiR::setAGCLinearMaxGain(uint16_t gain) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setAGCLinearMaxGain");
#endif

    sendCommand(commandCode(LEP_CID_AGC_LINEAR_MAX_GAIN, LEP_I2C_COMMAND_TYPE_SET), gain);
}

uint16_t LeptonFLiR::getAGCLinearMaxGain() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getAGCLinearMaxGain");
#endif

    uint16_t gain;
    receiveCommand(commandCode(LEP_CID_AGC_LINEAR_MAX_GAIN, LEP_I2C_COMMAND_TYPE_GET), &gain);
    return gain;
}

void LeptonFLiR::setAGCLinearMidpoint(uint16_t midpoint) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setAGCLinearMidpoint");
#endif

    sendCommand(commandCode(LEP_CID_AGC_LINEAR_MIDPOINT, LEP_I2C_COMMAND_TYPE_SET), midpoint);
}

uint16_t LeptonFLiR::getAGCLinearMidpoint() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getAGCLinearMidpoint");
#endif

    uint16_t midpoint;
    receiveCommand(commandCode(LEP_CID_AGC_LINEAR_MIDPOINT, LEP_I2C_COMMAND_TYPE_GET), &midpoint);
    return midpoint;
}

void LeptonFLiR::setAGCLinearDampeningFactor(uint16_t factor) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setAGCLinearDampeningFactor");
#endif

    sendCommand(commandCode(LEP_CID_AGC_LINEAR_DAMPENING_FACTOR, LEP_I2C_COMMAND_TYPE_SET), factor);
}

uint16_t LeptonFLiR::getAGCLinearDampeningFactor() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getAGCLinearDampeningFactor");
#endif

    uint16_t factor;
    receiveCommand(commandCode(LEP_CID_AGC_LINEAR_DAMPENING_FACTOR, LEP_I2C_COMMAND_TYPE_GET), &factor);
    return factor;
}

void LeptonFLiR::setAGCHEQDampeningFactor(uint16_t factor) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setAGCHEQDampeningFactor");
#endif

    sendCommand(commandCode(LEP_CID_AGC_HEQ_DAMPENING_FACTOR, LEP_I2C_COMMAND_TYPE_SET), factor);
}

uint16_t LeptonFLiR::getAGCHEQDampeningFactor() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getAGCHEQDampeningFactor");
#endif

    uint16_t factor;
    receiveCommand(commandCode(LEP_CID_AGC_HEQ_DAMPENING_FACTOR, LEP_I2C_COMMAND_TYPE_GET), &factor);
    return factor;
}

void LeptonFLiR::setAGCHEQMaxGain(uint16_t gain) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setAGCHEQMaxGain");
#endif

    sendCommand(commandCode(LEP_CID_AGC_HEQ_MAX_GAIN, LEP_I2C_COMMAND_TYPE_SET), gain);
}

uint16_t LeptonFLiR::getAGCHEQMaxGain() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getAGCHEQMaxGain");
#endif

    uint16_t gain;
    receiveCommand(commandCode(LEP_CID_AGC_HEQ_MAX_GAIN, LEP_I2C_COMMAND_TYPE_GET), &gain);
    return gain;
}

void LeptonFLiR::setAGCHEQClipLimitHigh(uint16_t limit) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setAGCHEQClipLimitHigh");
#endif

    sendCommand(commandCode(LEP_CID_AGC_HEQ_CLIP_LIMIT_HIGH, LEP_I2C_COMMAND_TYPE_SET), limit);
}

uint16_t LeptonFLiR::getAGCHEQClipLimitHigh() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getAGCHEQClipLimitHigh");
#endif

    uint16_t limit;
    receiveCommand(commandCode(LEP_CID_AGC_HEQ_CLIP_LIMIT_HIGH, LEP_I2C_COMMAND_TYPE_GET), &limit);
    return limit;
}

void LeptonFLiR::setAGCHEQClipLimitLow(uint16_t limit) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setAGCHEQClipLimitLow");
#endif

    sendCommand(commandCode(LEP_CID_AGC_HEQ_CLIP_LIMIT_LOW, LEP_I2C_COMMAND_TYPE_SET), limit);
}

uint16_t LeptonFLiR::getAGCHEQClipLimitLow() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getAGCHEQClipLimitLow");
#endif

    uint16_t limit;
    receiveCommand(commandCode(LEP_CID_AGC_HEQ_CLIP_LIMIT_LOW, LEP_I2C_COMMAND_TYPE_GET), &limit);
    return limit;
}

void LeptonFLiR::setAGCHEQBinExtension(uint16_t extension) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setAGCHEQBinExtension");
#endif

    sendCommand(commandCode(LEP_CID_AGC_HEQ_BIN_EXTENSION, LEP_I2C_COMMAND_TYPE_SET), extension);
}

uint16_t LeptonFLiR::getAGCHEQBinExtension() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getAGCHEQBinExtension");
#endif

    uint16_t extension;
    receiveCommand(commandCode(LEP_CID_AGC_HEQ_BIN_EXTENSION, LEP_I2C_COMMAND_TYPE_GET), &extension);
    return extension;
}

void LeptonFLiR::setAGCHEQMidpoint(uint16_t midpoint) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setAGCHEQMidpoint");
#endif

    sendCommand(commandCode(LEP_CID_AGC_HEQ_MIDPOINT, LEP_I2C_COMMAND_TYPE_SET), midpoint);
}

uint16_t LeptonFLiR::getAGCHEQMidpoint() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getAGCHEQMidpoint");
#endif

    uint16_t midpoint;
    receiveCommand(commandCode(LEP_CID_AGC_HEQ_MIDPOINT, LEP_I2C_COMMAND_TYPE_GET), &midpoint);
    return midpoint;
}

void LeptonFLiR::setAGCHEQEmptyCounts(uint16_t counts) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setAGCHEQEmptyCounts");
#endif

    sendCommand(commandCode(LEP_CID_AGC_HEQ_EMPTY_COUNTS, LEP_I2C_COMMAND_TYPE_SET), counts);
}

uint16_t LeptonFLiR::getAGCHEQEmptyCounts() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setAGCHEQEmptyCounts");
#endif

    uint16_t counts;
    receiveCommand(commandCode(LEP_CID_AGC_HEQ_EMPTY_COUNTS, LEP_I2C_COMMAND_TYPE_GET), &counts);
    return counts;
}

void LeptonFLiR::setAGCHEQNormalizationFactor(uint16_t factor) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setAGCHEQNormalizationFactor");
#endif

    sendCommand(commandCode(LEP_CID_AGC_HEQ_NORMALIZATION_FACTOR, LEP_I2C_COMMAND_TYPE_SET), factor);
}

uint16_t LeptonFLiR::getAGCHEQNormalizationFactor() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getAGCHEQNormalizationFactor");
#endif

    uint16_t factor;
    receiveCommand(commandCode(LEP_CID_AGC_HEQ_NORMALIZATION_FACTOR, LEP_I2C_COMMAND_TYPE_GET), &factor);
    return factor;
}

void LeptonFLiR::runSysPingCamera() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::runSysPingCamera");
#endif

    sendCommand(commandCode(LEP_CID_SYS_PING, LEP_I2C_COMMAND_TYPE_RUN));
}

void LeptonFLiR::setSysTelemetryLocation(LEP_SYS_TELEMETRY_LOCATION location) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setSysTelemetryLocation");
#endif

    sendCommand(commandCode(LEP_CID_SYS_TELEMETRY_LOCATION, LEP_I2C_COMMAND_TYPE_SET), (uint32_t)location);
}

LEP_SYS_TELEMETRY_LOCATION LeptonFLiR::getSysTelemetryLocation() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getSysTelemetryLocation");
#endif

    uint16_t location;
    receiveCommand(commandCode(LEP_CID_SYS_TELEMETRY_LOCATION, LEP_I2C_COMMAND_TYPE_GET), &location);
    return (LEP_SYS_TELEMETRY_LOCATION)location;
}

void LeptonFLiR::runSysFrameAveraging() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::runSysFrameAveraging");
#endif

    sendCommand(commandCode(LEP_CID_SYS_EXECTUE_FRAME_AVERAGE, LEP_I2C_COMMAND_TYPE_RUN));
}

void LeptonFLiR::setSysNumFramesToAverage(LEP_SYS_FRAME_AVERAGE average) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setSysNumFramesToAverage");
#endif

    sendCommand(commandCode(LEP_CID_SYS_NUM_FRAMES_TO_AVERAGE, LEP_I2C_COMMAND_TYPE_SET), (uint32_t)average);
}

LEP_SYS_FRAME_AVERAGE LeptonFLiR::getSysNumFramesToAverage() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getSysNumFramesToAverage");
#endif

    uint16_t average;
    receiveCommand(commandCode(LEP_CID_SYS_NUM_FRAMES_TO_AVERAGE, LEP_I2C_COMMAND_TYPE_GET), &average);
    return (LEP_SYS_FRAME_AVERAGE)average;
}

void LeptonFLiR::getSysSceneStatistics(LEP_SYS_SCENE_STATISTICS *statistics) {
    if (!statistics) return;

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getSysSceneStatistics");
#endif

    receiveCommand(commandCode(LEP_CID_SYS_SCENE_STATISTICS, LEP_I2C_COMMAND_TYPE_GET), (uint16_t *)statistics, sizeof(LEP_SYS_SCENE_STATISTICS) / 2);
}

void LeptonFLiR::setSysSceneRegion(LEP_SYS_SCENE_ROI *region) {
    if (!region) return;

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setSysSceneRegion");
#endif

    sendCommand(commandCode(LEP_CID_SYS_SCENE_ROI, LEP_I2C_COMMAND_TYPE_SET), (uint16_t *)region, sizeof(LEP_SYS_SCENE_ROI) / 2);
}

void LeptonFLiR::getSysSceneRegion(LEP_SYS_SCENE_ROI *region) {
    if (!region) return;

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getSysSceneRegion");
#endif

    receiveCommand(commandCode(LEP_CID_SYS_SCENE_ROI, LEP_I2C_COMMAND_TYPE_GET), (uint16_t *)region, sizeof(LEP_SYS_SCENE_ROI) / 2);
}

uint16_t LeptonFLiR::getSysThermalShutdownCount() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getSysThermalShutdownCount");
#endif

    uint16_t count;
    receiveCommand(commandCode(LEP_CID_SYS_THERMAL_SHUTDOWN_COUNT, LEP_I2C_COMMAND_TYPE_GET), &count);
    return count;
}

void LeptonFLiR::setSysShutterPosition(LEP_SYS_SHUTTER_POSITION position) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setSysShutterPosition");
#endif

    sendCommand(commandCode(LEP_CID_SYS_SHUTTER_POSITION, LEP_I2C_COMMAND_TYPE_SET), (uint32_t)position);
}

LEP_SYS_SHUTTER_POSITION LeptonFLiR::getSysShutterPosition() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getSysShutterPosition");
#endif

    uint16_t position;
    receiveCommand(commandCode(LEP_CID_SYS_SHUTTER_POSITION, LEP_I2C_COMMAND_TYPE_GET), &position);
    return (LEP_SYS_SHUTTER_POSITION)position;
}

void LeptonFLiR::setSysFFCShutterMode(LEP_SYS_FFC_SHUTTER_MODE *mode) {
    if (!mode) return;

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setSysFFCShutterMode");
#endif

    sendCommand(commandCode(LEP_CID_SYS_FFC_SHUTTER_MODE, LEP_I2C_COMMAND_TYPE_SET), (uint16_t *)mode, sizeof(LEP_SYS_FFC_SHUTTER_MODE) / 2);
}

void LeptonFLiR::getSysFFCShutterMode(LEP_SYS_FFC_SHUTTER_MODE *mode) {
    if (!mode) return;

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getSysFFCShutterMode");
#endif

    receiveCommand(commandCode(LEP_CID_SYS_FFC_SHUTTER_MODE, LEP_I2C_COMMAND_TYPE_GET), (uint16_t *)mode, sizeof(LEP_SYS_FFC_SHUTTER_MODE) / 2);
}

void LeptonFLiR::runSysFFCNormalization() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::runSysFFCNormalization");
#endif

    sendCommand(commandCode(LEP_CID_SYS_RUN_FFC, LEP_I2C_COMMAND_TYPE_RUN));
}

LEP_SYS_FFC_STATUS LeptonFLiR::getSysFFCNormalizationStatus() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getSysFFCNormalizationStatus");
#endif

    uint16_t status;
    receiveCommand(commandCode(LEP_CID_SYS_FFC_STATUS, LEP_I2C_COMMAND_TYPE_GET), &status);
    return (LEP_SYS_FFC_STATUS)status;
}

void LeptonFLiR::setVidUserColorLUT(LEP_VID_LUT_BUFFER *table) {
    if (!table) return;

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setVidUserColorLUT");
#endif

    sendCommand(commandCode(LEP_CID_VID_LUT_TRANSFER, LEP_I2C_COMMAND_TYPE_SET), (uint16_t *)table, sizeof(LEP_VID_LUT_BUFFER) / 2);
}

void LeptonFLiR::getVidUserColorLUT(LEP_VID_LUT_BUFFER *table) {
    if (!table) return;

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getVidUserColorLUT");
#endif

    receiveCommand(commandCode(LEP_CID_VID_LUT_TRANSFER, LEP_I2C_COMMAND_TYPE_GET), (uint16_t *)table, sizeof(LEP_VID_LUT_BUFFER) / 2);
}

void LeptonFLiR::setVidFocusRegion(LEP_VID_FOCUS_ROI *region) {
    if (!region) return;

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setVidFocusRegion");
#endif

    sendCommand(commandCode(LEP_CID_VID_FOCUS_ROI, LEP_I2C_COMMAND_TYPE_SET), (uint16_t *)region, sizeof(LEP_VID_FOCUS_ROI) / 2);
}

void LeptonFLiR::getVidFocusRegion(LEP_VID_FOCUS_ROI *region) {
    if (!region) return;

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getVidFocusRegion");
#endif

    receiveCommand(commandCode(LEP_CID_VID_FOCUS_ROI, LEP_I2C_COMMAND_TYPE_GET), (uint16_t *)region, sizeof(LEP_VID_FOCUS_ROI) / 2);
}

void LeptonFLiR::setVidFocusThreshold(uint32_t threshold) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setVidFocusThreshold");
#endif

    sendCommand(commandCode(LEP_CID_VID_FOCUS_THRESHOLD, LEP_I2C_COMMAND_TYPE_SET), threshold);
}

uint32_t LeptonFLiR::getVidFocusThreshold() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getVidFocusThreshold");
#endif

    uint32_t threshold;
    receiveCommand(commandCode(LEP_CID_VID_FOCUS_THRESHOLD, LEP_I2C_COMMAND_TYPE_GET), &threshold);
    return threshold;
}

uint32_t LeptonFLiR::getVidFocusMetric() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getVidFocusMetric");
#endif

    uint32_t metric;
    receiveCommand(commandCode(LEP_CID_VID_FOCUS_METRIC, LEP_I2C_COMMAND_TYPE_GET), &metric);
    return metric;
}

void LeptonFLiR::setVidSceneBasedNUCEnabled(bool enabled) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setVidSceneBasedNUCEnabled");
#endif

    sendCommand(commandCode(LEP_CID_VID_SBNUC_ENABLE, LEP_I2C_COMMAND_TYPE_SET), (uint32_t)enabled);
}

bool LeptonFLiR::getVidSceneBasedNUCEnabled() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getVidSceneBasedNUCEnabled");
#endif

    uint16_t enabled;
    receiveCommand(commandCode(LEP_CID_VID_SBNUC_ENABLE, LEP_I2C_COMMAND_TYPE_GET), &enabled);
    return enabled;
}

void LeptonFLiR::setVidGamma(uint32_t gamma) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::setVidGamma");
#endif

    sendCommand(commandCode(LEP_CID_VID_GAMMA_SELECT, LEP_I2C_COMMAND_TYPE_SET), gamma);
}

uint32_t LeptonFLiR::getVidGamma() {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("LeptonFLiR::getVidGamma");
#endif

    uint32_t gamma;
    receiveCommand(commandCode(LEP_CID_VID_GAMMA_SELECT, LEP_I2C_COMMAND_TYPE_GET), &gamma);
    return gamma;
}

#endif

static void byteToHexString(uint8_t value, char *buffer) {
    uint8_t highNibble = value / 16;
    uint8_t lowNibble = value % 16;
    if (highNibble < 10) buffer[0] = '0' + highNibble;
    else buffer[0] = 'A' + (highNibble - 10);
    if (lowNibble < 10) buffer[1] = '0' + lowNibble;
    else buffer[1] = 'A' + (lowNibble - 10);
}

void wordsToHexString(uint16_t *dataWords, int dataLength, char *buffer, int maxLength) {
    bool insertColons = maxLength >= (dataLength * 4) + (dataLength - 1);

    while (dataLength-- > 0 && maxLength > 3) {
        if (maxLength > 3) {
            byteToHexString(highByte(*dataWords), buffer);
            buffer += 2; maxLength -= 2;
            byteToHexString(lowByte(*dataWords), buffer);
            buffer += 2; maxLength -= 2;
            ++dataWords;
        }

        if (dataLength > 0 && insertColons && maxLength-- > 0)
            *buffer++ = ':';
    }

    if (maxLength-- > 0)
        *buffer++ = '\0';
}

float kelvin100ToCelsius(uint16_t kelvin100) {
    float kelvin = (kelvin100 / 100) + ((kelvin100 % 100) * 0.01f);
    return kelvin - 273.15f;
}

float kelvin100ToFahrenheit(uint16_t kelvin100) {
    float kelvin = (kelvin100 / 100) + ((kelvin100 % 100) * 0.01f);
    return roundf((((kelvin * 9.0f) / 5.0f) - 459.67f) * 100.0f) / 100.0f;
}

float kelvin100ToKelvin(uint16_t kelvin100) {
    return (kelvin100 / 100) + ((kelvin100 % 100) * 0.01f);
}

uint16_t celsiusToKelvin100(float celsius) {
    float kelvin = celsius + 273.15f;
    return (uint16_t)roundf(kelvin * 100.0f);
}

uint16_t fahrenheitToKelvin100(float fahrenheit) {
    float kelvin = ((fahrenheit + 459.67f) * 5.0f) / 9.0f;
    return (uint16_t)roundf(kelvin * 100.0f);
}

uint16_t kelvinToKelvin100(float kelvin) {
    return (uint16_t)roundf(kelvin * 100.0f);
}

float LeptonFLiR::kelvin100ToTemperature(uint16_t kelvin100) {
    switch (_tempMode) {
        case LeptonFLiR_TemperatureMode_Celsius:
            return kelvin100ToCelsius(kelvin100);
        case LeptonFLiR_TemperatureMode_Fahrenheit:
            return kelvin100ToFahrenheit(kelvin100);
        case LeptonFLiR_TemperatureMode_Kelvin:
            return kelvin100ToKelvin(kelvin100);
        default:
            return 0;
    }
}

uint16_t LeptonFLiR::temperatureToKelvin100(float temperature) {
    switch (_tempMode) {
        case LeptonFLiR_TemperatureMode_Celsius:
            return celsiusToKelvin100(temperature);
        case LeptonFLiR_TemperatureMode_Fahrenheit:
            return fahrenheitToKelvin100(temperature);
        case LeptonFLiR_TemperatureMode_Kelvin:
            return kelvinToKelvin100(temperature);
        default:
            return 0;
    }
}

uint8_t LeptonFLiR::getLastI2CError() {
    return _lastI2CError;
}

LEP_RESULT LeptonFLiR::getLastErrorCode() {
    return (LEP_RESULT)(*((int8_t *)(&_lastErrorCode)));
}

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT

void LeptonFLiR::printModuleInfo() {
    uint16_t data[32];

    Serial.println("SYS Camera Status:");
    receiveCommand(commandCode(LEP_CID_SYS_CAM_STATUS, LEP_I2C_COMMAND_TYPE_GET), data, 32);

    Serial.println("SYS Customer Serial Number:");
    receiveCommand(commandCode(LEP_CID_SYS_CUST_SERIAL_NUMBER, LEP_I2C_COMMAND_TYPE_GET), data, 32);

    Serial.println("SYS FLiR Serial Number:");
    receiveCommand(commandCode(LEP_CID_SYS_FLIR_SERIAL_NUMBER, LEP_I2C_COMMAND_TYPE_GET), data, 32);

    Serial.println("SYS Camera Uptime:");
    receiveCommand(commandCode(LEP_CID_SYS_CAM_UPTIME, LEP_I2C_COMMAND_TYPE_GET), data, 32);
    
    Serial.println("SYS Aux Temperature Kelvin:");
    receiveCommand(commandCode(LEP_CID_SYS_AUX_TEMPERATURE_KELVIN, LEP_I2C_COMMAND_TYPE_GET), data, 32);

    Serial.println("SYS FPA Temperature Kelvin:");
    receiveCommand(commandCode(LEP_CID_SYS_FPA_TEMPERATURE_KELVIN, LEP_I2C_COMMAND_TYPE_GET), data, 32);

    Serial.println("AGC Enable State:");
    receiveCommand(commandCode(LEP_CID_AGC_ENABLE_STATE, LEP_I2C_COMMAND_TYPE_GET), data, 32);

    Serial.println("SYS Telemetry Enable State:");
    receiveCommand(commandCode(LEP_CID_SYS_TELEMETRY_ENABLE_STATE, LEP_I2C_COMMAND_TYPE_GET), data, 32);
}

#endif

bool LeptonFLiR::waitCommandBegin(int timeout) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("    LeptonFLiR::waitCommandBegin");
#endif

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.print("  ");
#endif
    uint16_t status;
    readRegister(LEP_I2C_STATUS_REG, &status, 1, 1);
    
    if (!(status & LEP_I2C_STATUS_BUSY_BIT_MASK)) {
        _lastErrorCode = 0;
        return true;
    }

    unsigned long endTime = millis() + (unsigned long)timeout;

    while ((status & LEP_I2C_STATUS_BUSY_BIT_MASK) && (timeout <= 0 || millis() < endTime)) {
#ifdef LEPFLIR_USE_SCHEDULER
        Scheduler.yield();
#else
        delay(1);
#endif

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
        Serial.print("  ");
#endif
        readRegister(LEP_I2C_STATUS_REG, &status, 1, 1);
    }

    if (!(status & LEP_I2C_STATUS_BUSY_BIT_MASK)) {
        _lastErrorCode = 0;
        return true;
    }
    else {
        _lastErrorCode = LEP_TIMEOUT_ERROR;
        return false;
    }
}

bool LeptonFLiR::waitCommandFinish(int timeout) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.println("    LeptonFLiR::waitCommandFinish");
#endif

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.print("  ");
#endif
    uint16_t status;
    readRegister(LEP_I2C_STATUS_REG, &status, 1, 1);
    
    if (!(status & LEP_I2C_STATUS_BUSY_BIT_MASK)) {
        _lastErrorCode = (uint8_t)((status & LEP_I2C_STATUS_ERROR_CODE_BIT_MASK) >> LEP_I2C_STATUS_ERROR_CODE_BIT_SHIFT);
        return true;
    }

    unsigned long endTime = millis() + (unsigned long)timeout;

    while ((status & LEP_I2C_STATUS_BUSY_BIT_MASK) && (timeout <= 0 || millis() < endTime)) {
#ifdef LEPFLIR_USE_SCHEDULER
        Scheduler.yield();
#else
        delay(1);
#endif

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
        Serial.print("  ");
#endif
        readRegister(LEP_I2C_STATUS_REG, &status, 1, 1);
    }

    if (!(status & LEP_I2C_STATUS_BUSY_BIT_MASK)) {
        _lastErrorCode = (uint8_t)((status & LEP_I2C_STATUS_ERROR_CODE_BIT_MASK) >> LEP_I2C_STATUS_ERROR_CODE_BIT_SHIFT);
        return true;
    }
    else {
        _lastErrorCode = LEP_TIMEOUT_ERROR;
        return false;
    }
}

uint16_t LeptonFLiR::commandCode(uint16_t cmdID, uint16_t cmdType) {
    return (cmdID & LEP_I2C_COMMAND_MODULE_ID_BIT_MASK) | (cmdID & LEP_I2C_COMMAND_ID_BIT_MASK) | (cmdType & LEP_I2C_COMMAND_TYPE_BIT_MASK);
}

void LeptonFLiR::sendCommand(uint16_t cmdCode) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.print("  LeptonFLiR::sendCommand cmdCode: 0x");
    Serial.println(cmdCode, HEX);
#endif

    if (!waitCommandBegin(LEPFLIR_GEN_CMD_TIMEOUT))
        return;

    uint16_t cmdBuffer[2] = { cmdCode, 0 };
    if (writeRegister(LEP_I2C_COMMAND_REG, cmdBuffer, 2) == 0)
        waitCommandFinish(LEPFLIR_GEN_CMD_TIMEOUT);
}

void LeptonFLiR::sendCommand(uint16_t cmdCode, uint16_t value) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.print("  LeptonFLiR::sendCommand cmdCode: 0x");
    Serial.println(cmdCode, HEX);
#endif

    if (!waitCommandBegin(LEPFLIR_GEN_CMD_TIMEOUT))
        return;

    uint16_t cmdBuffer[3] = { cmdCode, (uint16_t)1, value };
    if (writeRegister(LEP_I2C_COMMAND_REG, cmdBuffer, 3) == 0)
        waitCommandFinish(LEPFLIR_GEN_CMD_TIMEOUT);
}

void LeptonFLiR::sendCommand(uint16_t cmdCode, uint32_t value) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.print("  LeptonFLiR::sendCommand cmdCode: 0x");
    Serial.println(cmdCode, HEX);
#endif

    if (!waitCommandBegin(LEPFLIR_GEN_CMD_TIMEOUT))
        return;

    uint16_t cmdBuffer[4] = { cmdCode, (uint16_t)2, (uint16_t)((value >> 16) & 0xFFFF), (uint16_t)(value & 0xFFFF) };
    if (writeRegister(LEP_I2C_COMMAND_REG, cmdBuffer, 4) == 0)
        waitCommandFinish(LEPFLIR_GEN_CMD_TIMEOUT);
}

void LeptonFLiR::sendCommand(uint16_t cmdCode, uint16_t *dataWords, int dataLength) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.print("  LeptonFLiR::sendCommand cmdCode: 0x");
    Serial.println(cmdCode, HEX);
#endif

    if (!waitCommandBegin(LEPFLIR_GEN_CMD_TIMEOUT))
        return;

    int retStat;
    if (dataLength <= 16) {
        uint16_t cmdBuffer[2] = { cmdCode, (uint16_t)dataLength };
        retStat = writeRegister(LEP_I2C_COMMAND_REG, cmdBuffer, 2, dataWords, dataLength);
    }
    else if (dataLength < LEP_I2C_DATA_BUFFER_0_LENGTH / 2) {
        if ((retStat = writeRegister(LEP_I2C_DATA_BUFFER_0, dataWords, dataLength)) == 0) {
            uint16_t cmdBuffer[2] = { cmdCode, (uint16_t)dataLength };
            retStat = writeRegister(LEP_I2C_COMMAND_REG, cmdBuffer, 2);
        }
    }
    else
        retStat = (_lastI2CError = 4);

    if (retStat == 0)
        waitCommandFinish(LEPFLIR_GEN_CMD_TIMEOUT);
}

int LeptonFLiR::receiveCommand(uint16_t cmdCode, uint16_t *value) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.print("  LeptonFLiR::receiveCommand cmdCode: 0x");
    Serial.println(cmdCode, HEX);
#endif

    if (!waitCommandBegin(LEPFLIR_GEN_CMD_TIMEOUT))
        return 0;

    uint16_t cmdBuffer[2] = { cmdCode, (uint16_t)0 };
    if (writeRegister(LEP_I2C_COMMAND_REG, cmdBuffer, 2) == 0) {
        if (waitCommandFinish(LEPFLIR_GEN_CMD_TIMEOUT)) {

            uint16_t respLength;
            if (readRegister(LEP_I2C_DATA_LENGTH_REG, &respLength, 1, 1) == 0) {
                respLength /= 2;

                if ((respLength == 1 && readRegister((uint16_t *)value, 1, 1) == 0) ||
                    (respLength > 1 && respLength <= 16 && readRegister(LEP_I2C_DATA_0_REG + ((respLength - 1) * 0x02), (uint16_t *)value, 1, 1) == 0) ||
                    (respLength > 16 && respLength < LEP_I2C_DATA_BUFFER_0_LENGTH / 2 && readRegister(LEP_I2C_DATA_BUFFER_0 + ((respLength - 1) * 0x02), (uint16_t *)value, 1, 1) == 0))
                    return 1;
                else
                    _lastI2CError = 4;
            }
        }
    }

    return 0;
}

int LeptonFLiR::receiveCommand(uint16_t cmdCode, uint32_t *value) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.print("  LeptonFLiR::receiveCommand cmdCode: 0x");
    Serial.println(cmdCode, HEX);
#endif

    if (!waitCommandBegin(LEPFLIR_GEN_CMD_TIMEOUT))
        return 0;

    uint16_t cmdBuffer[2] = { cmdCode, (uint16_t)0 };
    if (writeRegister(LEP_I2C_COMMAND_REG, cmdBuffer, 2) == 0) {
        if (waitCommandFinish(LEPFLIR_GEN_CMD_TIMEOUT)) {

            uint16_t respLength;
            if (readRegister(LEP_I2C_DATA_LENGTH_REG, &respLength, 1, 1) == 0) {
                respLength /= 2;

                if ((respLength == 2 && readRegister((uint16_t *)value, 2, 2) == 0) ||
                    (respLength > 1 && respLength <= 16 && readRegister(LEP_I2C_DATA_0_REG + ((respLength - 1) * 0x02), (uint16_t *)value, 2, 2) == 0) ||
                    (respLength > 16 && respLength < LEP_I2C_DATA_BUFFER_0_LENGTH / 2 && readRegister(LEP_I2C_DATA_BUFFER_0 + ((respLength - 2) * 0x02), (uint16_t *)value, 2, 2) == 0))
                    return 2;
                else
                    _lastI2CError = 4;
            }
        }
    }

    return 0;
}

int LeptonFLiR::receiveCommand(uint16_t cmdCode, uint16_t *respBuffer, int maxLength) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.print("  LeptonFLiR::receiveCommand cmdCode: 0x");
    Serial.println(cmdCode, HEX);
#endif

    if (!waitCommandBegin(LEPFLIR_GEN_CMD_TIMEOUT))
        return 0;

    uint16_t cmdBuffer[2] = { cmdCode, (uint16_t)0 };
    if (writeRegister(LEP_I2C_COMMAND_REG, cmdBuffer, 2) == 0) {
        if (waitCommandFinish(LEPFLIR_GEN_CMD_TIMEOUT)) {

            uint16_t respLength;
            if (readRegister(LEP_I2C_DATA_LENGTH_REG, &respLength, 1, 1) == 0) {
                respLength /= 2;

                if ((respLength > 0 && respLength <= 16 && readRegister(respBuffer, respLength, maxLength) == 0) ||
                    (respLength > 16 && respLength < LEP_I2C_DATA_BUFFER_0_LENGTH / 2 && readRegister(LEP_I2C_DATA_BUFFER_0, respBuffer, respLength, maxLength) == 0))
                    return respLength;
                else
                    _lastI2CError = 4;
            }
        }
    }

    return 0;
}

int LeptonFLiR::sendReceiveCommand(uint16_t cmdCode, uint16_t *dataWords, int dataLength, uint16_t *respBuffer, int maxLength) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.print("  LeptonFLiR::sendReceiveCommand cmdCode: 0x");
    Serial.println(cmdCode, HEX);
#endif

    if (!waitCommandBegin(LEPFLIR_GEN_CMD_TIMEOUT))
        return 0;

    int retStat;
    if (dataLength <= 16) {
        uint16_t cmdBuffer[2] = { cmdCode, (uint16_t)dataLength };
        retStat = writeRegister(LEP_I2C_COMMAND_REG, cmdBuffer, 2, dataWords, dataLength);
    }
    else if (dataLength < LEP_I2C_DATA_BUFFER_0_LENGTH / 2) {
        if ((retStat = writeRegister(LEP_I2C_DATA_BUFFER_0, dataWords, dataLength)) == 0) {
            uint16_t cmdBuffer[2] = { cmdCode, (uint16_t)dataLength };
            retStat = writeRegister(LEP_I2C_COMMAND_REG, cmdBuffer, 2);
        }
    }
    else
        retStat = (_lastI2CError = 4);
    
    if (retStat == 0) {
        if (waitCommandFinish(LEPFLIR_GEN_CMD_TIMEOUT)) {

            uint16_t respLength;
            if (readRegister(LEP_I2C_DATA_LENGTH_REG, &respLength, 1, 1) == 0) {
                respLength /= 2;

                if ((respLength > 0 && respLength <= 16 && readRegister(respBuffer, respLength, maxLength) == 0) ||
                    (respLength > 16 && respLength < LEP_I2C_DATA_BUFFER_0_LENGTH / 2 && readRegister(LEP_I2C_DATA_BUFFER_0, respBuffer, respLength, maxLength) == 0))
                    return respLength;
                else
                    _lastI2CError = 4;
            }
        }
    }

    return 0;
}

int LeptonFLiR::writeRegister(uint16_t regAddress, uint16_t *dataWords, int dataLength) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.print("    LeptonFLiR::writeRegister regAddress: 0x");
    Serial.print(regAddress, HEX);
    Serial.print(", dataWords[");
    Serial.print(dataLength);
    Serial.print("]: ");
    for (int i = 0; i < dataLength; ++i) {
        Serial.print(i > 0 ? "-0x" : "0x");
        Serial.print(dataWords[i], HEX);
    }
    Serial.println("");
#endif

    i2cWire_beginTransmission(LEP_I2C_DEVICE_ADDRESS);

    i2cWire_write(highByte(regAddress));
    i2cWire_write(lowByte(regAddress));

    while (dataLength-- > 0) {
        i2cWire_write(highByte(*dataWords));
        i2cWire_write(lowByte(*dataWords++));
    }

    return i2cWire_endTransmission();
}

int LeptonFLiR::writeRegister(uint16_t regAddress, uint16_t *dataWords1, int dataLength1, uint16_t *dataWords2, int dataLength2) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.print("    LeptonFLiR::writeRegister regAddress: 0x");
    Serial.print(regAddress, HEX);
    Serial.print(", dataWords[");
    Serial.print(dataLength1 + dataLength2);
    Serial.print("]: ");
    for (int i = 0; i < dataLength1 + dataLength2; ++i) {
        Serial.print(i > 0 ? "-0x" : "0x");
        Serial.print(i < dataLength1 ? dataWords1[i] : dataWords2[i - dataLength1], HEX);
    }
    Serial.println("");
#endif

    i2cWire_beginTransmission(LEP_I2C_DEVICE_ADDRESS);

    i2cWire_write(highByte(regAddress));
    i2cWire_write(lowByte(regAddress));

    while (dataLength1-- > 0) {
        i2cWire_write(highByte(*dataWords1));
        i2cWire_write(lowByte(*dataWords1++));
    }

    while (dataLength2-- > 0) {
        i2cWire_write(highByte(*dataWords2));
        i2cWire_write(lowByte(*dataWords2++));
    }

    return i2cWire_endTransmission();
}

int LeptonFLiR::readRegister(uint16_t regAddress, uint16_t *respBuffer, int respLength, int maxLength) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
    Serial.print("    LeptonFLiR::readRegister regAddress: 0x");
    Serial.println(regAddress, HEX);
#endif

    i2cWire_beginTransmission(LEP_I2C_DEVICE_ADDRESS);

    i2cWire_write(highByte(regAddress));
    i2cWire_write(lowByte(regAddress));

    if (i2cWire_endTransmission() == 0)
        return readRegister(respBuffer, respLength, maxLength);
    return _lastI2CError;
}

int LeptonFLiR::readRegister(uint16_t *respBuffer, int respLength, int maxLength) {
    int wordsRead = i2cWire_requestFrom(LEP_I2C_DEVICE_ADDRESS, respLength * 2) / 2;

    if (wordsRead > 0) {
#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
        int origWordsRead = wordsRead;
        int origMaxLength = maxLength;
        uint16_t *origRespBuffer = respBuffer;
#endif

        while (wordsRead > 0 && maxLength > 0) {
            --wordsRead; --maxLength;
            *respBuffer = ((uint16_t)(i2cWire_read() & 0xFF) << 8);
            *respBuffer++ |= (uint16_t)(i2cWire_read() & 0xFF);
        }

        while (wordsRead > 0) {
            --wordsRead;
            i2cWire_read();
            i2cWire_read();
        }

#ifdef LEPFLIR_ENABLE_DEBUG_OUTPUT
        Serial.print("      LeptonFLiR::readRegister respBuffer[l:");
        Serial.print(respLength);
        Serial.print(",m:");
        Serial.print(origMaxLength);
        Serial.print(",r:");
        Serial.print(origWordsRead);
        Serial.print("]: ");
        for (int i = 0; i < origWordsRead; ++i) {
            Serial.print(i > 0 ? "-0x" : "0x");
            Serial.print(origRespBuffer[i], HEX);
        }
        Serial.println("");
#endif

        return (_lastI2CError = 0);
    }

    return (_lastI2CError = 4);
}

#ifdef LEPFLIR_USE_SOFTWARE_I2C
bool __attribute__((noinline)) i2c_start(uint8_t addr);
void __attribute__((noinline)) i2c_stop(void) asm("ass_i2c_stop");
bool __attribute__((noinline)) i2c_write(uint8_t value) asm("ass_i2c_write");
uint8_t __attribute__((noinline)) i2c_read(bool last);
#endif

void LeptonFLiR::i2cWire_beginTransmission(uint8_t addr) {
    _lastI2CError = 0;
#ifndef LEPFLIR_USE_SOFTWARE_I2C
    _i2cWire->beginTransmission(addr);
#else
    i2c_start(addr);
#endif
}

uint8_t LeptonFLiR::i2cWire_endTransmission(void) {
#ifndef LEPFLIR_USE_SOFTWARE_I2C
    return (_lastI2CError = _i2cWire->endTransmission());
#else
    i2c_stop();
    return (_lastI2CError = 0);
#endif
}

uint8_t LeptonFLiR::i2cWire_requestFrom(uint8_t addr, uint8_t len) {
#ifndef LEPFLIR_USE_SOFTWARE_I2C
    return _i2cWire->requestFrom(addr, len);
#else
    i2c_start(addr | 0x01);
    return (_readBytes = len);
#endif
}

size_t LeptonFLiR::i2cWire_write(uint8_t data) {
#ifndef LEPFLIR_USE_SOFTWARE_I2C
    return _i2cWire->write(data);
#else
    return (size_t)i2c_write(data);
#endif
}

int LeptonFLiR::i2cWire_read(void) {
#ifndef LEPFLIR_USE_SOFTWARE_I2C
    return _i2cWire->read();
#else
    if (_readBytes > 1)
        return (int)i2c_read(_readBytes--);
    else {
        _readBytes = 0;
        int retVal = (int)i2c_read(true);
        i2c_stop();
        return retVal;
    }
#endif
}
