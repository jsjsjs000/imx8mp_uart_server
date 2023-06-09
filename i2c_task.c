// #define DEBUG_I2C
// #define DEBUG_LEDS

/* Freescale includes. */
#include <fsl_device_registers.h>
#include <fsl_debug_console.h>
#include <fsl_i2c.h>
#include <fsl_i2c_freertos.h>
#include <fsl_uart.h>
#include <fsl_uart_freertos.h>

/* FreeRTOS kernel includes. */
#include <FreeRTOS.h>
#include <task.h>
#include <queue.h>
#include <timers.h>
#include <semphr.h>

/* Board config */
#include "board_cfg/board.h"
#include "board_cfg/clock_config.h"
#include "board_cfg/pin_mux.h"

#include "i2c_task.h"

#define LED_COMMAND_OFF       0
#define LED_COMMAND_LIGHT_25  1
#define LED_COMMAND_LIGHT_50  2
#define LED_COMMAND_LIGHT_100 3

#define LED_OFF       0
#define LED_LIGHT_25  2
#define LED_LIGHT_50  3
#define LED_LIGHT_100 1

#define I2C_DELAY 750

static uint8_t g_master_buff[I2C_DATA_LENGTH];
static i2c_master_handle_t *g_m_handle;

static i2c_rtos_handle_t master_rtos_handle;
static i2c_master_config_t masterConfig;
static i2c_master_transfer_t masterXfer;
static uint32_t sourceClock;

volatile uint8_t led_r = LED_OFF;
volatile uint8_t led_g = LED_OFF;
volatile uint8_t led_b = LED_OFF;
volatile enum led_mode_t led_mode = LED_MODE_AUTO;

static void i2c_set_leds(uint8_t r, uint8_t g, uint8_t b);
static void i2c_print_leds_status(char *message, uint8_t r, uint8_t g, uint8_t b);
static void i2c_master_led_write(uint8_t r, uint8_t g, uint8_t b);
static void i2c_master_led_read(volatile uint8_t *r, volatile uint8_t *g, volatile uint8_t *b);
static void i2c_print_buffer(char *message, size_t length) __attribute__ ((unused));

void i2c_master_initialize(void)
{
	CLOCK_SetRootMux(kCLOCK_RootI2c2, kCLOCK_I2cRootmuxSysPll1Div5); /* Set I2C source to SysPLL1 Div5 160MHZ */
	CLOCK_SetRootDivider(kCLOCK_RootI2c2, 1U, 10U);                  /* Set root clock to 160MHZ / 10 = 16MHZ */

	NVIC_SetPriority(I2C_MASTER_IRQN, 3);

	/*
		masterConfig.baudRate_Bps = 100000U;
		masterConfig.enableStopHold = false;
		masterConfig.glitchFilterWidth = 0U;
		masterConfig.enableMaster = true;
	*/
	I2C_MasterGetDefaultConfig(&masterConfig);
	masterConfig.baudRate_Bps = I2C_BAUDRATE;
	sourceClock = I2C_MASTER_CLK_FREQ;

	status_t status = I2C_RTOS_Init(&master_rtos_handle, I2C_MASTER, &masterConfig, sourceClock);
	if (status != kStatus_Success)
	{
		PRINTF("I2C master: error during init, 0x%x\r\n", status);
	}

	g_m_handle = &master_rtos_handle.drv_handle;

	memset(&masterXfer, 0, sizeof(masterXfer));
	masterXfer.slaveAddress   = I2C_MASTER_SLAVE_ADDR_7BIT;
	masterXfer.direction      = kI2C_Write;
	masterXfer.subaddress     = 0x11; // 10 - register autoincrement, 01 - write from register
	masterXfer.subaddressSize = 1;
	masterXfer.data           = g_master_buff;
	masterXfer.dataSize       = 0;
	masterXfer.flags          = kI2C_TransferDefaultFlag;
}

void i2c_master_task(void *pvParameters)
{
	PRINTF("I2C task started.\r\n");

	i2c_master_led_read(&led_r, &led_g, &led_b);
	i2c_print_leds_status("LEDS read:", led_r, led_g, led_b);
	i2c_set_leds(LED_OFF, LED_OFF, LED_OFF);

	while (true)
	{
		if (led_mode == LED_MODE_AUTO)
		{
			int ticks = xTaskGetTickCount() * 1000 / configTICK_RATE_HZ;
			int cycle = ticks / I2C_DELAY;
			switch (cycle % 7)
			{
				case 0: i2c_set_leds(LED_LIGHT_25, LED_OFF, LED_OFF); break;
				case 1: i2c_set_leds(LED_OFF, LED_LIGHT_25, LED_OFF); break;
				case 2: i2c_set_leds(LED_OFF, LED_OFF, LED_LIGHT_25); break;
				case 3: i2c_set_leds(LED_LIGHT_25, LED_LIGHT_25, LED_OFF); break;
				case 4: i2c_set_leds(LED_LIGHT_25, LED_OFF, LED_LIGHT_25); break;
				case 5: i2c_set_leds(LED_OFF, LED_LIGHT_25, LED_LIGHT_25); break;
				case 6: i2c_set_leds(LED_LIGHT_50, LED_LIGHT_50, LED_LIGHT_25); break;
			}
		}
		else if (led_mode == LED_MODE_MANUAL)
		{
			i2c_master_led_write(led_command_to_led_i2c(led_r), led_command_to_led_i2c(led_g),
					led_command_to_led_i2c(led_b));
		}

		vTaskDelay(pdMS_TO_TICKS(20));
	}

	vTaskSuspend(NULL);
}

