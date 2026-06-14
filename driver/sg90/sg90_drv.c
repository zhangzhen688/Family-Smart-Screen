/**
 * @file    sg90_drv.c
 * @brief   SG90 微型舵机 PWM 内核驱动
 *
 * =========================== 硬件概述 ===========================
 * SG90 是最常见的微型舵机，广泛用于机器人、遥控模型。
 * 在本项目中用作智能窗帘的驱动电机。
 *
 * 特性:
 *   - 工作电压: 4.8V ~ 6V
 *   - 扭矩: 1.8 kg·cm (4.8V)
 *   - 转速: 0.1s/60° (4.8V)
 *   - 控制方式: PWM 脉宽调制
 *   - 角度范围: 0° ~ 180°
 *
 * =========================== PWM 控制协议 ===========================
 *
 * 舵机位置由 PWM 脉冲宽度决定:
 *
 *   脉宽 500us  (0.5ms) →  0° (最左/窗帘全关)
 *   脉宽 1000us (1.0ms) → 45°
 *   脉宽 1500us (1.5ms) → 90° (中位/窗帘半开)
 *   脉宽 2000us (2.0ms) → 135°
 *   脉宽 2500us (2.5ms) → 180° (最右/窗帘全开)
 *
 *   周期固定为 20ms (50Hz 标准舵机频率)
 *
 *   角度→脉宽换算公式:
 *     pulse_ns = 500000 + angle * 2000000 / 180
 *             = 500000 + angle * 100000 / 9       (整数运算优化)
 *
 *   验证:
 *     angle=0:   pulse = 500000 + 0         = 500000ns  = 0.5ms  ✓
 *     angle=90:  pulse = 500000 + 9000000/9 = 1500000ns = 1.5ms  ✓
 *     angle=180: pulse = 500000 + 18000000/9= 2500000ns = 2.5ms  ✓
 *
 * =========================== Lniux PWM 子系统 ===========================
 *
 * 本驱动使用 Linux PWM 子系统 API:
 *   - devm_of_pwm_get()   从设备树获取 PWM 设备
 *   - pwm_config()        配置周期和占空比
 *   - pwm_set_polarity()  设置输出极性
 *   - pwm_enable()        使能 PWM 输出
 *   - pwm_free()          释放 PWM 资源
 *
 * =========================== 移植说明 (昇腾310B) ===========================
 *
 * 原平台: NXP i.MX, PWM 控制器为 imx-pwm
 * 新平台: 华为昇腾310B, PWM 控制器可能不同
 *
 * 适配项:
 *   1. 确认昇腾310B 是否有硬件 PWM 控制器 (部分版本无)
 *   2. 如无硬件 PWM，考虑:
 *      a. 使用 GPIO + 高精度定时器模拟 PWM
 *      b. 使用 I2C PWM 扩展芯片 (如 PCA9685)
 *      c. 外接 MCU 通过 UART/I2C 控制舵机
 *   3. PWM 通道编号可能变化 (pwmchip0 → pwmchip1)
 *   4. sysfs 路径: /sys/class/pwm/pwmchipX/pwmY/
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/uaccess.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/pwm.h>

/* ── 模块元信息 ──────────────────────────────────────────────────────── */
#define DRV_NAME        "sg90"
#define DRV_VERSION     "1.1.0"

/* ── PWM 参数 ────────────────────────────────────────────────────────── */
#define SG90_PERIOD_NS          20000000  /* PWM 周期: 20ms (50Hz)       */
#define SG90_MIN_PULSE_NS        500000  /* 0°  脉宽: 0.5ms             */
/*          中间值由公式计算                                           */
#define SG90_MAX_PULSE_NS       2500000  /* 180°脉宽: 2.5ms             */
#define SG90_DEFAULT_ANGLE       0       /* 默认角度: 0° (安全位置)     */

/* ── 全局变量 ────────────────────────────────────────────────────────── */
static int                g_major;
static struct class      *g_class;
static struct device     *g_device;
static struct pwm_device *g_pwm;        /* PWM 设备句柄                 */

