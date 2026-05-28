#include <stdio.h>  // Standard C library for input/output 
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"  // Core FreeRTOS operating system 
#include "freertos/task.h"  // Task functions 
#include "esp_log.h"  // ESP32 logging
#include "driver/gpio.h"  // GPIO functions to control ESP32 pins
#include "driver/ledc.h"  // LEDC driver for PWM signal generation
#include "lcd_i2c.h"  // LCD display control over I2C communication
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include <string.h>
#include <stdlib.h>
#include "driver/uart.h"
#include "ultrasonic.h"
#include "rc522.h"

const char* TAG = "Project";

// GPIO Pin Definitions
// LCD - Exit LCD: SDA=GPIO21, SCL=GPIO22
#define LCD2_SDA_PIN   21
#define LCD2_SCL_PIN   22
#define LCD2_I2C_PORT  I2C_NUM_1
#define LCD2_ADDR      0x27

// Plain LEDs
#define LED1_PIN       25   // Slot 1 indicator (GPIO25)
#define LED2_PIN       26    // Slot 2 indicator (GPIO26)

// RGB LED (common cathode) via LEDC PWM
#define RGB_R_PIN      16
#define RGB_G_PIN      32
#define RGB_B_PIN      2

// LEDC config for RGB
#define LEDC_TIMER         LEDC_TIMER_1
#define LEDC_MODE          LEDC_LOW_SPEED_MODE
#define LEDC_FREQ_HZ       5000
#define LEDC_RESOLUTION    LEDC_TIMER_8_BIT
#define RGB_R_CHANNEL      LEDC_CHANNEL_3
#define RGB_G_CHANNEL      LEDC_CHANNEL_4
#define RGB_B_CHANNEL      LEDC_CHANNEL_5

#define BARRIER_SERVO_PIN 33
#define SERVO_LEDC_MODE       LEDC_LOW_SPEED_MODE
#define SERVO_LEDC_TIMER      LEDC_TIMER_0
#define SERVO_LEDC_DUTY_RES   LEDC_TIMER_13_BIT
#define SERVO_LEDC_FREQ_HZ    50
#define BARRIER_SERVO_CHANNEL   LEDC_CHANNEL_0

#define RFID_SPI_MISO_GPIO   19
#define RFID_SPI_MOSI_GPIO   23
#define RFID_SPI_SCK_GPIO    18
#define RFID_SPI_SDA_GPIO    5
#define RFID_RST_GPIO        17

// ultrasonic sensor Pin definitions
#define ultrasonic1_trig 27
#define ultrasonic1_echo 34

#define ultrasonic2_trig 14
#define ultrasonic2_echo 35

// system variables
#define ultrasonic_maxDistance 200
#define MAX_SPACES 2
#define SUDDEN_CHANGE_THRESHOLD 15 // consider any movement above this as sudden
#define TIME_TO_DEFINE_ANOMALY 2000 // sudden movement for 2s is an anomaly

// Semaphores initialization
SemaphoreHandle_t accidentDetectedBinSem;
SemaphoreHandle_t flashLEDBinSem;
SemaphoreHandle_t notifyLEDsBinSem;
SemaphoreHandle_t alertResetSem;

SemaphoreHandle_t barrierMutex;
SemaphoreHandle_t lcdMutex;
SemaphoreHandle_t parkingMutex;

// Queues initialization
QueueHandle_t entryRFIDQueue;
QueueHandle_t exitRFIDQueue;
QueueHandle_t gateQueue;
QueueHandle_t lcdQueue;

// Message structure used to send text to the LCD task
typedef struct {
    char message[32];   // LCD message buffer
} LCDMessage_t;

// Different types of RFID cards supported by the system
typedef enum {
    RFID_UNKNOWN = 0,   // Card is not recognized
    RFID_NORMAL,        // Normal vehicle card
    RFID_EMERGENCY      // Emergency/police vehicle card
} RFIDType_t;

// Commands that can be sent to the barrier task
typedef enum {
    GATE_CMD_NONE = 0,      // No gate action
    GATE_CMD_OPEN_BARRIER   // Open the servo barrier
} GateCommandType_t;

// Message structure used for gate commands
typedef struct {
    GateCommandType_t cmd;  // Command to be executed by the barrier task
} GateCommand_t;

// Stores information about a vehicle currently inside the parking lot
typedef struct {
    uint32_t tag_id;        // RFID tag ID of the vehicle
    RFIDType_t type;        // Type of vehicle/card
    TickType_t entry_tick;  // Time when the vehicle entered
    bool active;            // True if this parking session is currently in use
} ParkingSession_t;

// global variables
volatile int  g_availableSpaces   = 2;
atomic_bool g_emergencyMode  = false;
atomic_bool g_emergencyHandled = false;
volatile bool g_policeRFIDScanned = false; // TM1 sets when police tag scanned

static ParkingSession_t g_sessions[MAX_SPACES + 1]; // +1 for an emergency vehicle

// LCD handle
static lcd_i2c_t lcd;

// ultrasonic handles
ultrasonic_sensor_t us_sensor1;
ultrasonic_sensor_t us_sensor2;

// RFID handles
static rc522_handle_t rfid_scanner;

// Function Definitions
// LEd and LCD helper functions
static void rgb_ledc_init(void);
static void rgb_set_color(uint8_t r, uint8_t g, uint8_t b);
static void plain_leds_init(void);
static void lcd_init(void);

// RFID Helper functions
static void on_picc_state_changed(void *arg, esp_event_base_t base, int32_t event_id, void *event_data);
static void rfid_init(void);
static RFIDType_t get_rfid_type(uint32_t tag_id);

// Parking Management Helper Functions
static void route_scanned_tag(uint32_t tag_id);
static int find_vehicle_session(uint32_t tag_id); // Find vehicle session
static bool is_vehicle_inside(uint32_t tag_id); // Check if vehicle is already inside
static bool add_vehicle_session(uint32_t tag_id, RFIDType_t type, TickType_t entry_tick); // Add vehicle session
static bool remove_vehicle_session(uint32_t tag_id); // Remove vehicle session

