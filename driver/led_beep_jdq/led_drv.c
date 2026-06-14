/**
 * @file    led_drv.c
 * @brief   通用 GPIO 输出设备驱动 — LED / 蜂鸣器 / 继电器
 *
 * =========================== 设计思路 ===========================
 *
 * 这是一个"一对多"的平台驱动: 一个驱动同时管理多种 GPIO 输出设备。
 * LED、蜂鸣器、继电器本质上都是 GPIO 输出控制，差异仅在于:
 *   - 设备树节点名字不同 (led / beep / jdq)
 *   - 设备节点路径不同 (/dev/led_0, /dev/beep, /dev/jdq)
 *
 * 通过设备树中的 my_name 属性区分设备类型，避免为每种设备写重复代码。
 *
 * =========================== 驱动架构 ===========================
 *
 *   platform_driver 匹配 3 种 compatible 字符串
 *        │
 *   probe() 被多次调用 (每个设备树节点一次)
 *        │
 *   从 DT 读取 my_name → 申请 GPIO → 创建设备节点 /dev/<my_name>
 *        │                                     │
 *   dev_cnt 计数器跟踪设备数量          用户态 open/read/write
 *
 * =========================== 设备树示例 ===========================
 *
 *   leds {
 *       compatible = "hc-led";
 *       my_name = "led_0";     // → 创建设备节点 /dev/led_0
 *       gpios = <&gpio1 0 GPIO_ACTIVE_HIGH>;
 *   };
 *
 *   beep {
 *       compatible = "hc-beep";
 *       my_name = "beep";      // → 创建设备节点 /dev/beep
 *       gpios = <&gpio1 1 GPIO_ACTIVE_HIGH>;
 *   };
 *
 *   relay {
 *       compatible = "hc-jdq";
 *       my_name = "jdq";       // → 创建设备节点 /dev/jdq
 *       gpios = <&gpio1 2 GPIO_ACTIVE_HIGH>;
 *   };
 *
 * =========================== 移植说明 (昇腾310B) ===========================
 * 原平台: NXP i.MX (Alientek), Linux 4.1.15
 * 新平台: 华为昇腾310B, Linux 5.10+
 *
 * 适配项:
 *   1. 设备树 compatible 改为 "huawei,led" 等
 *   2. GPIO 编号完全重新映射 (昇腾的 GPIO 控制器不同于 i.MX)
 *   3. LED 数量可能不同 (代码支持 0~9 共10个, 当前项目使用6个)
 *   4. active-high / active-low 极性可能反转
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
#include <linux/slab.h>
#include <linux/device.h>

/* ── 模块元信息 ──────────────────────────────────────────────────────── */
#define DRV_NAME        "led_gpio"
#define DRV_VERSION     "1.1.0"
#define MAX_DEVICES     10           /* 最多支持 10 个 GPIO 设备      */
#define DEV_NAME_LEN    20           /* 设备名称最大长度               */

/* ── 全局状态 ────────────────────────────────────────────────────────── */
static int             g_major;                   /* 动态主设备号          */
static struct class   *g_dev_class;               /* 设备类                */
static int             g_dev_cnt;                 /* 已注册的设备数量       */
static char            g_dev_names[MAX_DEVICES][DEV_NAME_LEN]; /* 名称表  */
static struct gpio_desc *g_dev_gpio[MAX_DEVICES]; /* GPIO 描述符数组       */

/**
 * my_drv_read — 读取 GPIO 物理电平
 *
 * 用户态:
 *   char val;
 *   read(fd, &val, 1);  // val = 0 低电平, val = 1 高电平
 *
 * 注意: 返回的是物理电平，不是逻辑电平。
 *   如果 LED 是 active-low (0=亮, 1=灭)，应用层需要自己做映射。
 */
static ssize_t my_drv_read(struct file *filp, char __user *buf,
                            size_t size, loff_t *offset)
{
    int minor = iminor(file_inode(filp));
    char status;
    int err;

    /* 读取 GPIO 物理电平 (0=低, 1=高) */
    status = (char)gpiod_get_value(g_dev_gpio[minor]);

    err = copy_to_user(buf, &status, 1);
    if (err)
        return -EFAULT;

    return 1;  /* 返回读取的字节数 */
}

/**
 * my_drv_write — 设置 GPIO 逻辑电平
 *
 * 用户态:
 *   char val = 1;
 *   write(fd, &val, 1);  // 设置 GPIO 输出高电平
 *
 * gpiod_set_value() 设置的是逻辑值:
 *   - 如果 DT 中定义了 GPIO_ACTIVE_HIGH: 逻辑1 = 物理高
 *   - 如果 DT 中定义了 GPIO_ACTIVE_LOW:  逻辑1 = 物理低
 *   这种逻辑/物理的自动转换由 gpiod 子系统处理。
 */