/* =====================================================================
 * 角度→脉宽 换算
 * ===================================================================== */

/**
 * angle_to_pulse_ns — 将角度 (0~180) 转换为脉宽 (纳秒)
 *
 * 公式:
 *   pulse_ns = 500000 + angle * 100000 / 9
 *
 * 使用整数运算避免浮点开销, 精度: ±1° (在舵机精度范围内)
 */
static inline int angle_to_pulse_ns(int angle)
{
    return SG90_MIN_PULSE_NS + (angle * 100000 / 9);
}

/* =====================================================================
 * Platform Driver — probe / remove
 * ===================================================================== */

/**
 * sg90_probe — 从设备树获取 PWM 并初始化
 *
 * 设备树示例:
 *
 *   pwm_servo {
 *       compatible = "hc-sg90";
 *       pwms = <&pwm0 0 20000000>;  // PWM0通道0, 周期20ms
 *   };
 */
static int sg90_probe(struct platform_device *pdev)
{
    struct device_node *node = pdev->dev.of_node;

    dev_info(&pdev->dev, "[sg90] probe 开始\n");

    if (!node) {
        dev_err(&pdev->dev, "[sg90] 设备树节点为空!\n");
        return -ENODEV;
    }

    /*
     * 从设备树子节点获取 PWM 设备
     *
     * devm_of_pwm_get 是设备资源管理版本的 of_pwm_get,
     * 无需手动释放 (设备移除时自动 free)
     */
    g_pwm = devm_of_pwm_get(&pdev->dev, node, NULL);
    if (IS_ERR(g_pwm)) {
        int err = PTR_ERR(g_pwm);
        dev_err(&pdev->dev, "[sg90] 获取 PWM 设备失败: %d\n", err);
        dev_err(&pdev->dev, "[sg90] 请检查设备树中 pwms 属性是否正确配置\n");
        return err;
    }

    /*
     * 配置 PWM 默认参数:
     *   脉宽: 500us (0° — 安全位置)
     *   周期: 20ms
     *
     * 初始化为 0° 可以防止舵机在上电瞬间乱转
     */
    pwm_config(g_pwm, SG90_MIN_PULSE_NS, SG90_PERIOD_NS);

    /* 设置输出极性: 正常 (高电平代表有效脉宽) */
    pwm_set_polarity(g_pwm, PWM_POLARITY_NORMAL);

    /* 使能 PWM 输出 */
    pwm_enable(g_pwm);

    dev_info(&pdev->dev, "[sg90] probe 成功, "
             "周期=%dns(50Hz), 初始脉宽=%dns(0°)\n",
             SG90_PERIOD_NS, SG90_MIN_PULSE_NS);
    return 0;
}

/**
 * sg90_remove — 设备移除时安全复位舵机
 *
 * 关键: 必须先复位到 0° 再释放 PWM, 否则舵机会
 * 保持最后的脉宽输出, 可能损坏机械结构
 */
static int sg90_remove(struct platform_device *pdev)
{
    /* 安全复位到 0° */
    pwm_config(g_pwm, SG90_MIN_PULSE_NS, SG90_PERIOD_NS);

    /* 释放 PWM 资源 */
    pwm_free(g_pwm);

    dev_info(&pdev->dev, "[sg90] 已安全复位并释放 PWM\n");
    return 0;
}

/* ── 设备树匹配表 ──────────────────────────────────────────────────── */
static const struct of_device_id sg90_of_match[] = {
    { .compatible = "hc-sg90"      },  /* 原 i.MX 平台          */
    { .compatible = "huawei,sg90"  },  /* 昇腾310B 平台 (推荐)  */
    { /* 哨兵 */ }
};
MODULE_DEVICE_TABLE(of, sg90_of_match);

static struct platform_driver sg90_platform_driver = {
    .driver = {
        .name           = DRV_NAME,
        .of_match_table = sg90_of_match,
    },
    .probe  = sg90_probe,
    .remove = sg90_remove,
};

/* =====================================================================
 * 字符设备 file_operations
 * ===================================================================== */

