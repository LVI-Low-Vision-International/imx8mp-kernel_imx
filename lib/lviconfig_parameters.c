#include <linux/module.h>
#include <linux/lviconfig_parameters.h>
#include <linux/notifier.h>

//copy string to variable
#define STRDUP(s) kstrdup(s, GFP_KERNEL)

static int lviconfig_initialized = 0;

#define LVICONFIG_MONITOR_DEFAULT_LEVEL 128

//kernel version of atoi
static inline int katoi(const char *s)
{
	long res;
	if (kstrtol(s, 10, &res) == 0)
		return (int)res;
	return 0;
}

// Global address variable to keep track of the next available address
static uint32_t addr = 0;

/**
 * @brief Global configuration parameters.
 * These parameters are initialized with default values and can be accessed
 * throughout the kernel module.
 * They are used to store configuration settings for various components
 * of the system, such as camera settings, video settings, and more.
 * Each parameter is defined as a ConfigParam structure, which includes
 * the name, address, type, data pointer, and size of the parameter.
 */
ConfigParam confLicenseKey[CONFIG_LICENSE_KEY_AMT];
ConfigParam confFactoryDefaultVersion;
ConfigParam confProductId;
ConfigFunctions confFunctions;
ConfigColor confColor[CONFIG_COLOR_AMT];
ConfigCamera confCamera;
ConfigCameraMode confCameraMode[CONFIG_CAMERA_MODE_AMT];
ConfigMonitorMode confMonitorMode[CONFIG_MONITOR_MODE_AMT];
ConfigVideo confVideo[CONFIG_VIDEO_AMT];
ConfigGraphics confGraphics;
ConfigPTZ confPTZ;
ConfigBattery confBattery;
ConfigLighting confLighting;

/** Export symbols for use in other modules.
 * These symbols allow other kernel modules to access the configuration
 * parameters defined in this module.
 */
EXPORT_SYMBOL(confLicenseKey);
EXPORT_SYMBOL(confFactoryDefaultVersion);
EXPORT_SYMBOL(confProductId);
EXPORT_SYMBOL(confFunctions);
EXPORT_SYMBOL(confColor);
EXPORT_SYMBOL(confCamera);
EXPORT_SYMBOL(confCameraMode);
EXPORT_SYMBOL(confMonitorMode);
EXPORT_SYMBOL(confVideo);
EXPORT_SYMBOL(confGraphics);
EXPORT_SYMBOL(confPTZ);
EXPORT_SYMBOL(confBattery);
EXPORT_SYMBOL(confLighting);

/* Flat list of all ConfigParam pointers, built by build_param_list() at end of ConfigParam_InitAll.
 * Used by ConfigParam_ParseEEPROM and ConfigParam_ReadAndSaveAll to avoid enumerating params in 3 places.
 */
#define ALL_PARAMS_MAX 350
static ConfigParam *all_params[ALL_PARAMS_MAX];
static size_t num_params;

/* Blocking notifier list — subscribers are called synchronously (process context)
 * whenever a ConfigParam's in-memory value changes.
 */
static BLOCKING_NOTIFIER_HEAD(lviconfig_notifier_list);

int lviconfig_register_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&lviconfig_notifier_list, nb);
}
EXPORT_SYMBOL(lviconfig_register_notifier);

int lviconfig_unregister_notifier(struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&lviconfig_notifier_list, nb);
}
EXPORT_SYMBOL(lviconfig_unregister_notifier);

void lviconfig_notify_param_changed(ConfigParam *param)
{
	blocking_notifier_call_chain(&lviconfig_notifier_list, 0, param);
}
EXPORT_SYMBOL(lviconfig_notify_param_changed);

void lviconfig_notify_all_params_changed(void)
{
	size_t i;

	for (i = 0; i < num_params; i++)
		blocking_notifier_call_chain(&lviconfig_notifier_list, 0, all_params[i]);
}
EXPORT_SYMBOL(lviconfig_notify_all_params_changed);

/**
 * @brief Serialize src into dst according to type (little-endian).
 * @return 0 on success, -1 for unknown type.
 */
static int serialize_data(uint8_t *dst, const void *src, DataType type, uint8_t size)
{
	switch (type) {
	case TYPE_8:
		memcpy(dst, src, size);
		break;
	case TYPE_16:
		dst[0] = ((const uint16_t *)src)[0] & 0xFF;
		dst[1] = (((const uint16_t *)src)[0] >> 8) & 0xFF;
		break;
	case TYPE_32:
		dst[0] = ((const uint32_t *)src)[0] & 0xFF;
		dst[1] = (((const uint32_t *)src)[0] >> 8) & 0xFF;
		dst[2] = (((const uint32_t *)src)[0] >> 16) & 0xFF;
		dst[3] = (((const uint32_t *)src)[0] >> 24) & 0xFF;
		break;
	case TYPE_ARRAY:
		memcpy(dst, src, size);
		break;
	case TYPE_STRING:
		strncpy((char *)dst, (const char *)src, size - 1);
		dst[size - 1] = '\0';
		break;
	default:
		return -1;
	}
	return 0;
}

static void lviconfig_set_u8_param(ConfigParam *param, uint8_t value)
{
	if (!param || !param->data || param->size < sizeof(value))
		return;

	param->data[0] = value;
}

static void lviconfig_set_monitor_mode_defaults(ConfigMonitorMode *mode)
{
	if (!mode)
		return;

	lviconfig_set_u8_param(&mode->ColorGainR, LVICONFIG_MONITOR_DEFAULT_LEVEL);
	lviconfig_set_u8_param(&mode->ColorGainG, LVICONFIG_MONITOR_DEFAULT_LEVEL);
	lviconfig_set_u8_param(&mode->ColorGainB, LVICONFIG_MONITOR_DEFAULT_LEVEL);
	lviconfig_set_u8_param(&mode->Contrast, LVICONFIG_MONITOR_DEFAULT_LEVEL);
	lviconfig_set_u8_param(&mode->Brightness, LVICONFIG_MONITOR_DEFAULT_LEVEL);
	lviconfig_set_u8_param(&mode->Saturation, LVICONFIG_MONITOR_DEFAULT_LEVEL);
}

static bool lviconfig_monitor_mode_is_blank(const ConfigMonitorMode *mode)
{
	if (!mode)
		return false;

	if (!mode->ColorGainR.data || !mode->ColorGainG.data || !mode->ColorGainB.data || !mode->Contrast.data || !mode->Brightness.data || !mode->Saturation.data)
		return false;

	return mode->ColorGainR.data[0] == 0xFF && mode->ColorGainG.data[0] == 0xFF && mode->ColorGainB.data[0] == 0xFF && mode->Contrast.data[0] == 0xFF &&
	       mode->Brightness.data[0] == 0xFF && mode->Saturation.data[0] == 0xFF;
}

static void lviconfig_restore_blank_monitor_defaults(void)
{
	uint8_t i;

	for (i = 0; i < CONFIG_MONITOR_MODE_AMT; i++) {
		if (!lviconfig_monitor_mode_is_blank(&confMonitorMode[i]))
			continue;

		lviconfig_set_monitor_mode_defaults(&confMonitorMode[i]);
		pr_info("[%s] confMonitorMode[%u] is blank, using neutral display defaults\n", __func__, i);
	}
}

/**
 * @brief Initialize a configuration parameter.
 * This function initializes a ConfigParam structure with the given name,
 * data, and type. It allocates memory for the data and sets the size
 * based on the type.
 *
 * @param name_str The name of the configuration parameter.
 * @param data Pointer to the initial data for the parameter.
 * @param type The data type of the parameter (8-bit, 16-bit, 32-bit, array, string).
 * @return A ConfigParam structure initialized with the given values.
 */
