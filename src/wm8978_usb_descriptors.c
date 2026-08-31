#include <string.h>
#include "bsp/board_api.h"
#include "tusb.h"

#define USB_PID 0x4010
#define ITF_CDC 0
#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT 0x02
#define EPNUM_CDC_IN 0x82
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static tusb_desc_device_t const device_desc = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON, .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE, .idVendor = 0xcafe,
    .idProduct = USB_PID, .bcdDevice = 0x0100, .iManufacturer = 1,
    .iProduct = 2, .iSerialNumber = 3, .bNumConfigurations = 1,
};

static uint8_t const config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, CONFIG_TOTAL_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(ITF_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT,
                       EPNUM_CDC_IN, 64),
};

static char const *strings[] = { (const char[]){0x09, 0x04}, "Pico",
                                 "Pico WM8978 NAM Control", NULL };
static uint16_t desc_str[32];

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&device_desc;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return config_desc;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    if (index == 0) {
        desc_str[1] = 0x0409;
        desc_str[0] = (TUSB_DESC_STRING << 8) | 4;
        return desc_str;
    }
    if (index >= sizeof(strings) / sizeof(strings[0]) || !strings[index]) return NULL;
    const char *s = strings[index];
    uint8_t n = (uint8_t)strlen(s);
    if (n > 31) n = 31;
    for (uint8_t i = 0; i < n; ++i) desc_str[1 + i] = s[i];
    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
    return desc_str;
}