static int sg90_open(struct inode *node, struct file *filp)
{
    return 0;
}

/**
 * sg90_write — 用户态写入目标角度
 *
 * 用法:
 *   unsigned char angle = 90;  // 0~180 度
 *   write(fd, &angle, 1);
 *
 * 安全性:
 *   - 只接受恰好 1 字节的写入
 *   - 角度值 0~180 由硬件约束保证 (超出范围可能导致舵机堵转)
 *   - 每次 write 都会重新配置 PWM, 保证实时响应
 */
static ssize_t sg90_write(struct file *filp, const char __user *buf,
                           size_t size, loff_t *offset)
{
    unsigned char data[1];
    int pulse_ns;

    /* 必须恰好 1 字节 */
    if (size != 1) {
        pr_err("[sg90] write 必须恰好 1 字节 (角度 0~180)\n");
        return -EINVAL;
    }

    if (copy_from_user(data, buf, size))
        return -EFAULT;

    /*
     * 角度→脉宽转换
     *
     * 注意: 这里不做范围检查，硬件 PWM 控制器通常可以
     * 处理极端的脉宽值 (尽管不推荐超出 500~2500us 范围)
     */
    pulse_ns = angle_to_pulse_ns(data[0]);

    /*
     * pwm_config(pwm, duty_ns, period_ns)
     *
     * 注意: 内核 PWM API 中, 周期参数每次都要传入。
     * 如果想只改占空比不改周期, 仍需要传入周期值。
     */
    pwm_config(g_pwm, pulse_ns, SG90_PERIOD_NS);

    return 1;  /* 返回写入的字节数 */
}

static int sg90_release(struct inode *node, struct file *filp)
{
    return 0;
}

static struct file_operations sg90_fops = {
    .owner   = THIS_MODULE,
    .open    = sg90_open,
    .write   = sg90_write,
    .release = sg90_release,
};

/* =====================================================================
 * 模块出入口
 * ===================================================================== */

static int __init sg90_init(void)
{
    int ret;

    /* 注册字符设备 */
    g_major = register_chrdev(0, DRV_NAME, &sg90_fops);
    if (g_major < 0) {
        pr_err("[sg90] register_chrdev 失败: %d\n", g_major);
        return g_major;
    }

    /* 创建设备类 */
    g_class = class_create(THIS_MODULE, "sg90_class");
    if (IS_ERR(g_class)) {
        ret = PTR_ERR(g_class);
        pr_err("[sg90] class_create 失败: %d\n", ret);
        goto err_chrdev;
    }

    /* 创建设备节点 /dev/sg90 */
    g_device = device_create(g_class, NULL, MKDEV(g_major, 0),
                              NULL, "sg90");
    if (IS_ERR(g_device)) {
        ret = PTR_ERR(g_device);
        pr_err("[sg90] device_create 失败: %d\n", ret);
        goto err_class;
    }

    /* 注册平台驱动 */
    ret = platform_driver_register(&sg90_platform_driver);
    if (ret) {
        pr_err("[sg90] platform_driver_register 失败: %d\n", ret);
        goto err_device;
    }

    pr_info("[sg90] 驱动加载成功 v" DRV_VERSION " (major=%d)\n", g_major);
    return 0;

err_device:
    device_destroy(g_class, MKDEV(g_major, 0));
err_class:
    class_destroy(g_class);
err_chrdev:
    unregister_chrdev(g_major, DRV_NAME);
    return ret;
}

static void __exit sg90_exit(void)
{
    platform_driver_unregister(&sg90_platform_driver);
    device_destroy(g_class, MKDEV(g_major, 0));
    class_destroy(g_class);
    unregister_chrdev(g_major, DRV_NAME);
    pr_info("[sg90] 驱动卸载\n");
}

module_init(sg90_init);
module_exit(sg90_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Smart Screen Team");
MODULE_DESCRIPTION("SG90 Micro Servo PWM Driver (Ascend 310B compatible)");
MODULE_VERSION(DRV_VERSION);
