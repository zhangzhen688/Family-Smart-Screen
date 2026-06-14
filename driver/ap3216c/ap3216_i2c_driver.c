/**
 * @file    ap3216_i2c_driver.c
 * @brief   AP3216C 环境光/接近/红外传感器 I2C 内核驱动
 *
 * =========================== 硬件概述 ===========================
 * AP3216C 是一颗三合一传感器，集成：
 *   - ALS (Ambient Light Sensor)  环境光传感器，输出光照强度(lux)
 *   - PS  (Proximity Sensor)      接近传感器，检测物体远近
 *   - IR  (Infrared Sensor)       红外传感器
 *
 * =========================== 通信协议 ===========================
 * - 总线:     I2C
 * - 从机地址: 0x1E
 * - 寄存器:   单字节地址，16位数据(大端序)
 * - 关键寄存器:
 *    0x00  系统配置寄存器 (bit0=ALS使能, bit1=PS使能, bit2=软件复位)
 *    0x0A  IR 数据寄存器   (2字节，只读)
 *    0x0C  ALS 数据寄存器  (2字节，只读)
 *    0x0E  PS 数据寄存器   (2字节，只读)
 *
 * =========================== 驱动架构 ===========================
 * - 注册为 I2C 设备驱动，通过设备树 compatible 字符串匹配
 * - 创建字符设备 /dev/ap3216，用户态 read() 读取6字节传感器数据
 * - open() 时执行芯片软复位+使能，保证每次打开都是干净状态
 *
 * =========================== 移植说明 (昇腾310B) ===========================
 * 原平台: NXP i.MX (Alientek), Linux 4.1.15
 * 新平台: 华为昇腾310B (Ascend 310B), Linux 5.10+
 * 适配项:
 *   1. 设备树 compatible 改为 "huawei,ap3216c" 或在 DT 中保留原字符串
 *   2. I2C 总线编号可能变化 (原 i2c-0 → 需确认昇腾上对应的 I2C 控制器)
 *   3. GPIO 引脚如使用中断模式需重新映射
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/uaccess.h>
#include <linux/mod_devicetable.h>
#include <linux/fs.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/cdev.h>

/* ── 模块元信息 ─────────────────────────────────────────────────────── */
#define DRV_NAME        "ap3216c"
#define DRV_VERSION     "1.1.0"
#define AP3216C_I2C_ADDR 0x1E

/* ── AP3216C 寄存器定义 ─────────────────────────────────────────────── */
#define AP3216C_REG_SYS_CONF    0x00   /* 系统配置寄存器           */
#define AP3216C_REG_IR_DATA     0x0A   /* IR 数据 (2字节，大端)    */
#define AP3216C_REG_ALS_DATA    0x0C   /* 环境光数据 (2字节，大端)  */
#define AP3216C_REG_PS_DATA     0x0E   /* 接近传感器数据 (2字节，大端) */

/* ── 系统配置寄存器位定义 ────────────────────────────────────────────── */
#define AP3216C_CONF_ALS_EN     (1 << 0)  /* bit0: 环境光使能       */
#define AP3216C_CONF_PS_EN      (1 << 1)  /* bit1: 接近传感使能      */
#define AP3216C_CONF_SW_RESET   (1 << 2)  /* bit2: 软件复位          */

/* ── 用户态读取数据长度: IR(2B) + ALS(2B) + PS(2B) = 6 字节 ───────── */
#define AP3216C_DATA_LEN        6

/* ── 全局变量 ──────────────────────────────────────────────────────── */
static int              g_major;             /* 动态分配的主设备号        */
static struct class    *g_ap3216_class;      /* 设备类 (用于自动创建设备节点) */
static struct i2c_client *g_ap3216_client;   /* probe 时保存的 I2C 客户端 */
static struct device   *g_ap3216_device;     /* 创建的设备节点            */

/* =====================================================================
 * 设备打开 — 芯片初始化
 * =====================================================================
 *
 * 每次 open() 执行以下流程:
 *   Step 1: 写 0x04 到配置寄存器 → 触发软件复位
 *           bit2=1 会将所有寄存器恢复默认值，约需 10ms
 *   Step 2: 等待 15ms (给足复位时间，datasheet 建议 ≥ 10ms)
 *   Step 3: 写 0x03 到配置寄存器 → 使能 ALS + PS
 *           bit0=1 ALS 开始连续转换
 *           bit1=1 PS  开始连续转换
 *
 * 这样设计的好处: 设备不使用时可以关闭(fd close)，使用时重新初始化，
 * 既能省电又能保证每次读取数据的有效性。
 */
