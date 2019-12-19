#include "dv_explorer.h"

#include <math.h>

static void dvExplorerLog(enum caer_log_level logLevel, dvExplorerHandle handle, const char *format, ...)
	ATTRIBUTE_FORMAT(3);
static bool dvExplorerSendDefaultFPGAConfig(caerDeviceHandle cdh);
static bool dvExplorerSendDefaultBiasConfig(caerDeviceHandle cdh);
static void dvExplorerEventTranslator(void *vhd, const uint8_t *buffer, size_t bytesSent);
static void dvExplorerTSMasterStatusUpdater(void *userDataPtr, int status, uint32_t param);

// FX3 Debug Transfer Support
static void allocateDebugTransfers(dvExplorerHandle handle);
static void cancelAndDeallocateDebugTransfers(dvExplorerHandle handle);
static void LIBUSB_CALL libUsbDebugCallback(struct libusb_transfer *transfer);
static void debugTranslator(dvExplorerHandle handle, const uint8_t *buffer, size_t bytesSent);

static void dvExplorerLog(enum caer_log_level logLevel, dvExplorerHandle handle, const char *format, ...) {
	va_list argumentList;
	va_start(argumentList, format);
	caerLogVAFull(atomic_load_explicit(&handle->state.deviceLogLevel, memory_order_relaxed), logLevel,
		handle->info.deviceString, format, argumentList);
	va_end(argumentList);
}

ssize_t dvExplorerFind(caerDeviceDiscoveryResult *discoveredDevices) {
	// Set to NULL initially (for error return).
	*discoveredDevices = NULL;

	struct usb_info *foundDVExplorer = NULL;

	ssize_t result = usbDeviceFind(USB_DEFAULT_DEVICE_VID, DV_EXPLORER_DEVICE_PID, DV_EXPLORER_REQUIRED_LOGIC_VERSION,
		DV_EXPLORER_REQUIRED_LOGIC_PATCH_LEVEL, DV_EXPLORER_REQUIRED_FIRMWARE_VERSION, &foundDVExplorer);

	if (result <= 0) {
		// Error or nothing found, return right away.
		return (result);
	}

	// Allocate memory for discovered devices in expected format.
	*discoveredDevices = calloc((size_t) result, sizeof(struct caer_device_discovery_result));
	if (*discoveredDevices == NULL) {
		free(foundDVExplorer);
		return (-1);
	}

	// Transform from generic USB format into device discovery one.
	caerLogDisable(true);
	for (size_t i = 0; i < (size_t) result; i++) {
		// This is a DV_EXPLORER.
		(*discoveredDevices)[i].deviceType         = CAER_DEVICE_DV_EXPLORER;
		(*discoveredDevices)[i].deviceErrorOpen    = foundDVExplorer[i].errorOpen;
		(*discoveredDevices)[i].deviceErrorVersion = foundDVExplorer[i].errorVersion;
		struct caer_dvx_info *dvExplorerInfoPtr    = &((*discoveredDevices)[i].deviceInfo.dvExplorerInfo);

		dvExplorerInfoPtr->deviceUSBBusNumber     = foundDVExplorer[i].busNumber;
		dvExplorerInfoPtr->deviceUSBDeviceAddress = foundDVExplorer[i].devAddress;
		strncpy(dvExplorerInfoPtr->deviceSerialNumber, foundDVExplorer[i].serialNumber, MAX_SERIAL_NUMBER_LENGTH + 1);

		dvExplorerInfoPtr->firmwareVersion = foundDVExplorer[i].firmwareVersion;
		dvExplorerInfoPtr->logicVersion    = (!foundDVExplorer[i].errorOpen) ? (foundDVExplorer[i].logicVersion) : (-1);

		// Reopen DVS128 device to get additional info, if possible at all.
		if (!foundDVExplorer[i].errorOpen && !foundDVExplorer[i].errorVersion) {
			caerDeviceHandle dvs = dvExplorerOpen(
				0, dvExplorerInfoPtr->deviceUSBBusNumber, dvExplorerInfoPtr->deviceUSBDeviceAddress, NULL);
			if (dvs != NULL) {
				*dvExplorerInfoPtr = caerDVExplorerInfoGet(dvs);

				dvExplorerClose(dvs);
			}
		}

		// Set/Reset to invalid values, not part of discovery.
		dvExplorerInfoPtr->deviceID     = -1;
		dvExplorerInfoPtr->deviceString = NULL;
	}
	caerLogDisable(false);

	free(foundDVExplorer);
	return (result);
}

static inline float calculateIMUAccelScale(uint8_t imuAccelScale) {
	// Accelerometer scale is:
	// 0 - +- 2 g  - 16384 LSB/g
	// 1 - +- 4 g  - 8192 LSB/g
	// 2 - +- 8 g  - 4096 LSB/g
	// 3 - +- 16 g - 2048 LSB/g
	float accelScale = 65536.0F / (float) U32T(4 * (1 << imuAccelScale));

	return (accelScale);
}

static inline float calculateIMUGyroScale(uint8_t imuGyroScale) {
	// Invert for ascending scale:
	uint8_t imuGyroScaleAsc = U8T(4 - imuGyroScale);

	// Gyroscope ascending scale is:
	// 0 - +- 125 °/s  - 262.4 LSB/°/s
	// 1 - +- 250 °/s  - 131.2 LSB/°/s
	// 2 - +- 500 °/s  - 65.6 LSB/°/s
	// 3 - +- 1000 °/s - 32.8 LSB/°/s
	// 4 - +- 2000 °/s - 16.4 LSB/°/s
	float gyroScale = 65536.0F / (float) U32T(250 * (1 << imuGyroScaleAsc));

	return (gyroScale);
}

static inline void freeAllDataMemory(dvExplorerState state) {
	dataExchangeDestroy(&state->dataExchange);

	// Since the current event packets aren't necessarily
	// already assigned to the current packet container, we
	// free them separately from it.
	if (state->currentPackets.polarity != NULL) {
		free(&state->currentPackets.polarity->packetHeader);
		state->currentPackets.polarity = NULL;

		containerGenerationSetPacket(&state->container, POLARITY_EVENT, NULL);
	}

	if (state->currentPackets.special != NULL) {
		free(&state->currentPackets.special->packetHeader);
		state->currentPackets.special = NULL;

		containerGenerationSetPacket(&state->container, SPECIAL_EVENT, NULL);
	}

	if (state->currentPackets.imu6 != NULL) {
		free(&state->currentPackets.imu6->packetHeader);
		state->currentPackets.imu6 = NULL;

		containerGenerationSetPacket(&state->container, IMU6_EVENT_PKT_POS, NULL);
	}

	containerGenerationDestroy(&state->container);
}

