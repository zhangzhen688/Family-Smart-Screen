/**
 * @file    sr501_drv_dtb.c
 * @brief   SR501 PIR 人体红外传感器 GPIO 中断驱动
 *
 * =========================== 硬件概述 ===========================
 *
 * SR501 是最常见的 PIR (Passive InfraRed) 人体红外传感器模块。
 * 在本项目中用于智能场景联动 (检测到人 → 自动开灯/开窗帘)。
 *
 * 特性:
 *   - 检测距离: 3~7米 (可调)
 *   - 检测角度: <120° 锥形
 *   - 输出信号: 数字电平
 *       HIGH (3.3V) = 检测到人体移动
 *       LOW  (0V)  = 无人/静止
 *   - 延时: 检测到人后保持 HIGH 一段时间 (可调, 默认约5秒)
 *   - 封锁时间: 每次触发后有短暂的不应期 (约2.5秒)
 *
 * =========================== 驱动架构 ===========================
 *
 * 本驱动提供两种数据获取方式:
 *
 *   方式一: 轮询 (polling)
 *     - read() → 立即返回当前 GPIO 电平
 *     - 适用于简单场景, 定时查询即可
 *
 *   方式二: 异步通知 (asynchronous notification)
 *     - 使用 fasync + SIGIO 信号
 *     - GPIO 电平变化时 (中断触发) → 消抖 → 发送 SIGIO 给用户进程
 *     - 用户进程注册 SIGIO handler, 收到信号后 read() 获取状态
 *     - 适用于事件驱动场景, CPU 占用更低
 *
 * =========================== 消抖策略 ===========================
 *
 * SR501 的 PIR 传感器在检测到人体移动时会产生多次电平跳变
 * (因为人体移动不是瞬间完成的), 直接上报会导致"抖动"。
 *
 * 消抖方案:
 *   1. GPIO 双边沿中断 (IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING)
 *      → 任何电平变化都触发 ISR
 *   2. ISR 中不直接上报, 而是重置一个 20ms 定时器 (mod_timer)
 *   3. 定时器回调 timeout_handler:
 *      - 读取当前 GPIO 电平 (此时电平已稳定)
 *      - 发送 SIGIO + POLL_IN 通知用户态
 *
 *   这样只有在持续 20ms 内无新中断的情况下才会通知用户态，
 *   有效过滤了 PIR 传感器的抖动。
 *
 * =========================== 移植说明 (昇腾310B) ===========================
 * 原平台: NXP i.MX (Alientek), Linux 4.1.15
 * 新平台: 华为昇腾310B, Linux 5.10+
 *
 * 适配项:
 *   1. 设备树 compatible 改为 "huawei,sr501"
 *   2. GPIO 中断号重新映射
 *   3. 消抖时间 (20ms) 可能需要调整 (取决于昇腾的中断延迟)
 *   4. setup_timer 在新内核中已废弃 → 改用 timer_setup
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

/* ── 模块元信息 ──────────────────────────────────────────────────────── */
#define DRV_NAME            "sr501"
#define DRV_VERSION         "1.1.0"

/* ── 消抖参数 ────────────────────────────────────────────────────────── */
#define SR501_DEBOUNCE_JIFFIES  (HZ / 50)  /* 消抖时间: 20ms             */

/* ── 全局变量 ────────────────────────────────────────────────────────── */
static int              g_major;          /* 动态主设备号                */
static struct class    *g_class;          /* 设备类                     */
static struct device   *g_device;         /* 设备节点                   */
static int              g_irq;            /* 中断号                     */
static int              g_sr501_gpio;     /* GPIO 引脚编号              */
static struct gpio_desc *g_sr501_desc;   /* GPIO 描述符                */
static struct timer_list g_timer;         /* 消抖定时器                  */

/* ── 异步通知队列 ────────────────────────────────────────────────────── */
static struct fasync_struct *g_fasync_queue;

/**
 * timeout_handler — 消抖定时器回调
 *
 * 当中断发生后 20ms 内无新中断, 说明电平已稳定。
 * 此时:
 *   1. 读取当前 GPIO 电平
 *   2. 通过 kill_fasync 发送 SIGIO 信号给注册了异步通知的进程
 *
 * 用户态收到 SIGIO 后, 应该 read() 获取具体状态。
 *
 * @param data  未使用 (类型由内核 timer API 决定)
 */
