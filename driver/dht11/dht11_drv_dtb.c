/**
 * @file    dht11_drv_dtb.c
 * @brief   DHT11 数字温湿度传感器 GPIO 内核驱动
 *
 * =========================== 硬件概述 ===========================
 * DHT11 是一款低成本数字温湿度传感器，使用单总线(OneWire)协议。
 *   - 温度范围: 0~50°C, 精度 ±2°C
 *   - 湿度范围: 20~95%RH, 精度 ±5%RH
 *   - 采样周期: ≥1秒 (芯片内部限制)
 *
 * =========================== 单总线通信协议 ===========================
 *
 * DHT11 的 OneWire 不是达拉斯标准的 1-Wire，而是一个简化的自定义协议。
 * 完整的一次读取分为 4 个阶段:
 *
 *   ┌──────────────────────────────────────────────────────────────┐
 *   │ 主机发起始信号  │ DHT11响应 │  40位数据(5字节)  │  结束      │
 *   │  拉低≥18ms     │ 拉低80us  │ 每bit:50us低+     │ 主机拉高   │
 *   │  释放总线       │ 拉高80us  │ 26~70us高=0/1    │            │
 *   └──────────────────────────────────────────────────────────────┘
 *
 * 数据格式 (5字节，高位先出):
 *   Byte 0: 湿度整数部分 (例如: 45 表示 45%RH)
 *   Byte 1: 湿度小数部分 (DHT11 始终为 0)
 *   Byte 2: 温度整数部分 (例如: 25 表示 25°C)
 *   Byte 3: 温度小数部分 (DHT11 始终为 0)
 *   Byte 4: 校验和 = Byte0 + Byte1 + Byte2 + Byte3
 *
 * 位判定 (通过高电平持续时间区分):
 *   - 高电平 ≈ 26~28us → 逻辑 '0'
 *   - 高电平 ≈ 70us    → 逻辑 '1'
 *   我们取中间值约 50us 作为判定阈值。
 *
 * =========================== 时序关键点 ===========================
 *
 * DHT11 的单总线协议对时序要求非常严格 (微秒级)，因此:
 *   1. 使用 local_irq_save()/local_irq_restore() 在读取期间关本地中断
 *      防止内核调度/中断打断 bit-banging 时序导致数据错误
 *   2. 使用 udelay() 进行微秒级等待 (不是 mdelay!)
 *   3. 每个信号沿都用 gpio_get_value() 忙等轮询，确保精确捕捉
 *   4. 不依赖 GPIO 中断 (中断延迟不可控，会导致时序漂移)
 *
 * =========================== 移植说明 (昇腾310B) ===========================
 *
 * 关键风险: DHT11 的微秒级时序对 CPU 频率和中断延迟高度敏感。
 * 昇腾310B (Cortex-A55) 的 CPU 频率可能与原 i.MX 平台不同:
 *
 *   1. udelay() 的精度依赖于内核的 loops_per_jiffy 校准 → 通常无问题
 *   2. GPIO 读写的延迟 → 需实测验证，必要时调整等待微秒数
 *   3. 中断延迟: 如果 local_irq_save 仍不够，考虑迁移到 PREEMPT_RT 内核
 *
 * 备选方案: 如果昇騰310B上时序无法满足，可考虑:
 *   - 使用 DHT22 (更宽松的时序)
 *   - 外接 MCU (STM32/ESP32) 处理 OneWire，通过 UART/I2C 与主控通信
 *
 * 原平台: NXP i.MX (Alientek), Linux 4.1.15
 * 新平台: 华为昇腾310B (Ascend 310B), Linux 5.10+
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/interrupt.h>
#include <linux/irqreturn.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/timer.h>
#include <linux/delay.h>
#include <linux/irqflags.h>

/* ── 模块元信息 ──────────────────────────────────────────────────────── */
#define DRV_NAME        "dht11"
#define DRV_VERSION     "1.1.0"