caerDeviceHandle dvExplorerOpen(
	uint16_t deviceID, uint8_t busNumberRestrict, uint8_t devAddressRestrict, const char *serialNumberRestrict) {
	errno = 0;

	caerLog(CAER_LOG_DEBUG, __func__, "Initializing %s.", DV_EXPLORER_DEVICE_NAME);

	dvExplorerHandle handle = calloc(1, sizeof(*handle));
	if (handle == NULL) {
		// Failed to allocate memory for device handle!
		caerLog(CAER_LOG_CRITICAL, __func__, "Failed to allocate memory for device handle.");
		errno = CAER_ERROR_MEMORY_ALLOCATION;
		return (NULL);
	}

	// Set main deviceType correctly right away.
	handle->deviceType = CAER_DEVICE_DV_EXPLORER;

	dvExplorerState state = &handle->state;

	// Initialize state variables to default values (if not zero, taken care of by calloc above).
	dataExchangeSettingsInit(&state->dataExchange);

	// Packet settings (size (in events) and time interval (in µs)).
	containerGenerationSettingsInit(&state->container);

	// Logging settings (initialize to global log-level).
	enum caer_log_level globalLogLevel = caerLogLevelGet();
	atomic_store(&state->deviceLogLevel, globalLogLevel);
	atomic_store(&state->usbState.usbLogLevel, globalLogLevel);

	// Set device thread name. Maximum length of 15 chars due to Linux limitations.
	char usbThreadName[MAX_THREAD_NAME_LENGTH + 1];
	snprintf(usbThreadName, MAX_THREAD_NAME_LENGTH + 1, "%s %" PRIu16, DV_EXPLORER_DEVICE_NAME, deviceID);
	usbThreadName[MAX_THREAD_NAME_LENGTH] = '\0';

	usbSetThreadName(&state->usbState, usbThreadName);
	handle->info.deviceString = usbThreadName; // Temporary, until replaced by full string.

	// Try to open a DV_EXPLORER device on a specific USB port.
	struct usb_info usbInfo;

	if (!usbDeviceOpen(&state->usbState, USB_DEFAULT_DEVICE_VID, DV_EXPLORER_DEVICE_PID, busNumberRestrict,
			devAddressRestrict, serialNumberRestrict, DV_EXPLORER_REQUIRED_LOGIC_VERSION,
			DV_EXPLORER_REQUIRED_LOGIC_PATCH_LEVEL, DV_EXPLORER_REQUIRED_FIRMWARE_VERSION, &usbInfo)) {
		if (errno == CAER_ERROR_OPEN_ACCESS) {
			dvExplorerLog(
				CAER_LOG_CRITICAL, handle, "Failed to open device, no matching device could be found or opened.");
		}
		else {
			dvExplorerLog(CAER_LOG_CRITICAL, handle,
				"Failed to open device, see above log message for more information (errno=%d).", errno);
		}

		free(handle);

		// errno set by usbDeviceOpen().
		return (NULL);
	}

	char *usbInfoString = usbGenerateDeviceString(usbInfo, DV_EXPLORER_DEVICE_NAME, deviceID);
	if (usbInfoString == NULL) {
		dvExplorerLog(CAER_LOG_CRITICAL, handle, "Failed to generate USB information string.");

		usbDeviceClose(&state->usbState);
		free(handle);

		errno = CAER_ERROR_MEMORY_ALLOCATION;
		return (NULL);
	}

	// Setup USB.
	usbSetDataCallback(&state->usbState, &dvExplorerEventTranslator, handle);
	usbSetDataEndpoint(&state->usbState, USB_DEFAULT_DATA_ENDPOINT);
	usbSetTransfersNumber(&state->usbState, 8);
	usbSetTransfersSize(&state->usbState, 8192);

	// Start USB handling thread.
	if (!usbThreadStart(&state->usbState)) {
		usbDeviceClose(&state->usbState);
		free(usbInfoString);
		free(handle);

		errno = CAER_ERROR_COMMUNICATION;
		return (NULL);
	}

	// Populate info variables based on data from device.
	uint32_t param32 = 0;

	handle->info.deviceID = I16T(deviceID);
	strncpy(handle->info.deviceSerialNumber, usbInfo.serialNumber, MAX_SERIAL_NUMBER_LENGTH + 1);
	handle->info.deviceUSBBusNumber     = usbInfo.busNumber;
	handle->info.deviceUSBDeviceAddress = usbInfo.devAddress;
	handle->info.deviceString           = usbInfoString;

	handle->info.firmwareVersion = usbInfo.firmwareVersion;
	handle->info.logicVersion    = usbInfo.logicVersion;

	spiConfigReceive(&state->usbState, DVX_SYSINFO, DVX_SYSINFO_CHIP_IDENTIFIER, &param32);
	handle->info.chipID = I16T(param32);
	spiConfigReceive(&state->usbState, DVX_SYSINFO, DVX_SYSINFO_DEVICE_IS_MASTER, &param32);
	handle->info.deviceIsMaster = param32;
	spiConfigReceive(&state->usbState, DVX_SYSINFO, DVX_SYSINFO_LOGIC_CLOCK, &param32);
	state->deviceClocks.logicClock = U16T(param32);
	spiConfigReceive(&state->usbState, DVX_SYSINFO, DVX_SYSINFO_USB_CLOCK, &param32);
	state->deviceClocks.usbClock = U16T(param32);
	spiConfigReceive(&state->usbState, DVX_SYSINFO, DVX_SYSINFO_CLOCK_DEVIATION, &param32);
	state->deviceClocks.clockDeviationFactor = U16T(param32);

	// Calculate actual clock frequencies.
	state->deviceClocks.logicClockActual = (float) ((double) state->deviceClocks.logicClock
													* ((double) state->deviceClocks.clockDeviationFactor / 1000.0));
	state->deviceClocks.usbClockActual   = (float) ((double) state->deviceClocks.usbClock
                                                  * ((double) state->deviceClocks.clockDeviationFactor / 1000.0));

	dvExplorerLog(CAER_LOG_DEBUG, handle, "Clock frequencies: LOGIC %f, USB %f.",
		(double) state->deviceClocks.logicClockActual, (double) state->deviceClocks.usbClockActual);

	spiConfigReceive(&state->usbState, DVX_DVS, DVX_DVS_SIZE_COLUMNS, &param32);
	state->dvs.sizeX = I16T(param32);
	spiConfigReceive(&state->usbState, DVX_DVS, DVX_DVS_SIZE_ROWS, &param32);
	state->dvs.sizeY = I16T(param32);

	spiConfigReceive(&state->usbState, DVX_DVS, DVX_DVS_ORIENTATION_INFO, &param32);
	state->dvs.invertXY = param32 & 0x04;

	dvExplorerLog(CAER_LOG_DEBUG, handle, "DVS Size X: %d, Size Y: %d, Invert: %d.", state->dvs.sizeX, state->dvs.sizeY,
		state->dvs.invertXY);

	if (state->dvs.invertXY) {
		handle->info.dvsSizeX = state->dvs.sizeY;
		handle->info.dvsSizeY = state->dvs.sizeX;
	}
	else {
		handle->info.dvsSizeX = state->dvs.sizeX;
		handle->info.dvsSizeY = state->dvs.sizeY;
	}

	spiConfigReceive(&state->usbState, DVX_IMU, DVX_IMU_TYPE, &param32);
	handle->info.imuType = U8T(param32);

	spiConfigReceive(&state->usbState, DVX_IMU, DVX_IMU_ORIENTATION_INFO, &param32);
	state->imu.flipX = param32 & 0x04;
	state->imu.flipY = param32 & 0x02;
	state->imu.flipZ = param32 & 0x01;

	dvExplorerLog(CAER_LOG_DEBUG, handle, "IMU Flip X: %d, Flip Y: %d, Flip Z: %d.", state->imu.flipX, state->imu.flipY,
		state->imu.flipZ);

	// Extra features:
	spiConfigReceive(&state->usbState, DVX_MUX, DVX_MUX_HAS_STATISTICS, &param32);
	handle->info.muxHasStatistics = param32;

	spiConfigReceive(&state->usbState, DVX_DVS, DVX_DVS_HAS_STATISTICS, &param32);
	handle->info.dvsHasStatistics = param32;

	spiConfigReceive(&state->usbState, DVX_EXTINPUT, DVX_EXTINPUT_HAS_GENERATOR, &param32);
	handle->info.extInputHasGenerator = param32;

	// On FX3, start the debug transfers once everything else is ready.
	allocateDebugTransfers(handle);

	dvExplorerLog(CAER_LOG_DEBUG, handle, "Initialized device successfully with USB Bus=%" PRIu8 ":Addr=%" PRIu8 ".",
		usbInfo.busNumber, usbInfo.devAddress);

	return ((caerDeviceHandle) handle);
}

bool dvExplorerClose(caerDeviceHandle cdh) {
	dvExplorerHandle handle = (dvExplorerHandle) cdh;
	dvExplorerState state   = &handle->state;

	dvExplorerLog(CAER_LOG_DEBUG, handle, "Shutting down ...");

	// Stop debug transfers on FX3 devices.
	cancelAndDeallocateDebugTransfers(handle);

	// Shut down USB handling thread.
	usbThreadStop(&state->usbState);

	// Finally, close the device fully.
	usbDeviceClose(&state->usbState);

	dvExplorerLog(CAER_LOG_DEBUG, handle, "Shutdown successful.");

	// Free memory.
	free(handle->info.deviceString);
	free(handle);

	return (true);
}

struct caer_dvx_info caerDVExplorerInfoGet(caerDeviceHandle cdh) {
	dvExplorerHandle handle = (dvExplorerHandle) cdh;

	// Check if the pointer is valid.
	if (handle == NULL) {
		struct caer_dvx_info emptyInfo = {0, .deviceString = NULL};
		return (emptyInfo);
	}

	// Check if device type is supported.
	if (handle->deviceType != CAER_DEVICE_DV_EXPLORER) {
		struct caer_dvx_info emptyInfo = {0, .deviceString = NULL};
		return (emptyInfo);
	}

	// Return a copy of the device information.
	return (handle->info);
}

bool dvExplorerSendDefaultConfig(caerDeviceHandle cdh) {
	// First send default bias config.
	if (!dvExplorerSendDefaultBiasConfig(cdh)) {
		return (false);
	}

	// Send default FPGA config.
	if (!dvExplorerSendDefaultFPGAConfig(cdh)) {
		return (false);
	}

	return (true);
}

