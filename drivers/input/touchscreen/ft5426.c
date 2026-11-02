#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_gpio.h>
#include <linux/irq.h>
#include <asm/mach/map.h> 
#include <asm/uaccess.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/i2c.h>
#include <linux/miscdevice.h>
#include <linux/delay.h>
#include <linux/input/mt.h>

// sudo cp ft5426.ko /home/zy/linux/nfs/rootfs/lib/modules/4.1.15/ -f
// sudo cp ft5426.ko ft5426APP /home/zy/linux/nfs/rootfs/lib/modules/4.1.15/ -f

#define ft5426_MINOR MISC_DYNAMIC_MINOR
#define ft5426_NAME "ft5426"

#define MAX_SUPPORT_POINTS 5        /* 5 点触摸 */ 
#define TOUCH_EVENT_DOWN 0x00       /* 按下 */ 
#define TOUCH_EVENT_UP 0x01         /* 抬起 */ 
#define TOUCH_EVENT_ON 0x02         /* 接触 */ 
#define TOUCH_EVENT_RESERVED 0x03   /* 保留 */ 

// FT5426 触摸屏寄存器地址定义
#define FT5X06_TD_STATUS_REG 0X02   /* 状态寄存器地址 */ 
#define FT5x06_DEVICE_MODE_REG 0X00 /* 模式寄存器 */ 
#define FT5426_IDG_MODE_REG 0XA4    /* 中断模式 */ 
#define FT5X06_READLEN 29           /* 要读取的寄存器个数 */ 

struct ft5426 {
    struct device *dev;         /**< device structure */
    int irq_gpio;                /**< GPIO number for interrupt */
    int reset_gpio;              /**< GPIO number for reset */
    int irq;                     /**< IRQ number */
    struct input_dev *input_dev; /**< Input device structure */
    struct i2c_client *client;   /**< i2c client device */
    irqreturn_t (*irq_handler)(int irq, void *dev_id); /**< Interrupt handler function pointer */
}ft5426;

/* 从ft5426读取数据 */
static int ft5426_read_regs(struct ft5426 *dev, u8 reg, void *val, int len)
{
    struct i2c_client *client = dev->client;
    struct i2c_msg msgs[2];
    int ret;

    /* msg[0]为要读取的数据地址 */
    msgs[0].addr = client->addr;
    msgs[0].flags = 0;/* 表示写操作，写入要读取的数据地址 */
    msgs[0].len = 1;
    msgs[0].buf = &reg;

    /* msg[1]为要读取的数据 */
    msgs[1].addr = client->addr;
    
    msgs[1].flags = I2C_M_RD;/* 表示读操作 */
    msgs[1].len = len;/* 要读取的reg长度 */
    msgs[1].buf = val;

    ret = i2c_transfer(client->adapter, msgs, 2);
    if (ret < 0) {
        dev_err(&client->dev, "i2c_transfer failed: %d\n", ret);
        return ret;
    }

    return 0;
}

/* 向ft5426写入数据 */
static int ft5426_write_regs(struct ft5426 *dev, u8 reg, void *val, int len)
{
    struct i2c_client *client = dev->client;
    struct i2c_msg msg;
    int ret;
    u8 *buf;

    buf = kmalloc(len + 1, GFP_KERNEL);
    if (!buf) {
        dev_err(&client->dev, "Failed to allocate memory for write buffer\n");
        return -ENOMEM;
    }

    buf[0] = reg;//保存要写入的寄存器地址
    memcpy(buf + 1, val, len);//保存要写入的数据

    msg.addr = client->addr;
    msg.flags = 0;
    msg.len = len + 1;
    msg.buf = buf;

    ret = i2c_transfer(client->adapter, &msg, 1);
    if (ret < 0) {
        dev_err(&client->dev, "i2c_transfer failed: %d\n", ret);
        kfree(buf);
        return ret;
    }

    kfree(buf);
    return 0;
}

