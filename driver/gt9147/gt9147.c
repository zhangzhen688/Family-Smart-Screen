/**
 * @file    gt9147.c
 * @brief   Goodix GT9147 5点电容触摸屏 I2C 内核驱动
 *
 * =========================== 硬件概述 ===========================
 * GT9147 是汇顶科技(GOODiX)的5点电容触摸控制器，广泛用于嵌入式设备。
 * 兼容芯片: GT1151, GT1158, GT5663, GT5688 (通过固件版本自动识别)
 *
 * 特性:
 *   - 最多 5 点同时触摸
 *   - I2C 通信接口 (寄存器地址为2字节)
 *   - 独立的 RESET 和 INT 引脚
 *   - 自动上报触摸坐标 (通过 INT 引脚触发中断)
 *
 * =========================== 通信协议 ===========================
 *
 * I2C 寄存器映像:
 *   0x8040  控制寄存器 (写0x02=软复位)
 *   0x8047  配置起始地址 (9xx系列)
 *   0x804D  模式切换寄存器
 *   0x8050  配置起始地址 (1xx系列)
 *   0x80FF  校验和寄存器
 *   0x8140  产品ID寄存器 (6字节: 4字节ID + 2字节版本号)
 *   0x814E  触摸状态寄存器 (bit[3:0]=触摸点数, bit7=有数据标志)
 *   0x814F  触摸点1数据 (5字节: track_id, x_low, x_high, y_low, y_high)
 *   0x8157  触摸点2数据
 *   0x815F  触摸点3数据
 *   0x8167  触摸点4数据
 *   0x816F  触摸点5数据
 *
 * =========================== 硬件时序 ===========================
 *
 * GT9147 上电初始化流程 (必须严格遵循):
 *   1. RESET 拉低 ≥10ms     → 芯片复位
 *   2. RESET 拉高 ≥10ms     → 退出复位
 *   3. INT   拉低 ≥50ms     → 进入正常模式 (否则芯片在休眠)
 *   4. INT   设为输入       → 释放 INT 线，等待中断
 *
 * INT 引脚行为:
 *   - 有触摸数据: INT 产生上升沿 (触发中断)
 *   - 中断处理中: 读状态寄存器(0x814E) → 处理触摸数据 → 写0清状态
 *
 * =========================== 驱动架构 ===========================
 *
 * - 注册为 I2C 设备驱动
 * - 通过 Linux Input 子系统上报多点触摸事件 (ABS_MT)
 * - 使用 devm_request_threaded_irq 注册中断 (线程化, 不阻塞)
 * - 读取固件获取 max_x / max_y / 中断触发类型
 *
 * =========================== 移植说明 (昇腾310B) ===========================
 * 原平台: NXP i.MX (Alientek), Linux 4.1.15
 * 新平台: 华为昇腾310B, Linux 5.10+
 *
 * 适配项:
 *   1. 设备树 compatible 改为 "goodix,gt9147" (与主线内核一致)
 *   2. I2C 总线编号变化 → 确认昇腾上 LCD 连接的 I2C 控制器
 *   3. GPIO 引脚重新映射: interrupt-gpios + reset-gpios
 *   4. 中断触发类型由固件决定，不要硬编码
 *   5. max_x/max_y 从固件读取后可能与实际屏幕不匹配 → 需要在应用层做坐标映射
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/gpio/consumer.h>
#include <linux/of_irq.h>
#include <linux/of_gpio.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <asm/unaligned.h>

/* ── 模块元信息 ──────────────────────────────────────────────────────── */
#define DRV_NAME        "gt9147"
#define DRV_VERSION     "1.1.0"

/* ── GT9147 寄存器地址 ──────────────────────────────────────────────── */
#define GT_CTRL_REG         0x8040  /* 控制寄存器 (软复位)             */
#define GT_CFGS_9xx_REG     0x8047  /* 配置起始地址 (9xx系列)          */
#define GT_MODSW_REG        0x804D  /* 模式切换寄存器                  */
#define GT_CFGS_1xx_REG     0x8050  /* 配置起始地址 (1xx系列)          */
#define GT_CHECK_REG        0x80FF  /* 校验和寄存器                    */
#define GT_PID_REG          0x8140  /* 产品ID寄存器 (6字节)            */
#define GT_GSTID_REG        0x814E  /* 触摸状态寄存器                  */