static bool dvExplorerSendDefaultFPGAConfig(caerDeviceHandle cdh) {
	dvExplorerHandle handle = (dvExplorerHandle) cdh;

	dvExplorerConfigSet(cdh, DVX_MUX, DVX_MUX_TIMESTAMP_RESET, false);
	dvExplorerConfigSet(cdh, DVX_MUX, DVX_MUX_DROP_EXTINPUT_ON_TRANSFER_STALL, true);
	dvExplorerConfigSet(cdh, DVX_MUX, DVX_MUX_DROP_DVS_ON_TRANSFER_STALL, false);

	dvExplorerConfigSet(cdh, DVX_DVS, DVX_DVS_WAIT_ON_TRANSFER_STALL, true);

	dvExplorerConfigSet(cdh, DVX_IMU, DVX_IMU_ACCEL_DATA_RATE, BOSCH_ACCEL_800HZ); // 800 Hz.
	dvExplorerConfigSet(cdh, DVX_IMU, DVX_IMU_ACCEL_FILTER, BOSCH_ACCEL_NORMAL);   // Normal mode.
	dvExplorerConfigSet(cdh, DVX_IMU, DVX_IMU_ACCEL_RANGE, BOSCH_ACCEL_4G);        // +- 4 g.
	dvExplorerConfigSet(cdh, DVX_IMU, DVX_IMU_GYRO_DATA_RATE, BOSCH_GYRO_800HZ);   // 800 Hz.
	dvExplorerConfigSet(cdh, DVX_IMU, DVX_IMU_GYRO_FILTER, BOSCH_GYRO_NORMAL);     // Normal mode.
	dvExplorerConfigSet(cdh, DVX_IMU, DVX_IMU_GYRO_RANGE, BOSCH_GYRO_500DPS);      // +- 500 °/s

	dvExplorerConfigSet(cdh, DVX_EXTINPUT, DVX_EXTINPUT_DETECT_RISING_EDGES, false);
	dvExplorerConfigSet(cdh, DVX_EXTINPUT, DVX_EXTINPUT_DETECT_FALLING_EDGES, false);
	dvExplorerConfigSet(cdh, DVX_EXTINPUT, DVX_EXTINPUT_DETECT_PULSES, true);
	dvExplorerConfigSet(cdh, DVX_EXTINPUT, DVX_EXTINPUT_DETECT_PULSE_POLARITY, true);
	dvExplorerConfigSet(cdh, DVX_EXTINPUT, DVX_EXTINPUT_DETECT_PULSE_LENGTH,
		10); // in µs, converted to cycles @ LogicClock later

	if (handle->info.extInputHasGenerator) {
		// Disable generator by default. Has to be enabled manually after sendDefaultConfig() by user!
		dvExplorerConfigSet(cdh, DVX_EXTINPUT, DVX_EXTINPUT_RUN_GENERATOR, false);
		dvExplorerConfigSet(cdh, DVX_EXTINPUT, DVX_EXTINPUT_GENERATE_PULSE_POLARITY, true);
		dvExplorerConfigSet(cdh, DVX_EXTINPUT, DVX_EXTINPUT_GENERATE_PULSE_INTERVAL,
			10); // in µs, converted to cycles @ LogicClock later
		dvExplorerConfigSet(cdh, DVX_EXTINPUT, DVX_EXTINPUT_GENERATE_PULSE_LENGTH,
			5); // in µs, converted to cycles @ LogicClock later
		dvExplorerConfigSet(cdh, DVX_EXTINPUT, DVX_EXTINPUT_GENERATE_INJECT_ON_RISING_EDGE, false);
		dvExplorerConfigSet(cdh, DVX_EXTINPUT, DVX_EXTINPUT_GENERATE_INJECT_ON_FALLING_EDGE, false);
	}

	dvExplorerConfigSet(cdh, DVX_USB, DVX_USB_EARLY_PACKET_DELAY,
		8); // in 125µs time-slices (defaults to 1ms)

	return (true);
}

static bool dvExplorerSendDefaultBiasConfig(caerDeviceHandle cdh) {
	// Default bias configuration.
	return (true);
}