#if 0
static u8 ft5426_read_reg(struct ft5426 *dev, u8 reg)
{
    u8 value;
    int ret = ft5426_read_regs(dev, reg, &value, 1);
    if (ret < 0) {
        dev_err(dev->dev, "Failed to read register 0x%02x\n", reg);
        return 0; // 返回一个默认值，或者根据需要处理错误
    }
    return value;
}
#endif

static irqreturn_t ft5426_irq_handler(int irq, void *dev_id)
{
    struct ft5426 *dev = dev_id;
    u8 readbuf[29];
    int i,type, x, y, id;
    int offset, tplen;
    bool down = false;

    offset = 1; // 偏移量，也就是0x02+1=0x03，表示从0x03开始读取数据
    tplen = 6; // 每个触摸点数据占6个寄存器地址 两个寄存器为空

    memset(readbuf, 0, sizeof(readbuf));

    /* 读取触摸点数据 */
    ft5426_read_regs(dev, FT5X06_TD_STATUS_REG, readbuf, FT5X06_READLEN);

    /* 解析触摸点数据并上报 */
    // 这里需要根据FT5426的数据格式解析buf，并使用input_report_*函数上报触摸事件
    for(i = 0; i < MAX_SUPPORT_POINTS; i++) {
        u8 *buf = &readbuf[offset + i * tplen];
        type = buf[0] >> 6;         //获取按键类型
        if(type == TOUCH_EVENT_RESERVED)
            continue;//如果按键类型重复，直接跳过此次循环

        x = ((buf[2] << 8) | buf[3]) & 0x0fff;
        y = ((buf[0] << 8) | buf[1]) & 0x0fff;

        id = (buf[2] >> 4) & 0x0f;
        down = (type != TOUCH_EVENT_UP);

        input_mt_slot(dev->input_dev, id);
        input_mt_report_slot_state(dev->input_dev, MT_TOOL_FINGER, down);

        if(!down)
            continue;

        input_report_abs(dev->input_dev, ABS_MT_POSITION_X, x);
        input_report_abs(dev->input_dev, ABS_MT_POSITION_Y, y);
    }

    input_mt_report_pointer_emulation(dev->input_dev, true);
    input_sync(dev->input_dev);

    return IRQ_HANDLED;
}

static int ft5426_ts_reset(struct i2c_client *client, struct ft5426 *dev)//第一次初始化时，复位触摸屏
{
    int ret = 0;
    if(gpio_is_valid(dev->reset_gpio)) {
        ret = devm_gpio_request_one(&client->dev, dev->reset_gpio, GPIOF_OUT_INIT_LOW, "ft5426_reset");//申请IO口并设置为低电平
        if(ret < 0) {
            dev_err(&client->dev, "Failed to request reset GPIO: %d\n", ret);
            return ret;
        }

        msleep(5);
        gpio_set_value(dev->reset_gpio, 1);
        msleep(200);
    }
    return 0;
}

static int ft5426_ts_irq(struct i2c_client *client, struct ft5426 *dev)
{
    int ret = 0;
    if(gpio_is_valid(dev->irq_gpio)) {
        ret = devm_gpio_request_one(&client->dev, dev->irq_gpio, GPIOF_IN, "ft5426_irq");
        if(ret < 0) {
            dev_err(&client->dev, "Failed to request irq GPIO: %d\n", ret);
            return ret;
        }
    }

    //中断线程化
    ret = devm_request_threaded_irq(&client->dev, gpio_to_irq(dev->irq_gpio), NULL, dev->irq_handler, 
                            IRQF_TRIGGER_FALLING | IRQF_ONESHOT, "ft5426_irq", dev);
    if(ret < 0) {
        dev_err(&client->dev, "Failed to request irq: %d\n", ret);
        return ret;
    }

    return 0;
}