static int ap3216c_open(struct inode *node, struct file *filp)
{
    int ret;

    /* Step 1: 软件复位 — 写 0x04 到系统配置寄存器 */
    ret = i2c_smbus_write_byte_data(g_ap3216_client,
                                     AP3216C_REG_SYS_CONF,
                                     AP3216C_CONF_SW_RESET);
    if (ret < 0) {
        pr_err("[ap3216c] 软件复位失败, i2c write error: %d\n", ret);
        return -EIO;
    }

    /* Step 2: 等待复位完成 (datasheet: ≥ 10ms，这里取 15ms 留余量) */
    mdelay(15);

    /* Step 3: 使能 ALS + PS — 写 0x03 到系统配置寄存器 */
    ret = i2c_smbus_write_byte_data(g_ap3216_client,
                                     AP3216C_REG_SYS_CONF,
                                     AP3216C_CONF_ALS_EN | AP3216C_CONF_PS_EN);
    if (ret < 0) {
        pr_err("[ap3216c] 使能传感器失败, i2c write error: %d\n", ret);
        return -EIO;
    }

    pr_debug("[ap3216c] 设备已初始化 (复位+使能)\n");
    return 0;
}

/* =====================================================================
 * 设备读取 — 获取6字节传感器数据
 * =====================================================================
 *
 * 返回给用户态的数据格式 (6 字节，大端序):
 *   buf[0] = IR  高字节
 *   buf[1] = IR  低字节
 *   buf[2] = ALS 高字节
 *   buf[3] = ALS 低字节
 *   buf[4] = PS  高字节
 *   buf[5] = PS  低字节
 *
 * 使用 i2c_smbus_read_word_data() 读取16位数据，
 * 该函数内部处理了 I2C 总线锁和重试逻辑，比裸 i2c_transfer 更简洁。
 *
 * 注意:
 *   - 用户态必须恰好请求 6 字节，否则返回 -EINVAL
 *   - 使用 copy_to_user 确保安全地从内核态传递数据
 */
static ssize_t ap3216c_read(struct file *filp, char __user *buf,
                             size_t size, loff_t *offset)
{
    int var, err;
    char data[AP3216C_DATA_LEN];

    /* 校验请求大小: 必须是 6 字节 */
    if (size != AP3216C_DATA_LEN)
        return -EINVAL;

    /*
     * 读取 IR 数据 (寄存器 0x0A)
     * i2c_smbus_read_word_data 返回的是主机字节序的16位值，
     * 这里手动拆分为大端序的两个字节
     */
    var = i2c_smbus_read_word_data(g_ap3216_client, AP3216C_REG_IR_DATA);
    if (var < 0) {
        pr_err("[ap3216c] 读取 IR 寄存器失败: %d\n", var);
        return -EIO;
    }
    data[0] = (var >> 8) & 0xFF;  /* IR 高字节 */
    data[1] =  var       & 0xFF;  /* IR 低字节 */

    /* 读取 ALS 环境光数据 (寄存器 0x0C) */
    var = i2c_smbus_read_word_data(g_ap3216_client, AP3216C_REG_ALS_DATA);
    if (var < 0) {
        pr_err("[ap3216c] 读取 ALS 寄存器失败: %d\n", var);
        return -EIO;
    }
    data[2] = (var >> 8) & 0xFF;  /* ALS 高字节 */
    data[3] =  var       & 0xFF;  /* ALS 低字节 */

    /* 读取 PS 接近传感器数据 (寄存器 0x0E) */
    var = i2c_smbus_read_word_data(g_ap3216_client, AP3216C_REG_PS_DATA);
    if (var < 0) {
        pr_err("[ap3216c] 读取 PS 寄存器失败: %d\n", var);
        return -EIO;
    }
    data[4] = (var >> 8) & 0xFF;  /* PS 高字节 */
    data[5] =  var       & 0xFF;  /* PS 低字节 */

    /* 安全复制到用户空间 */
    err = copy_to_user(buf, data, size);
    if (err) {
        pr_err("[ap3216c] copy_to_user 失败, 剩余 %d 字节\n", err);
        return -EFAULT;
    }

    return size;
}

/* ── file_operations 结构体: 向 VFS 注册驱动操作 ───────────────────── */
static struct file_operations ap3216c_fops = {
    .owner = THIS_MODULE,
    .open  = ap3216c_open,
    .read  = ap3216c_read,
};

/* =====================================================================
 * I2C 驱动核心 — probe / remove
 * =====================================================================
 *
 * Linux I2C 驱动模型:
 *   1. 内核启动时解析设备树，为每个 i2c 子节点创建 i2c_client
 *   2. I2C 总线匹配到驱动后，调用 probe 函数
 *   3. probe 中注册字符设备 + 创建设备节点
 *   4. 设备移除(或驱动卸载)时调用 remove 做清理
 */

/**
 * ap3216c_probe — 设备匹配成功后初始化
 *
 * 被调用时机:
 *   - 设备树中存在 compatible="alientek,ap3216c" 的节点
 *   - 或通过 i2c_new_device() 手动创建了匹配的设备
 *
 * 执行流程:
 *   1. 注册字符设备 (动态主设备号)
 *   2. 创建设备类
 *   3. 创建设备节点 /dev/ap3216
 *   4. 保存 i2c_client 指针供 read/write 使用
 */