bool dvExplorerConfigSet(caerDeviceHandle cdh, int8_t modAddr, uint8_t paramAddr, uint32_t param) {
	dvExplorerHandle handle = (dvExplorerHandle) cdh;
	dvExplorerState state   = &handle->state;

	switch (modAddr) {
		case CAER_HOST_CONFIG_USB:
			return (usbConfigSet(&state->usbState, paramAddr, param));
			break;

		case CAER_HOST_CONFIG_DATAEXCHANGE:
			return (dataExchangeConfigSet(&state->dataExchange, paramAddr, param));
			break;

		case CAER_HOST_CONFIG_PACKETS:
			return (containerGenerationConfigSet(&state->container, paramAddr, param));
			break;

		case CAER_HOST_CONFIG_LOG:
			switch (paramAddr) {
				case CAER_HOST_CONFIG_LOG_LEVEL:
					atomic_store(&state->deviceLogLevel, U8T(param));

					// Set USB log-level to this value too.
					atomic_store(&state->usbState.usbLogLevel, U8T(param));
					break;

				default:
					return (false);
					break;
			}
			break;

		case DVX_MUX:
			switch (paramAddr) {
				case DVX_MUX_RUN:
				case DVX_MUX_TIMESTAMP_RUN:
				case DVX_MUX_RUN_CHIP:
				case DVX_MUX_DROP_EXTINPUT_ON_TRANSFER_STALL:
				case DVX_MUX_DROP_DVS_ON_TRANSFER_STALL:
					return (spiConfigSend(&state->usbState, DVX_MUX, paramAddr, param));
					break;

				case DVX_MUX_TIMESTAMP_RESET: {
					// Use multi-command VR for more efficient implementation of reset,
					// that also guarantees returning to the default state.
					if (param) {
						struct spi_config_params spiMultiConfig[2];

						spiMultiConfig[0].moduleAddr = DVX_MUX;
						spiMultiConfig[0].paramAddr  = DVX_MUX_TIMESTAMP_RESET;
						spiMultiConfig[0].param      = true;

						spiMultiConfig[1].moduleAddr = DVX_MUX;
						spiMultiConfig[1].paramAddr  = DVX_MUX_TIMESTAMP_RESET;
						spiMultiConfig[1].param      = false;

						return (spiConfigSendMultiple(&state->usbState, spiMultiConfig, 2));
					}
					break;
				}

				default:
					return (false);
					break;
			}
			break;

		case DVX_DVS:
			switch (paramAddr) {
				case DVX_DVS_RUN:
				case DVX_DVS_WAIT_ON_TRANSFER_STALL:
					return (spiConfigSend(&state->usbState, DVX_DVS, paramAddr, param));
					break;

				default:
					return (false);
					break;
			}
			break;

		case DVX_IMU:
			switch (paramAddr) {
				case DVX_IMU_RUN_ACCELEROMETER:
				case DVX_IMU_RUN_GYROSCOPE:
				case DVX_IMU_RUN_TEMPERATURE:
				case DVX_IMU_ACCEL_DATA_RATE:
				case DVX_IMU_ACCEL_FILTER:
				case DVX_IMU_ACCEL_RANGE:
				case DVX_IMU_GYRO_DATA_RATE:
				case DVX_IMU_GYRO_FILTER:
				case DVX_IMU_GYRO_RANGE:
					return (spiConfigSend(&state->usbState, DVX_IMU, paramAddr, param));
					break;

				default:
					return (false);
					break;
			}
			break;

		case DVX_EXTINPUT:
			switch (paramAddr) {
				case DVX_EXTINPUT_RUN_DETECTOR:
				case DVX_EXTINPUT_DETECT_RISING_EDGES:
				case DVX_EXTINPUT_DETECT_FALLING_EDGES:
				case DVX_EXTINPUT_DETECT_PULSES:
				case DVX_EXTINPUT_DETECT_PULSE_POLARITY:
					return (spiConfigSend(&state->usbState, DVX_EXTINPUT, paramAddr, param));
					break;

				case DVX_EXTINPUT_DETECT_PULSE_LENGTH: {
					// Times are in µs on host, but in cycles @ LOGIC_CLOCK_FREQ
					// on FPGA, so we must multiply here.
					float timeCC = roundf((float) param * state->deviceClocks.logicClockActual);
					return (spiConfigSend(&state->usbState, DVX_EXTINPUT, paramAddr, U32T(timeCC)));
					break;
				}

				case DVX_EXTINPUT_RUN_GENERATOR:
				case DVX_EXTINPUT_GENERATE_PULSE_POLARITY:
				case DVX_EXTINPUT_GENERATE_INJECT_ON_RISING_EDGE:
				case DVX_EXTINPUT_GENERATE_INJECT_ON_FALLING_EDGE:
					if (handle->info.extInputHasGenerator) {
						return (spiConfigSend(&state->usbState, DVX_EXTINPUT, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DVX_EXTINPUT_GENERATE_PULSE_INTERVAL:
				case DVX_EXTINPUT_GENERATE_PULSE_LENGTH: {
					if (handle->info.extInputHasGenerator) {
						// Times are in µs on host, but in cycles @ LOGIC_CLOCK_FREQ
						// on FPGA, so we must multiply here.
						float timeCC = roundf((float) param * state->deviceClocks.logicClockActual);
						return (spiConfigSend(&state->usbState, DVX_EXTINPUT, paramAddr, U32T(timeCC)));
					}
					else {
						return (false);
					}
					break;
				}

				default:
					return (false);
					break;
			}
			break;

		case DVX_BIAS:
			break;

		case DVX_SYSINFO:
			// No SystemInfo parameters can ever be set!
			return (false);
			break;

		case DVX_USB:
			switch (paramAddr) {
				case DVX_USB_RUN:
					return (spiConfigSend(&state->usbState, DVX_USB, paramAddr, param));
					break;

				case DVX_USB_EARLY_PACKET_DELAY: {
					// Early packet delay is 125µs slices on host, but in cycles
					// @ USB_CLOCK_FREQ on FPGA, so we must multiply here.
					float delayCC = roundf((float) param * 125.0F * state->deviceClocks.usbClockActual);
					return (spiConfigSend(&state->usbState, DVX_USB, paramAddr, U32T(delayCC)));
					break;
				}

				default:
					return (false);
					break;
			}
			break;

		default:
			return (false);
			break;
	}

	return (true);
}

bool dvExplorerConfigGet(caerDeviceHandle cdh, int8_t modAddr, uint8_t paramAddr, uint32_t *param) {
	dvExplorerHandle handle = (dvExplorerHandle) cdh;
	dvExplorerState state   = &handle->state;

	switch (modAddr) {
		case CAER_HOST_CONFIG_USB:
			return (usbConfigGet(&state->usbState, paramAddr, param));
			break;

		case CAER_HOST_CONFIG_DATAEXCHANGE:
			return (dataExchangeConfigGet(&state->dataExchange, paramAddr, param));
			break;

		case CAER_HOST_CONFIG_PACKETS:
			return (containerGenerationConfigGet(&state->container, paramAddr, param));
			break;

		case CAER_HOST_CONFIG_LOG:
			switch (paramAddr) {
				case CAER_HOST_CONFIG_LOG_LEVEL:
					*param = atomic_load(&state->deviceLogLevel);
					break;

				default:
					return (false);
					break;
			}
			break;

		case DVX_MUX:
			switch (paramAddr) {
				case DVX_MUX_RUN:
				case DVX_MUX_TIMESTAMP_RUN:
				case DVX_MUX_RUN_CHIP:
				case DVX_MUX_DROP_EXTINPUT_ON_TRANSFER_STALL:
				case DVX_MUX_DROP_DVS_ON_TRANSFER_STALL:
					return (spiConfigReceive(&state->usbState, DVX_MUX, paramAddr, param));
					break;

				case DVX_MUX_TIMESTAMP_RESET:
					// Always false because it's an impulse, it resets itself automatically.
					*param = false;
					break;

				case DVX_MUX_STATISTICS_EXTINPUT_DROPPED:
				case DVX_MUX_STATISTICS_EXTINPUT_DROPPED + 1:
				case DVX_MUX_STATISTICS_DVS_DROPPED:
				case DVX_MUX_STATISTICS_DVS_DROPPED + 1:
					if (handle->info.muxHasStatistics) {
						return (spiConfigReceive(&state->usbState, DVX_MUX, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				default:
					return (false);
					break;
			}
			break;

		case DVX_DVS:
			switch (paramAddr) {
				case DVX_DVS_RUN:
				case DVX_DVS_WAIT_ON_TRANSFER_STALL:
					return (spiConfigReceive(&state->usbState, DVX_DVS, paramAddr, param));
					break;

				case DVX_DVS_STATISTICS_TRANSACTIONS_SUCCESS:
				case DVX_DVS_STATISTICS_TRANSACTIONS_SUCCESS + 1:
				case DVX_DVS_STATISTICS_TRANSACTIONS_SKIPPED:
				case DVX_DVS_STATISTICS_TRANSACTIONS_SKIPPED + 1:
					if (handle->info.dvsHasStatistics) {
						return (spiConfigReceive(&state->usbState, DVX_DVS, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				default:
					return (false);
					break;
			}
			break;

		case DVX_IMU:
			switch (paramAddr) {
				case DVX_IMU_RUN_ACCELEROMETER:
				case DVX_IMU_RUN_GYROSCOPE:
				case DVX_IMU_RUN_TEMPERATURE:
				case DVX_IMU_ACCEL_DATA_RATE:
				case DVX_IMU_ACCEL_FILTER:
				case DVX_IMU_ACCEL_RANGE:
				case DVX_IMU_GYRO_DATA_RATE:
				case DVX_IMU_GYRO_FILTER:
				case DVX_IMU_GYRO_RANGE:
					return (spiConfigReceive(&state->usbState, DVX_IMU, paramAddr, param));
					break;

				default:
					return (false);
					break;
			}
			break;

		case DVX_EXTINPUT:
			switch (paramAddr) {
				case DVX_EXTINPUT_RUN_DETECTOR:
				case DVX_EXTINPUT_DETECT_RISING_EDGES:
				case DVX_EXTINPUT_DETECT_FALLING_EDGES:
				case DVX_EXTINPUT_DETECT_PULSES:
				case DVX_EXTINPUT_DETECT_PULSE_POLARITY:
					return (spiConfigReceive(&state->usbState, DVX_EXTINPUT, paramAddr, param));
					break;

				case DVX_EXTINPUT_DETECT_PULSE_LENGTH: {
					// Times are in µs on host, but in cycles @ LOGIC_CLOCK_FREQ
					// on FPGA, so we must divide here.
					uint32_t cyclesValue = 0;
					if (!spiConfigReceive(&state->usbState, DVX_EXTINPUT, paramAddr, &cyclesValue)) {
						return (false);
					}

					float delayCC = roundf((float) cyclesValue / state->deviceClocks.logicClockActual);
					*param        = U32T(delayCC);

					return (true);
					break;
				}

				case DVX_EXTINPUT_RUN_GENERATOR:
				case DVX_EXTINPUT_GENERATE_PULSE_POLARITY:
				case DVX_EXTINPUT_GENERATE_INJECT_ON_RISING_EDGE:
				case DVX_EXTINPUT_GENERATE_INJECT_ON_FALLING_EDGE:
					if (handle->info.extInputHasGenerator) {
						return (spiConfigReceive(&state->usbState, DVX_EXTINPUT, paramAddr, param));
					}
					else {
						return (false);
					}
					break;

				case DVX_EXTINPUT_GENERATE_PULSE_INTERVAL:
				case DVX_EXTINPUT_GENERATE_PULSE_LENGTH: {
					if (handle->info.extInputHasGenerator) {
						// Times are in µs on host, but in cycles @ LOGIC_CLOCK_FREQ
						// on FPGA, so we must divide here.
						uint32_t cyclesValue = 0;
						if (!spiConfigReceive(&state->usbState, DVX_EXTINPUT, paramAddr, &cyclesValue)) {
							return (false);
						}

						float delayCC = roundf((float) cyclesValue / state->deviceClocks.logicClockActual);
						*param        = U32T(delayCC);

						return (true);
					}
					else {
						return (false);
					}
					break;
				}

				default:
					return (false);
					break;
			}
			break;

		case DVX_BIAS:
			break;

		case DVX_SYSINFO:
			// No SystemInfo parameters can ever be get! Use the info struct!
			return (false);
			break;

		case DVX_USB:
			switch (paramAddr) {
				case DVX_USB_RUN:
					return (spiConfigReceive(&state->usbState, DVX_USB, paramAddr, param));
					break;

				case DVX_USB_EARLY_PACKET_DELAY: {
					// Early packet delay is 125µs slices on host, but in cycles
					// @ USB_CLOCK_FREQ on FPGA, so we must divide here.
					uint32_t cyclesValue = 0;
					if (!spiConfigReceive(&state->usbState, DVX_USB, paramAddr, &cyclesValue)) {
						return (false);
					}

					float delayCC = roundf((float) cyclesValue / (125.0F * state->deviceClocks.usbClockActual));
					*param        = U32T(delayCC);

					return (true);
					break;
				}

				default:
					return (false);
					break;
			}
			break;

		default:
			return (false);
			break;
	}

	return (true);
}

bool dvExplorerDataStart(caerDeviceHandle cdh, void (*dataNotifyIncrease)(void *ptr),
	void (*dataNotifyDecrease)(void *ptr), void *dataNotifyUserPtr, void (*dataShutdownNotify)(void *ptr),
	void *dataShutdownUserPtr) {
	dvExplorerHandle handle = (dvExplorerHandle) cdh;
	dvExplorerState state   = &handle->state;

	usbSetShutdownCallback(&state->usbState, dataShutdownNotify, dataShutdownUserPtr);

	// Store new data available/not available anymore call-backs.
	dataExchangeSetNotify(&state->dataExchange, dataNotifyIncrease, dataNotifyDecrease, dataNotifyUserPtr);

	containerGenerationCommitTimestampReset(&state->container);

	if (!dataExchangeBufferInit(&state->dataExchange)) {
		dvExplorerLog(CAER_LOG_CRITICAL, handle, "Failed to initialize data exchange buffer.");
		return (false);
	}

	// Allocate packets.
	if (!containerGenerationAllocate(&state->container, DV_EXPLORER_EVENT_TYPES)) {
		freeAllDataMemory(state);

		dvExplorerLog(CAER_LOG_CRITICAL, handle, "Failed to allocate event packet container.");
		return (false);
	}

	state->currentPackets.polarity
		= caerPolarityEventPacketAllocate(DV_EXPLORER_POLARITY_DEFAULT_SIZE, I16T(handle->info.deviceID), 0);
	if (state->currentPackets.polarity == NULL) {
		freeAllDataMemory(state);

		dvExplorerLog(CAER_LOG_CRITICAL, handle, "Failed to allocate polarity event packet.");
		return (false);
	}

	state->currentPackets.special
		= caerSpecialEventPacketAllocate(DV_EXPLORER_SPECIAL_DEFAULT_SIZE, I16T(handle->info.deviceID), 0);
	if (state->currentPackets.special == NULL) {
		freeAllDataMemory(state);

		dvExplorerLog(CAER_LOG_CRITICAL, handle, "Failed to allocate special event packet.");
		return (false);
	}

	state->currentPackets.imu6
		= caerIMU6EventPacketAllocate(DV_EXPLORER_IMU_DEFAULT_SIZE, I16T(handle->info.deviceID), 0);
	if (state->currentPackets.imu6 == NULL) {
		freeAllDataMemory(state);

		dvExplorerLog(CAER_LOG_CRITICAL, handle, "Failed to allocate IMU6 event packet.");
		return (false);
	}

	// Ignore multi-part events (IMU) at startup, so that any initial
	// incomplete event is ignored. The START events reset this as soon as
	// the first one is observed.
	state->imu.ignoreEvents = true;

	// Ensure no data is left over from previous runs, if the camera
	// wasn't shut-down properly. First ensure it is shut down completely.
	dvExplorerConfigSet(cdh, DVX_DVS, DVX_DVS_RUN, false);
	dvExplorerConfigSet(cdh, DVX_IMU, DVX_IMU_RUN_ACCELEROMETER, false);
	dvExplorerConfigSet(cdh, DVX_IMU, DVX_IMU_RUN_GYROSCOPE, false);
	dvExplorerConfigSet(cdh, DVX_IMU, DVX_IMU_RUN_TEMPERATURE, false);
	dvExplorerConfigSet(cdh, DVX_EXTINPUT, DVX_EXTINPUT_RUN_DETECTOR, false);

	dvExplorerConfigSet(cdh, DVX_MUX, DVX_MUX_RUN, false);
	dvExplorerConfigSet(cdh, DVX_MUX, DVX_MUX_TIMESTAMP_RUN, false);
	dvExplorerConfigSet(cdh, DVX_USB, DVX_USB_RUN, false);

	dvExplorerConfigSet(cdh, DVX_MUX, DVX_MUX_RUN_CHIP, false);

	// Then wait 10ms for FPGA device side buffers to clear.
	struct timespec clearSleep = {.tv_sec = 0, .tv_nsec = 10000000};
	thrd_sleep(&clearSleep, NULL);

	// And reset the USB side of things.
	usbControlResetDataEndpoint(&state->usbState, USB_DEFAULT_DATA_ENDPOINT);

	if (!usbDataTransfersStart(&state->usbState)) {
		freeAllDataMemory(state);

		dvExplorerLog(CAER_LOG_CRITICAL, handle, "Failed to start data transfers.");
		return (false);
	}

	if (dataExchangeStartProducers(&state->dataExchange)) {
		// Enable data transfer on USB end-point 2.
		dvExplorerConfigSet(cdh, DVX_MUX, DVX_MUX_RUN_CHIP, true);

		// Wait 200 ms for biases to stabilize.
		struct timespec biasEnSleep = {.tv_sec = 0, .tv_nsec = 200000000};
		thrd_sleep(&biasEnSleep, NULL);

		dvExplorerConfigSet(cdh, DVX_USB, DVX_USB_RUN, true);
		dvExplorerConfigSet(cdh, DVX_MUX, DVX_MUX_TIMESTAMP_RUN, true);
		dvExplorerConfigSet(cdh, DVX_MUX, DVX_MUX_RUN, true);

		// Wait 50 ms for data transfer to be ready.
		struct timespec noDataSleep = {.tv_sec = 0, .tv_nsec = 50000000};
		thrd_sleep(&noDataSleep, NULL);

		dvExplorerConfigSet(cdh, DVX_DVS, DVX_DVS_RUN, true);
		dvExplorerConfigSet(cdh, DVX_IMU, DVX_IMU_RUN_ACCELEROMETER, true);
		dvExplorerConfigSet(cdh, DVX_IMU, DVX_IMU_RUN_GYROSCOPE, true);
		dvExplorerConfigSet(cdh, DVX_IMU, DVX_IMU_RUN_TEMPERATURE, true);
		dvExplorerConfigSet(cdh, DVX_EXTINPUT, DVX_EXTINPUT_RUN_DETECTOR, true);
	}

	return (true);
}

bool dvExplorerDataStop(caerDeviceHandle cdh) {
	dvExplorerHandle handle = (dvExplorerHandle) cdh;
	dvExplorerState state   = &handle->state;

	if (dataExchangeStopProducers(&state->dataExchange)) {
		// Disable data transfer on USB end-point 2. Reverse order of enabling.
		dvExplorerConfigSet(cdh, DVX_DVS, DVX_DVS_RUN, false);
		dvExplorerConfigSet(cdh, DVX_IMU, DVX_IMU_RUN_ACCELEROMETER, false);
		dvExplorerConfigSet(cdh, DVX_IMU, DVX_IMU_RUN_GYROSCOPE, false);
		dvExplorerConfigSet(cdh, DVX_IMU, DVX_IMU_RUN_TEMPERATURE, false);
		dvExplorerConfigSet(cdh, DVX_EXTINPUT, DVX_EXTINPUT_RUN_DETECTOR, false);

		dvExplorerConfigSet(cdh, DVX_MUX, DVX_MUX_RUN, false);
		dvExplorerConfigSet(cdh, DVX_MUX, DVX_MUX_TIMESTAMP_RUN, false);
		dvExplorerConfigSet(cdh, DVX_USB, DVX_USB_RUN, false);

		dvExplorerConfigSet(cdh, DVX_MUX, DVX_MUX_RUN_CHIP, false);
	}

	usbDataTransfersStop(&state->usbState);

	dataExchangeBufferEmpty(&state->dataExchange);

	// Free current, uncommitted packets and ringbuffer.
	freeAllDataMemory(state);

	// Reset packet positions.
	state->currentPackets.polarityPosition = 0;
	state->currentPackets.specialPosition  = 0;
	state->currentPackets.imu6Position     = 0;

	// Reset private composite events.
	memset(&state->imu.currentEvent, 0, sizeof(struct caer_imu6_event));

	return (true);
}

caerEventPacketContainer dvExplorerDataGet(caerDeviceHandle cdh) {
	dvExplorerHandle handle = (dvExplorerHandle) cdh;
	dvExplorerState state   = &handle->state;

	return (dataExchangeGet(&state->dataExchange, &state->usbState.dataTransfersRun));
}

#define TS_WRAP_ADD 0x8000

static inline bool ensureSpaceForEvents(
	caerEventPacketHeader *packet, size_t position, size_t numEvents, dvExplorerHandle handle) {
	if ((position + numEvents) <= (size_t) caerEventPacketHeaderGetEventCapacity(*packet)) {
		return (true);
	}

	caerEventPacketHeader grownPacket
		= caerEventPacketGrow(*packet, caerEventPacketHeaderGetEventCapacity(*packet) * 2);
	if (grownPacket == NULL) {
		dvExplorerLog(CAER_LOG_CRITICAL, handle, "Failed to grow event packet of type %d.",
			caerEventPacketHeaderGetEventType(*packet));
		return (false);
	}

	*packet = grownPacket;
	return (true);
}

static void dvExplorerEventTranslator(void *vhd, const uint8_t *buffer, size_t bufferSize) {
	dvExplorerHandle handle = vhd;
	dvExplorerState state   = &handle->state;

	// Return right away if not running anymore. This prevents useless work if many
	// buffers are still waiting when shut down, as well as incorrect event sequences
	// if a TS_RESET is stuck on ring-buffer commit further down, and detects shut-down;
	// then any subsequent buffers should also detect shut-down and not be handled.
	if (!usbDataTransfersAreRunning(&state->usbState)) {
		return;
	}

	// Truncate off any extra partial event.
	if ((bufferSize & 0x01) != 0) {
		dvExplorerLog(
			CAER_LOG_ALERT, handle, "%zu bytes received via USB, which is not a multiple of two.", bufferSize);
		bufferSize &= ~((size_t) 0x01);
	}

	for (size_t bufferPos = 0; bufferPos < bufferSize; bufferPos += 2) {
		// Allocate new packets for next iteration as needed.
		if (!containerGenerationAllocate(&state->container, DV_EXPLORER_EVENT_TYPES)) {
			dvExplorerLog(CAER_LOG_CRITICAL, handle, "Failed to allocate event packet container.");
			return;
		}

		if (state->currentPackets.special == NULL) {
			state->currentPackets.special = caerSpecialEventPacketAllocate(
				DV_EXPLORER_SPECIAL_DEFAULT_SIZE, I16T(handle->info.deviceID), state->timestamps.wrapOverflow);
			if (state->currentPackets.special == NULL) {
				dvExplorerLog(CAER_LOG_CRITICAL, handle, "Failed to allocate special event packet.");
				return;
			}
		}

		if (state->currentPackets.polarity == NULL) {
			state->currentPackets.polarity = caerPolarityEventPacketAllocate(
				DV_EXPLORER_POLARITY_DEFAULT_SIZE, I16T(handle->info.deviceID), state->timestamps.wrapOverflow);
			if (state->currentPackets.polarity == NULL) {
				dvExplorerLog(CAER_LOG_CRITICAL, handle, "Failed to allocate polarity event packet.");
				return;
			}
		}

		if (state->currentPackets.imu6 == NULL) {
			state->currentPackets.imu6 = caerIMU6EventPacketAllocate(
				DV_EXPLORER_IMU_DEFAULT_SIZE, I16T(handle->info.deviceID), state->timestamps.wrapOverflow);
			if (state->currentPackets.imu6 == NULL) {
				dvExplorerLog(CAER_LOG_CRITICAL, handle, "Failed to allocate IMU6 event packet.");
				return;
			}
		}

		bool tsReset   = false;
		bool tsBigWrap = false;

		uint16_t event = le16toh(*((const uint16_t *) (&buffer[bufferPos])));

		// Check if timestamp.
		if ((event & 0x8000) != 0) {
			handleTimestampUpdateNewLogic(&state->timestamps, event, handle->info.deviceString, &state->deviceLogLevel);

			containerGenerationCommitTimestampInit(&state->container, state->timestamps.current);
		}
		else {
			// Look at the code, to determine event and data type.
			uint8_t code  = U8T((event & 0x7000) >> 12);
			uint16_t data = (event & 0x0FFF);

			switch (code) {
				case 0: // Special event
					switch (data) {
						case 0: // Ignore this, but log it.
							dvExplorerLog(CAER_LOG_ERROR, handle, "Caught special reserved event!");
							break;

						case 1: { // Timetamp reset
							handleTimestampResetNewLogic(
								&state->timestamps, handle->info.deviceString, &state->deviceLogLevel);

							containerGenerationCommitTimestampReset(&state->container);
							containerGenerationCommitTimestampInit(&state->container, state->timestamps.current);

							// Defer timestamp reset event to later, so we commit it
							// alone, in its own packet.
							// Commit packets when doing a reset to clearly separate them.
							tsReset = true;

							// Update Master/Slave status on incoming TS resets.
							// Async call to not deadlock here.
							spiConfigReceiveAsync(&state->usbState, DVX_SYSINFO, DVX_SYSINFO_DEVICE_IS_MASTER,
								&dvExplorerTSMasterStatusUpdater, &handle->info);

							break;
						}

						case 2: { // External input (falling edge)
							dvExplorerLog(CAER_LOG_DEBUG, handle, "External input (falling edge) event received.");

							if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.special,
									(size_t) state->currentPackets.specialPosition, 1, handle)) {
								caerSpecialEvent currentSpecialEvent = caerSpecialEventPacketGetEvent(
									state->currentPackets.special, state->currentPackets.specialPosition);
								caerSpecialEventSetTimestamp(currentSpecialEvent, state->timestamps.current);
								caerSpecialEventSetType(currentSpecialEvent, EXTERNAL_INPUT_FALLING_EDGE);
								caerSpecialEventValidate(currentSpecialEvent, state->currentPackets.special);
								state->currentPackets.specialPosition++;
							}

							break;
						}

						case 3: { // External input (rising edge)
							dvExplorerLog(CAER_LOG_DEBUG, handle, "External input (rising edge) event received.");

							if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.special,
									(size_t) state->currentPackets.specialPosition, 1, handle)) {
								caerSpecialEvent currentSpecialEvent = caerSpecialEventPacketGetEvent(
									state->currentPackets.special, state->currentPackets.specialPosition);
								caerSpecialEventSetTimestamp(currentSpecialEvent, state->timestamps.current);
								caerSpecialEventSetType(currentSpecialEvent, EXTERNAL_INPUT_RISING_EDGE);
								caerSpecialEventValidate(currentSpecialEvent, state->currentPackets.special);
								state->currentPackets.specialPosition++;
							}

							break;
						}

						case 4: { // External input (pulse)
							dvExplorerLog(CAER_LOG_DEBUG, handle, "External input (pulse) event received.");

							if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.special,
									(size_t) state->currentPackets.specialPosition, 1, handle)) {
								caerSpecialEvent currentSpecialEvent = caerSpecialEventPacketGetEvent(
									state->currentPackets.special, state->currentPackets.specialPosition);
								caerSpecialEventSetTimestamp(currentSpecialEvent, state->timestamps.current);
								caerSpecialEventSetType(currentSpecialEvent, EXTERNAL_INPUT_PULSE);
								caerSpecialEventValidate(currentSpecialEvent, state->currentPackets.special);
								state->currentPackets.specialPosition++;
							}

							break;
						}

						case 5: { // IMU Start (6 axes)
							dvExplorerLog(CAER_LOG_DEBUG, handle, "IMU6 Start event received.");

							state->imu.ignoreEvents = false;
							state->imu.count        = 0;
							state->imu.type         = 0;

							memset(&state->imu.currentEvent, 0, sizeof(struct caer_imu6_event));

							break;
						}

						case 7: { // IMU End
							if (state->imu.ignoreEvents) {
								break;
							}
							dvExplorerLog(CAER_LOG_DEBUG, handle, "IMU End event received.");

							if (state->imu.count == IMU_TOTAL_COUNT) {
								// Timestamp at event-stream insertion point.
								caerIMU6EventSetTimestamp(&state->imu.currentEvent, state->timestamps.current);

								caerIMU6EventValidate(&state->imu.currentEvent, state->currentPackets.imu6);

								// IMU6 and APS operate on an internal event and copy that to the actual output
								// packet here, in the END state, for a reason: if a packetContainer, with all its
								// packets, is committed due to hitting any of the triggers that are not TS reset
								// or TS wrap-around related, like number of polarity events, the event in the packet
								// would be left incomplete, and the event in the new packet would be corrupted.
								// We could avoid this like for the TS reset/TS wrap-around case (see forceCommit) by
								// just deleting that event, but these kinds of commits happen much more often and the
								// possible data loss would be too significant. So instead we keep a private event,
								// fill it, and then only copy it into the packet here in the END state, at which point
								// the whole event is ready and cannot be broken/corrupted in any way anymore.
								if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.imu6,
										(size_t) state->currentPackets.imu6Position, 1, handle)) {
									caerIMU6Event imuCurrentEvent = caerIMU6EventPacketGetEvent(
										state->currentPackets.imu6, state->currentPackets.imu6Position);
									memcpy(imuCurrentEvent, &state->imu.currentEvent, sizeof(struct caer_imu6_event));
									state->currentPackets.imu6Position++;
								}
							}
							else {
								dvExplorerLog(CAER_LOG_INFO, handle,
									"IMU End: failed to validate IMU sample count (%" PRIu8 "), discarding samples.",
									state->imu.count);
							}

							break;
						}

						case 16: { // External generator (falling edge)
							dvExplorerLog(CAER_LOG_DEBUG, handle, "External generator (falling edge) event received.");

							if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.special,
									(size_t) state->currentPackets.specialPosition, 1, handle)) {
								caerSpecialEvent currentSpecialEvent = caerSpecialEventPacketGetEvent(
									state->currentPackets.special, state->currentPackets.specialPosition);
								caerSpecialEventSetTimestamp(currentSpecialEvent, state->timestamps.current);
								caerSpecialEventSetType(currentSpecialEvent, EXTERNAL_GENERATOR_FALLING_EDGE);
								caerSpecialEventValidate(currentSpecialEvent, state->currentPackets.special);
								state->currentPackets.specialPosition++;
							}

							break;
						}

						case 17: { // External generator (rising edge)
							dvExplorerLog(CAER_LOG_DEBUG, handle, "External generator (rising edge) event received.");

							if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.special,
									(size_t) state->currentPackets.specialPosition, 1, handle)) {
								caerSpecialEvent currentSpecialEvent = caerSpecialEventPacketGetEvent(
									state->currentPackets.special, state->currentPackets.specialPosition);
								caerSpecialEventSetTimestamp(currentSpecialEvent, state->timestamps.current);
								caerSpecialEventSetType(currentSpecialEvent, EXTERNAL_GENERATOR_RISING_EDGE);
								caerSpecialEventValidate(currentSpecialEvent, state->currentPackets.special);
								state->currentPackets.specialPosition++;
							}

							break;
						}

						default:
							dvExplorerLog(
								CAER_LOG_ERROR, handle, "Caught special event that can't be handled: %d.", data);
							break;
					}
					break;

				case 1: { // Y column address. 10 bits (9 - 0) contain address, bit 11 start of frame marker.
					uint16_t columnAddr = data & 0x03FF;
					bool startOfFrame   = data & 0x0800;

					if (startOfFrame) {
						dvExplorerLog(CAER_LOG_DEBUG, handle, "Start of Frame column marker detected.");
					}

					// Check range conformity.
					if (columnAddr >= state->dvs.sizeY) {
						dvExplorerLog(CAER_LOG_ALERT, handle,
							"DVS: Y address out of range (0-%d): %" PRIu16 ", due to USB communication issue.",
							state->dvs.sizeY - 1, columnAddr);
						break; // Skip invalid Y address (don't update lastY).
					}

					state->dvs.lastY = columnAddr;
					break;
				}

				case 2:
				case 3: { // 8-pixel group event presence and polarity.
						  // 2 is OFF polarity, 3 is ON.
					if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.polarity,
							(size_t) state->currentPackets.polarityPosition, 8, handle)) {
						bool polarity = code & 0x01;

						for (uint16_t i = 0, mask = 0x0080; i < 8; i++, mask >>= 1) {
							// Check if event present first.
							if ((data & mask) == 0) {
								continue;
							}

							// Received event!
							caerPolarityEvent currentPolarityEvent = caerPolarityEventPacketGetEvent(
								state->currentPackets.polarity, state->currentPackets.polarityPosition);

							// Timestamp at event-stream insertion point.
							caerPolarityEventSetTimestamp(currentPolarityEvent, state->timestamps.current);
							caerPolarityEventSetPolarity(currentPolarityEvent, polarity);
							if (state->dvs.invertXY) {
								caerPolarityEventSetY(currentPolarityEvent, state->dvs.lastX + i);
								caerPolarityEventSetX(currentPolarityEvent, state->dvs.lastY);
							}
							else {
								caerPolarityEventSetY(currentPolarityEvent, state->dvs.lastY);
								caerPolarityEventSetX(currentPolarityEvent, state->dvs.lastX + i);
							}
							caerPolarityEventValidate(currentPolarityEvent, state->currentPackets.polarity);
							state->currentPackets.polarityPosition++;
						}
					}

					break;
				}

				case 4: {
					// Handle SGROUP and MGROUP events.
					if ((data & 0x0FC0) == 0) {
						// SGROUP address.
						uint16_t rowAddress = data & 0x003F;
						rowAddress *= 8; // 8 pixels per group.
						state->dvs.lastX = rowAddress;
					}
					else {
						// TODO: support MGROUP encoding.
						dvExplorerLog(CAER_LOG_ALERT, handle, "Got MGROUP event.");
					}
					break;
				}

				case 5: {
					// Misc 8bit data.
					uint8_t misc8Code = U8T((data & 0x0F00) >> 8);
					uint8_t misc8Data = U8T(data & 0x00FF);

					switch (misc8Code) {
						case 0:
							if (state->imu.ignoreEvents) {
								break;
							}
							dvExplorerLog(CAER_LOG_DEBUG, handle, "IMU Data event (%" PRIu8 ") received.", misc8Data);

							// IMU data event.
							switch (state->imu.count) {
								case 0:
								case 2:
								case 4:
								case 6:
								case 8:
								case 10:
								case 12:
									state->imu.tmpData = misc8Data;
									break;

								case 1: {
									int16_t accelX = I16T((state->imu.tmpData << 8) | misc8Data);
									if (state->imu.flipX) {
										accelX = I16T(-accelX);
									}
									caerIMU6EventSetAccelX(&state->imu.currentEvent, accelX / state->imu.accelScale);
									break;
								}

								case 3: {
									int16_t accelY = I16T((state->imu.tmpData << 8) | misc8Data);
									if (state->imu.flipY) {
										accelY = I16T(-accelY);
									}
									caerIMU6EventSetAccelY(&state->imu.currentEvent, accelY / state->imu.accelScale);
									break;
								}

								case 5: {
									int16_t accelZ = I16T((state->imu.tmpData << 8) | misc8Data);
									if (state->imu.flipZ) {
										accelZ = I16T(-accelZ);
									}
									caerIMU6EventSetAccelZ(&state->imu.currentEvent, accelZ / state->imu.accelScale);

									// IMU parser count depends on which data is present.
									if (!(state->imu.type & IMU_TYPE_TEMP)) {
										if (state->imu.type & IMU_TYPE_GYRO) {
											// No temperature, but gyro.
											state->imu.count = U8T(state->imu.count + 2);
										}
										else {
											// No others enabled.
											state->imu.count = U8T(state->imu.count + 8);
										}
									}
									break;
								}

								case 7: {
									// Temperature is signed. Formula for converting to °C:
									// (SIGNED_VAL / 512) + 23
									int16_t temp = I16T((state->imu.tmpData << 8) | misc8Data);
									caerIMU6EventSetTemp(&state->imu.currentEvent, (temp / 512.0F) + 23.0F);

									// IMU parser count depends on which data is present.
									if (!(state->imu.type & IMU_TYPE_GYRO)) {
										// No others enabled.
										state->imu.count = U8T(state->imu.count + 6);
									}
									break;
								}

								case 9: {
									int16_t gyroX = I16T((state->imu.tmpData << 8) | misc8Data);
									if (state->imu.flipX) {
										gyroX = I16T(-gyroX);
									}
									caerIMU6EventSetGyroX(&state->imu.currentEvent, gyroX / state->imu.gyroScale);
									break;
								}

								case 11: {
									int16_t gyroY = I16T((state->imu.tmpData << 8) | misc8Data);
									if (state->imu.flipY) {
										gyroY = I16T(-gyroY);
									}
									caerIMU6EventSetGyroY(&state->imu.currentEvent, gyroY / state->imu.gyroScale);
									break;
								}

								case 13: {
									int16_t gyroZ = I16T((state->imu.tmpData << 8) | misc8Data);
									if (state->imu.flipZ) {
										gyroZ = I16T(-gyroZ);
									}
									caerIMU6EventSetGyroZ(&state->imu.currentEvent, gyroZ / state->imu.gyroScale);
									break;
								}

								default:
									dvExplorerLog(CAER_LOG_ERROR, handle, "Got invalid IMU update sequence.");
									break;
							}

							state->imu.count++;

							break;

						case 3: {
							if (state->imu.ignoreEvents) {
								break;
							}
							dvExplorerLog(
								CAER_LOG_DEBUG, handle, "IMU Scale Config event (%" PRIu16 ") received.", data);

							// Set correct IMU accel and gyro scales, used to interpret subsequent
							// IMU samples from the device.
							state->imu.accelScale = calculateIMUAccelScale(U16T(data >> 2) & 0x03);
							state->imu.gyroScale  = calculateIMUGyroScale(data & 0x03);

							// Set expected type of data to come from IMU (accel, gyro, temp).
							state->imu.type = (data >> 5) & 0x07;

							// IMU parser start count depends on which data is present.
							if (state->imu.type & IMU_TYPE_ACCEL) {
								// Accelerometer.
								state->imu.count = 0;
							}
							else if (state->imu.type & IMU_TYPE_TEMP) {
								// Temperature
								state->imu.count = 6;
							}
							else if (state->imu.type & IMU_TYPE_GYRO) {
								// Gyroscope.
								state->imu.count = 8;
							}
							else {
								// Nothing, should never happen.
								state->imu.count = 14;

								dvExplorerLog(CAER_LOG_ERROR, handle, "IMU Scale Config: no IMU sensors enabled.");
							}

							break;
						}

						default:
							dvExplorerLog(CAER_LOG_ERROR, handle, "Caught Misc8 event that can't be handled.");
							break;
					}

					break;
				}

				case 7: { // Timestamp wrap
					tsBigWrap = handleTimestampWrapNewLogic(
						&state->timestamps, data, TS_WRAP_ADD, handle->info.deviceString, &state->deviceLogLevel);

					if (tsBigWrap) {
						if (ensureSpaceForEvents((caerEventPacketHeader *) &state->currentPackets.special,
								(size_t) state->currentPackets.specialPosition, 1, handle)) {
							caerSpecialEvent currentSpecialEvent = caerSpecialEventPacketGetEvent(
								state->currentPackets.special, state->currentPackets.specialPosition);
							caerSpecialEventSetTimestamp(currentSpecialEvent, INT32_MAX);
							caerSpecialEventSetType(currentSpecialEvent, TIMESTAMP_WRAP);
							caerSpecialEventValidate(currentSpecialEvent, state->currentPackets.special);
							state->currentPackets.specialPosition++;
						}
					}
					else {
						containerGenerationCommitTimestampInit(&state->container, state->timestamps.current);
					}

					break;
				}

				default:
					dvExplorerLog(CAER_LOG_ERROR, handle, "Caught event that can't be handled.");
					break;
			}
		}

		// Thresholds on which to trigger packet container commit.
		// tsReset and tsBigWrap are already defined above.
		// Trigger if any of the global container-wide thresholds are met.
		int32_t currentPacketContainerCommitSize = containerGenerationGetMaxPacketSize(&state->container);
		bool containerSizeCommit                 = (currentPacketContainerCommitSize > 0)
								   && ((state->currentPackets.polarityPosition >= currentPacketContainerCommitSize)
									   || (state->currentPackets.specialPosition >= currentPacketContainerCommitSize)
									   || (state->currentPackets.imu6Position >= currentPacketContainerCommitSize));

		bool containerTimeCommit = containerGenerationIsCommitTimestampElapsed(
			&state->container, state->timestamps.wrapOverflow, state->timestamps.current);

		// Commit packet containers to the ring-buffer, so they can be processed by the
		// main-loop, when any of the required conditions are met.
		if (tsReset || tsBigWrap || containerSizeCommit || containerTimeCommit) {
			// One or more of the commit triggers are hit. Set the packet container up to contain
			// any non-empty packets. Empty packets are not forwarded to save memory.
			bool emptyContainerCommit = true;

			if (state->currentPackets.polarityPosition > 0) {
				containerGenerationSetPacket(
					&state->container, POLARITY_EVENT, (caerEventPacketHeader) state->currentPackets.polarity);

				state->currentPackets.polarity         = NULL;
				state->currentPackets.polarityPosition = 0;
				emptyContainerCommit                   = false;
			}

			if (state->currentPackets.specialPosition > 0) {
				containerGenerationSetPacket(
					&state->container, SPECIAL_EVENT, (caerEventPacketHeader) state->currentPackets.special);

				state->currentPackets.special         = NULL;
				state->currentPackets.specialPosition = 0;
				emptyContainerCommit                  = false;
			}

			if (state->currentPackets.imu6Position > 0) {
				containerGenerationSetPacket(
					&state->container, IMU6_EVENT_PKT_POS, (caerEventPacketHeader) state->currentPackets.imu6);

				state->currentPackets.imu6         = NULL;
				state->currentPackets.imu6Position = 0;
				emptyContainerCommit               = false;
			}

			if (tsReset || tsBigWrap) {
				// Ignore all IMU6 (composite) events, until a new IMU6
				// Start event comes in, for the next packet.
				// This is to correctly support the forced packet commits that a TS reset,
				// or a TS big wrap, impose. Continuing to parse events would result
				// in a corrupted state of the first event in the new packet, as it would
				// be incomplete, incorrect and miss vital initialization data.
				// See IMU6 END states for more details on a related issue.
				state->imu.ignoreEvents = true;
			}

			containerGenerationExecute(&state->container, emptyContainerCommit, tsReset, state->timestamps.wrapOverflow,
				state->timestamps.current, &state->dataExchange, &state->usbState.dataTransfersRun,
				handle->info.deviceID, handle->info.deviceString, &state->deviceLogLevel);
		}
	}
}