static ssize_t my_drv_write(struct file *filp, const char __user *buf,
                             size_t size, loff_t *offset)
{
    int minor = iminor(file_inode(filp));
    char status;
    int err;

    err = copy_from_user(&status, buf, 1);
    if (err)
        return -EFAULT;

    /* 设置 GPIO 逻辑值 (gpiod 自动处理 active-low 反转) */
    gpiod_set_value(g_dev_gpio[minor], status);

    return size;
}

static int my_drv_open(struct inode *node, struct file *filp)
{
    return 0;
}

static int my_drv_release(struct inode *node, struct file *filp)
{
    return 0;
}

/* ── file_operations: 向 VFS 注册操作接口 ──────────────────────────── */
static struct file_operations g_dev_fops = {
    .owner   = THIS_MODULE,
    .read    = my_drv_read,
    .write   = my_drv_write,
    .open    = my_drv_open,
    .release = my_drv_release,
};

/**
 * my_probe — 设备匹配时初始化单个 GPIO 设备
 *
 * 此函数可能被多次调用 (有多少个设备树节点就调用多少次)。
 * 每次调用:
 *   1. 从 DT 读取设备名称 (my_name 属性)
 *   2. 通过 gpiod_get 获取 GPIO 描述符
 *   3. 创建设备节点 /dev/<my_name>
 *   4. dev_cnt 递增
 */
static int my_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct device_node *np = dev->of_node;
    const char *my_name = NULL;
    int ret;

    /* 从设备树读取设备名称 (my_name 属性) */
    ret = of_property_read_string(np, "my_name", &my_name);
    if (ret || !my_name) {
        dev_err(dev, "[led_gpio] 设备树缺少 my_name 属性\n");
        return -EINVAL;
    }

    /* 超过最大设备数? */
    if (g_dev_cnt >= MAX_DEVICES) {
        dev_err(dev, "[led_gpio] 设备数已达上限 %d\n", MAX_DEVICES);
        return -ENOMEM;
    }

    /* 保存设备名称 */
    strncpy(g_dev_names[g_dev_cnt], my_name, DEV_NAME_LEN - 1);
    g_dev_names[g_dev_cnt][DEV_NAME_LEN - 1] = '\0';

    /*
     * 获取 GPIO 描述符
     *
     * gpiod_get(dev, con_id, flags) 使用新的 gpiod 接口:
     *   - dev:     设备指针 (用于设备绑定和错误消息)
     *   - con_id:  连接ID, 对应 DT 中的 "xxx-gpios" 名称
     *   - flags:   初始输出值 (GPIOD_OUT_LOW = 初始低电平)
     *
     * 在设备树中:
     *   leds {
     *       my_name = "led_0";
     *       gpios = <&gpio1 0 GPIO_ACTIVE_HIGH>;
     *   };
     * gpiod_get 通过 con_id = NULL 获取第一个 gpios 属性。
     */
    g_dev_gpio[g_dev_cnt] = gpiod_get(dev, NULL, GPIOD_OUT_LOW);
    if (IS_ERR(g_dev_gpio[g_dev_cnt])) {
        ret = PTR_ERR(g_dev_gpio[g_dev_cnt]);
        dev_err(dev, "[led_gpio] gpiod_get('%s') 失败: %d\n", my_name, ret);
        return ret;
    }

    /*
     * 设置为输出方向，初始高电平(无效态)
     *
     * 对于 LED (active-low): 物理高 = 灯灭
     * 对于蜂鸣器 (active-high): 物理高 = 不响
     * 对于继电器 (active-high): 物理高 = 断开
     *
     * 所有设备初始化为"关闭"状态
     */
    gpiod_direction_output(g_dev_gpio[g_dev_cnt], 1);

    /* 创建设备节点 /dev/<my_name>，次设备号 = g_dev_cnt */
    device_create(g_dev_class, dev, MKDEV(g_major, g_dev_cnt),
                  NULL, my_name);

    dev_info(dev, "[led_gpio] '%s' probe 成功 (次设备号: %d)\n",
             my_name, g_dev_cnt);

    g_dev_cnt++;
    return 0;
}

/**
 * my_remove — 移除设备时清理
 *
 * 通过 my_name 属性匹配要移除的设备，然后:
 *   1. 释放 GPIO
 *   2. 销毁设备节点
 *   3. 清空名称表对应位置
 */