/* ── DHT11 协议常量 ─────────────────────────────────────────────────── */
#define DHT11_START_LOW_MS      18    /* 主机起始信号: 拉低 ≥18ms        */
#define DHT11_RESP_WAIT_US      40    /* 等待 DHT11 响应: 40us           */
#define DHT11_RESP_HOLD_US      80    /* DHT11 响应脉冲宽度: 80us         */
#define DHT11_BIT_THRESHOLD_US  50    /* 位判定阈值: >50us高电平 → 1      */
#define DHT11_DATA_BYTES        5     /* 40位 = 5字节                     */
#define DHT11_BITS_PER_BYTE     8     /* 每个字节8位, MSB first           */

/* ── 全局变量 ────────────────────────────────────────────────────────── */
static int              g_major;          /* 动态主设备号                  */
static struct class    *g_class;          /* 设备类                       */
static struct device   *g_device;         /* 设备节点                     */
static int              g_dht11_gpio;     /* GPIO 引脚编号                */
static struct gpio_desc *g_dht11_desc;   /* GPIO 描述符                  */

/* ── 前向声明 ────────────────────────────────────────────────────────── */

/**
 * dht11_read_raw — 核心: 通过 GPIO bit-banging 读取 DHT11 40位数据
 *
 * 这是整个驱动最核心、最敏感的函数。
 * 调用者必须确保已进入临界区 (关本地中断)。
 *
 * 时序流程:
 *   Phase A: 主机发送起始信号
 *            GPIO → 输出模式, 拉低 18ms
 *            GPIO → 输入模式, 等待 DHT11 响应
 *
 *   Phase B: DHT11 响应
 *            等待 GPIO 变低 (DHT11 拉低 80us)
 *            等待 GPIO 变高 (DHT11 拉高 80us)
 *            如果在超时时间内未检测到 → 传感器不响应 → 返回错误
 *
 *   Phase C: 40位数据读取
 *            每位数据由 DHT11 产生:
 *              拉低 50us (起始) → 拉高 26~70us (数据位)
 *            代码流程:
 *              while (GPIO为低) ;  // 跳过50us低电平
 *              udelay(50);         // 等待到高电平中段 (~50us处)
 *              读 GPIO 电平 → 此时为高说明高电平>50us → '1'
 *                             此时为低说明高电平≤50us → '0'
 *              等待 GPIO 变低 (结束当前bit)
 *
 *   Phase D: 校验
 *            checksum = data[0]+data[1]+data[2]+data[3]
 *            与 data[4] 比较，不匹配则数据损坏
 *
 * @param data  输出缓冲区 (至少 5 字节)
 * @return      0=成功, -1=超时/校验失败
 */