/* 5个触摸点数据寄存器 */
#define GT_TP1_REG          0x814F
#define GT_TP2_REG          0x8157
#define GT_TP3_REG          0x815F
#define GT_TP4_REG          0x8167
#define GT_TP5_REG          0x816F

/* 每个触摸点数据: 5字节 (id, x_low, x_high, y_low, y_high) */
#define GT_TP_DATA_LEN      5

/* ── 硬件参数 ───────────────────────────────────────────────────────── */
#define MAX_SUPPORT_POINTS   5       /* GT9147 最多支持 5 点触摸       */
#define GT_RESET_LOW_MS     10      /* 复位拉低时长 (ms)              */
#define GT_RESET_HIGH_MS    10      /* 复位拉高后等待 (ms)            */
#define GT_INT_LOW_MS       50      /* INT 拉低时长 (ms)              */
#define GT_SOFTRESET_WAIT_MS 100    /* 软复位等待 (ms)                */

/* ── 中断触发类型查找表 (索引由固件提供) ──────────────────────────────── */
static const u32 gt_irq_type_table[] = {
    IRQ_TYPE_EDGE_RISING,   /* 0: 上升沿触发  */
    IRQ_TYPE_EDGE_FALLING,  /* 1: 下降沿触发  */
    IRQ_TYPE_LEVEL_LOW,     /* 2: 低电平触发  */
    IRQ_TYPE_LEVEL_HIGH,    /* 3: 高电平触发  */
};

/**
 * struct gt9147_dev — GT9147 设备私有数据
 *
 * 所有设备状态集中在一个结构体中，通过 probe 时分配并通过
 * i2c_set_clientdata 绑定到 i2c_client，中断处理函数通过
 * dev_id 参数找回该结构体。
 */
struct gt9147_dev {
    int             irq_pin;        /* 中断 GPIO 引脚编号           */
    int             reset_pin;      /* 复位 GPIO 引脚编号           */
    int             irqnum;         /* 中断号                       */
    int             irqtype;        /* 中断触发类型 (索引)          */
    int             max_x;          /* 触摸屏最大 X 坐标 (从固件读) */
    int             max_y;          /* 触摸屏最大 Y 坐标 (从固件读) */
    struct input_dev *input;        /* Linux Input 子系统设备       */
    struct i2c_client *client;      /* I2C 客户端指针               */
};

static struct gt9147_dev g_gt9147;

/**
 * gt9147_read_regs — 从 GT9147 寄存器批量读取数据
 *
 * GT9147 使用 2 字节寄存器地址 (大端序)。
 * 与标准 SMBus 不同，需要先发送 2 字节地址再读取数据。
 * 因此必须使用 i2c_transfer + i2c_msg 的底层接口。
 *
 * I2C 事务:
 *   Msg0: [START] [SLAVE_ADDR|W] [ADDR_HI] [ADDR_LO] [STOP]
 *   Msg1: [START] [SLAVE_ADDR|R] [DATA0]...[DATAn] [STOP]
 *
 * @param dev  设备结构体
 * @param reg  16位寄存器首地址
 * @param buf  输出缓冲区
 * @param len  读取长度
 * @return     0=成功, 负数=失败
 */
static int gt9147_read_regs(struct gt9147_dev *dev, u16 reg,
                              u8 *buf, int len)
{
    int ret;
    u8 reg_addr[2];
    struct i2c_msg msg[2];
    struct i2c_client *client = dev->client;

    /* 将16位寄存器地址拆为大端序的2字节 */
    reg_addr[0] = (reg >> 8) & 0xFF;
    reg_addr[1] =  reg       & 0xFF;

    /* msg[0]: 写 — 发送要读取的寄存器地址 */
    msg[0].addr  = client->addr;
    msg[0].flags = 0;              /* 0 = 写操作 */
    msg[0].buf   = reg_addr;
    msg[0].len   = 2;

    /* msg[1]: 读 — 接收数据 */
    msg[1].addr  = client->addr;
    msg[1].flags = I2C_M_RD;       /* 读操作 */
    msg[1].buf   = buf;
    msg[1].len   = len;

    ret = i2c_transfer(client->adapter, msg, 2);
    if (ret == 2) {
        return 0;  /* 两条消息都成功 */
    } else if (ret >= 0) {
        return -EREMOTEIO;  /* 部分成功 = 失败 */
    } else {
        return ret;         /* 负错误码 */
    }
}