ConfigParam ConfigParam_Init(const char *name_str, const void *data, DataType type)
{
	ConfigParam param;
	param.name = STRDUP(name_str);
	if (!param.name) {
		param.size = 0;
		param.address = 0;
		param.data = NULL;
		return param;
	}
	param.address = addr;
	param.type = type;

	switch (type) {
	case TYPE_8:
		param.size = 1;
		break;
	case TYPE_16:
		param.size = 2;
		break;
	case TYPE_32:
		param.size = 4;
		break;
	case TYPE_ARRAY:
		param.size = TYPE_ARRAY_SIZE_MAX;
		break;
	case TYPE_STRING:
		if (data) {
			param.size = strlen((const char *)data) + 1;
		} else {
			param.size = 1;
		}
		break;
	default:
		kfree((void *)param.name);
		param.name = NULL;
		param.size = 0;
		param.address = 0;
		param.data = NULL;
		return param;
	}

	param.data = (uint8_t *)kmalloc(param.size, GFP_KERNEL);
	if (param.data == NULL) {
		kfree((void *)param.name);
		param.name = NULL;
		param.size = 0;
		param.address = 0;
		return param;
	}

	if (data)
		serialize_data(param.data, data, type, param.size);
	else
		memset(param.data, 0, param.size);

	addr += param.size;
	return param;
}

/**
 * @brief Build the flat all_params[] list from all initialized ConfigParam fields.
 * Called at the end of ConfigParam_InitAll. After this, ParseEEPROM and
 * ReadAndSaveAll can iterate over all_params[] instead of enumerating fields manually.
 */
static void build_param_list(void)
{
	uint8_t i;

	num_params = 0;

	all_params[num_params++] = &confProductId;
	all_params[num_params++] = &confFactoryDefaultVersion;
	for (i = 0; i < CONFIG_LICENSE_KEY_AMT; i++)
		all_params[num_params++] = &confLicenseKey[i];
	all_params[num_params++] = &confFunctions.Seesaw;
	all_params[num_params++] = &confFunctions.ReferenceLine;
	all_params[num_params++] = &confFunctions.Curtain;
	for (i = 0; i < CONFIG_COLOR_AMT; i++) {
		all_params[num_params++] = &confColor[i].Group;
		all_params[num_params++] = &confColor[i].ArtificialColorPoint1;
		all_params[num_params++] = &confColor[i].ArtificialColorPoint2;
		all_params[num_params++] = &confColor[i].ArtificialColorPoint3;
		all_params[num_params++] = &confColor[i].ArtificialColorPoint4;
		all_params[num_params++] = &confColor[i].CameraBackground;
		all_params[num_params++] = &confColor[i].ReferenceLine;
		all_params[num_params++] = &confColor[i].Restricted;
	}
	all_params[num_params++] = &confCamera.Type;
	all_params[num_params++] = &confCamera.RGain;
	all_params[num_params++] = &confCamera.BGain;
	all_params[num_params++] = &confCamera.FirstLine;
	all_params[num_params++] = &confCamera.FirstPixel;
	all_params[num_params++] = &confCamera.UsableLines;
	all_params[num_params++] = &confCamera.UsablePixels;
	all_params[num_params++] = &confCamera.UsableRatio;
	all_params[num_params++] = &confCamera.Interface;
	all_params[num_params++] = &confCamera.NaturalColorShutterTime;
	all_params[num_params++] = &confCamera.NaturalColorBrightness;
	all_params[num_params++] = &confCamera.NaturalColorIris;
	all_params[num_params++] = &confCamera.NaturalColorExposureCompensation;
	all_params[num_params++] = &confCamera.NaturalColorGainPeak;
	all_params[num_params++] = &confCamera.ArtificialColorShutterTime;
	all_params[num_params++] = &confCamera.ArtificialColorBrightness;
	all_params[num_params++] = &confCamera.ArtificialColorIris;
	all_params[num_params++] = &confCamera.ArtificialColorExposureCompensation;
	all_params[num_params++] = &confCamera.ArtificialColorGainPeak;
	for (i = 0; i < CONFIG_CAMERA_MODE_AMT; i++) {
		all_params[num_params++] = &confCameraMode[i].Zoom;
		all_params[num_params++] = &confCameraMode[i].ZoomMin;
		all_params[num_params++] = &confCameraMode[i].ZoomMax;
		all_params[num_params++] = &confCameraMode[i].ZoomSpeed;
		all_params[num_params++] = &confCameraMode[i].Focus;
		all_params[num_params++] = &confCameraMode[i].FocusMin;
		all_params[num_params++] = &confCameraMode[i].FocusMax;
		all_params[num_params++] = &confCameraMode[i].FocusSpeed;
		all_params[num_params++] = &confCameraMode[i].NaturalColorExposure;
		all_params[num_params++] = &confCameraMode[i].ArtificialColorExposure;
		all_params[num_params++] = &confCameraMode[i].WhiteBalance;
	}
	for (i = 0; i < CONFIG_MONITOR_MODE_AMT; i++) {
		all_params[num_params++] = &confMonitorMode[i].ColorGainR;
		all_params[num_params++] = &confMonitorMode[i].ColorGainG;
		all_params[num_params++] = &confMonitorMode[i].ColorGainB;
		all_params[num_params++] = &confMonitorMode[i].Contrast;
		all_params[num_params++] = &confMonitorMode[i].Brightness;
		all_params[num_params++] = &confMonitorMode[i].Saturation;
		all_params[num_params++] = &confMonitorMode[i].AutoPosNegEnable;
		all_params[num_params++] = &confMonitorMode[i].AutoPosNegMin;
		all_params[num_params++] = &confMonitorMode[i].AutoPosNegMax;
		all_params[num_params++] = &confMonitorMode[i].NaturalColorContrast;
		all_params[num_params++] = &confMonitorMode[i].NaturalColorBrightnessDefault;
		all_params[num_params++] = &confMonitorMode[i].NaturalColorBrightnessCoefficient;
		all_params[num_params++] = &confMonitorMode[i].NaturalColorDefault;
		all_params[num_params++] = &confMonitorMode[i].NaturalColorCoefficient;
		all_params[num_params++] = &confMonitorMode[i].ArtificalColorContrast;
		all_params[num_params++] = &confMonitorMode[i].ArtificalColorBrightness;
		all_params[num_params++] = &confMonitorMode[i].BlackLinesTop;
		all_params[num_params++] = &confMonitorMode[i].BlackLinesBottom;
		all_params[num_params++] = &confMonitorMode[i].GammaCorrection1;
		all_params[num_params++] = &confMonitorMode[i].GammaCorrection2;
		all_params[num_params++] = &confMonitorMode[i].GammaCorrection3;
		all_params[num_params++] = &confMonitorMode[i].GammaCorrection4;
		all_params[num_params++] = &confMonitorMode[i].GammaCorrection5;
	}
	for (i = 0; i < CONFIG_VIDEO_AMT; i++) {
		all_params[num_params++] = &confVideo[i].PaletteSelection;
		all_params[num_params++] = &confVideo[i].Type;
		all_params[num_params++] = &confVideo[i].LinesPerFrame;
		all_params[num_params++] = &confVideo[i].LinesActiveInCompletePicture;
		all_params[num_params++] = &confVideo[i].FirstActiveLine;
		all_params[num_params++] = &confVideo[i].HorizontalTotalLength;
		all_params[num_params++] = &confVideo[i].HorizontalSyncLength;
		all_params[num_params++] = &confVideo[i].HorizontalActiveVideoStart;
		all_params[num_params++] = &confVideo[i].HorizontalActiveVideoLength;
		all_params[num_params++] = &confVideo[i].VerticalSyncLength;
		all_params[num_params++] = &confVideo[i].VoltageDac;
		all_params[num_params++] = &confVideo[i].SyncRgb;
		all_params[num_params++] = &confVideo[i].AspectRatio;
	}
	all_params[num_params++] = &confGraphics.ReferenceLineWidth;
	all_params[num_params++] = &confGraphics.ReferenceLineLowSpeed;
	all_params[num_params++] = &confGraphics.ReferenceLineHighSpeed;
	all_params[num_params++] = &confGraphics.ReferenceLineHighSpeedDelay;
	all_params[num_params++] = &confGraphics.ReferenceLineSwitchDelay;
	all_params[num_params++] = &confGraphics.PosNegSpeed;
	all_params[num_params++] = &confPTZ.LimitUp;
	all_params[num_params++] = &confPTZ.LimitDown;
	all_params[num_params++] = &confPTZ.LimitLeft;
	all_params[num_params++] = &confPTZ.LimitRight;
	all_params[num_params++] = &confBattery.Type;
	all_params[num_params++] = &confBattery.NominalCapacity;
	all_params[num_params++] = &confBattery.TempCoefficientCapacity;
	all_params[num_params++] = &confBattery.AgeCoefficientCapacity;
	all_params[num_params++] = &confBattery.LowCapacityLimit;
	all_params[num_params++] = &confLighting.Intensity;
}