static void dvExplorerTSMasterStatusUpdater(void *userDataPtr, int status, uint32_t param) {
	// If any USB error happened, discard.
	if (status != LIBUSB_TRANSFER_COMPLETED) {
		return;
	}

	// Get new Master/Slave information from device.
	struct caer_dvx_info *info = userDataPtr;

	atomic_thread_fence(memory_order_seq_cst);
	info->deviceIsMaster = param;
	atomic_thread_fence(memory_order_seq_cst);
}

//////////////////////////////////
/// FX3 Debug Transfer Support ///
//////////////////////////////////
static void allocateDebugTransfers(dvExplorerHandle handle) {
	// Allocate transfers and set them up.
	for (size_t i = 0; i < DEBUG_TRANSFER_NUM; i++) {
		handle->state.fx3Support.debugTransfers[i] = libusb_alloc_transfer(0);
		if (handle->state.fx3Support.debugTransfers[i] == NULL) {
			dvExplorerLog(CAER_LOG_CRITICAL, handle,
				"Unable to allocate further libusb transfers (debug channel, %zu of %" PRIu32 ").", i,
				DEBUG_TRANSFER_NUM);
			continue;
		}

		// Create data buffer.
		handle->state.fx3Support.debugTransfers[i]->length = DEBUG_TRANSFER_SIZE;
		handle->state.fx3Support.debugTransfers[i]->buffer = malloc(DEBUG_TRANSFER_SIZE);
		if (handle->state.fx3Support.debugTransfers[i]->buffer == NULL) {
			dvExplorerLog(CAER_LOG_CRITICAL, handle,
				"Unable to allocate buffer for libusb transfer %zu (debug channel). Error: %d.", i, errno);

			libusb_free_transfer(handle->state.fx3Support.debugTransfers[i]);
			handle->state.fx3Support.debugTransfers[i] = NULL;

			continue;
		}

		// Initialize Transfer.
		handle->state.fx3Support.debugTransfers[i]->dev_handle = handle->state.usbState.deviceHandle;
		handle->state.fx3Support.debugTransfers[i]->endpoint   = DEBUG_ENDPOINT;
		handle->state.fx3Support.debugTransfers[i]->type       = LIBUSB_TRANSFER_TYPE_INTERRUPT;
		handle->state.fx3Support.debugTransfers[i]->callback   = &libUsbDebugCallback;
		handle->state.fx3Support.debugTransfers[i]->user_data  = handle;
		handle->state.fx3Support.debugTransfers[i]->timeout    = 0;
		handle->state.fx3Support.debugTransfers[i]->flags      = LIBUSB_TRANSFER_FREE_BUFFER;

		if ((errno = libusb_submit_transfer(handle->state.fx3Support.debugTransfers[i])) == LIBUSB_SUCCESS) {
			atomic_fetch_add(&handle->state.fx3Support.activeDebugTransfers, 1);
		}
		else {
			dvExplorerLog(CAER_LOG_CRITICAL, handle,
				"Unable to submit libusb transfer %zu (debug channel). Error: %s (%d).", i, libusb_strerror(errno),
				errno);

			// The transfer buffer is freed automatically here thanks to
			// the LIBUSB_TRANSFER_FREE_BUFFER flag set above.
			libusb_free_transfer(handle->state.fx3Support.debugTransfers[i]);
			handle->state.fx3Support.debugTransfers[i] = NULL;
		}
	}

	if (atomic_load(&handle->state.fx3Support.activeDebugTransfers) == 0) {
		// Didn't manage to allocate any USB transfers, log failure.
		dvExplorerLog(CAER_LOG_CRITICAL, handle, "Unable to allocate any libusb transfers (debug channel).");
	}
}