/**
 * gt9147_write_regs — 向 GT9147 寄存器批量写入数据
 *
 * I2C 事务:
 *   [START] [SLAVE_ADDR|W] [ADDR_HI] [ADDR_LO] [DATA0]...[DATAn] [STOP]
 *
 * @param dev  设备结构体
 * @param reg  16位寄存器首地址
 * @param buf  数据缓冲区
 * @param len  写入长度
 * @return     传输的消息数量, 负数=失败
 */
static int gt9147_write_regs(struct gt9147_dev *dev, u16 reg,
                               u8 *buf, u8 len)
{
    u8 b[256];
    struct i2c_msg msg;
    struct i2c_client *client = dev->client;

    /*
     * 构建消息缓冲区: [ADDR_HI] [ADDR_LO] [DATA...]
     * 这样可以在一次 I2C 事务中完成 "发地址+写数据"
     */
    b[0] = (reg >> 8) & 0xFF;  /* 寄存器地址高字节 */
    b[1] =  reg       & 0xFF;  /* 寄存器地址低字节 */
    memcpy(&b[2], buf, len);    /* 要写入的数据     */

    msg.addr  = client->addr;
    msg.flags = 0;              /* 0 = 写操作 */
    msg.buf   = b;
    msg.len   = len + 2;        /* 地址2字节 + 数据len字节 */

    return i2c_transfer(client->adapter, &msg, 1);
}

/**
 * gt9147_reset — 硬件复位 GT9147
 *
 * 严格遵循 GT9147 数据手册的复位时序:
 *   RESET=0, 等待10ms    → 进入复位状态
 *   RESET=1, 等待10ms    → 退出复位，芯片初始化
 *   INT=0,   等待50ms    → 进入正常模式(否则芯片在睡眠)
 *   INT 设为输入         → 释放 INT 线，等待触摸中断
 *
 * 不按此顺序操作会导致芯片不工作或触摸数据异常。
 */
static int gt9147_reset(struct i2c_client *client, struct gt9147_dev *dev)
{
    int ret;

    /* 申请复位引脚，默认输出高电平 */
    if (gpio_is_valid(dev->reset_pin)) {
        ret = devm_gpio_request_one(&client->dev,
                                     dev->reset_pin,
                                     GPIOF_OUT_INIT_HIGH,
                                     "gt9147_reset");
        if (ret) {
            dev_err(&client->dev, "[gt9147] 申请复位引脚失败: %d\n", ret);
            return ret;
        }
    }

    /* 申请中断引脚，默认输出高电平 */
    if (gpio_is_valid(dev->irq_pin)) {
        ret = devm_gpio_request_one(&client->dev,
                                     dev->irq_pin,
                                     GPIOF_OUT_INIT_HIGH,
                                     "gt9147_int");
        if (ret) {
            dev_err(&client->dev, "[gt9147] 申请中断引脚失败: %d\n", ret);
            return ret;
        }
    }

    /* ── 硬件复位序列 ───────────────────────────────── */
    gpio_set_value(dev->reset_pin, 0);   /* RESET 拉低 */
    msleep(GT_RESET_LOW_MS);             /* 等待 10ms  */

    gpio_set_value(dev->reset_pin, 1);   /* RESET 拉高 */
    msleep(GT_RESET_HIGH_MS);            /* 等待 10ms  */

    gpio_set_value(dev->irq_pin, 0);     /* INT 拉低   */
    msleep(GT_INT_LOW_MS);               /* 等待 50ms  */

    gpio_direction_input(dev->irq_pin);  /* INT 设为输入 */

    dev_info(&client->dev, "[gt9147] 硬件复位完成\n");
    return 0;
}

