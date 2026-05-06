#ifndef LVICAM_CTRL_H
#define LVICAM_CTRL_H

#include <linux/ioctl.h>

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
#include <stddef.h>
#endif

#define LVICAM_IOC_MAGIC 'L'

// IOCTL Commands
#define LVICAM_CTRL_IOCTL_READ_DATA _IOR(LVICAM_IOC_MAGIC, 1, struct lvicam_i2c_cmd)
#define LVICAM_CTRL_IOCTL_WRITE_DATA _IOW(LVICAM_IOC_MAGIC, 2, struct lvicam_i2c_cmd)
#define LVICAM_CTRL_IOCTL_READ_SEESAW _IOR(LVICAM_IOC_MAGIC, 3, struct lvicam_seesaw_status)

struct lvicam_i2c_cmd {
	uint8_t subreg;
	uint16_t data;
	size_t size;
};

struct lvicam_seesaw_status {
	int value; // Current value of the seesaw
};

// ******* FPGA STATUS REGISTERS ********

// FPGA Status Register: 0x80
#define FPGA_FLAGS_INIT_STATUS_REG 0x80
#define FPGA_FLAG_PLL_SYS_LOCKED (1 << 3) // Bit 3: System PLL locked
#define FPGA_FLAG_PLL_CAM_LOCKED (1 << 2) // Bit 2: Camera PLL locked
#define FPGA_FLAG_CAMERA_ID_OK (1 << 1) // Bit 1: Camera ID matched config
#define FPGA_FLAG_CAMERA_INITIALIZED (1 << 0) // Bit 0: Camera initialized

// Camera ID Register: 0x81
#define CAM_ID_STATUS_REG 0x81
// Known camera ID values:
#define CAM_ID_WONWOO_MC108_M3 0x465A
#define CAM_ID_WONWOO_MC105 0x0E52
#define CAM_ID_WONWOO_MM405 0x0E6A
#define CAM_ID_SONY_FCB_EV9520L 0x0711
#define CAM_ID_TAMRON_MP1010_VC 0x0023
#define CAM_ID_TAMRON_MP3010M_EV 0xF017

// Zoom Position Registers
#define CAM1_ZOOM_POS_STATUS_REG 0xC0 // [15:0] Zoom absolute value - camera 1 (Reading camera)
#define CAM2_ZOOM_POS_STATUS_REG 0xC1 // [15:0] Zoom absolute value - camera 2 (Distance camera)

// Zoom Step Registers
#define CAM1_ZOOM_POS_STEP_STATUS_REG 0xC3 // [7:0] Zoom step value (0-22) - camera 1 (Reading camera)
#define CAM2_ZOOM_POS_STEP_STATUS_REG 0xC4 // [7:0] Zoom step value (0-22) - camera 2 (Distance camera)

// Reference Line Position Registers
#define REFLINE_HOR_POS_STATUS_REG 0xC6 // [15:0] Horizontal position of reference line
#define REFLINE_VER_POS_STATUS_REG 0xC7 // [15:0] Vertical position of reference line

// Curtain Position Registers
#define CURTAIN1_HOR_POS_STATUS_REG 0xC8 // [15:0] Upper horizontal curtain position
#define CURTAIN2_HOR_POS_STATUS_REG 0xC9 // [15:0] Lower horizontal curtain position
#define CURTAIN1_VER_POS_STATUS_REG 0xCA // [15:0] Left vertical curtain position
#define CURTAIN2_VER_POS_STATUS_REG 0xCB // [15:0] Right vertical curtain position

// Picture Mode Register: 0xCC
#define PICTURE_MODE_STATUS_REG 0xCC
#define PICTURE_MODE_ART_MASK 0xF0 // Bits 7:4 - Artifical color palette index (0-15)
#define PICTURE_MODE_NAT_MASK 0x0C // Bits 3:2 - Natural color palette index (0-3)
#define PICTURE_MODE_NAT_ON 0x02 // Bit 1: 1 = Natural mode, 0 = Artificial mode
#define PICTURE_MODE_CAMERA_FLIRP 0x01 // Bit 0: 1 = Distance camera, 0 = Readout camera

// PN Level Register: 0xCD
#define PN_LEVEL_STATUS_REG 0xCD // [7:0] P/N level in artificial color mode