static int dht11_read_raw(u8 data[DHT11_DATA_BYTES])
{
    int i, j;
    u8 checksum;

    if (!data)
        return -EINVAL;

    /* ── 清空数据缓冲区 ──────────────────────────────────────── */
    memset(data, 0, DHT11_DATA_BYTES);

    /*
     * Phase A: 主机发送起始信号
     *   拉低总线 ≥18ms，然后释放 (设为输入，由上拉电阻拉高)
     */
    gpio_direction_output(g_dht11_gpio, 0);
    mdelay(DHT11_START_LOW_MS);

    gpio_direction_input(g_dht11_gpio);

    /*
     * Phase B: 等待 DHT11 响应
     *   DHT11 检测到总线被释放后会:
     *     1) 拉低 80us  → gpio_get_value() == 0
     *     2) 拉高 80us  → gpio_get_value() == 1
     *
     *   我们需要在这两个状态转换时分别等待确认。
     *   这里使用了优化的单次 udelay 方案:
     *     先等待 40us，然后检查是否有响应脉冲
     *     再等待 80us，检查传感器是否完成响应
     */

    /* 等待 DHT11 拉低 (响应起始) */
    udelay(DHT11_RESP_WAIT_US);
    if (gpio_get_value(g_dht11_gpio)) {
        pr_err("[dht11] 传感器未响应 (拉低阶段): 总线一直为高\n");
        return -EIO;
    }

    /* 等待 DHT11 拉高 (响应结束) */
    udelay(DHT11_RESP_HOLD_US);
    if (!gpio_get_value(g_dht11_gpio)) {
        pr_err("[dht11] 传感器未响应 (拉高阶段): 总线一直为低\n");
        return -EIO;
    }

    /* 再等待 80us 确保进入数据阶段 */
    udelay(DHT11_RESP_HOLD_US);

    /*
     * Phase C: 读取 40 位数据 (5字节)
     *
     * 每位数据的读取方法 (关键!):
     *
     *          | 50us低 | 26~70us高 |
     *   GPIO   ─┐       └───────────┐
     *           │                   │
     *           ├─ 等低电平结束 ────┤
     *                              ├── 等50us ──┤ 读点
     *
     *   1. 等待当前bit的低电平结束 (while !gpio_get_value)
     *   2. 延时 50us，此时:
     *      - 如果该bit是 '0' (高电平26us)，此时 GPIO 已经回到低电平
     *      - 如果该bit是 '1' (高电平70us)，此时 GPIO 仍然为高
     *   3. 读取 GPIO 电平: 高 → bit=1, 低 → bit=0
     *   4. 如果 bit=1，等待高电平结束 (防止下一bit的 while 循环提前退出)
     */
    for (i = 0; i < DHT11_DATA_BYTES; i++) {
        for (j = 7; j >= 0; j--) {
            /* 等待当前 bit 的低电平结束 (DHT11 会先拉低 50us) */
            while (!gpio_get_value(g_dht11_gpio))
                cpu_relax();  /* 提示 CPU 我们在自旋等待 */

            /*
             * 延时到高电平中段 (~50us处):
             *   - 如果是 '0': 高电平只有 26us，此时已回到低
             *   - 如果是 '1': 高电平有 70us，此时仍为高
             */
            udelay(DHT11_BIT_THRESHOLD_US);

            /* 判决: 在高电平中段读取 GPIO */
            if (gpio_get_value(g_dht11_gpio)) {
                /* 高电平 > 50us → 该位是 1 */
                data[i] |= (1 << j);

                /* 等待高电平结束 (接下来 while 循环靠低电平结束触发) */
                while (gpio_get_value(g_dht11_gpio))
                    cpu_relax();
            }
            /*
             * else: 高电平 ≤ 50us → 该位是 0
             *   data[i] 对应位保持为 0 (memset 已清零)
             *   此时 GPIO 已经是低电平，下一轮 while 立即退出
             */
        }
    }

    /*
     * Phase D: 校验和验证
     *   校验失败时打印详细数据便于调试，但不阻塞返回
     *   (上层可决定是否重试)
     */
    checksum = data[0] + data[1] + data[2] + data[3];
    if (checksum != data[4]) {
        pr_err("[dht11] 校验和错误! "
               "湿度=%d.%d, 温度=%d.%d, "
               "计算校验=0x%02x, 期望校验=0x%02x\n",
               data[0], data[1], data[2], data[3],
               checksum, data[4]);
        return -EILSEQ;  /* EILSEQ: Illegal byte sequence */
    }

    return 0;
}

/* =====================================================================
 * 字符设备 file_operations
 * ===================================================================== */

/**
 * dht11_open — 设备打开 (无需特殊初始化)
 */
static int dht11_open(struct inode *node, struct file *filp)
{
    return 0;
}