/**
 * @brief Initialize all configuration parameters with default values.
 * This function MUST be called on boot BEFORE calling ConfigParam_ParseEEPROM.
 * Initializing these parameter ensures that drivers that include lviconfig_parameters.h
 * can access the parameters.
 */
void ConfigParam_InitAll(void)
{
	char name[64];
	uint32_t default_numeric_data = 0;
	const char *default_string_data = "";
	uint8_t i;

	addr = 0;

	/* INIT PRODUCT ID */
	confProductId = ConfigParam_Init("confProductId", &default_numeric_data, TYPE_32);

	/* INIT FACTORY DEFAULT */
	confFactoryDefaultVersion = ConfigParam_Init("confFactoryDefaultVersion", "D12345B", TYPE_STRING);

	/* INIT LICENSE KEYS */
	for (i = 0; i < CONFIG_LICENSE_KEY_AMT; i++) {
		const char *string_val;
		switch (i) {
		case 0:
			string_val = "999999";
			break;
		case 1:
			string_val = "999999";
			break;
		case 2:
			string_val = "999999";
			break;
		default:
			string_val = default_string_data;
			break;
		}
		snprintf(name, sizeof(name), "confLicenseKey[%u]", i);

		confLicenseKey[i] = ConfigParam_Init(name, string_val, TYPE_STRING);
	}

	/* INIT FUNCTIONS */
	snprintf(name, sizeof(name), "confFunctions");

	confFunctions.name = STRDUP(name);
	confFunctions.Seesaw = ConfigParam_Init("Seesaw", &default_numeric_data, TYPE_8);
	confFunctions.ReferenceLine = ConfigParam_Init("ReferenceLine", &default_numeric_data, TYPE_8);
	confFunctions.Curtain = ConfigParam_Init("Curtain", &default_numeric_data, TYPE_8);

	/* INIT COLOR */
	for (i = 0; i < CONFIG_COLOR_AMT; i++) {
		snprintf(name, sizeof(name), "confColor[%u]", i);

		confColor[i].name = STRDUP(name);
		confColor[i].Group = ConfigParam_Init("Group", &default_numeric_data, TYPE_32);
		confColor[i].ArtificialColorPoint1 = ConfigParam_Init("ArtificialColorPoint1", &default_numeric_data, TYPE_32);
		confColor[i].ArtificialColorPoint2 = ConfigParam_Init("ArtificialColorPoint2", &default_numeric_data, TYPE_32);
		confColor[i].ArtificialColorPoint3 = ConfigParam_Init("ArtificialColorPoint3", &default_numeric_data, TYPE_32);
		confColor[i].ArtificialColorPoint4 = ConfigParam_Init("ArtificialColorPoint4", &default_numeric_data, TYPE_32);
		confColor[i].CameraBackground = ConfigParam_Init("CameraBackground", &default_numeric_data, TYPE_32);
		confColor[i].ReferenceLine = ConfigParam_Init("ReferenceLine", &default_numeric_data, TYPE_32);
		confColor[i].Restricted = ConfigParam_Init("Restricted", &default_numeric_data, TYPE_32);
	}

	/* INIT CAMERA */
	snprintf(name, sizeof(name), "confCamera");

	confCamera.name = STRDUP(name);
	confCamera.Type = ConfigParam_Init("Type", &default_numeric_data, TYPE_8);
	confCamera.RGain = ConfigParam_Init("RGain", &default_numeric_data, TYPE_8);
	confCamera.BGain = ConfigParam_Init("BGain", &default_numeric_data, TYPE_8);
	confCamera.FirstLine = ConfigParam_Init("FirstLine", &default_numeric_data, TYPE_16);
	confCamera.FirstPixel = ConfigParam_Init("FirstPixel", &default_numeric_data, TYPE_16);
	confCamera.UsableLines = ConfigParam_Init("UsableLines", &default_numeric_data, TYPE_16);
	confCamera.UsablePixels = ConfigParam_Init("UsablePixels", &default_numeric_data, TYPE_16);
	confCamera.UsableRatio = ConfigParam_Init("UsableRatio", &default_numeric_data, TYPE_16);
	confCamera.Interface = ConfigParam_Init("Interface", &default_numeric_data, TYPE_8);
	confCamera.NaturalColorShutterTime = ConfigParam_Init("NaturalColorShutterTime", &default_numeric_data, TYPE_8);
	confCamera.NaturalColorBrightness = ConfigParam_Init("NaturalColorBrightness", &default_numeric_data, TYPE_8);
	confCamera.NaturalColorIris = ConfigParam_Init("NaturalColorIris", &default_numeric_data, TYPE_8);
	confCamera.NaturalColorExposureCompensation = ConfigParam_Init("NaturalColorExposureCompensation", &default_numeric_data, TYPE_8);
	confCamera.NaturalColorGainPeak = ConfigParam_Init("NaturalColorGainPeak", &default_numeric_data, TYPE_8);
	confCamera.ArtificialColorShutterTime = ConfigParam_Init("ArtificialColorShutterTime", &default_numeric_data, TYPE_8);
	confCamera.ArtificialColorBrightness = ConfigParam_Init("ArtificialColorBrightness", &default_numeric_data, TYPE_8);
	confCamera.ArtificialColorIris = ConfigParam_Init("ArtificialColorIris", &default_numeric_data, TYPE_8);
	confCamera.ArtificialColorExposureCompensation = ConfigParam_Init("ArtificialColorExposureCompensation", &default_numeric_data, TYPE_8);
	confCamera.ArtificialColorGainPeak = ConfigParam_Init("ArtificialColorGainPeak", &default_numeric_data, TYPE_8);

	/* INIT CAMERA MODE */
	for (i = 0; i < CONFIG_CAMERA_MODE_AMT; i++) {
		snprintf(name, sizeof(name), "confCameraMode[%u]", i);

		confCameraMode[i].name = STRDUP(name);
		confCameraMode[i].Zoom = ConfigParam_Init("Zoom", &default_numeric_data, TYPE_16);
		confCameraMode[i].ZoomMin = ConfigParam_Init("ZoomMin", &default_numeric_data, TYPE_16);
		confCameraMode[i].ZoomMax = ConfigParam_Init("ZoomMax", &default_numeric_data, TYPE_16);
		confCameraMode[i].ZoomSpeed = ConfigParam_Init("ZoomSpeed", &default_numeric_data, TYPE_16);
		confCameraMode[i].Focus = ConfigParam_Init("Focus", &default_numeric_data, TYPE_32);
		confCameraMode[i].FocusMin = ConfigParam_Init("FocusMin", &default_numeric_data, TYPE_32);
		confCameraMode[i].FocusMax = ConfigParam_Init("FocusMax", &default_numeric_data, TYPE_32);
		confCameraMode[i].FocusSpeed = ConfigParam_Init("FocusSpeed", &default_numeric_data, TYPE_32);
		confCameraMode[i].NaturalColorExposure = ConfigParam_Init("NaturalColorExposure", &default_numeric_data, TYPE_32);
		confCameraMode[i].ArtificialColorExposure = ConfigParam_Init("ArtificialColorExposure", &default_numeric_data, TYPE_32);
		confCameraMode[i].WhiteBalance = ConfigParam_Init("WhiteBalance", &default_numeric_data, TYPE_32);
	}

	/* INIT MONITOR MODE */
	for (i = 0; i < CONFIG_MONITOR_MODE_AMT; i++) {
		snprintf(name, sizeof(name), "confMonitorMode[%u]", i);

		confMonitorMode[i].name = STRDUP(name);
		confMonitorMode[i].ColorGainR = ConfigParam_Init("ColorGainR", &default_numeric_data, TYPE_8);
		confMonitorMode[i].ColorGainG = ConfigParam_Init("ColorGainG", &default_numeric_data, TYPE_8);
		confMonitorMode[i].ColorGainB = ConfigParam_Init("ColorGainB", &default_numeric_data, TYPE_8);
		confMonitorMode[i].Contrast = ConfigParam_Init("Contrast", &default_numeric_data, TYPE_8);
		confMonitorMode[i].Brightness = ConfigParam_Init("Brightness", &default_numeric_data, TYPE_8);
		confMonitorMode[i].Saturation = ConfigParam_Init("Saturation", &default_numeric_data, TYPE_8);
		confMonitorMode[i].AutoPosNegEnable = ConfigParam_Init("AutoPosNegEnable", &default_numeric_data, TYPE_32);
		confMonitorMode[i].AutoPosNegMin = ConfigParam_Init("AutoPosNegMin", &default_numeric_data, TYPE_32);
		confMonitorMode[i].AutoPosNegMax = ConfigParam_Init("AutoPosNegMax", &default_numeric_data, TYPE_32);
		confMonitorMode[i].NaturalColorContrast = ConfigParam_Init("NaturalColorContrast", &default_numeric_data, TYPE_32);
		confMonitorMode[i].NaturalColorBrightnessDefault = ConfigParam_Init("NaturalColorBrightnessDefault", &default_numeric_data, TYPE_32);
		confMonitorMode[i].NaturalColorBrightnessCoefficient = ConfigParam_Init("NaturalColorBrightnessCoefficient", &default_numeric_data, TYPE_32);
		confMonitorMode[i].NaturalColorDefault = ConfigParam_Init("NaturalColorDefault", &default_numeric_data, TYPE_32);
		confMonitorMode[i].NaturalColorCoefficient = ConfigParam_Init("NaturalColorCoefficient", &default_numeric_data, TYPE_32);
		confMonitorMode[i].ArtificalColorContrast = ConfigParam_Init("ArtificalColorContrast", &default_numeric_data, TYPE_32);
		confMonitorMode[i].ArtificalColorBrightness = ConfigParam_Init("ArtificalColorBrightness", &default_numeric_data, TYPE_32);
		confMonitorMode[i].BlackLinesTop = ConfigParam_Init("BlackLinesTop", &default_numeric_data, TYPE_32);
		confMonitorMode[i].BlackLinesBottom = ConfigParam_Init("BlackLinesBottom", &default_numeric_data, TYPE_32);
		confMonitorMode[i].GammaCorrection1 = ConfigParam_Init("GammaCorrection1", &default_numeric_data, TYPE_32);
		confMonitorMode[i].GammaCorrection2 = ConfigParam_Init("GammaCorrection2", &default_numeric_data, TYPE_32);
		confMonitorMode[i].GammaCorrection3 = ConfigParam_Init("GammaCorrection3", &default_numeric_data, TYPE_32);
		confMonitorMode[i].GammaCorrection4 = ConfigParam_Init("GammaCorrection4", &default_numeric_data, TYPE_32);
		confMonitorMode[i].GammaCorrection5 = ConfigParam_Init("GammaCorrection5", &default_numeric_data, TYPE_32);

		lviconfig_set_monitor_mode_defaults(&confMonitorMode[i]);
	}

	/* INIT VIDEO */
	for (i = 0; i < CONFIG_VIDEO_AMT; i++) {
		snprintf(name, sizeof(name), "confVideo[%u]", i);

		confVideo[i].name = STRDUP(name);
		confVideo[i].PaletteSelection = ConfigParam_Init("PaletteSelection", &default_numeric_data, TYPE_8);
		confVideo[i].Type = ConfigParam_Init("Type", &default_numeric_data, TYPE_8);
		confVideo[i].LinesPerFrame = ConfigParam_Init("LinesPerFrame", &default_numeric_data, TYPE_16);
		confVideo[i].LinesActiveInCompletePicture = ConfigParam_Init("LinesActiveInCompletePicture", &default_numeric_data, TYPE_16);
		confVideo[i].FirstActiveLine = ConfigParam_Init("FirstActiveLine", &default_numeric_data, TYPE_8);
		confVideo[i].HorizontalTotalLength = ConfigParam_Init("HorizontalTotalLength", &default_numeric_data, TYPE_16);
		confVideo[i].HorizontalSyncLength = ConfigParam_Init("HorizontalSyncLength", &default_numeric_data, TYPE_8);
		confVideo[i].HorizontalActiveVideoStart = ConfigParam_Init("HorizontalActiveVideoStart", &default_numeric_data, TYPE_8);
		confVideo[i].HorizontalActiveVideoLength = ConfigParam_Init("HorizontalActiveVideoLength", &default_numeric_data, TYPE_16);
		confVideo[i].VerticalSyncLength = ConfigParam_Init("VerticalSyncLength", &default_numeric_data, TYPE_8);
		confVideo[i].VoltageDac = ConfigParam_Init("VoltageDac", &default_numeric_data, TYPE_8);
		confVideo[i].SyncRgb = ConfigParam_Init("SyncRgb", &default_numeric_data, TYPE_8);
		confVideo[i].AspectRatio = ConfigParam_Init("AspectRatio", &default_numeric_data, TYPE_16);
	}

	/* INIT GRAPHICS */
	snprintf(name, sizeof(name), "confGraphics");

	confGraphics.name = STRDUP(name);
	confGraphics.ReferenceLineWidth = ConfigParam_Init("ReferenceLineWidth", &default_numeric_data, TYPE_16);
	confGraphics.ReferenceLineLowSpeed = ConfigParam_Init("ReferenceLineLowSpeed", &default_numeric_data, TYPE_16);
	confGraphics.ReferenceLineHighSpeed = ConfigParam_Init("ReferenceLineHighSpeed", &default_numeric_data, TYPE_16);
	confGraphics.ReferenceLineHighSpeedDelay = ConfigParam_Init("ReferenceLineHighSpeedDelay", &default_numeric_data, TYPE_16);
	confGraphics.ReferenceLineSwitchDelay = ConfigParam_Init("ReferenceLineSwitchDelay", &default_numeric_data, TYPE_16);
	confGraphics.PosNegSpeed = ConfigParam_Init("PosNegSpeed", &default_numeric_data, TYPE_16);

	/* INIT PTZ */
	snprintf(name, sizeof(name), "confPTZ");

	confPTZ.name = STRDUP(name);
	confPTZ.LimitUp = ConfigParam_Init("LimitUp", &default_numeric_data, TYPE_32);
	confPTZ.LimitDown = ConfigParam_Init("LimitDown", &default_numeric_data, TYPE_32);
	confPTZ.LimitLeft = ConfigParam_Init("LimitLeft", &default_numeric_data, TYPE_32);
	confPTZ.LimitRight = ConfigParam_Init("LimitRight", &default_numeric_data, TYPE_32);

	/* INIT BATTERY */
	snprintf(name, sizeof(name), "confBattery");

	confBattery.name = STRDUP(name);
	confBattery.Type = ConfigParam_Init("Type", &default_numeric_data, TYPE_8);
	confBattery.NominalCapacity = ConfigParam_Init("NominalCapacity", &default_numeric_data, TYPE_16);
	confBattery.TempCoefficientCapacity = ConfigParam_Init("TempCoefficientCapacity", &default_numeric_data, TYPE_16);
	confBattery.AgeCoefficientCapacity = ConfigParam_Init("AgeCoefficientCapacity", &default_numeric_data, TYPE_16);
	confBattery.LowCapacityLimit = ConfigParam_Init("LowCapacityLimit", &default_numeric_data, TYPE_16);

	/* INIT LIGHTING */
	snprintf(name, sizeof(name), "confLighting");

	confLighting.name = STRDUP(name);
	confLighting.Intensity = ConfigParam_Init("Intensity", &default_numeric_data, TYPE_16);

	build_param_list();
}
EXPORT_SYMBOL(ConfigParam_InitAll);