// ********* FPGA CONTROL REGISTERS *********

// FPGA I2C Slave Address
#define FPGA_I2C_SLAVE_ADDR 0x10

// ********* Art Colour Control Registers *********
#define ART_COL_PAL_INIT_CONTROL_REG 0x01 // Multi-byte write
// Reserved: 0x02 - 0x04

// ********* ID Control Registers *********
#define PRODUCT_ID_CONTROL_REG 0x05 // [7:0] Write
#define PRODUCT_ID_MLZIP_FHD_17_TOUCH 0x1A // Example product ID for MLZIP FHD 17" Touch

// #define PANEL_ID_CONTROL_REG 0x06  // [7:0] Write
#define CAMERA_ID_CONTROL_REG 0x07 // [15:0] Write

// ********* User Panel Control Registers *********
#define USER_PANEL1_CMD_CONTROL_REG 0x08 // Multi-byte write
#define CMD_NATCOL_DN 0x13
#define CMD_NATCOL_UP 0x14
#define CMD_ARTCOL_DN 0x15
#define CMD_ARTCOL_UP 0x16
#define CMD_ZOOM_CW 0x19
#define CMD_ZOOM_CCW 0x1A
#define CMD_ZOOM_PUSH_DN 0x1B
#define CMD_ZOOM_PUSH_UP 0x1C
#define CMD_FUNCTION_CW 0x28
#define CMD_FUNCTION_CCW 0x29
#define CMD_FUNCTION_BTN_DN 0x2A
#define CMD_FUNCTION_BTN_UP 0x2B

// ********* Panel1 Edge Control Bits *********
#define USER_PANEL1_EDGE_CONTROL_REG 0x09 // [7:0] Write
#define EDGE_CTRL_NATCOL_BIT (1 << 3)
#define EDGE_CTRL_ARTCOL_BIT (1 << 2)
#define EDGE_CTRL_ZOOM_PUSH_BIT (1 << 1)
#define EDGE_CTRL_FUNCTION_PUSH_BIT (1 << 0)
#define EDGE_CTRL_DEFAULT 0xF0 // Reserved bits 7:4 set to 1, all edge bits inactive by default

// ********* Camera Control Registers *********
// Camera 1 (Readout)
#define CAM1_ZOOM_SPEED_CONTROL_REG 0x10 // [15:0] Write
#define CAM1_ZOOM_POS_MIN_CONTROL_REG 0x11 // [15:0] Write
#define CAM1_ZOOM_POS_MAX_CONTROL_REG 0x12 // [15:0] Write
#define CAM1_ZOOM_POS_INIT_CONTROL_REG 0x13 // [15:0] Write

// Camera 2 (Distance)
#define CAM2_ZOOM_POS_MIN_CONTROL_REG 0x14 // [15:0] Write
#define CAM2_ZOOM_POS_MAX_CONTROL_REG 0x15 // [15:0] Write
#define CAM2_ZOOM_POS_INIT_CONTROL_REG 0x16 // [15:0] Write
#define CAM2_ZOOM_SPEED_CONTROL_REG 0x17 // [15:0] Write

// Reserved: 0x18, 0x19

// Zoom Increment Step Tables (Multi-word writes)
#define CAM1_ZOOM_POS_INC_STEP_CONTROL_REG 0x1A // [15:0] * N steps
#define CAM1_ZOOM_POS_INIT_INC_CONTROL_REG 0x1B // [7:0]  Write

#define CAM2_ZOOM_POS_INC_STEP_CONTROL_REG 0x1C // [15:0] * N steps
#define CAM2_ZOOM_POS_INIT_INC_CONTROL_REG 0x1D // [7:0]  Write

// Reserved: 0x1E, 0x1F

// ********* Activated Functions Control Register *********
// NOTE: Sub Address not clearly defined in doc (marked as 0x??), define as placeholder
#define ACTIVATED_FUNCTIONS_CONTROL_REG 0x20 // [7:0] Write (bitmask)
#define ACT_FUNC_BITMASK_OBJECT_SHOW_VIEW (1 << 0) // Example use: Bit 0 toggles ObjectShowView

#endif // LVICAM_CTRL_H