static void timeout_handler(unsigned long data)
{
    (void)data;

    /*
     * kill_fasync:
     *   - g_fasync_queue: 通过 fasync_helper 维护的异步通知队列
     *   - SIGIO:          发送的信号类型
     *   - POLL_IN:        事件类型 (有数据可读)
     *
     * 注意: kill_fasync 可以在中断上下文中安全调用
     * (它在内部使用 rcu_read_lock 保护队列遍历)
     */
    kill_fasync(&g_fasync_queue, SIGIO, POLL_IN);
}

/**
 * sr501_handler — GPIO 中断处理函数 (ISR)
 *
 * 在双边沿中断触发时被调用。
 * 不做实际的数据处理, 只重置消抖定时器:
 *   mod_timer 会将定时器到期时间设为 jiffies + DEBOUNCE_JIFFIES
 *
 * 如果 20ms 内无新中断 → 定时器到期 → timeout_handler 被调用
 * 如果 20ms 内有新中断 → 定时器被重新设置 → 消抖周期重新开始
 *
 * @param irq     中断号
 * @param dev_id  设备ID (未使用)
 * @return        IRQ_HANDLED (已处理)
 */
static irqreturn_t sr501_handler(int irq, void *dev_id)
{
    (void)irq;
    (void)dev_id;

    /* 重置消抖定时器: 从当前时间起 20ms 后触发 */
    mod_timer(&g_timer, jiffies + SR501_DEBOUNCE_JIFFIES);

    return IRQ_HANDLED;
}

/* =====================================================================
 * Platform Driver — probe / remove
 * ===================================================================== */

/**
 * sr501_probe — 从设备树获取 GPIO, 配置中断, 初始化定时器
 *
 * 设备树示例:
 *
 *   pir_sensor {
 *       compatible = "hc-sr501";
 *       gpios = <&gpio4 5 GPIO_ACTIVE_HIGH>;
 *   };
 */
static int sr501_probe(struct platform_device *pdev)
{
    struct device_node *node = pdev->dev.of_node;
    int ret;

    /* 从设备树获取 GPIO 引脚编号 */
    g_sr501_gpio = of_get_gpio(node, 0);
    if (!gpio_is_valid(g_sr501_gpio)) {
        dev_err(&pdev->dev, "[sr501] 设备树中未找到有效 GPIO\n");
        return -ENODEV;
    }

    /* 将 GPIO 编号转为描述符 (兼容 gpiod 接口) */
    g_sr501_desc = gpio_to_desc(g_sr501_gpio);
    if (!g_sr501_desc) {
        dev_err(&pdev->dev, "[sr501] gpio_to_desc 失败\n");
        return -ENODEV;
    }

    /* GPIO → 中断号 */
    g_irq = gpio_to_irq(g_sr501_gpio);
    if (g_irq < 0) {
        dev_err(&pdev->dev, "[sr501] gpio_to_irq 失败: %d\n", g_irq);
        return g_irq;
    }

    /*
     * 注册中断处理函数
     *
     * 触发条件: IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING
     *   → 高→低 和 低→高 两种边沿都会触发中断
     *   → 确保能捕获到人体进入和离开两种事件
     *
     * 为什么不用线程化中断 (request_threaded_irq)?
     *   ISR 中只调用了 mod_timer (非睡眠, O(1)操作),
     *   不需要线程化, 直接在上半部处理即可。
     */
    ret = request_irq(g_irq, sr501_handler,
                      IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
                      "sr501_irq", NULL);
    if (ret) {
        dev_err(&pdev->dev, "[sr501] request_irq 失败: %d\n", ret);
        return ret;
    }

    /*
     * 初始化消抖定时器
     *
     * 注意: setup_timer 初始化 timer_list 并设置回调,
     * 但不会启动定时器。定时器由 ISR 中的 mod_timer 首次启动。
     *
     * add_timer 将定时器加入内核定时器链表 (但不激活)
     */
    setup_timer(&g_timer, timeout_handler, 0);
    add_timer(&g_timer);

    dev_info(&pdev->dev, "[sr501] probe 成功, GPIO=%d, IRQ=%d, 消抖=%dms\n",
             g_sr501_gpio, g_irq,
             SR501_DEBOUNCE_JIFFIES * 1000 / HZ);
    return 0;
}

static int sr501_remove(struct platform_device *pdev)
{
    del_timer_sync(&g_timer);   /* 等待定时器完成后再释放 */
    free_irq(g_irq, NULL);
    gpiod_put(g_sr501_desc);

    dev_info(&pdev->dev, "[sr501] 驱动已移除\n");
    return 0;
}