/**
 * @brief User-space compatible function to find a parameter by passing a string corresponding to the sought parameter.
 * Example use:
 * Some parameters are structured in a way that they can be indexed.
 * For example, if you want to find the "Zoom" parameter of the primary camera mode,
 * you can pass the string "confCameraMode[0].Zoom".
 * If you want to find the "confLicenseKey" parameter, you can pass "confLicenseKey[0]" for the first license key.
 * 
 * @param path: The string representing the parameter to find.
 * @return: Parameter corresponding to the passed string (if valid) */
ConfigParam *ConfigParam_FindByName(const char *path)
{
	char *path_copy;
	char *token;
	ConfigParam *param = NULL;
	int index = 0;
	char *strsep_current_ptr;

	path_copy = STRDUP(path);
	if (!path_copy) {
		return NULL;
	}

	strsep_current_ptr = path_copy;

	token = strsep(&strsep_current_ptr, ".[]");

	if (!token) {
		kfree(path_copy);
		return NULL;
	}

	if (strcmp(token, "confLicenseKey") == 0) {
		token = strsep(&strsep_current_ptr, "[]");
		if (token) {
			index = katoi(token);
			if (index >= 0 && index < CONFIG_LICENSE_KEY_AMT) {
				param = &confLicenseKey[index];
			}
		}
	} else if (strcmp(token, "confFactoryDefaultVersion") == 0) {
		param = &confFactoryDefaultVersion;
	} else if (strcmp(token, "confProductId") == 0) {
		param = &confProductId;
	} else if (strcmp(token, "confFunctions") == 0) {
		if (strsep_current_ptr && *strsep_current_ptr == '.') {
			strsep_current_ptr++; // Consume the dot
		}
		// Check if the rest of the string is empty or if we are at the end
		if (!strsep_current_ptr || *strsep_current_ptr == '\0') {
			token = NULL; // No more sub-fields
		} else {
			token = strsep(&strsep_current_ptr, ".");
		}

		if (token) {
			if (strcmp(token, "Seesaw") == 0)
				param = &confFunctions.Seesaw;
			else if (strcmp(token, "ReferenceLine") == 0)
				param = &confFunctions.ReferenceLine;
			else if (strcmp(token, "Curtain") == 0)
				param = &confFunctions.Curtain;
		}
	} else if (strcmp(token, "confColor") == 0) {
		token = strsep(&strsep_current_ptr,
			       "[]"); // Get index
		if (token) {
			index = katoi(token);
			if (index >= 0 && index < CONFIG_COLOR_AMT) {
				if (strsep_current_ptr && *strsep_current_ptr == '.') {
					strsep_current_ptr++;
				}
				if (!strsep_current_ptr || *strsep_current_ptr == '\0') // FIX HERE
				{
					token = NULL;
				} else {
					token = strsep(&strsep_current_ptr, ".");
				}

				if (token) {
					if (strcmp(token, "Group") == 0)
						param = &confColor[index].Group;
					else if (strcmp(token, "ArtificialColorPoint1") == 0)
						param = &confColor[index].ArtificialColorPoint1;
					else if (strcmp(token, "ArtificialColorPoint2") == 0)
						param = &confColor[index].ArtificialColorPoint2;
					else if (strcmp(token, "ArtificialColorPoint3") == 0)
						param = &confColor[index].ArtificialColorPoint3;
					else if (strcmp(token, "ArtificialColorPoint4") == 0)
						param = &confColor[index].ArtificialColorPoint4;
					else if (strcmp(token, "CameraBackground") == 0)
						param = &confColor[index].CameraBackground;
					else if (strcmp(token, "ReferenceLine") == 0)
						param = &confColor[index].ReferenceLine;
					else if (strcmp(token, "Restricted") == 0)
						param = &confColor[index].Restricted;
				}
			}
		}
	} else if (strcmp(token, "confCamera") == 0) {
		if (strsep_current_ptr && *strsep_current_ptr == '.') {
			strsep_current_ptr++; // Consume the dot
		}
		// Check if the rest of the string is empty or if we are at the end
		if (!strsep_current_ptr || *strsep_current_ptr == '\0') // This was one of the warning lines
		{
			token = NULL; // No more sub-fields
		} else {
			token = strsep(&strsep_current_ptr, ".");
		}

		if (token) {
			if (strcmp(token, "Type") == 0)
				param = &confCamera.Type;
			else if (strcmp(token, "RGain") == 0)
				param = &confCamera.RGain;
			else if (strcmp(token, "BGain") == 0)
				param = &confCamera.BGain;
			else if (strcmp(token, "FirstLine") == 0)
				param = &confCamera.FirstLine;
			else if (strcmp(token, "UsableLines") == 0)
				param = &confCamera.UsableLines;
			else if (strcmp(token, "FirstPixel") == 0)
				param = &confCamera.FirstPixel;
			else if (strcmp(token, "UsablePixels") == 0)
				param = &confCamera.UsablePixels;
			else if (strcmp(token, "UsableRatio") == 0)
				param = &confCamera.UsableRatio;
			else if (strcmp(token, "Interface") == 0)
				param = &confCamera.Interface;
			else if (strcmp(token, "ArtificialColorShutterTime") == 0)
				param = &confCamera.ArtificialColorShutterTime;
			else if (strcmp(token, "ArtificialColorBrightness") == 0)
				param = &confCamera.ArtificialColorBrightness;
			else if (strcmp(token, "ArtificialColorIris") == 0)
				param = &confCamera.ArtificialColorIris;
			else if (strcmp(token, "ArtificialColorExposureCompensation") == 0)
				param = &confCamera.ArtificialColorExposureCompensation;
			// Added missing confCamera parameters based on error log
			else if (strcmp(token, "NaturalColorShutterTime") == 0)
				param = &confCamera.NaturalColorShutterTime;
			else if (strcmp(token, "NaturalColorBrightness") == 0)
				param = &confCamera.NaturalColorBrightness;
			else if (strcmp(token, "NaturalColorIris") == 0)
				param = &confCamera.NaturalColorIris;
			else if (strcmp(token, "NaturalColorExposureCompensation") == 0)
				param = &confCamera.NaturalColorExposureCompensation;
			else if (strcmp(token, "NaturalColorGainPeak") == 0)
				param = &confCamera.NaturalColorGainPeak;
			else if (strcmp(token, "ArtificialColorGainPeak") == 0)
				param = &confCamera.ArtificialColorGainPeak;
		}
	}
	// Added missing sections based on error log
	else if (strcmp(token, "confCameraMode") == 0) {
		token = strsep(&strsep_current_ptr,
			       "[]"); // Get index
		if (token) {
			index = katoi(token);
			if (index >= 0 && index < CONFIG_CAMERA_MODE_AMT) {
				if (strsep_current_ptr && *strsep_current_ptr == '.') {
					strsep_current_ptr++;
				}
				if (!strsep_current_ptr || *strsep_current_ptr == '\0') // FIX HERE
				{
					token = NULL;
				} else {
					token = strsep(&strsep_current_ptr, ".");
				}

				if (token) {
					if (strcmp(token, "Zoom") == 0)
						param = &confCameraMode[index].Zoom;
					else if (strcmp(token, "ZoomMin") == 0)
						param = &confCameraMode[index].ZoomMin;
					else if (strcmp(token, "ZoomMax") == 0)
						param = &confCameraMode[index].ZoomMax;
					else if (strcmp(token, "ZoomSpeed") == 0)
						param = &confCameraMode[index].ZoomSpeed;
					else if (strcmp(token, "Focus") == 0)
						param = &confCameraMode[index].Focus;
					else if (strcmp(token, "FocusMin") == 0)
						param = &confCameraMode[index].FocusMin;
					else if (strcmp(token, "FocusMax") == 0)
						param = &confCameraMode[index].FocusMax;
					else if (strcmp(token, "FocusSpeed") == 0)
						param = &confCameraMode[index].FocusSpeed;
					else if (strcmp(token, "NaturalColorExposure") == 0)
						param = &confCameraMode[index].NaturalColorExposure;
					else if (strcmp(token, "ArtificialColorExposure") == 0)
						param = &confCameraMode[index].ArtificialColorExposure;
					else if (strcmp(token, "WhiteBalance") == 0)
						param = &confCameraMode[index].WhiteBalance;
				}
			}
		}
	} else if (strcmp(token, "confMonitorMode") == 0) {
		token = strsep(&strsep_current_ptr, "[]"); // Get index
		if (token) {
			index = katoi(token);
			if (index >= 0 && index < CONFIG_MONITOR_MODE_AMT) {
				if (strsep_current_ptr && *strsep_current_ptr == '.') {
					strsep_current_ptr++;
				}
				if (!strsep_current_ptr || *strsep_current_ptr == '\0') // FIX HERE
				{
					token = NULL;
				} else {
					token = strsep(&strsep_current_ptr, ".");
				}

				if (token) {
					if (strcmp(token, "ColorGainR") == 0)
						param = &confMonitorMode[index].ColorGainR;
					else if (strcmp(token, "ColorGainG") == 0)
						param = &confMonitorMode[index].ColorGainG;
					else if (strcmp(token, "ColorGainB") == 0)
						param = &confMonitorMode[index].ColorGainB;
					else if (strcmp(token, "Contrast") == 0)
						param = &confMonitorMode[index].Contrast;
					else if (strcmp(token, "Brightness") == 0)
						param = &confMonitorMode[index].Brightness;
					else if (strcmp(token, "Saturation") == 0)
						param = &confMonitorMode[index].Saturation;
					else if (strcmp(token, "AutoPosNegEnable") == 0)
						param = &confMonitorMode[index].AutoPosNegEnable;
					else if (strcmp(token, "AutoPosNegMin") == 0)
						param = &confMonitorMode[index].AutoPosNegMin;
					else if (strcmp(token, "AutoPosNegMax") == 0)
						param = &confMonitorMode[index].AutoPosNegMax;
					else if (strcmp(token, "NaturalColorContrast") == 0)
						param = &confMonitorMode[index].NaturalColorContrast;
					else if (strcmp(token, "NaturalColorBrightnessDefault") == 0)
						param = &confMonitorMode[index].NaturalColorBrightnessDefault;
					else if (strcmp(token, "NaturalColorBrightnessCoefficient") == 0)
						param = &confMonitorMode[index].NaturalColorBrightnessCoefficient;
					else if (strcmp(token, "NaturalColorDefault") == 0)
						param = &confMonitorMode[index].NaturalColorDefault;
					else if (strcmp(token, "NaturalColorCoefficient") == 0)
						param = &confMonitorMode[index].NaturalColorCoefficient;
					else if (strcmp(token, "ArtificalColorContrast") == 0)
						param = &confMonitorMode[index].ArtificalColorContrast;
					else if (strcmp(token, "ArtificalColorBrightness") == 0)
						param = &confMonitorMode[index].ArtificalColorBrightness;
					else if (strcmp(token, "BlackLinesTop") == 0)
						param = &confMonitorMode[index].BlackLinesTop;
					else if (strcmp(token, "BlackLinesBottom") == 0)
						param = &confMonitorMode[index].BlackLinesBottom;
					else if (strcmp(token, "GammaCorrection1") == 0)
						param = &confMonitorMode[index].GammaCorrection1;
					else if (strcmp(token, "GammaCorrection2") == 0)
						param = &confMonitorMode[index].GammaCorrection2;
					else if (strcmp(token, "GammaCorrection3") == 0)
						param = &confMonitorMode[index].GammaCorrection3;
					else if (strcmp(token, "GammaCorrection4") == 0)
						param = &confMonitorMode[index].GammaCorrection4;
					else if (strcmp(token, "GammaCorrection5") == 0)
						param = &confMonitorMode[index].GammaCorrection5;
				}
			}
		}
	} else if (strcmp(token, "confVideo") == 0) {
		token = strsep(&strsep_current_ptr, "[]"); // Get index
		if (token) {
			index = katoi(token);
			if (index >= 0 && index < CONFIG_VIDEO_AMT) {
				if (strsep_current_ptr && *strsep_current_ptr == '.') {
					strsep_current_ptr++;
				}
				if (!strsep_current_ptr || *strsep_current_ptr == '\0') // FIX HERE
				{
					token = NULL;
				} else {
					token = strsep(&strsep_current_ptr, ".");
				}

				if (token) {
					if (strcmp(token, "PaletteSelection") == 0)
						param = &confVideo[index].PaletteSelection;
					else if (strcmp(token, "Type") == 0)
						param = &confVideo[index].Type;
					else if (strcmp(token, "LinesPerFrame") == 0)
						param = &confVideo[index].LinesPerFrame;
					else if (strcmp(token, "LinesActiveInCompletePicture") == 0)
						param = &confVideo[index].LinesActiveInCompletePicture;
					else if (strcmp(token, "FirstActiveLine") == 0)
						param = &confVideo[index].FirstActiveLine;
					else if (strcmp(token, "HorizontalTotalLength") == 0)
						param = &confVideo[index].HorizontalTotalLength;
					else if (strcmp(token, "HorizontalSyncLength") == 0)
						param = &confVideo[index].HorizontalSyncLength;
					else if (strcmp(token, "HorizontalActiveVideoStart") == 0)
						param = &confVideo[index].HorizontalActiveVideoStart;
					else if (strcmp(token, "HorizontalActiveVideoLength") == 0)
						param = &confVideo[index].HorizontalActiveVideoLength;
					else if (strcmp(token, "VerticalSyncLength") == 0)
						param = &confVideo[index].VerticalSyncLength;
					else if (strcmp(token, "VoltageDac") == 0)
						param = &confVideo[index].VoltageDac;
					else if (strcmp(token, "SyncRgb") == 0)
						param = &confVideo[index].SyncRgb;
					else if (strcmp(token, "AspectRatio") == 0)
						param = &confVideo[index].AspectRatio;
				}
			}
		}
	} else if (strcmp(token, "confGraphics") == 0) {
		if (strsep_current_ptr && *strsep_current_ptr == '.') {
			strsep_current_ptr++;
		}
		if (!strsep_current_ptr || *strsep_current_ptr == '\0') {
			token = NULL;
		} else {
			token = strsep(&strsep_current_ptr, ".");
		}

		if (token) {
			if (strcmp(token, "ReferenceLineWidth") == 0)
				param = &confGraphics.ReferenceLineWidth;
			else if (strcmp(token, "ReferenceLineLowSpeed") == 0)
				param = &confGraphics.ReferenceLineLowSpeed;
			else if (strcmp(token, "ReferenceLineHighSpeed") == 0)
				param = &confGraphics.ReferenceLineHighSpeed;
			else if (strcmp(token, "ReferenceLineHighSpeedDelay") == 0)
				param = &confGraphics.ReferenceLineHighSpeedDelay;
			else if (strcmp(token, "ReferenceLineSwitchDelay") == 0)
				param = &confGraphics.ReferenceLineSwitchDelay;
			else if (strcmp(token, "PosNegSpeed") == 0)
				param = &confGraphics.PosNegSpeed;
		}
	} else if (strcmp(token, "confPTZ") == 0) {
		if (strsep_current_ptr && *strsep_current_ptr == '.') {
			strsep_current_ptr++;
		}
		if (!strsep_current_ptr || *strsep_current_ptr == '\0') {
			token = NULL;
		} else {
			token = strsep(&strsep_current_ptr, ".");
		}

		if (token) {
			if (strcmp(token, "LimitUp") == 0)
				param = &confPTZ.LimitUp;
			else if (strcmp(token, "LimitDown") == 0)
				param = &confPTZ.LimitDown;
			else if (strcmp(token, "LimitLeft") == 0)
				param = &confPTZ.LimitLeft;
			else if (strcmp(token, "LimitRight") == 0)
				param = &confPTZ.LimitRight;
		}
	} else if (strcmp(token, "confBattery") == 0) {
		if (strsep_current_ptr && *strsep_current_ptr == '.') {
			strsep_current_ptr++;
		}
		if (!strsep_current_ptr || *strsep_current_ptr == '\0') {
			token = NULL;
		} else {
			token = strsep(&strsep_current_ptr, ".");
		}

		if (token) {
			if (strcmp(token, "Type") == 0)
				param = &confBattery.Type;
			else if (strcmp(token, "NominalCapacity") == 0)
				param = &confBattery.NominalCapacity;
			else if (strcmp(token, "TempCoefficientCapacity") == 0)
				param = &confBattery.TempCoefficientCapacity;
			else if (strcmp(token, "AgeCoefficientCapacity") == 0)
				param = &confBattery.AgeCoefficientCapacity;
			else if (strcmp(token, "LowCapacityLimit") == 0)
				param = &confBattery.LowCapacityLimit;
		}
	} else if (strcmp(token, "confLighting") == 0) {
		if (strsep_current_ptr && *strsep_current_ptr == '.') {
			strsep_current_ptr++;
		}
		if (!strsep_current_ptr || *strsep_current_ptr == '\0') {
			token = NULL;
		} else {
			token = strsep(&strsep_current_ptr, ".");
		}

		if (token) {
			if (strcmp(token, "Intensity") == 0)
				param = &confLighting.Intensity;
			// Additional parameters for lighting can be added here
		}
	}
	/* Add other config sections as needed */

	kfree(path_copy);
	return param;
}
EXPORT_SYMBOL(ConfigParam_FindByName);