static void cancelAndDeallocateDebugTransfers(dvExplorerHandle handle) {
	// Wait for all transfers to go away.
	struct timespec waitForTerminationSleep = {.tv_sec = 0, .tv_nsec = 1000000};

	while (atomic_load(&handle->state.fx3Support.activeDebugTransfers) > 0) {
		// Continue trying to cancel all transfers until there are none left.
		// It seems like one cancel pass is not enough and some hang around.
		for (size_t i = 0; i < DEBUG_TRANSFER_NUM; i++) {
			if (handle->state.fx3Support.debugTransfers[i] != NULL) {
				errno = libusb_cancel_transfer(handle->state.fx3Support.debugTransfers[i]);
				if ((errno != LIBUSB_SUCCESS) && (errno != LIBUSB_ERROR_NOT_FOUND)) {
					dvExplorerLog(CAER_LOG_CRITICAL, handle,
						"Unable to cancel libusb transfer %zu (debug channel). Error: %s (%d).", i,
						libusb_strerror(errno), errno);
					// Proceed with trying to cancel all transfers regardless of errors.
				}
			}
		}

		// Sleep for 1ms to avoid busy loop.
		thrd_sleep(&waitForTerminationSleep, NULL);
	}

	// No more transfers in flight, deallocate them all here.
	for (size_t i = 0; i < DEBUG_TRANSFER_NUM; i++) {
		if (handle->state.fx3Support.debugTransfers[i] != NULL) {
			libusb_free_transfer(handle->state.fx3Support.debugTransfers[i]);
			handle->state.fx3Support.debugTransfers[i] = NULL;
		}
	}
}