static int my_remove(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    struct device_node *np = dev->of_node;
    const char *my_name = NULL;
    int ret;
    int i;

    ret = of_property_read_string(np, "my_name", &my_name);
    if (ret || !my_name)
        return -EINVAL;

    /* 遍历名称表，找到匹配的设备 */
    for (i = 0; i < g_dev_cnt; i++) {
        if (strcmp(g_dev_names[i], my_name) == 0) {
            int gpio = desc_to_gpio(g_dev_gpio[i]);

            gpiod_put(g_dev_gpio[i]);         /* 释放 GPIO */
            device_destroy(g_dev_class, MKDEV(g_major, i)); /* 销毁节点 */
            g_dev_names[i][0] = '\0';          /* 清空名称   */

            dev_info(dev, "[led_gpio] '%s' 已移除 (GPIO=%d)\n",
                     my_name, gpio);
            return 0;
        }
    }

    dev_warn(dev, "[led_gpio] 未找到要移除的设备 '%s'\n", my_name);
    return -ENODEV;
}

/* ── 设备树匹配表 ──────────────────────────────────────────────────── */
static const struct of_device_id g_dev_dt_match[] = {
    { .compatible = "hc-led"  },    /* 原 i.MX: LED       */
    { .compatible = "hc-beep" },    /* 原 i.MX: 蜂鸣器     */
    { .compatible = "hc-jdq"  },    /* 原 i.MX: 继电器     */

    { .compatible = "huawei,led"  },  /* 昇腾310B: LED     */
    { .compatible = "huawei,beep" },  /* 昇腾310B: 蜂鸣器   */
    { .compatible = "huawei,jdq"  },  /* 昇腾310B: 继电器   */
    { /* 哨兵 */ }
};
MODULE_DEVICE_TABLE(of, g_dev_dt_match);

static struct platform_driver g_dev_driver = {
    .probe  = my_probe,
    .remove = my_remove,
    .driver = {
        .name           = DRV_NAME,
        .of_match_table = g_dev_dt_match,
    },
};

/* =====================================================================
 * 模块出入口
 * ===================================================================== */

/**
 * dev_init — 模块加载
 *
 * 执行顺序:
 *   1. register_chrdev(0) → 动态分配主设备号 (0 = 自动选择)
 *   2. class_create       → 创建设备类
 *   3. platform_driver_register → 注册平台驱动 (触发所有 probe)
 *
 * device_create 不在 init 中调用，而是在每个 probe 中按需创建。
 */
static int __init dev_init(void)
{
    int ret;

    /* 注册字符设备，动态分配主设备号 */
    g_major = register_chrdev(0, DRV_NAME, &g_dev_fops);
    if (g_major < 0) {
        pr_err("[led_gpio] register_chrdev 失败: %d\n", g_major);
        return g_major;
    }

    /* 创建设备类 */
    g_dev_class = class_create(THIS_MODULE, "gpio_output_class");
    if (IS_ERR(g_dev_class)) {
        ret = PTR_ERR(g_dev_class);
        pr_err("[led_gpio] class_create 失败: %d\n", ret);
        unregister_chrdev(g_major, DRV_NAME);
        return ret;
    }

    /* 注册平台驱动 */
    ret = platform_driver_register(&g_dev_driver);
    if (ret) {
        pr_err("[led_gpio] platform_driver_register 失败: %d\n", ret);
        class_destroy(g_dev_class);
        unregister_chrdev(g_major, DRV_NAME);
        return ret;
    }

    g_dev_cnt = 0;
    pr_info("[led_gpio] 驱动加载成功 v" DRV_VERSION " (major=%d, 最大设备数=%d)\n",
            g_major, MAX_DEVICES);
    return 0;
}

static void __exit dev_exit(void)
{
    int i;

    /* 清理所有已注册的设备 */
    for (i = 0; i < g_dev_cnt; i++) {
        if (g_dev_names[i][0] != '\0') {
            gpiod_put(g_dev_gpio[i]);
            device_destroy(g_dev_class, MKDEV(g_major, i));
        }
    }

    platform_driver_unregister(&g_dev_driver);
    class_destroy(g_dev_class);
    unregister_chrdev(g_major, DRV_NAME);
    pr_info("[led_gpio] 驱动卸载\n");
}

module_init(dev_init);
module_exit(dev_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Smart Screen Team");
MODULE_DESCRIPTION("Generic GPIO Output Driver — LED/Buzzer/Relay "
                   "(Ascend 310B compatible)");
MODULE_VERSION(DRV_VERSION);