/**
 * @brief Set the parameter's data and write the data to EEPROM on the corresponding parameter EEPROM address
 * @param param: Parameter to be altered
 * @param data: Data to be written to parameter's data variable
 */
void ConfigParam_SetData(ConfigParam *param, const void *data, int (*platform_eeprom_read)(uint32_t, uint8_t *), int (*platform_eeprom_write)(uint32_t, uint8_t))
{
	uint8_t i;
	uint8_t *new_data;
	uint8_t eeprom_byte;
	int needs_write;

	if (!param || !param->data || !data) {
		pr_err("ConfigParam_SetData: Invalid parameter or data\n");
		return;
	}

	new_data = (uint8_t *)kmalloc(param->size, GFP_KERNEL);
	if (!new_data)
		return;

	if (serialize_data(new_data, data, param->type, param->size) != 0) {
		kfree(new_data);
		return;
	}

	for (i = 0; i < param->size; i++) {
		needs_write = 1;

		if (platform_eeprom_read(param->address + i, &eeprom_byte) == 0) {
			if (eeprom_byte == new_data[i]) {
				needs_write = 0; /* Data matches, skip write */
			}
		}

		if (needs_write) {
			if (platform_eeprom_write(param->address + i, new_data[i]) != 0) {
				pr_err("[%s]: Failed to write byte %u of param %s\n", __func__, i, param->name ? param->name : "UNKNOWN");
			}
		}
	}

	memcpy(param->data, new_data, param->size);
	kfree(new_data);

	lviconfig_notify_param_changed(param);
}
EXPORT_SYMBOL(ConfigParam_SetData);

