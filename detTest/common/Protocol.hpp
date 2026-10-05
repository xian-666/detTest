#pragma once
#include <cstdint>
#include <cstring>

// 无符号类型别名
using u8 = uint8_t;

// 扳机模式枚举
enum class TriggerMode : u8 {
    Off = 0x00,
    Race = 0x01,
    Gun = 0x02,
    Shot = 0x03
};

// 肩键效果类型 (对应GeneralAssistant)
enum class ShoulderEffectType : u8 {
    Null = 0x00,
    Fast = 0x01,
    Soft = 0x02,
    Heavy = 0x03
};

// 肩键效果强度
enum class ShoulderEffectStrength : u8 {
    Low = 0x00,
    Medium = 0x01,
    Strong = 0x02
};

// 肩键方向
enum class ShoulderOrientation : u8 {
    LeftShoulder = 0x00,
    RightShoulder = 0x01,
    LeftFollowShoulder = 0x02,
    RightFollowShoulder = 0x03,
    LeftShoulderMotor = 0x04,
    RightShoulderMotor = 0x05,
	CustorLeftShoulder = 0x06,
	CustorRightShoulder = 0x07
};

// 1. 赛车模式参数
#pragma pack(push, 1)
typedef struct {
    u8 start;
    u8 streng;
    u8 padding[5];
} mt_race_param_t;

// 2. 枪械模式参数 (机枪)
typedef struct {
    u8 start;
    u8 streng;
    u8 start_streng;
    u8 freq;
    u8 padding[3];
} mt_gun_param_t;

// 3. 射击模式参数 (霰弹/狙击)
typedef struct {
    u8 start;
    u8 streng;
    u8 range;
    u8 padding[4];
} mt_shot_param_t;

// 4. 肩键效果参数 (协议 0x76) - 与LT/RT相同64字节格式
typedef struct {
    u8 vibrationTime;        // 每步震动时间
    u8 firstVibrationTime;   // 首次振动时间
    u8 vibrationStep;        // 有效振动步数
    u8 vibrationStrength1;   // 强度1
    u8 vibrationStrength2;   // 强度2
    u8 vibrationStrength3;   // 强度3
    u8 padding[57];          // 填充到60字节
} mt_shoulder_param_t;

// 命令结构体 (64字节 HID包)
struct TriggerCommand {
    u8 report_id;      // 0x02
    u8 cmd_type;       // 0x56 for trigger, 0x76 for shoulder
    u8 trigger_idx;    // 0x00 Left, 0x01 Right
    u8 mode;           // TriggerMode (for 0x56) or ShoulderEffectType (for 0x76)

    union {
        mt_race_param_t race;
        mt_gun_param_t gun;
        mt_shot_param_t shot;
        mt_shoulder_param_t shoulder;
        u8 raw_params[60];
    } params;

    TriggerCommand() {
        memset(this, 0, sizeof(TriggerCommand));
        report_id = 0x02;
        cmd_type = 0x56;
        trigger_idx = 0xff;
        mode = 0xff;
    }

    // 重载 == 运算符用于去重比较
    bool operator==(const TriggerCommand& other) const {
        return report_id == other.report_id &&
            cmd_type == other.cmd_type &&
            trigger_idx == other.trigger_idx &&
            mode == other.mode &&
            memcmp(&params, &other.params, sizeof(params)) == 0;
    }

    bool operator!=(const TriggerCommand& other) const {
        return !(*this == other);
    }
};
#pragma pack(pop)