/**
 * gt9147_read_firmware — 从芯片读取固件配置信息
 *
 * 流程:
 *   1. 读取产品ID (0x8140, 6字节) → 识别芯片型号 (1151/1158/5663/5688/9xx)
 *   2. 根据型号读取对应配置区域 (1xx系列用0x8050, 9xx系列用0x8047)
 *   3. 解析配置: max_x, max_y, irq_type
 *
 * 配置数据布局 (在0x8047或0x8050开始的区域):
 *   [0]:    配置版本
 *   [1:2]:  X 分辨率 (小端序)
 *   [3:4]:  Y 分辨率 (小端序)
 *   [5]:    触摸点数
 *   [6]:    中断触发类型 (bit[1:0]: 0=上升沿, 1=下降沿, 2=低电平, 3=高电平)
 */
static int gt9147_read_firmware(struct i2c_client *client,
                                 struct gt9147_dev *dev)
{
    int ret;
    u8 data[7] = {0};
    u16 id;
    char id_str[5];

    /* 读取产品ID */
    ret = gt9147_read_regs(dev, GT_PID_REG, data, 6);
    if (ret) {
        dev_err(&client->dev, "[gt9147] 读取产品ID失败: %d\n", ret);
        return ret;
    }

    /* 解析产品ID字符串 (前4字节 ASCII) */
    memcpy(id_str, data, 4);
    id_str[4] = '\0';
    if (kstrtou16(id_str, 10, &id))
        id = 0x1001;  /* 解析失败，默认为 9xx 系列 */

    dev_info(&client->dev, "[gt9147] 芯片ID: %d, 固件版本: %04x\n",
             id, get_unaligned_le16(&data[4]));

    /*
     * 根据芯片型号选择配置寄存器起始地址:
     *   1xx系列 (1151/1158/5663/5688) → GT_CFGS_1xx_REG (0x8050)
     *   9xx系列 (默认)                → GT_CFGS_9xx_REG (0x8047)
     */
    switch (id) {
    case 1151:
    case 1158:
    case 5663:
    case 5688:
        ret = gt9147_read_regs(dev, GT_CFGS_1xx_REG, data, 7);
        break;
    default:
        ret = gt9147_read_regs(dev, GT_CFGS_9xx_REG, data, 7);
        break;
    }
    if (ret) {
        dev_err(&client->dev, "[gt9147] 读取配置失败: %d\n", ret);
        return ret;
    }

    /* 解析配置数据 */
    dev->max_x   = (data[2] << 8) | data[1];   /* X 分辨率 (小端) */
    dev->max_y   = (data[4] << 8) | data[3];   /* Y 分辨率 (小端) */
    dev->irqtype = data[6] & 0x03;             /* 中断触发类型     */

    dev_info(&client->dev,
             "[gt9147] 分辨率: %d x %d, 中断触发类型: %d\n",
             dev->max_x, dev->max_y, dev->irqtype);

    return 0;
}

/**
 * gt9147_irq_handler — 触摸中断处理函数 (线程化执行)
 *
 * GT9147 的中断处理流程:
 *   1. 读取状态寄存器 (0x814E) → 获取触摸点数
 *   2. 如果有触摸: 读取触摸点1数据 (5字节) → 上报给 Input 子系统
 *   3. 写入 0x00 到状态寄存器 → 清除中断标志 (否则不会产生下一次中断!)
 *
 * 当前实现: 单点触摸模式
 *   由于 GT9147 没有硬件检测每个触摸点的按下/抬起事件，
 *   多点触摸的正确处理非常复杂。本驱动暂时使用单点触摸上报。
 *   (多点触摸的正确实现需要 tracking ID 的状态机管理)
 */