/**
 * dht11_read — 用户态读取温湿度数据
 *
 * 返回 5 字节:
 *   buf[0] = 湿度整数
 *   buf[1] = 湿度小数 (DHT11 始终为 0)
 *   buf[2] = 温度整数
 *   buf[3] = 温度小数 (DHT11 始终为 0)
 *   buf[4] = 校验和
 *
 * 关键设计: 使用 local_irq_save 进入临界区
 *   DHT11 的 OneWire 协议要求微秒级精度的 GPIO bit-banging，
 *   如果读取过程中被中断/抢占导致时序漂移，会读取到错误数据。
 *   local_irq_save() 临时关闭本地 CPU 中断，防止被打断。
 *
 *   注意: 临界区时长约 5-10ms，对系统实时性影响很小。
 */
static ssize_t dht11_read(struct file *filp, char __user *buf,
                           size_t size, loff_t *offset)
{
    u8 data[DHT11_DATA_BYTES];
    unsigned long flags;
    int ret;

    /* 一次完整读取即可，不支持 lseek + 分片读 */
    if (*offset > 0)
        return 0;

    /*
     * 进入临界区 — 关闭本地中断
     *
     * 为什么不用 mutex/spinlock?
     *   因为即使拿到锁，内核仍可能在该 CPU 上调度其他任务或响应中断，
     *   只有 local_irq_save 能保证该 CPU 上不发生任何上下文切换。
     *
     * 对昇腾310B的影响:
     *   Cortex-A55 的中断延迟比 Cortex-A7 更低，理论上时序裕度更大。
     *   但多核场景下仍需要 local_irq_save (它只关本地 CPU 中断)。
     */
    local_irq_save(flags);

    ret = dht11_read_raw(data);

    /* 退出临界区 — 恢复之前的中断状态 */
    local_irq_restore(flags);

    if (ret < 0) {
        /* 错误码已在 dht11_read_raw 中打印，这里直接透传 */
        return ret;
    }

    /*
     * 数据验证通过，复制到用户空间
     * 如果 size > 5 字节，只返回 5 字节 (DHT11 最多只有5字节数据)
     */
    if (size > DHT11_DATA_BYTES)
        size = DHT11_DATA_BYTES;

    if (copy_to_user(buf, data, size))
        return -EFAULT;

    *offset += size;
    return size;
}

static int dht11_release(struct inode *node, struct file *filp)
{
    return 0;
}

static struct file_operations dht11_fops = {
    .owner   = THIS_MODULE,
    .open    = dht11_open,
    .read    = dht11_read,
    .release = dht11_release,
};

/* =====================================================================
 * Platform Driver — probe / remove
 * =====================================================================
 *
 * 为什么用 platform_driver 而不是直接在 init 中做一切?
 *   platform_driver 是 Linux 设备驱动模型的核心机制:
 *   - probe 只在设备树节点存在时才被调用 (正确的硬件描述)
 *   - 支持电源管理 (suspend/resume)
 *   - 支持设备热插拔场景 (虽然 GPIO 传感器不会热插拔)
 *   - 与设备树绑定, 配置参数通过 DT 传递而非硬编码
 */

/**
 * dht11_probe — 从设备树读取 GPIO 配置并初始化
 *
 * 设备树示例:
 *
 *   dht11 {
 *       compatible = "hc-dht11";    // 或 "huawei,dht11"
 *       gpios = <&gpio3 12 0>;      // GPIO3_12, 任意标志
 *   };
 */
static int dht11_probe(struct platform_device *pdev)
{
    struct device_node *node = pdev->dev.of_node;

    /* 从设备树获取 GPIO 引脚编号 */
    g_dht11_gpio = of_get_gpio(node, 0);
    if (!gpio_is_valid(g_dht11_gpio)) {
        dev_err(&pdev->dev, "[dht11] 设备树中未找到有效 GPIO\n");
        return -ENODEV;
    }

    /* 将 GPIO 编号转为描述符 (兼容新 gpiod 接口) */
    g_dht11_desc = gpio_to_desc(g_dht11_gpio);
    if (!g_dht11_desc) {
        dev_err(&pdev->dev, "[dht11] gpio_to_desc 失败\n");
        return -ENODEV;
    }

    /* 初始化为输出高电平 (空闲状态) */
    gpiod_direction_output(g_dht11_desc, 1);

    dev_info(&pdev->dev, "[dht11] probe 成功, GPIO=%d\n", g_dht11_gpio);
    return 0;
}