/* ── 设备树匹配表 ──────────────────────────────────────────────────── */
static const struct of_device_id sr501_of_match[] = {
    { .compatible = "hc-sr501"      },  /* 原 i.MX 平台          */
    { .compatible = "huawei,sr501"  },  /* 昇腾310B 平台 (推荐)  */
    { /* 哨兵 */ }
};
MODULE_DEVICE_TABLE(of, sr501_of_match);

static struct platform_driver sr501_platform_driver = {
    .driver = {
        .name           = DRV_NAME,
        .of_match_table = sr501_of_match,
    },
    .probe  = sr501_probe,
    .remove = sr501_remove,
};

/* =====================================================================
 * 字符设备 file_operations
 * ===================================================================== */

static int sr501_open(struct inode *node, struct file *filp)
{
    return 0;
}

/**
 * sr501_read — 读取当前人体检测状态
 *
 * 返回 1 字节:
 *   0 = 无人/静止
 *   1 = 检测到人体移动
 *
 * 这是轮询方式，适合定时查询场景。
 * 如需事件驱动，应使用 fasync + SIGIO。
 */
static ssize_t sr501_read(struct file *filp, char __user *buf,
                           size_t size, loff_t *offset)
{
    char val;
    int err;

    /* 读取当前 GPIO 物理电平 */
    val = (char)gpiod_get_value(g_sr501_desc);

    err = copy_to_user(buf, &val, 1);
    if (err)
        return -EFAULT;

    return 1;  /* 返回 1 字节 */
}

static int sr501_release(struct inode *node, struct file *filp)
{
    /* 进程关闭 fd 时，从异步通知队列中移除 */
    sr501_fasync(-1, filp, 0);
    return 0;
}

/**
 * sr501_fasync — 注册/注销异步通知
 *
 * 用户态调用 fcntl(fd, F_SETFL, O_ASYNC) 时，
 * 内核 VFS 层会调用此函数。
 *
 * fasync_helper 负责:
 *   - 将当前进程加入/移出 g_fasync_queue
 *   - 当 kill_fasync 被调用时，遍历队列发送信号
 *
 * @param fd   文件描述符 (-1 表示注销)
 * @param filp 文件指针
 * @param on   1=注册, 0=注销
 */
static int sr501_fasync(int fd, struct file *filp, int on)
{
    return fasync_helper(fd, filp, on, &g_fasync_queue);
}

static struct file_operations sr501_fops = {
    .owner   = THIS_MODULE,
    .open    = sr501_open,
    .release = sr501_release,
    .read    = sr501_read,
    .fasync  = sr501_fasync,   /* 异步通知支持 */
};

/* =====================================================================
 * 模块出入口
 * ===================================================================== */

static int __init sr501_init(void)
{
    int ret;

    /* 注册字符设备 */
    g_major = register_chrdev(0, DRV_NAME, &sr501_fops);
    if (g_major < 0) {
        pr_err("[sr501] register_chrdev 失败: %d\n", g_major);
        return g_major;
    }

    /* 创建设备类 */
    g_class = class_create(THIS_MODULE, "sr501_class");
    if (IS_ERR(g_class)) {
        ret = PTR_ERR(g_class);
        pr_err("[sr501] class_create 失败: %d\n", ret);
        goto err_chrdev;
    }

    /* 创建设备节点 /dev/sr501 */
    g_device = device_create(g_class, NULL, MKDEV(g_major, 0),
                              NULL, "sr501");
    if (IS_ERR(g_device)) {
        ret = PTR_ERR(g_device);
        pr_err("[sr501] device_create 失败: %d\n", ret);
        goto err_class;
    }

    /* 注册平台驱动 */
    ret = platform_driver_register(&sr501_platform_driver);
    if (ret) {
        pr_err("[sr501] platform_driver_register 失败: %d\n", ret);
        goto err_device;
    }

    pr_info("[sr501] 驱动加载成功 v" DRV_VERSION " (major=%d)\n", g_major);
    return 0;

err_device:
    device_destroy(g_class, MKDEV(g_major, 0));
err_class:
    class_destroy(g_class);
err_chrdev:
    unregister_chrdev(g_major, DRV_NAME);
    return ret;
}

static void __exit sr501_exit(void)
{
    platform_driver_unregister(&sr501_platform_driver);
    device_destroy(g_class, MKDEV(g_major, 0));
    class_destroy(g_class);
    unregister_chrdev(g_major, DRV_NAME);
    pr_info("[sr501] 驱动卸载\n");
}

module_init(sr501_init);
module_exit(sr501_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Smart Screen Team");
MODULE_DESCRIPTION("SR501 PIR Motion Sensor GPIO Interrupt Driver "
                   "(Ascend 310B compatible)");
MODULE_VERSION(DRV_VERSION);