static int ap3216c_probe(struct i2c_client *client,
                          const struct i2c_device_id *id)
{
    struct device *dev;
    int ret;

    g_ap3216_client = client;

    /* ── Step 1: 注册字符设备，获取动态主设备号 ─────────────── */
    g_major = register_chrdev(0, DRV_NAME, &ap3216c_fops);
    if (g_major < 0) {
        dev_err(&client->dev, "[ap3216c] register_chrdev 失败: %d\n", g_major);
        return g_major;
    }

    /* ── Step 2: 创建设备类 (用于 udev 自动创建设备节点) ────── */
    g_ap3216_class = class_create(THIS_MODULE, "ap3216_class");
    if (IS_ERR(g_ap3216_class)) {
        ret = PTR_ERR(g_ap3216_class);
        dev_err(&client->dev, "[ap3216c] class_create 失败: %d\n", ret);
        goto err_unregister_chrdev;
    }

    /* ── Step 3: 创建设备节点 /dev/ap3216 ───────────────────── */
    g_ap3216_device = device_create(g_ap3216_class, &client->dev,
                                     MKDEV(g_major, 0), NULL,
                                     "ap3216");
    if (IS_ERR(g_ap3216_device)) {
        ret = PTR_ERR(g_ap3216_device);
        dev_err(&client->dev, "[ap3216c] device_create 失败: %d\n", ret);
        goto err_class_destroy;
    }

    dev_info(&client->dev,
             "[ap3216c] probe 成功, addr=0x%02x, major=%d, /dev/ap3216\n",
             client->addr, g_major);
    return 0;

err_class_destroy:
    class_destroy(g_ap3216_class);
err_unregister_chrdev:
    unregister_chrdev(g_major, DRV_NAME);
    return ret;
}

/**
 * ap3216c_remove — 设备移除时清理
 *
 * 清理顺序与 probe 相反 (后创建的先销毁):
 *   1. 销毁设备节点
 *   2. 销毁设备类
 *   3. 注销字符设备
 */
static int ap3216c_remove(struct i2c_client *client)
{
    device_destroy(g_ap3216_class, MKDEV(g_major, 0));
    class_destroy(g_ap3216_class);
    unregister_chrdev(g_major, DRV_NAME);

    dev_info(&client->dev, "[ap3216c] 驱动已移除\n");
    return 0;
}

/* =====================================================================
 * 设备树兼容匹配表
 * =====================================================================
 *
 * 多平台兼容策略:
 *   - "alientek,ap3216c"  — 原 i.MX 平台 (向后兼容，不改已有 DT)
 *   - "huawei,ap3216c"    — 昇腾310B 平台 (新 DT 请使用这个)
 *
 * 设备树示例:
 *
 *   &i2c0 {
 *       ap3216c@1e {
 *           compatible = "huawei,ap3216c";   // 昇腾310B
 *           reg = <0x1E>;
 *       };
 *   };
 */
static const struct of_device_id ap3216c_dt_match[] = {
    { .compatible = "alientek,ap3216c" },   /* 原 i.MX 平台   */
    { .compatible = "huawei,ap3216c"   },   /* 昇腾310B 平台   */
    { /* 哨兵: 必须为空 */ },
};
MODULE_DEVICE_TABLE(of, ap3216c_dt_match);

/* ── 传统 I2C 设备 ID 表 (非设备树方式，保留兼容) ──────────────────── */
static const struct i2c_device_id ap3216c_i2c_id[] = {
    { "ap3216c", 0 },
    { /* 哨兵 */ }
};
MODULE_DEVICE_TABLE(i2c, ap3216c_i2c_id);

/* ── I2C 驱动结构体 ─────────────────────────────────────────────────── */
static struct i2c_driver ap3216c_i2c_driver = {
    .driver = {
        .owner          = THIS_MODULE,
        .name           = DRV_NAME,
        .of_match_table = ap3216c_dt_match,
    },
    .probe    = ap3216c_probe,
    .remove   = ap3216c_remove,
    .id_table = ap3216c_i2c_id,
};

/* =====================================================================
 * 模块出入口 — 注册/注销 I2C 驱动
 * ===================================================================== */

static int __init ap3216c_init(void)
{
    int ret;

    ret = i2c_add_driver(&ap3216c_i2c_driver);
    if (ret) {
        pr_err("[ap3216c] I2C 驱动注册失败: %d\n", ret);
        return ret;
    }

    pr_info("[ap3216c] 驱动加载成功 v" DRV_VERSION "\n");
    return 0;
}

static void __exit ap3216c_exit(void)
{
    i2c_del_driver(&ap3216c_i2c_driver);
    pr_info("[ap3216c] 驱动卸载\n");
}

module_init(ap3216c_init);
module_exit(ap3216c_exit);

/* ── 模块许可证与描述 ────────────────────────────────────────────────── */
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Smart Screen Team");
MODULE_DESCRIPTION("AP3216C ALS+PS+IR Sensor I2C Driver (Ascend 310B compatible)");
MODULE_VERSION(DRV_VERSION);