/**
 * @brief Reads the data from the EEPROM for a given ConfigParam.
 * Returns a pointer to the data if successful, or NULL if there was an error.
 * @param param: The ConfigParam to read data from.
 * @return: Pointer to the data if successful, NULL if there was an error.
 */
const void *ConfigParam_GetData(ConfigParam *param, int (*platform_eeprom_read)(uint32_t, uint8_t *))
{
	uint8_t read_success;
	uint8_t i;

	if (param == NULL || param->data == NULL)
		return NULL;

	read_success = 1;
	for (i = 0; i < param->size; i++) {
		if (platform_eeprom_read(param->address + i, &param->data[i]) != 0) {
			read_success = 0;
			break;
		}
	}

	if (!read_success) {
		return NULL;
	}
	return (const void *)param->data;
}
EXPORT_SYMBOL(ConfigParam_GetData);

/**
 * @brief Parses the EEPROM to load configuration parameters.
 * This function reads all configuration parameters from the EEPROM
 * and initializes them with the stored values.
 * 
 * This function MUST be called AFTER the ConfigParam_InitAll() function
 * to ensure that all parameters are initialized before reading from EEPROM.
 */
void ConfigParam_ParseEEPROM(int (*platform_eeprom_read)(uint32_t, uint8_t *))
{
	size_t i;

	for (i = 0; i < num_params; i++)
		ConfigParam_GetData(all_params[i], platform_eeprom_read);

	lviconfig_restore_blank_monitor_defaults();
}
EXPORT_SYMBOL(ConfigParam_ParseEEPROM);

