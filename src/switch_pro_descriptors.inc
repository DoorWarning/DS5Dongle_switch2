uint8_t const desc_hid_report_switch_pro[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop Controls)
    0x15, 0x00,       // Logical Minimum (0)
    0x09, 0x04,       // Usage (Joystick)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x30,       //   Report ID (48)
    0x05, 0x01,       //   Usage Page (Generic Desktop Controls)
    0x05, 0x09,       //   Usage Page (Button)
    0x19, 0x01,       //   Usage Minimum (1)
    0x29, 0x0A,       //   Usage Maximum (10)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x0A,       //   Report Count (10)
    0x55, 0x00,       //   Unit Exponent (0)
    0x65, 0x00,       //   Unit (None)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x05, 0x09,       //   Usage Page (Button)
    0x19, 0x0B,       //   Usage Minimum (11)
    0x29, 0x0E,       //   Usage Maximum (14)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x04,       //   Report Count (4)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x02,       //   Report Count (2)
    0x81, 0x03,       //   Input (Const,Var,Abs)
    0x0B, 0x01, 0x00, 0x01, 0x00,
    0xA1, 0x00,       //   Collection (Physical)
    0x0B, 0x30, 0x00, 0x01, 0x00,
    0x0B, 0x31, 0x00, 0x01, 0x00,
    0x0B, 0x32, 0x00, 0x01, 0x00,
    0x0B, 0x35, 0x00, 0x01, 0x00,
    0x15, 0x00,       //     Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00,
    0x75, 0x10,       //     Report Size (16)
    0x95, 0x04,       //     Report Count (4)
    0x81, 0x02,       //     Input (Data,Var,Abs)
    0xC0,             //   End Collection
    0x0B, 0x39, 0x00, 0x01, 0x00,
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x07,       //   Logical Maximum (7)
    0x35, 0x00,       //   Physical Minimum (0)
    0x46, 0x3B, 0x01, //   Physical Maximum (315)
    0x65, 0x14,       //   Unit (Eng Rot:Angular Pos)
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x05, 0x09,       //   Usage Page (Button)
    0x19, 0x0F,       //   Usage Minimum (15)
    0x29, 0x12,       //   Usage Maximum (18)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x04,       //   Report Count (4)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x34,       //   Report Count (52)
    0x81, 0x03,       //   Input (Const,Var,Abs)
    0x06, 0x00, 0xFF, //   Usage Page (Vendor Specific)
    0x85, 0x21,       //   Report ID (33)
    0x09, 0x01,       //   Usage (1)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x3F,       //   Report Count (63)
    0x81, 0x03,       //   Input (Const,Var,Abs)
    0x85, 0x81,       //   Report ID (129)
    0x09, 0x02,       //   Usage (2)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x3F,       //   Report Count (63)
    0x81, 0x03,       //   Input (Const,Var,Abs)
    0x85, 0x01,       //   Report ID (1)
    0x09, 0x03,       //   Usage (3)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x3F,       //   Report Count (63)
    0x91, 0x83,       //   Output (Const,Var,Abs,Volatile)
    0x85, 0x10,       //   Report ID (16)
    0x09, 0x04,       //   Usage (4)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x3F,       //   Report Count (63)
    0x91, 0x83,       //   Output (Const,Var,Abs,Volatile)
    0x85, 0x80,       //   Report ID (128)
    0x09, 0x05,       //   Usage (5)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x3F,       //   Report Count (63)
    0x91, 0x83,       //   Output (Const,Var,Abs,Volatile)
    0x85, 0x82,       //   Report ID (130)
    0x09, 0x06,       //   Usage (6)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x3F,       //   Report Count (63)
    0x91, 0x83,       //   Output (Const,Var,Abs,Volatile)
    0x85, 0xF6,       //   Report ID (246)
    0x09, 0x07,       //   Usage (7)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x3F,       //   Report Count (63)
    0xB1, 0x02,       //   Feature (Data,Var,Abs)
    0x85, 0xF7,       //   Report ID (247)
    0x09, 0x08,       //   Usage (8)
    0xB1, 0x02,       //   Feature (Data,Var,Abs)
    0xC0,             // End Collection
};

static_assert(sizeof(desc_hid_report_switch_pro) == 219);

uint8_t descriptor_configuration_switch_pro[] = {
    0x09, // bLength
    0x02, // bDescriptorType (CONFIGURATION)
    0x29, 0x00, // wTotalLength: 41
    0x01, // bNumInterfaces
    0x01, // bConfigurationValue
    0x00, // iConfiguration
    0xA0, // bmAttributes: bus powered, remote wakeup
    0xFA, // bMaxPower: 500mA

    0x09, // bLength
    0x04, // bDescriptorType (INTERFACE)
    0x00, // bInterfaceNumber
    0x00, // bAlternateSetting
    0x02, // bNumEndpoints
    0x03, // bInterfaceClass: HID
    0x00, // bInterfaceSubClass
    0x00, // bInterfaceProtocol
    0x00, // iInterface

    0x09, // bLength
    0x21, // bDescriptorType (HID)
    0x11, 0x01, // bcdHID
    0x00, // bCountryCode
    0x01, // bNumDescriptors
    0x22, // bDescriptorType: Report
    static_cast<uint8_t>(sizeof(desc_hid_report_switch_pro) & 0xFF),
    static_cast<uint8_t>((sizeof(desc_hid_report_switch_pro) >> 8) & 0xFF),

    0x07, // bLength
    0x05, // bDescriptorType (ENDPOINT)
    0x81, // bEndpointAddress: IN EP1
    0x03, // bmAttributes: Interrupt
    0x40, 0x00, // wMaxPacketSize
    0x08, // bInterval

    0x07, // bLength
    0x05, // bDescriptorType (ENDPOINT)
    0x01, // bEndpointAddress: OUT EP1
    0x03, // bmAttributes: Interrupt
    0x40, 0x00, // wMaxPacketSize
    0x08, // bInterval
};

static_assert(sizeof(descriptor_configuration_switch_pro) == 0x29);