// Servo helper functions
static void servo_init(void);
static uint32_t servo_angle_to_duty(uint32_t angle);
static void servo_set_angle(ledc_channel_t channel, uint32_t angle);

// Ultrasonic helper function
static void ultrasonic_sensor_init(void);

// Tasks
void vLCDDisplayTask(void *pvParameters);
void vLEDIndicatorTask(void *pvParameters);
void vEmergencyResponseTask(void *pvParameters);
void vEntryAccessControlTask(void *pvParameters);
void vExitAccessControlTask(void *pvParameters);
void vBarrierControlTask(void *pvParameters);
void AnomalyDetectionTask(void *pvParameters);

//Function Definitions

// LED and LCD Helper Functions
// RGB LED initialization
static void rgb_ledc_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_RESOLUTION,
        .freq_hz         = LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_cfg);

    ledc_channel_config_t ch = {
        .speed_mode = LEDC_MODE,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ch.channel = RGB_R_CHANNEL; ch.gpio_num = RGB_R_PIN; ledc_channel_config(&ch);
    ch.channel = RGB_G_CHANNEL; ch.gpio_num = RGB_G_PIN; ledc_channel_config(&ch);
    ch.channel = RGB_B_CHANNEL; ch.gpio_num = RGB_B_PIN; ledc_channel_config(&ch);
}

//RGB LED color setting helper
static void rgb_set_color(uint8_t r, uint8_t g, uint8_t b)
{
    ledc_set_duty(LEDC_MODE, RGB_R_CHANNEL, r);
    ledc_update_duty(LEDC_MODE, RGB_R_CHANNEL);
    ledc_set_duty(LEDC_MODE, RGB_G_CHANNEL, g);
    ledc_update_duty(LEDC_MODE, RGB_G_CHANNEL);
    ledc_set_duty(LEDC_MODE, RGB_B_CHANNEL, b);
    ledc_update_duty(LEDC_MODE, RGB_B_CHANNEL);
}

// Normal LEDs initialization
static void plain_leds_init(void)
{
    gpio_config_t io_cfg = {
        .pin_bit_mask = (1ULL << LED1_PIN) | (1ULL << LED2_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_cfg);
    gpio_set_level(LED1_PIN, 1); // ON = slot free
    gpio_set_level(LED2_PIN, 1);
}

// LCD initialization
static void lcd_init(void)
{
    // Configuration for the LCD
    lcd_i2c_config_t lcd_cfg = {
        .i2c_port = LCD2_I2C_PORT,
        .address  = LCD2_ADDR,     // Compiler suggested 'address'
        .sda_gpio = LCD2_SDA_PIN,  // Compiler confirmed 'sda_gpio'
        .scl_gpio = LCD2_SCL_PIN,  // Compiler confirmed 'scl_gpio'
        .clk_speed_hz = 100000,
        .rows = 2,  
        .cols = 16, 
    };
    // Explicitly set backlight if the struct supports it
    // If you get an error on this line, just remove it.
    lcd_cfg.backlight = 1; 
    
    lcd_i2c_init(&lcd, &lcd_cfg);
    lcd_i2c_clear(&lcd);
    lcd_i2c_set_cursor(&lcd, 0, 0);
    lcd_i2c_write_str(&lcd, "  PARKING SYS   ");
    lcd_i2c_set_cursor(&lcd, 0, 1);
    lcd_i2c_write_str(&lcd, "  Welcome    ");
}

// RFID Helper Functions
static void on_picc_state_changed(void *arg, esp_event_base_t base,
    int32_t event_id, void *event_data)
{
rc522_event_data_t *data = (rc522_event_data_t *)event_data;
rc522_tag_t *tag = (rc522_tag_t *)data->ptr;

uint32_t tag_id = (uint32_t)tag->serial_number;
ESP_LOGI(TAG, "RFID scanned: 0x%08lX", (unsigned long)tag_id);
route_scanned_tag(tag_id);
}

static RFIDType_t get_rfid_type(uint32_t tag_id)
{
    switch (tag_id)
    {
        case 0x00040301: // Blue Card
        case 0x00443311: // Green Card
        case 0x00CC7755: // Yellow Card
            return RFID_NORMAL;

        case 0x0000CCAA: // Red Card
            return RFID_EMERGENCY;

        default:
            return RFID_UNKNOWN;
    }
}

static void rfid_init(void)
{
    rc522_config_t config = {
        .spi.host        = SPI3_HOST,
        .spi.miso_gpio   = RFID_SPI_MISO_GPIO,
        .spi.mosi_gpio   = RFID_SPI_MOSI_GPIO,
        .spi.sck_gpio    = RFID_SPI_SCK_GPIO,
        .spi.sda_gpio    = RFID_SPI_SDA_GPIO,
    };

    ESP_ERROR_CHECK(rc522_create(&config, &rfid_scanner));
    ESP_ERROR_CHECK(rc522_register_events(rfid_scanner,
        RC522_EVENT_ANY, on_picc_state_changed, NULL));
    ESP_ERROR_CHECK(rc522_start(rfid_scanner));
}

// Servo Helper Functions
static void servo_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = SERVO_LEDC_MODE,
        .timer_num       = SERVO_LEDC_TIMER,
        .duty_resolution = SERVO_LEDC_DUTY_RES,
        .freq_hz         = SERVO_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer_cfg);

    ledc_channel_config_t barrier_cfg = {
        .gpio_num   = BARRIER_SERVO_PIN,
        .speed_mode = SERVO_LEDC_MODE,
        .channel    = BARRIER_SERVO_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = SERVO_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&barrier_cfg);
}