static void LIBUSB_CALL libUsbDebugCallback(struct libusb_transfer *transfer) {
	dvExplorerHandle handle = transfer->user_data;

	// Completed or cancelled transfers are what we expect to handle here, so
	// if they do have data attached, try to parse them.
	if (((transfer->status == LIBUSB_TRANSFER_COMPLETED) || (transfer->status == LIBUSB_TRANSFER_CANCELLED))
		&& (transfer->actual_length > 0)) {
		// Handle debug data.
		debugTranslator(handle, transfer->buffer, (size_t) transfer->actual_length);
	}

	if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
		// Submit transfer again.
		if (libusb_submit_transfer(transfer) == LIBUSB_SUCCESS) {
			return;
		}
	}

	// Cannot recover (cancelled, no device, or other critical error).
	// Signal this by adjusting the counter and exiting.
	// Freeing the transfers is taken care of by cancelAndDeallocateDebugTransfers().
	atomic_fetch_sub(&handle->state.fx3Support.activeDebugTransfers, 1);
}

static void debugTranslator(dvExplorerHandle handle, const uint8_t *buffer, size_t bytesSent) {
	// Check if this is a debug message (length 7-64 bytes).
	if ((bytesSent >= 7) && (buffer[0] == 0x00)) {
		// Debug message, log this.
		dvExplorerLog(CAER_LOG_ERROR, handle, "Error message: '%s' (code %u at time %u).", &buffer[6], buffer[1],
			*((const uint32_t *) &buffer[2]));
	}
	else {
		// Unknown/invalid debug message, log this.
		dvExplorerLog(CAER_LOG_WARNING, handle, "Unknown/invalid debug message.");
	}
}