static irqreturn_t gt9147_irq_handler(int irq, void *dev_id)
{
    struct gt9147_dev *dev = dev_id;
    u8 status;
    int ret;
    int input_x, input_y;
    int touch_id;

    /* Step 1: 读取触摸状态 */
    ret = gt9147_read_regs(dev, GT_GSTID_REG, &status, 1);
    if (ret || status == 0x00) {
        /* 无触摸数据, 或者是异常读取 */
        goto done;
    }

    /*
     * status & 0x0F = 触摸点数 (0~5)
     * status & 0x80 = 数据有效标志 (有些固件版本使用)
     */
    if (!(status & 0x0F))
        goto done;

    /*
     * Step 2: 读取触摸点1数据
     *   touch_data[0] = track_id (bit[3:0])
     *   touch_data[1] = X 坐标低8位
     *   touch_data[2] = X 坐标高8位
     *   touch_data[3] = Y 坐标低8位
     *   touch_data[4] = Y 坐标高8位
     */
    {
        u8 tp_data[GT_TP_DATA_LEN];
        ret = gt9147_read_regs(dev, GT_TP1_REG, tp_data, GT_TP_DATA_LEN);
        if (ret)
            goto clear_status;

        touch_id = tp_data[0] & 0x0F;
        input_x  = tp_data[1] | (tp_data[2] << 8);
        input_y  = tp_data[3] | (tp_data[4] << 8);

        /* 上报触摸按下事件 */
        input_mt_slot(dev->input, touch_id);
        input_mt_report_slot_state(dev->input, MT_TOOL_FINGER, true);
        input_report_abs(dev->input, ABS_MT_POSITION_X, input_x);
        input_report_abs(dev->input, ABS_MT_POSITION_Y, input_y);
        input_report_abs(dev->input, ABS_X, input_x);
        input_report_abs(dev->input, ABS_Y, input_y);
        input_report_key(dev->input, BTN_TOUCH, 1);
    }

clear_status:
    /*
     * Step 3: 清除中断状态 — 必须写0到状态寄存器!
     *   如果不清除，GT9147 不会产生下一次中断。
     */
    {
        u8 zero = 0x00;
        gt9147_write_regs(dev, GT_GSTID_REG, &zero, 1);
    }

    /* 上报同步事件 (一次完整的触摸帧结束) */
    input_sync(dev->input);

done:
    return IRQ_HANDLED;
}

/**
 * gt9147_probe — I2C 设备匹配后初始化
 *
 * 完整初始化流程:
 *   1. 从设备树获取 GPIO
 *   2. 硬件复位
 *   3. 软复位
 *   4. 读取固件配置 (分辨率 + 中断类型)
 *   5. 注册 Input 子系统设备 (多点触摸)
 *   6. 注册中断处理函数
 */