static int ft5426_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    int ret = 0;
    u8 config = 0x00; // 配置寄存器的值，根据需要设置
    printk("ft5426 probe\n");

    ft5426.client = client;
    ft5426.dev = &client->dev;
    ft5426.irq_handler = ft5426_irq_handler;

    /* 获取irq和reset GPIO */
    ft5426.irq_gpio = of_get_named_gpio(client->dev.of_node, "interrupts-gpios", 0);
    ft5426.reset_gpio = of_get_named_gpio(client->dev.of_node, "reset-gpios", 0);

    ft5426_ts_reset(client, &ft5426);
    ft5426_ts_irq(client, &ft5426);

    //初始化触摸屏寄存器
    ft5426_write_regs(&ft5426, FT5x06_DEVICE_MODE_REG, &config, 0); //正常模式
    config = 0x01;
    ft5426_write_regs(&ft5426, FT5426_IDG_MODE_REG, &config, 0); //中断模式

    //input框架
    ft5426.input_dev = devm_input_allocate_device(&client->dev);
    if (!ft5426.input_dev) { 
        dev_err(&client->dev, "Failed to allocate input device\n");
        return -ENOMEM;
    }

    ft5426.input_dev->name = client->name;
    ft5426.input_dev->id.bustype = BUS_I2C;//供用户态读取的描述信息
    ft5426.input_dev->dev.parent = &client->dev;//`/sys`文件系统下，input 设备节点会挂在 i2c 设备节点下面

//      `evbit` 是事件类型位图，标记这个输入设备支持哪些事件类型。
    __set_bit(EV_KEY, ft5426.input_dev->evbit);//EV_KEY是按键事件类型，表示这个输入设备可以产生按键事件
    __set_bit(EV_ABS, ft5426.input_dev->evbit);//EV_ABS是绝对坐标事件类型，表示这个输入设备可以产生绝对坐标事件
//      `keybit` 是按键位图，标记这个输入设备支持哪些按键。
//      `BTN_TOUCH`：触摸按键，代表**触摸屏是否有手指按下**
    __set_bit(BTN_TOUCH, ft5426.input_dev->keybit);

    input_mt_init_slots(ft5426.input_dev, MAX_SUPPORT_POINTS, 0);//初始化多点触摸
    input_set_abs_params(ft5426.input_dev, ABS_MT_POSITION_X, 0, 1024, 0, 0);
    input_set_abs_params(ft5426.input_dev, ABS_MT_POSITION_Y, 0, 600, 0, 0);
    input_set_abs_params(ft5426.input_dev, ABS_MT_PRESSURE, 0, 255, 0, 0);//压力值

    ret = input_register_device(ft5426.input_dev);

    return 0;
}

static int ft5426_remove(struct i2c_client *client)
{
    printk("ft5426 remove\n");
    input_unregister_device(ft5426.input_dev);
    return 0;
}

/* 设备树匹配表 */
static const struct of_device_id ft5426_of_match[] = {
    { .compatible = "ft5426", },
    { }
};

static const struct i2c_device_id ft5426_id[] = {
    { "ft5426", 0 },
    { }
};

/* i2c_driver */
static struct i2c_driver ft5426_driver = {
    .driver = {
        .name = "ft5426",   //驱动名称(习惯性保留，与设备树中的compatible属性对应，无设备树时，驱动名称与i2c_client的name属性对应)
        .owner = THIS_MODULE,
        .of_match_table = of_match_ptr(ft5426_of_match),//匹配表
    },
    .probe = ft5426_probe,
    .remove = ft5426_remove,
    .id_table = ft5426_id,
};

static int __init ft5426_init(void)
{
    int ret = 0;

    ret = i2c_add_driver(&ft5426_driver);

    if(ret){
        printk("failed to add i2c driver\n");
        return -1;
    }

    return 0;
}

static void __exit ft5426_exit(void)
{
    i2c_del_driver(&ft5426_driver);
}


/* 驱动入口函数 */
module_init(ft5426_init);
module_exit(ft5426_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("ZY_YORK");