/**
 * @brief Prints the configuration parameter to the kernel log.
 * This function prints the parameter's name, address, size, type, and value.
 * It handles different types of parameters (8-bit, 16-bit, 32-bit, string, array).
 * 
 * @param param: The ConfigParam to print.
 */
void ConfigParam_PrintParam(ConfigParam *param, int (*platform_eeprom_read)(uint32_t, uint8_t *))
{
	const void *data;
	const int name_width = 36; // Consistent with user-space for padding
	uint32_t i; // Loop variable for arrays
	const uint8_t *array_data;

	if (!param) {
		pr_info("[%s] Parameter is NULL\n", __func__);
		return;
	}
	// It's good practice to also check param->name, though ConfigParam_InitAll should set it.
	if (!param->name) {
		pr_info("[%s] Parameter name is NULL (Addr: 0x%04X)\n", __func__, param->address);
		// Still try to print what we can if data is available
	}

	data = ConfigParam_GetData(param, platform_eeprom_read); // Reads from EEPROM
	if (!data) {
		pr_info("[%s] No data for %s (Addr: 0x%04X)\n", __func__, param->name ? param->name : "UNKNOWN", param->address);
		return;
	}

	switch (param->type) {
	case TYPE_8:
		pr_info("[%s] \tParameter: %-*s Addr: 0x%04X  Size: %2u  Type: TYPE_8         Value: 0x%02X\n", __func__, name_width, param->name ? param->name : "N/A", param->address, param->size,
			*(const uint8_t *)data);
		break;
	case TYPE_16:
		pr_info("[%s] \tParameter: %-*s Addr: 0x%04X  Size: %2u  Type: TYPE_16        Value: 0x%04X\n", __func__, name_width, param->name ? param->name : "N/A", param->address, param->size,
			*(const uint16_t *)data);
		break;
	case TYPE_32:
		pr_info("[%s] \tParameter: %-*s Addr: 0x%04X  Size: %2u  Type: TYPE_32        Value: 0x%08X\n", __func__, name_width, param->name ? param->name : "N/A", param->address, param->size,
			*(const uint32_t *)data);
		break;
	case TYPE_STRING:
		pr_info("[%s] \tParameter: %-*s Addr: 0x%04X  Size: %2u  Type: TYPE_STRING    Value: \"%s\"\n", __func__, name_width, param->name ? param->name : "N/A", param->address, param->size,
			(const char *)data);
		break;
	case TYPE_ARRAY:
		// Start the line using pr_info (which might add KERN_INFO etc.)
		pr_info("[%s] \tParameter: %-*s Addr: 0x%04X  Size: %2u  Type: TYPE_ARRAY     Value: [", __func__, name_width, param->name ? param->name : "N/A", param->address,
			param->size); // No newline here

		array_data = (const uint8_t *)data;
		for (i = 0; i < param->size; ++i) {
			// Continue the line with KERN_CONT
			// Limit printed elements to avoid excessive log spam for very large arrays
			if (i >= 32 && param->size > 36) { // Print up to ~32 elements, then "..."
				printk(KERN_CONT "...");
				break;
			}
			printk(KERN_CONT "0x%02X", array_data[i]);
			if (i < param->size - 1) {
				printk(KERN_CONT ", ");
			}
		}
		printk(KERN_CONT "]\n"); // End the line
		break;
	default:
		pr_info("[%s] \tParameter: %-*s Addr: 0x%04X  Size: %2u  Type: UNKNOWN TYPE\n", __func__, name_width, param->name ? param->name : "N/A", param->address, param->size);
		break;
	}
}