static int gt9147_probe(struct i2c_client *client,
                         const struct i2c_device_id *id)
{
    struct device *dev = &client->dev;
    u8 data;
    int ret;

    g_gt9147.client = client;

    /* ── Step 1: 从设备树获取 GPIO ──────────────────────────── */
    g_gt9147.irq_pin   = of_get_named_gpio(dev->of_node, "interrupt-gpios", 0);
    g_gt9147.reset_pin = of_get_named_gpio(dev->of_node, "reset-gpios", 0);

    dev_info(dev, "[gt9147] GPIO: reset=%d, int=%d\n",
             g_gt9147.reset_pin, g_gt9147.irq_pin);

    /* ── Step 2: 硬件复位 ──────────────────────────────────── */
    ret = gt9147_reset(client, &g_gt9147);
    if (ret) {
        dev_err(dev, "[gt9147] 硬件复位失败: %d\n", ret);
        return ret;
    }

    /* ── Step 3: 软复位 ────────────────────────────────────── */
    data = 0x02;
    gt9147_write_regs(&g_gt9147, GT_CTRL_REG, &data, 1); /* 触发软复位 */
    mdelay(GT_SOFTRESET_WAIT_MS);

    data = 0x00;
    gt9147_write_regs(&g_gt9147, GT_CTRL_REG, &data, 1); /* 停止软复位 */
    mdelay(GT_SOFTRESET_WAIT_MS);

    /* ── Step 4: 读取固件配置 ────────────────────────────────── */
    ret = gt9147_read_firmware(client, &g_gt9147);
    if (ret) {
        dev_err(dev, "[gt9147] 读取固件失败: %d\n", ret);
        return ret;
    }

    /* ── Step 5: 注册 Input 设备 ─────────────────────────────── */
    g_gt9147.input = devm_input_allocate_device(dev);
    if (!g_gt9147.input) {
        dev_err(dev, "[gt9147] 分配 input_dev 失败\n");
        return -ENOMEM;
    }

    g_gt9147.input->name          = "GT9147 Touchscreen";
    g_gt9147.input->id.bustype    = BUS_I2C;
    g_gt9147.input->dev.parent    = dev;

    /* 设置支持的事件类型 */
    __set_bit(EV_KEY, g_gt9147.input->evbit);
    __set_bit(EV_ABS, g_gt9147.input->evbit);
    __set_bit(BTN_TOUCH, g_gt9147.input->keybit);

    /* 设置绝对坐标参数 */
    input_set_abs_params(g_gt9147.input, ABS_X,
                         0, g_gt9147.max_x, 0, 0);
    input_set_abs_params(g_gt9147.input, ABS_Y,
                         0, g_gt9147.max_y, 0, 0);
    input_set_abs_params(g_gt9147.input, ABS_MT_POSITION_X,
                         0, g_gt9147.max_x, 0, 0);
    input_set_abs_params(g_gt9147.input, ABS_MT_POSITION_Y,
                         0, g_gt9147.max_y, 0, 0);

    /* 初始化多点触摸 slot */
    ret = input_mt_init_slots(g_gt9147.input, MAX_SUPPORT_POINTS,
                               INPUT_MT_DIRECT);
    if (ret) {
        dev_err(dev, "[gt9147] input_mt_init_slots 失败: %d\n", ret);
        return ret;
    }

    /* 注册 input 设备 */
    ret = input_register_device(g_gt9147.input);
    if (ret) {
        dev_err(dev, "[gt9147] input_register_device 失败: %d\n", ret);
        return ret;
    }

    /* ── Step 6: 注册中断 ────────────────────────────────────
     *
     * 使用 devm_request_threaded_irq (线程化中断):
     *   - hardirq handler 为 NULL (不需要快速处理)
     *   - thread handler 为 gt9147_irq_handler (在进程上下文中执行)
     *   这样可以安全地进行 I2C 通信 (I2C 操作可能睡眠)
     *
     * IRQF_ONESHOT: 保证中断处理期间该中断线保持禁用，防止重入
     */
    ret = devm_request_threaded_irq(
        dev,
        client->irq,
        NULL,                                     /* 无 hardirq handler */
        gt9147_irq_handler,                       /* 线程化 handler     */
        gt_irq_type_table[g_gt9147.irqtype] | IRQF_ONESHOT,
        client->name,
        &g_gt9147);
    if (ret) {
        dev_err(dev, "[gt9147] 注册中断失败: %d\n", ret);
        input_unregister_device(g_gt9147.input);
        return ret;
    }

    dev_info(dev, "[gt9147] probe 成功 v" DRV_VERSION
             " (分辨率: %dx%d)\n",
             g_gt9147.max_x, g_gt9147.max_y);
    return 0;
}

static int gt9147_remove(struct i2c_client *client)
{
    input_unregister_device(g_gt9147.input);
    dev_info(&client->dev, "[gt9147] 驱动已移除\n");
    return 0;
}

/* ── 设备树匹配表 ──────────────────────────────────────────────────── */
static const struct of_device_id gt9147_of_match[] = {
    { .compatible = "goodix,gt9147" },  /* 主线 Linux 兼容 (推荐)     */
    { .compatible = "goodix,gt1151" },  /* 兼容 GT1151                */
    { .compatible = "goodix,gt5688" },  /* 兼容 GT5688                */
    { /* 哨兵 */ }
};
MODULE_DEVICE_TABLE(of, gt9147_of_match);

/* ── 传统 I2C 设备 ID 表 ────────────────────────────────────────────── */
static const struct i2c_device_id gt9147_id_table[] = {
    { "gt9147", 0 },
    { "gt1151", 0 },
    { "gt5688", 0 },
    { /* 哨兵 */ }
};
MODULE_DEVICE_TABLE(i2c, gt9147_id_table);

/* ── I2C 驱动结构体 ─────────────────────────────────────────────────── */
static struct i2c_driver gt9147_i2c_driver = {
    .driver = {
        .name           = DRV_NAME,
        .owner          = THIS_MODULE,
        .of_match_table = gt9147_of_match,
    },
    .id_table = gt9147_id_table,
    .probe    = gt9147_probe,
    .remove   = gt9147_remove,
};

module_i2c_driver(gt9147_i2c_driver);

/* ── 模块许可证与描述 ────────────────────────────────────────────────── */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Smart Screen Team");
MODULE_DESCRIPTION("Goodix GT9147 5-Point Capacitive Touchscreen I2C Driver "
                   "(Ascend 310B compatible)");
MODULE_VERSION(DRV_VERSION);