static int dht11_remove(struct platform_device *pdev)
{
    dev_info(&pdev->dev, "[dht11] 驱动已移除\n");
    return 0;
}

/* ── 设备树匹配表 ──────────────────────────────────────────────────── */
static const struct of_device_id dht11_of_match[] = {
    { .compatible = "hc-dht11"      },  /* 原 i.MX 平台                */
    { .compatible = "huawei,dht11"  },  /* 昇腾310B 平台 (推荐)        */
    { /* 哨兵 */ }
};
MODULE_DEVICE_TABLE(of, dht11_of_match);

static struct platform_driver dht11_platform_driver = {
    .driver = {
        .name           = "dht11",
        .of_match_table = dht11_of_match,
    },
    .probe  = dht11_probe,
    .remove = dht11_remove,
};

/* =====================================================================
 * 模块出入口
 * ===================================================================== */

/**
 * dht11_init — 模块加载
 *
 * 执行顺序:
 *   1. register_chrdev → 注册字符设备
 *   2. class_create    → 创建设备类 (供 udev 使用)
 *   3. device_create   → 创建设备节点 /dev/dht11
 *   4. platform_driver_register → 注册平台驱动 (触发 probe)
 *
 * 注意: 先注册字符设备再注册平台驱动。
 *   如果反过来，用户态程序可能在设备节点创建前就尝试 open()。
 */
static int __init dht11_init(void)
{
    int ret;

    /* ── 注册字符设备 ─────────────────────────────────── */
    g_major = register_chrdev(0, DRV_NAME, &dht11_fops);
    if (g_major < 0) {
        pr_err("[dht11] register_chrdev 失败: %d\n", g_major);
        return g_major;
    }

    /* ── 创建设备类 ────────────────────────────────────── */
    g_class = class_create(THIS_MODULE, "dht11_class");
    if (IS_ERR(g_class)) {
        ret = PTR_ERR(g_class);
        pr_err("[dht11] class_create 失败: %d\n", ret);
        goto err_chrdev;
    }

    /* ── 创建设备节点 /dev/dht11 ───────────────────────── */
    g_device = device_create(g_class, NULL, MKDEV(g_major, 0),
                              NULL, "dht11");
    if (IS_ERR(g_device)) {
        ret = PTR_ERR(g_device);
        pr_err("[dht11] device_create 失败: %d\n", ret);
        goto err_class;
    }

    /* ── 注册平台驱动 ──────────────────────────────────── */
    ret = platform_driver_register(&dht11_platform_driver);
    if (ret) {
        pr_err("[dht11] platform_driver_register 失败: %d\n", ret);
        goto err_device;
    }

    pr_info("[dht11] 驱动加载成功 v" DRV_VERSION " (major=%d)\n", g_major);
    return 0;

err_device:
    device_destroy(g_class, MKDEV(g_major, 0));
err_class:
    class_destroy(g_class);
err_chrdev:
    unregister_chrdev(g_major, DRV_NAME);
    return ret;
}

static void __exit dht11_exit(void)
{
    platform_driver_unregister(&dht11_platform_driver);
    device_destroy(g_class, MKDEV(g_major, 0));
    class_destroy(g_class);
    unregister_chrdev(g_major, DRV_NAME);
    pr_info("[dht11] 驱动卸载\n");
}

module_init(dht11_init);
module_exit(dht11_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Smart Screen Team");
MODULE_DESCRIPTION("DHT11 Temperature/Humidity Sensor OneWire GPIO Driver "
                   "(Ascend 310B compatible)");
MODULE_VERSION(DRV_VERSION);