static uint32_t servo_angle_to_duty(uint32_t angle)
{
    uint32_t min_duty = 205;
    uint32_t max_duty = 1024;

    if (angle > 180) angle = 180;

    return min_duty + ((max_duty - min_duty) * angle) / 180;
}

static void servo_set_angle(ledc_channel_t channel, uint32_t angle)
{
    uint32_t duty = servo_angle_to_duty(angle);
    ledc_set_duty(SERVO_LEDC_MODE, channel, duty);
    ledc_update_duty(SERVO_LEDC_MODE, channel);
}

// Ultrasonic Helper Function
static void ultrasonic_sensor_init(void){
    us_sensor1.trigger_pin = ultrasonic1_trig;
    us_sensor1.echo_pin = ultrasonic1_echo;

    us_sensor2.trigger_pin = ultrasonic2_trig;
    us_sensor2.echo_pin = ultrasonic2_echo;
    
    ultrasonic_init(&us_sensor1);
    ultrasonic_init(&us_sensor2);
}

// Parking Management Helper Functions
static int find_vehicle_session(uint32_t tag_id)
{
    for (int i = 0; i < MAX_SPACES + 1; i++)
    {
        // Look for an active session with the same RFID tag
        if (g_sessions[i].active && g_sessions[i].tag_id == tag_id)
        {
            return i;
        }
    }
    return -1;  // Vehicle was not found
}

static bool is_vehicle_inside(uint32_t tag_id)
{
    return (find_vehicle_session(tag_id) >= 0);
}

static bool add_vehicle_session(uint32_t tag_id, RFIDType_t type, TickType_t entry_tick)
{
    // Emergency vehicles are allowed one extra slot
    int limit = (type == RFID_EMERGENCY) ? MAX_SPACES + 1 : MAX_SPACES;

    for (int i = 0; i < limit; i++)
    {
        // Store the vehicle in the first free session slot
        if (!g_sessions[i].active)
        {
            g_sessions[i].tag_id = tag_id;
            g_sessions[i].type = type;
            g_sessions[i].entry_tick = entry_tick;
            g_sessions[i].active = true;
            return true;
        }
    }
    return false;  // No free session slot available
}

static bool remove_vehicle_session(uint32_t tag_id)
{
    int idx = find_vehicle_session(tag_id);

    if (idx < 0)
    {
        return false;
    }

    // Clear the session after the vehicle exits
    g_sessions[idx].tag_id = 0;
    g_sessions[idx].type = RFID_UNKNOWN;
    g_sessions[idx].entry_tick = 0;
    g_sessions[idx].active = false;
    return true;
}