/**
 * @brief Prints all configuration parameters to the kernel log.
 * This function iterates through all configuration parameters and prints
 * their details using ConfigParam_PrintParam.
 */
void ConfigParam_PrintAll(int (*platform_eeprom_read)(uint32_t, uint8_t *))
{
	size_t i;

	pr_info("[%s] --- All Configuration Parameters (Kernel) ---\n", __func__);
	for (i = 0; i < num_params; i++)
		ConfigParam_PrintParam(all_params[i], platform_eeprom_read);
	pr_info("[%s] --- End of Configuration Parameters (Kernel) ---\n", __func__);
}
EXPORT_SYMBOL(ConfigParam_PrintAll);

int ConfigParam_IsInitialized(void)
{
	return lviconfig_initialized;
}
EXPORT_SYMBOL(ConfigParam_IsInitialized);

void ConfigParam_MarkInitialized(void)
{
	lviconfig_initialized = 1;
}
EXPORT_SYMBOL(ConfigParam_MarkInitialized);

/**
 * @brief Save a single parameter to EEPROM if it differs from EEPROM contents.
 * Reads EEPROM data first and only writes bytes that differ to reduce EEPROM wear.
 * @param param: Parameter to save
 * @return: 1 if parameter was written, 0 if no write needed, negative on error
 */
int ConfigParam_ReadAndSaveParam(ConfigParam *param, uint8_t save, int (*platform_eeprom_read)(uint32_t, uint8_t *), int (*platform_eeprom_write)(uint32_t, uint8_t))
{
	uint8_t i;
	uint8_t eeprom_byte;
	int needs_write;
	int wrote_any = 0;

	if (!param || !param->data)
		return -EINVAL;

	for (i = 0; i < param->size; i++) {
		needs_write = 1;
		if (platform_eeprom_read(param->address + i, &eeprom_byte) == 0) {
			if (eeprom_byte == param->data[i]) {
				needs_write = 0; /* Data matches, skip write */
			}
		}
		if (needs_write && save) {
			if (platform_eeprom_write(param->address + i, param->data[i]) != 0) {
				pr_err("[%s]: Failed to write byte %u of param %s\n", __func__, i, param->name ? param->name : "UNKNOWN");
				return -EIO;
			}
			wrote_any = 1;
		}
	}

	if (wrote_any && save) {
		pr_info("[%s]: Wrote param %s (addr=0x%04X, size=%u)\n", __func__, param->name ? param->name : "UNKNOWN", param->address, param->size);
	}

	return wrote_any;
}

/**
 * @brief Save all configuration parameters to EEPROM.
 * Only writes parameters that differ from current EEPROM contents.
 * @return: Number of parameters that were written, or negative error code
 */
int ConfigParam_ReadAndSaveAll(uint8_t save, int (*platform_eeprom_read)(uint32_t, uint8_t *), int (*platform_eeprom_write)(uint32_t, uint8_t))
{
	size_t i;
	int ret;
	int total_written = 0;

	pr_info("[%s]: Starting save of all configuration parameters\n", __func__);

	for (i = 0; i < num_params; i++) {
		ret = ConfigParam_ReadAndSaveParam(all_params[i], save, platform_eeprom_read, platform_eeprom_write);
		if (ret < 0)
			return ret;
		total_written += ret;
	}

	pr_info("[%s]: Save complete. %d parameters needed writing\n", __func__, total_written);
	return total_written;
}
EXPORT_SYMBOL(ConfigParam_ReadAndSaveAll);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Max Borglowe");
MODULE_DESCRIPTION("LVI Config Parameters Library ultra");