static void i2c_set_leds(uint8_t r, uint8_t g, uint8_t b)
{
	led_r = r;
	led_g = g;
	led_b = b;
	i2c_master_led_write(led_r, led_g, led_b);
	i2c_print_leds_status("LEDS write:", led_r, led_g, led_b);
}

__attribute__ ((unused))
static char* i2c_get_led_status(uint8_t value)
{
	switch (value)
	{
		case LED_OFF:       return "  0%";
		case LED_LIGHT_25:  return " 25%";
		case LED_LIGHT_50:  return " 50%";
		case LED_LIGHT_100: return "100%";
	}
	return "undef";
}

static void i2c_print_leds_status(char *message, uint8_t r, uint8_t g, uint8_t b)
{
#ifdef DEBUG_LEDS
	PRINTF("%s R=%s, G=%s, B=%s\r\n", message, i2c_get_led_status(r),
			i2c_get_led_status(g), i2c_get_led_status(b));
#endif
}

static void i2c_master_led_write(uint8_t r, uint8_t g, uint8_t b)
{
	int j = 0;
	g_master_buff[j++] = 0x00; // PCS0 - Frequency Prescaler 0, BLINK0 = (PSC0 + 1) / 152 // 97
	g_master_buff[j++] = 0x40; // PWM0 - Pulse Width Modulation 0
	g_master_buff[j++] = 0x00; // PCS1 - Frequency Prescaler 1, BLINK1 = (PSC1 + 1) / 152
	g_master_buff[j++] = 0x80; // PWM1 - Pulse Width Modulation 1
	g_master_buff[j++] = (b << 4) | (g << 2) | (r << 0); // B G R, 00 - off, 01 - on, 10 - PWM0, 11 - PWM1 // e1

#ifdef DEBUG_I2C
	i2c_print_buffer("Master will send data:", j);
#endif

	masterXfer.direction = kI2C_Write;
	masterXfer.dataSize = j;

	status_t status = I2C_RTOS_Transfer(&master_rtos_handle, &masterXfer);
	if (status != kStatus_Success)
	{
		PRINTF("I2C master: error during write transaction, 0x%x\r\n", status);
	}
	else
	{
#ifdef DEBUG_I2C
		PRINTF("I2C master: wrote bytes\r\n");
#endif
	}
}

static void i2c_master_led_read(volatile uint8_t *r, volatile uint8_t *g, volatile uint8_t *b)
{
	memset(&g_master_buff, 0, I2C_DATA_LENGTH);

	masterXfer.direction = kI2C_Read;
	masterXfer.dataSize = 5;

	status_t status = I2C_RTOS_Transfer(&master_rtos_handle, &masterXfer);
	if (status != kStatus_Success)
	{
		PRINTF("I2C master: error during read transaction, 0x%x\r\n", status);
	}
	else
	{
#ifdef DEBUG_I2C
		i2c_print_buffer("Master received data:", masterXfer.dataSize);
#endif

		*r = g_master_buff[4] & 0x03;
		*g = (g_master_buff[4] >> 2) & 0x03;
		*b = (g_master_buff[4] >> 4) & 0x03;
	}
}

static void i2c_print_buffer(char *message, size_t length)
{
	PRINTF("%s\r\n", message);
	for (uint32_t i = 0; i < length; i++)
	{
		if (i % 8 == 0)
		{
			PRINTF("\r\n");
		}
		PRINTF("%02x ", g_master_buff[i]);
	}
	PRINTF("\r\n\r\n");
}

uint8_t led_command_to_led_i2c(uint8_t a)
{
	switch (a)
	{
		case LED_COMMAND_OFF:       return LED_OFF;
		case LED_COMMAND_LIGHT_25:  return LED_LIGHT_25;
		case LED_COMMAND_LIGHT_50:  return LED_LIGHT_50;
		case LED_COMMAND_LIGHT_100: return LED_LIGHT_100;
		default: return LED_OFF;
	}
}

uint8_t led_i2c_to_led_command(uint8_t a)
{
	switch (a)
	{
		case LED_OFF:       return LED_COMMAND_OFF;
		case LED_LIGHT_25:  return LED_COMMAND_LIGHT_25;
		case LED_LIGHT_50:  return LED_COMMAND_LIGHT_50;
		case LED_LIGHT_100: return LED_COMMAND_LIGHT_100;
		default: return LED_COMMAND_OFF;
	}
}
