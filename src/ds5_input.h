#pragma once

#include <cstdint>

enum PowerState : uint8_t {
    Discharging         = 0x00, // Use PowerPercent
    Charging            = 0x01, // Use PowerPercent
    Complete            = 0x02, // PowerPercent not valid? assume 100%?
    AbnormalVoltage     = 0x0A, // PowerPercent not valid?
    AbnormalTemperature = 0x0B, // PowerPercent not valid?
    ChargingError       = 0x0F  // PowerPercent not valid?
};

enum Direction : uint8_t {
    North = 0,
    NorthEast,
    East,
    SouthEast,
    South,
    SouthWest,
    West,
    NorthWest,
    None = 8
};

struct __attribute__((packed)) TouchFingerData { // 4
    /*0.0*/ uint32_t Index : 7;
    /*0.7*/ uint32_t NotTouching : 1;
    /*1.0*/ uint32_t FingerX : 12;
    /*2.4*/ uint32_t FingerY : 12;
};

struct __attribute__((packed)) TouchData { // 9
    /*0*/ TouchFingerData Finger[2];
    /*8*/ uint8_t Timestamp;
};

struct __attribute__((packed)) USBGetStateData { // 63
/* 0  */ uint8_t LeftStickX;
/* 1  */ uint8_t LeftStickY;
/* 2  */ uint8_t RightStickX;
/* 3  */ uint8_t RightStickY;
/* 4  */ uint8_t TriggerLeft;
/* 5  */ uint8_t TriggerRight;
/* 6  */ uint8_t SeqNo; // always 0x01 on BT
/* 7.0*/ Direction DPad : 4;
/* 7.4*/ uint8_t ButtonSquare : 1;
/* 7.5*/ uint8_t ButtonCross : 1;
/* 7.6*/ uint8_t ButtonCircle : 1;
/* 7.7*/ uint8_t ButtonTriangle : 1;
/* 8.0*/ uint8_t ButtonL1 : 1;
/* 8.1*/ uint8_t ButtonR1 : 1;
/* 8.2*/ uint8_t ButtonL2 : 1;
/* 8.3*/ uint8_t ButtonR2 : 1;
/* 8.4*/ uint8_t ButtonCreate : 1;
/* 8.5*/ uint8_t ButtonOptions : 1;
/* 8.6*/ uint8_t ButtonL3 : 1;
/* 8.7*/ uint8_t ButtonR3 : 1;
/* 9.0*/ uint8_t ButtonHome : 1;
/* 9.1*/ uint8_t ButtonPad : 1;
/* 9.2*/ uint8_t ButtonMute : 1;
/* 9.3*/ uint8_t UNK1 : 1; // appears unused
/* 9.4*/ uint8_t ButtonLeftFunction : 1; // DualSense Edge
/* 9.5*/ uint8_t ButtonRightFunction : 1; // DualSense Edge
/* 9.6*/ uint8_t ButtonLeftPaddle : 1; // DualSense Edge
/* 9.7*/ uint8_t ButtonRightPaddle : 1; // DualSense Edge
/*10  */ uint8_t UNK2; // appears unused
/*11  */ uint32_t UNK_COUNTER; // Linux driver calls this reserved, tools leak calls the 2 high bytes "random"
/*15  */ int16_t AngularVelocityX;
/*17  */ int16_t AngularVelocityZ;
/*19  */ int16_t AngularVelocityY;
/*21  */ int16_t AccelerometerX;
/*23  */ int16_t AccelerometerY;
/*25  */ int16_t AccelerometerZ;
/*27  */ uint32_t SensorTimestamp;
/*31  */ int8_t Temperature; // reserved2 in Linux driver
/*32  */ TouchData Touch;
/*41.0*/ uint8_t TriggerRightStopLocation: 4; // trigger stop can be a range from 0 to 9 (F/9.0 for Apple interface)
/*41.4*/ uint8_t TriggerRightStatus: 4;
/*42.0*/ uint8_t TriggerLeftStopLocation: 4;
/*42.4*/ uint8_t TriggerLeftStatus: 4; // 0 feedbackNoLoad
                                       // 1 feedbackLoadApplied
                                       // 0 weaponReady
                                       // 1 weaponFiring
                                       // 2 weaponFired
                                       // 0 vibrationNotVibrating
                                       // 1 vibrationIsVibrating
/*43  */ uint32_t HostTimestamp; // mirrors data from report write
/*47.0*/ uint8_t TriggerRightEffect: 4; // Active trigger effect, previously we thought this was status max
/*47.4*/ uint8_t TriggerLeftEffect: 4;  // 0 for reset and all other effects
                                        // 1 for feedback effect
                                        // 2 for weapon effect
                                        // 3 for vibration
/*48  */ uint32_t DeviceTimeStamp;
/*52.0*/ uint8_t PowerPercent : 4; // 0x00-0x0A
/*52.4*/ PowerState Power : 4;
/*53.0*/ uint8_t PluggedHeadphones : 1;
/*53.1*/ uint8_t PluggedMic : 1;
/*53.2*/ uint8_t MicMuted: 1; // Mic muted by powersave/mute command
/*53.3*/ uint8_t PluggedUsbData : 1;
/*53.4*/ uint8_t PluggedUsbPower : 1; // appears that this cannot be 1 if PluggedUsbData is 1
/*53.5*/ uint8_t UsbPowerOnBT : 1; // appears this is only 1 if BT connected and USB powered
/*53.5*/ uint8_t DockDetect : 1;
/*53.5*/ uint8_t PluggedUnk : 1;
/*54.0*/ uint8_t PluggedExternalMic : 1; // Is external mic active (automatic in mic auto mode)
/*54.1*/ uint8_t HapticLowPassFilter : 1; // Is the Haptic Low-Pass-Filter active?
/*54.2*/ uint8_t PluggedUnk3 : 6;
/*55  */ uint8_t AesCmac[8];
};

static_assert(sizeof(USBGetStateData) == 63);