static void route_scanned_tag(uint32_t tag_id)
{
    bool alreadyInside = false;

    // Lock parking data while checking the vehicle state
    if (xSemaphoreTake(parkingMutex, pdMS_TO_TICKS(200)) != pdTRUE)
    {
        ESP_LOGW(TAG, "RFID routing failed: parkingMutex busy");
        return;
    }

    alreadyInside = is_vehicle_inside(tag_id);

    if (alreadyInside)
    {
        // If the vehicle is already inside, treat the scan as an exit
        if (xQueueSend(exitRFIDQueue, &tag_id, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            ESP_LOGW(TAG, "exitRFIDQueue full");
        }
    }
    else
    {
        // If the vehicle is not inside, treat the scan as an entry
        if (xQueueSend(entryRFIDQueue, &tag_id, pdMS_TO_TICKS(100)) != pdTRUE)
        {
            ESP_LOGW(TAG, "entryRFIDQueue full");
        }
    }
    xSemaphoreGive(parkingMutex);
}

// Tasks
// ─── Task 1: LCD Display Task ─────────────────────────────────────────────────
// Reads LCDMessage_t from lcdQueue and displays it on the LCD
// Message format: "line1|line2" 
void vLCDDisplayTask(void *pvParameters)
{
    LCDMessage_t msg;

    for (;;) //Inifinte loop to keep the task running
    {
        // Wait indefinitely (portMAX_DELAY) until a message arrives in the lcdQueue.
        // The task sleeps here and consumes no CPU time while waiting.
        if (xQueueReceive(lcdQueue, &msg, portMAX_DELAY) == pdTRUE)
        {

            char line1[17] = {0};
            char line2[17] = {0};

            // Parse line1 and line2 separated by '|'
            char *body = msg.message;
            char *sep  = strchr(body, '|');
            if (sep)
            {
                int len = sep - body;
                if (len > 16) len = 16;
                strncpy(line1, body, len);
                line1[len] = '\0';
                strncpy(line2, sep + 1, 16);
                line2[16] = '\0';
            }
            else
            {
                strncpy(line1, body, 16);
                line1[16] = '\0';
            }

            // display on LCD
            if (xSemaphoreTake(lcdMutex, pdMS_TO_TICKS(200)) == pdTRUE)
            {
                lcd_i2c_clear(&lcd);
                vTaskDelay(pdMS_TO_TICKS(5));
                lcd_i2c_set_cursor(&lcd, 0, 0);
                lcd_i2c_write_str(&lcd, line1);
                lcd_i2c_set_cursor(&lcd, 0, 1);
                lcd_i2c_write_str(&lcd, line2);
                xSemaphoreGive(lcdMutex);
            }
        }
    }
}

// ─── Task 2: LED Indicator and Alert Task ────────────────────────────────────
// Normal mode: plain LEDs reflect slot availability, RGB shows green/red.
// Emergency mode: RGB flashes red and blue until g_emergencyMode clears.
// Triggered by notifyLEDsBinSem (normal) or flashLEDBinSem (emergency).
void vLEDIndicatorTask(void *pvParameters)
{
    for (;;) // Infinite loop to keep the task running
    {
        // Check the global emergency flag. If a crash/alert was detected, this will be true.
        if (g_emergencyMode)
        {
            // Trap the task in this loop as long as the emergency is active.
            // This pauses all normal parking lot LED updates.
            while (g_emergencyMode)
            {
                rgb_set_color(255, 0, 0);   // Set RGB LED to full Red

                // Block the task for 300ms. This allows other RTOS tasks to run 
                // rather than stalling the CPU while waiting to change colors.
                vTaskDelay(pdMS_TO_TICKS(300));


                rgb_set_color(0, 0, 255);   // Set RGB LED to full Blue
                vTaskDelay(pdMS_TO_TICKS(300)); // Block for another 300ms
            }

            /* Once the emergency clears (flag becomes false), we exit the while loop.
            We artificially give the normal notification semaphore here.
            This forces the code below to immediately evaluate and restore the 
            proper normal state (Green/Red) instead of waiting for the next car. */
            
            xSemaphoreGive(notifyLEDsBinSem);
        }

        // Drain flashLEDBinSem non-blocking (emergency mode flag already set by crash task)
        xSemaphoreTake(flashLEDBinSem, 0);

        // Wait up to 200 ticks (approx 200ms) for a notification that the parking state changed.
        // If a car enters/leaves, another task "gives" this semaphore.
        if (xSemaphoreTake(notifyLEDsBinSem, portMAX_DELAY) == pdTRUE)
        {
            if (atomic_load(&g_emergencyMode)) continue;

            // Read the current number of available spaces from the global variable
            int spaces = g_availableSpaces;

            // Update standard indicator LEDs using ternary operators (condition ? true : false)
            gpio_set_level(LED1_PIN, (spaces >= 1) ? 1 : 0);    // If spaces >= 1, turn on LED1. Otherwise, turn it off.
            gpio_set_level(LED2_PIN, (spaces >= 2) ? 1 : 0);    // If spaces >= 2, turn on LED2. Otherwise, turn it off.

            // Update the main RGB entry status light
            if (spaces > 0)
                rgb_set_color(0, 200, 0);   // green = spaces available
            else
                rgb_set_color(200, 0, 0);   // red = lot full

            vTaskDelay(pdMS_TO_TICKS(2000));  // let ACCESS GRANTED show for 2 seconds

            LCDMessage_t statusMsg;
            if (spaces > 0){
                snprintf(statusMsg.message, sizeof(statusMsg.message),
                        "SLOTS FREE: %d|SCAN TO ENTER", (uint8_t)spaces);
            }
            else {
                snprintf(statusMsg.message, sizeof(statusMsg.message),
                        "SLOTS FREE: 0|PARKING FULL");
            }
            
            xQueueSend(lcdQueue, &statusMsg, pdMS_TO_TICKS(100));
        }
    }
}

// ─── Task 3: Emergency Response Task ─────────────────────────────────────────
// Waits on accidentDetectedBinSem. On trigger:
//   - Sets g_emergencyMode (LED task picks this up)
//   - Displays warning on both LCDs
//   - Grants instant entry if g_policeRFIDScanned is set
//   - Stays in alert until crash signal stops, then clears emergency mode
void vEmergencyResponseTask(void *pvParameters)
{
    LCDMessage_t msg;

    for (;;) // Infinite loop to keep the task alive in the background
    {
        /* The task halts here indefinitely (portMAX_DELAY) until a crash/sensor task 
        detects an accident and "gives" the accidentDetectedBinSem semaphore. */
        xSemaphoreTake(accidentDetectedBinSem, portMAX_DELAY);

        // Turn on the global emergency flag.
        atomic_store(&g_emergencyMode, true);
        xSemaphoreGive(notifyLEDsBinSem);
        ESP_LOGW(TAG, "EMERGENCY DETECTED - System in alert mode");

        // Warn on LCD
        snprintf(msg.message, sizeof(msg.message), "! EMERGENCY !|NO ACCESS-ALERT");
        xQueueSend(lcdQueue, &msg, pdMS_TO_TICKS(100));

        /* This loop acts as an "Emergency Watchdog". It tries to take the accident semaphore again, but only waits 3 seconds (3000ms).
         If the sensor is STILL detecting a crash, it will give the semaphore, the task takes it, prints the log, 
         checks for police vehicles, opens gate if detected, and restarts the 3-second timer. If 3 seconds pass 
         and NO signal is received, xSemaphoreTake fails (returns pdFALSE), meaning the crash is cleared, 
         and the while loop naturally breaks.*/
        bool policeEntered = false;
        bool lcdNeedsEmergencyMsg = true;
        while (xSemaphoreTake(accidentDetectedBinSem, pdMS_TO_TICKS(3000)) == pdTRUE)
        {
            ESP_LOGW(TAG, "Emergency still active...");

            if (lcdNeedsEmergencyMsg)  // only show if not handled yet
            {
                snprintf(msg.message, sizeof(msg.message), "! EMERGENCY !|NO ACCESS-ALERT");
                xQueueSend(lcdQueue, &msg, pdMS_TO_TICKS(100));
                lcdNeedsEmergencyMsg = false;
            }

            if (xSemaphoreTake(parkingMutex, pdMS_TO_TICKS(200)) == pdTRUE)
            {
                if (g_policeRFIDScanned)
                {
                    ESP_LOGI(TAG, "police Card Scanned");
                    if (policeEntered){ // this runs when police scans again; to exit
                        ESP_LOGI(TAG, "Police Left");
                        g_policeRFIDScanned = false;
                        policeEntered = false;
                        xSemaphoreGive(parkingMutex);

                        ESP_LOGI(TAG, "Emergency RFID - exit granted");

                        atomic_store(&g_emergencyHandled, true);

                        ESP_LOGI(TAG, "Police Left - Emergency Handled");
                    }
                    else { // this runs when police scans for first time; to enter
                        g_policeRFIDScanned = false;
                        policeEntered = true;
                        xSemaphoreGive(parkingMutex);

                        ESP_LOGI(TAG, "Emergency RFID - instant entry granted");
                        lcdNeedsEmergencyMsg = true; 
                        }
                }
                else
                {
                    xSemaphoreGive(parkingMutex);
                }
            }
            else {
                ESP_LOGW(TAG, "Could not take parking mutex");
            }
        }

        // Clear emergency (i.e) 3 seconds have passed with no accident signal. Turn off the flags.
        if (xSemaphoreTake(parkingMutex, pdMS_TO_TICKS(200)) == pdTRUE){
            g_policeRFIDScanned = false;
            xSemaphoreGive(parkingMutex);
        }
        atomic_store(&g_emergencyMode, false);
        ESP_LOGI(TAG, "Emergency cleared - resuming normal operations");

        // Send "SYSTEM NORMAL" messages to both Entry and Exit LCDs.
        snprintf(msg.message, sizeof(msg.message), "SYSTEM NORMAL | Access Allowed");
        xQueueSend(lcdQueue, &msg, pdMS_TO_TICKS(100));

        // Inform the LED task to update the plain and RGB LEDs back to their normal space-availability states (Green/Red based on parking slots).
        xSemaphoreGive(notifyLEDsBinSem);
    }
}

// Task 4: Entry access control task
// This task receives RFID tags that are treated as entry attempts.
// It decides whether the vehicle is allowed to enter and then updates the system.
void vEntryAccessControlTask(void *pvParameters)
{
    uint32_t scannedTag;        // Stores the RFID tag received from the queue
    LCDMessage_t lcdMsg;        // Message that will be sent to the LCD task
    GateCommand_t gateCmd;      // Command that will be sent to the barrier task

    for (;;)
    {
        // Wait here until a scanned RFID tag is sent to the entry queue
        if (xQueueReceive(entryRFIDQueue, &scannedTag, portMAX_DELAY) == pdTRUE)
        {
            RFIDType_t tagType;        // Stores whether the tag is normal, emergency, or unknown
            bool allowEntry = false;   // Final decision for allowing entry
            bool alreadyInside = false;
            bool lotFull = false;
            bool emergencyMode = false;
            bool sessionAdded = false; // Used in case we need to undo an added session later

            // Check what type of RFID card was scanned
            tagType = get_rfid_type(scannedTag);

            // Take the parking mutex before reading or modifying shared parking data
            if (xSemaphoreTake(parkingMutex, pdMS_TO_TICKS(200)) != pdTRUE)
            {
                snprintf(lcdMsg.message, sizeof(lcdMsg.message),
                         "SYSTEM BUSY|TRY AGAIN");

                // Inform the user that the system is busy
                xQueueSend(lcdQueue, &lcdMsg, pdMS_TO_TICKS(100));

                ESP_LOGW(TAG, "Entry task could not take parkingMutex within 200 ms");
                continue;
            }

            // Check the current parking status while the mutex is held
            alreadyInside = is_vehicle_inside(scannedTag);
            lotFull = (g_availableSpaces <= 0);
            emergencyMode = atomic_load(&g_emergencyMode);

            // Unknown RFID cards are not allowed
            if (tagType == RFID_UNKNOWN)
            {
                allowEntry = false;
            }

            // A vehicle already inside should not be allowed to enter again
            else if (alreadyInside)
            {
                allowEntry = false;
            }

            // During emergency mode, only emergency vehicles are allowed to enter
            else if (emergencyMode)
            {
                if (tagType == RFID_EMERGENCY)
                {
                    allowEntry = true;
                    g_policeRFIDScanned = true;   // Mark that emergency/police RFID was scanned
                }
            }

            // Normal system operation
            else
            {
                // Normal cars can enter only if the lot is not full
                if ((tagType == RFID_NORMAL) && !lotFull)
                {
                    allowEntry = true;
                }

                // Emergency vehicles are allowed even if the lot is full
                else if (tagType == RFID_EMERGENCY)
                {
                    allowEntry = true;
                }
            }

            if (allowEntry)
            {
                // Add a new active parking session and store the entry time
                if (add_vehicle_session(scannedTag, tagType, xTaskGetTickCount()))
                {
                    sessionAdded = true;

                    // Only normal cars reduce the available parking spaces
                    if ((tagType == RFID_NORMAL) && (g_availableSpaces > 0))
                    {
                        g_availableSpaces--;
                    }

                    if (tagType == RFID_EMERGENCY)
                    {
                        ESP_LOGI(TAG, "Police car session added");
                    }
                }
                else
                {
                    // If there was no space in the session array, entry cannot continue
                    allowEntry = false;
                }
            }

            // Release the parking mutex after finishing parking-data checks/updates
            xSemaphoreGive(parkingMutex);

            if (allowEntry)
            {
                gateCmd.cmd = GATE_CMD_OPEN_BARRIER;

                // Ask the barrier task to open the servo gate
                if (xQueueSend(gateQueue, &gateCmd, pdMS_TO_TICKS(100)) != pdTRUE)
                {
                    ESP_LOGE(TAG, "Failed to send barrier command within 100 ms");

                    // If the gate command failed, undo the parking session that was added earlier
                    if (sessionAdded)
                    {
                        if (xSemaphoreTake(parkingMutex, pdMS_TO_TICKS(200)) == pdTRUE)
                        {
                            remove_vehicle_session(scannedTag);

                            // Restore the available space if this was a normal vehicle
                            if (tagType == RFID_NORMAL && g_availableSpaces < MAX_SPACES)
                            {
                                g_availableSpaces++;
                            }

                            xSemaphoreGive(parkingMutex);
                        }
                    }

                    snprintf(lcdMsg.message, sizeof(lcdMsg.message),
                             "GATE BUSY|SCAN AGAIN");

                    xQueueSend(lcdQueue, &lcdMsg, pdMS_TO_TICKS(100));
                    continue;
                }

                // Prepare the correct LCD message for successful entry
                if (tagType == RFID_EMERGENCY)
                {
                    snprintf(lcdMsg.message, sizeof(lcdMsg.message),
                             "EMERG ACCESS|ENTRY OK");
                }
                else
                {
                    snprintf(lcdMsg.message, sizeof(lcdMsg.message),
                             "ACCESS GRANTED|WELCOME");
                }

                // Send success message to the LCD task
                xQueueSend(lcdQueue, &lcdMsg, pdMS_TO_TICKS(100));

                // Notify the LED task to update the parking indicators
                xSemaphoreGive(notifyLEDsBinSem);

                ESP_LOGI(TAG, "Entry approved for tag: 0x%08lX",
                         (unsigned long)scannedTag);
            }
            else
            {
                // Prepare an LCD message explaining why entry was denied
                if (tagType == RFID_UNKNOWN)
                {
                    snprintf(lcdMsg.message, sizeof(lcdMsg.message),
                             "ACCESS DENIED|UNKNOWN TAG");
                }
                else if (alreadyInside)
                {
                    snprintf(lcdMsg.message, sizeof(lcdMsg.message),
                             "DUPLICATE TAG|ALREADY INSIDE");
                }
                else if (emergencyMode && tagType != RFID_EMERGENCY)
                {
                    snprintf(lcdMsg.message, sizeof(lcdMsg.message),
                             "EMERGENCY|NO ENTRY");
                }
                else if (lotFull && tagType == RFID_NORMAL)
                {
                    snprintf(lcdMsg.message, sizeof(lcdMsg.message),
                             "NO PARKING|DENIED");
                }
                else
                {
                    snprintf(lcdMsg.message, sizeof(lcdMsg.message),
                             "ACCESS DENIED|TRY AGAIN");
                }

                // Send denial message to the LCD task
                xQueueSend(lcdQueue, &lcdMsg, pdMS_TO_TICKS(100));

                ESP_LOGW(TAG, "Entry denied for tag: 0x%08lX",
                         (unsigned long)scannedTag);
            }
        }
    }
}

// Task 5: Exit Access Control task
// This task handles RFID exit scans, frees parking slots,
// calculates parked duration/cost, and opens the barrier if exit is allowed.
void vExitAccessControlTask(void *pvParameters)
{
    uint32_t scannedTag;        // RFID tag received from the exit queue
    LCDMessage_t lcdMsg;        // Message to send to the LCD task
    GateCommand_t gateCmd;      // Command to send to the barrier task

    for (;;)
    {
        // Wait until a scanned RFID tag is routed to the exit queue
        if (xQueueReceive(exitRFIDQueue, &scannedTag, portMAX_DELAY) == pdTRUE)
        {
            int sessionIndex = -1;             // Index of the vehicle session in the session array
            TickType_t currentTick;            // Current FreeRTOS tick count
            TickType_t parkedTicks = 0;        // Total parked time in ticks
            uint32_t parkedSeconds = 0;        // Total parked time converted to seconds
            uint32_t cost = 0;                 // Parking cost

            bool allowExit = false;            // Final decision for allowing exit
            bool emergencyMode = false;        // Stores whether emergency mode is active
            bool sessionRemoved = false;       // Used in case we need to undo removal later
            RFIDType_t tagType = RFID_UNKNOWN; // Stores the RFID type of the exiting vehicle
            TickType_t originalEntryTick = 0;  // Entry time saved when the vehicle entered

            // Get current time so we can calculate how long the car stayed
            currentTick = xTaskGetTickCount();

            // Lock parking data before checking or modifying sessions
            if (xSemaphoreTake(parkingMutex, pdMS_TO_TICKS(200)) != pdTRUE)
            {
                snprintf(lcdMsg.message, sizeof(lcdMsg.message),
                         "SYSTEM BUSY|TRY AGAIN");

                xQueueSend(lcdQueue, &lcdMsg, pdMS_TO_TICKS(100));
                ESP_LOGW(TAG, "Exit task could not take parkingMutex within 200 ms");
                continue;
            }

            // Check current emergency state and find the vehicle session
            emergencyMode = atomic_load(&g_emergencyMode);
            sessionIndex = find_vehicle_session(scannedTag);

            if (sessionIndex < 0)
            {
                // If there is no active session, this vehicle was not recorded as inside
                allowExit = false;
            }
            else
            {
                // Read stored session details before removing the vehicle
                tagType = g_sessions[sessionIndex].type;
                originalEntryTick = g_sessions[sessionIndex].entry_tick;

                // During emergency mode, normal vehicles are not allowed to leave
                if (emergencyMode && tagType != RFID_EMERGENCY)
                {
                    allowExit = false;
                }

                // Emergency/police vehicle is allowed to exit during emergency mode
                else if (emergencyMode && tagType == RFID_EMERGENCY)
                {
                    allowExit = true;

                    // Remove the emergency vehicle session
                    if (remove_vehicle_session(scannedTag))
                    {
                        sessionRemoved = true;
                    }
                    else
                    {
                        allowExit = false;
                    }

                    // Mark that the emergency/police RFID was scanned
                    g_policeRFIDScanned = true;

                    // Release mutex here because this branch sends LCD message early
                    xSemaphoreGive(parkingMutex);

                    snprintf(lcdMsg.message, sizeof(lcdMsg.message),
                             "EMERGENCY MODE|EXIT OK");
                    xQueueSend(lcdQueue, &lcdMsg, pdMS_TO_TICKS(100));
                }
                else
                {
                    allowExit = true;

                    // Calculate parked duration in ticks
                    parkedTicks = currentTick - originalEntryTick;

                    // Convert ticks to seconds using the FreeRTOS tick rate
                    uint32_t tickRate = (uint32_t)configTICK_RATE_HZ;
                    if (tickRate == 0) tickRate = 1000; // fallback: assume 1000 Hz

                    parkedSeconds = parkedTicks / tickRate;

                    // Simple cost formula: 1 cost unit per 10 seconds
                    cost = (parkedSeconds > 0) ? (parkedSeconds / 10) : 0;

                    // Remove the vehicle session after exit is approved
                    if (remove_vehicle_session(scannedTag))
                    {
                        sessionRemoved = true;

                        // Normal vehicles free one parking space when they leave
                        if (tagType == RFID_NORMAL && g_availableSpaces < MAX_SPACES)
                        {
                            g_availableSpaces++;
                        }
                    }
                    else
                    {
                        allowExit = false;
                    }
                }
            }

            // Release parking data after checks and updates are complete
            xSemaphoreGive(parkingMutex);

            if (allowExit)
            {
                gateCmd.cmd = GATE_CMD_OPEN_BARRIER;

                // Ask the barrier task to open the gate
                if (xQueueSend(gateQueue, &gateCmd, pdMS_TO_TICKS(100)) != pdTRUE)
                {
                    ESP_LOGE(TAG, "Failed to send barrier command within 100 ms");

                    // If the barrier command failed, restore the removed session
                    if (sessionRemoved)
                    {
                        if (xSemaphoreTake(parkingMutex, pdMS_TO_TICKS(200)) == pdTRUE)
                        {
                            if (add_vehicle_session(scannedTag, tagType, originalEntryTick))
                            {
                                // Undo the space increase for normal vehicles
                                if (tagType == RFID_NORMAL && g_availableSpaces > 0)
                                {
                                    g_availableSpaces--;
                                }
                            }

                            xSemaphoreGive(parkingMutex);
                        }
                    }

                    snprintf(lcdMsg.message, sizeof(lcdMsg.message),
                             "GATE BUSY|SCAN AGAIN");
                    xQueueSend(lcdQueue, &lcdMsg, pdMS_TO_TICKS(100));
                    continue;
                }

                // For normal vehicles, show duration and cost on the LCD
                if (tagType != RFID_EMERGENCY)
                {
                    snprintf(lcdMsg.message, sizeof(lcdMsg.message),
                         "T:%lus C:%lu|EXIT OK",
                         (unsigned long)parkedSeconds,
                         (unsigned long)cost);

                    xQueueSend(lcdQueue, &lcdMsg, pdMS_TO_TICKS(100));

                    // Notify LEDs because available spaces changed
                    xSemaphoreGive(notifyLEDsBinSem);

                    ESP_LOGI(TAG, "Exit approved for tag: 0x%08lX, duration=%lus, cost=%lu",
                            (unsigned long)scannedTag,
                            (unsigned long)parkedSeconds,
                            (unsigned long)cost);
                }

            }
            else
            {
                // Choose the correct LCD message based on why exit was denied
                if (sessionIndex < 0)
                {
                    snprintf(lcdMsg.message, sizeof(lcdMsg.message),
                             "EXIT DENIED|TAG NOT FOUND");
                }
                else if (emergencyMode && tagType != RFID_EMERGENCY)
                {
                    snprintf(lcdMsg.message, sizeof(lcdMsg.message),
                             "EMERGENCY MODE|NO NORMAL EXIT");
                }
                else
                {
                    snprintf(lcdMsg.message, sizeof(lcdMsg.message),
                             "EXIT DENIED|TRY AGAIN");
                }

                xQueueSend(lcdQueue, &lcdMsg, pdMS_TO_TICKS(100));

                ESP_LOGW(TAG, "Exit denied for tag: 0x%08lX",
                         (unsigned long)scannedTag);
            }
        }
    }
}

// Task 6: Barrier Control Task
// This task controls the servo motor used to open and close the parking barrier.
void vBarrierControlTask(void *pvParameters)
{
    GateCommand_t gateCmd;   // Stores the gate command received from the queue

    for (;;)
    {
        // Wait until another task sends a command to the gate queue
        if (xQueueReceive(gateQueue, &gateCmd, portMAX_DELAY) == pdTRUE)
        {
            switch (gateCmd.cmd)
            {
                case GATE_CMD_OPEN_BARRIER:
                {
                    // Take the barrier mutex so only one task can control the servo at a time
                    if (xSemaphoreTake(barrierMutex, pdMS_TO_TICKS(200)) == pdTRUE)
                    {
                        ESP_LOGI(TAG, "Opening barrier");

                        // Move servo to open position
                        servo_set_angle(BARRIER_SERVO_CHANNEL, 90);

                        // Keep barrier open for 3 seconds
                        vTaskDelay(pdMS_TO_TICKS(3000));

                        // Move servo back to closed position
                        servo_set_angle(BARRIER_SERVO_CHANNEL, 0);

                        // Small delay to let the servo finish closing
                        vTaskDelay(pdMS_TO_TICKS(500));

                        // Release the barrier so another command can use it later
                        xSemaphoreGive(barrierMutex);
                    }
                    else
                    {
                        ESP_LOGW(TAG, "Could not take barrierMutex within 200 ms");
                    }

                    break;
                }

                default:
                {
                    // Handles invalid or unexpected gate commands
                    ESP_LOGW(TAG, "Unknown gate command received");
                    break;
                }
            }
        }
    }
}

// Task 7: Anomaly Detection Task -> Detects anomaly in parking slots; if there is sudden movement in any slot
void AnomalyDetectionTask(void *pvParameters){

    uint32_t distance_sen1 = 0; // variable to store distance reading of sensor 1
    uint32_t distance_sen2 = 0; // variable to store distance reading of sensor 2

    // variables to store previous reading
    uint32_t prevDistance1 = 0;
    uint32_t prevDistance2 = 0;

    TickType_t crashTimer = 0; // timer to keep track of duration

    // flags to detect anomaly
    bool slot1Anomaly = false;
    bool slot2Anomaly = false;
    bool anomalyDetected = false;

    // flags to know if a car is in the slot
    bool wasParked1 = false;
    bool wasParked2 = false;
    bool firstReading = true; // flag so we can skip checks in first iteration so prev values can be initialized properly

    // infinite loop so task always comes in waiting queue of FreeRTOS scheduler
    while(true){
        // take reading
        esp_err_t res1 = ultrasonic_measure_cm(&us_sensor1, ultrasonic_maxDistance, &distance_sen1);
        esp_err_t res2 = ultrasonic_measure_cm(&us_sensor2, ultrasonic_maxDistance, &distance_sen2);

        // if measurement gives error, skip iteration
        if(res1 != ESP_OK || res2 != ESP_OK){
            ESP_LOGW(TAG, "Error in Ultrasonic measurement");
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        if (firstReading){ // set previous values and skip as if they are 0, and there is a car parked already, anomaly will be falsely detected
            prevDistance1 = distance_sen1;
            prevDistance2 = distance_sen2;
            firstReading = false;
            continue;
        }

        // when parked, both sensors should measure 5 cm. If this value increases or decreases suddenly and stays there,
        // we detect an anamoly.

        if ((distance_sen1 >= 5 && distance_sen1 <= 10)){ // car is parked in slot 1
            wasParked1 = true;
        }

        else if (distance_sen1 > 80){  // car left slot1
            wasParked1 = false;
        }

        if (distance_sen2 >= 5 && distance_sen2 <= 10){ // car is parked in slot 2
            wasParked2 = true;
        }

        else if (distance_sen2 > 80){  // car left slot2
            wasParked2 = false;
        }

        // set flag to true if anomaly persists for 2 seconds
        slot1Anomaly = wasParked1 && abs((int)distance_sen1 - (int)prevDistance1) >= SUDDEN_CHANGE_THRESHOLD;
        slot2Anomaly = wasParked2 && abs((int)distance_sen2 - (int)prevDistance2) >= SUDDEN_CHANGE_THRESHOLD;

        // detect anamoly in slot 1 or slot 2
        if (slot1Anomaly || slot2Anomaly){
            if (!anomalyDetected){
                anomalyDetected = true;
                crashTimer = xTaskGetTickCount(); // start timer
                ESP_LOGI(TAG, "Sudden movement in Parking");
            }
            else if (atomic_load(&g_emergencyHandled)) { // police car left, set prev distance trakers to current distance and reset flags
                anomalyDetected = false;
                prevDistance1 = distance_sen1;
                prevDistance2 = distance_sen2;
                atomic_store(&g_emergencyHandled, false);
            }
            else if ((xTaskGetTickCount() - crashTimer) >= pdMS_TO_TICKS(TIME_TO_DEFINE_ANOMALY)){ // if time threshold exceeded
                ESP_LOGI(TAG, "Anamoly detcted; Crash or Accident in parking");
                atomic_store(&g_emergencyHandled, false);
                xSemaphoreGive(accidentDetectedBinSem); // give semaphore to emergency response task
                xSemaphoreGive(flashLEDBinSem); // give semaphore to led indicator task

            }
        }
        else { // no anomaly is detected
            if (anomalyDetected) { // previous anomaly was resolved
                ESP_LOGI(TAG, "Distance returned to normal.");
            }
            anomalyDetected = false; // reset flag
            
            // only update baseline previous distances when things are normal
            prevDistance1 = distance_sen1;
            prevDistance2 = distance_sen2;
        }

        vTaskDelay(pdMS_TO_TICKS(500));
    }

}

void app_main() {

    ESP_LOGI(TAG, "Initializing FreeRTOS synchronization objects...");

    accidentDetectedBinSem = xSemaphoreCreateBinary();
    flashLEDBinSem         = xSemaphoreCreateBinary();
    notifyLEDsBinSem       = xSemaphoreCreateBinary();

    entryRFIDQueue = xQueueCreate(10, sizeof(uint32_t));
    exitRFIDQueue  = xQueueCreate(10, sizeof(uint32_t));
    gateQueue      = xQueueCreate(10, sizeof(GateCommand_t));
    lcdQueue       = xQueueCreate(10, sizeof(LCDMessage_t));

    lcdMutex  = xSemaphoreCreateMutex();
    parkingMutex   = xSemaphoreCreateMutex();
    barrierMutex = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "Initializing RGB LED");
    rgb_ledc_init();
    ESP_LOGI(TAG, "RGB LED Initialized");

    ESP_LOGI(TAG, "Initializing Plain LEDs");
    plain_leds_init();
    ESP_LOGI(TAG, "Plain LEDs initialized");
    
    ESP_LOGI(TAG, "Initializing LCD");
    lcd_init();
    ESP_LOGI(TAG, "LCd Initialized");

    ESP_LOGI(TAG, "Initializing RFID");
    rfid_init();

    //esp_log_level_set("rc522", ESP_LOG_NONE);
    
    ESP_LOGI(TAG, "RFID Initialized");

    ESP_LOGI(TAG, "Initalizing Servo");
    servo_init();
    ESP_LOGI(TAG, "Servo Initialized");

    //esp_log_level_set("Project", ESP_LOG_WARN);

    ESP_LOGI(TAG, "Initializing Ultrasonics");
    ultrasonic_sensor_init();
    ESP_LOGI(TAG, "Ultrasonics Initialized");

    xSemaphoreGive(notifyLEDsBinSem);

    xTaskCreate(vEntryAccessControlTask, "ENTRY", 4096, NULL, 5, NULL); // medium priority
    xTaskCreate(vExitAccessControlTask, "EXIT", 4096, NULL, 5, NULL); // medium priority
    xTaskCreate(vBarrierControlTask, "BARRIER", 3072, NULL, 4, NULL); // medium - low priority
    
    xTaskCreate(vLCDDisplayTask,       "LCD",  4096, NULL, 3, NULL); // low priority
    xTaskCreate(vLEDIndicatorTask,     "LED",  2048, NULL, 3, NULL); // low priority
    xTaskCreate(vEmergencyResponseTask,"EMRG", 2048, NULL, 6, NULL); // second highest priority

    xTaskCreate(AnomalyDetectionTask, "ANAMOLY", 3072, NULL, 7, NULL); // high priority
}