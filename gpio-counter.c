/*
 * gpio_counter.c
 *
 * (c) 2017 Paweł Knioła <pawel.kn@gmail.com>
 *
 * Generic GPIO impulse counter. Counts impulses using GPIO interrupts.
 * See file: Documentation/input/misc/gpio-counter.txt for more information.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/gpio.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/pm.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>

#include <asm/uaccess.h>

#define DRV_NAME "gpio-counter"

struct gpio_counter_platform_data {
    unsigned int gpio;
    unsigned int inverted;
    unsigned int debounce_ms;
};

struct gpio_counter {
    struct miscdevice miscdev;
    const struct gpio_counter_platform_data *pdata;

    unsigned long count;
    unsigned int irq;

    bool state;
    bool last_state;

    struct delayed_work debounce_work;
};

static ssize_t gpio_counter_read(struct file *file, char __user * userbuf, size_t count, loff_t * ppos)
{
    struct miscdevice *miscdev = file->private_data;
    struct device *dev = miscdev->parent;
    struct gpio_counter *counter = dev_get_drvdata(dev);

    char buf[25];
    int len = sprintf(buf, "%lu\n", counter->count);

    if ((len < 0) || (len > count))
        return -EINVAL;

    if (*ppos != 0)
        return 0;

    if (copy_to_user(userbuf, buf, len))
        return -EINVAL;

    *ppos = len;
    return len;
}

static ssize_t gpio_counter_write(struct file *file, const char __user * userbuf, size_t count, loff_t * ppos)
{
    struct miscdevice *miscdev = file->private_data;
    struct device *dev = miscdev->parent;
    struct gpio_counter *counter = dev_get_drvdata(dev);

    if (kstrtoul_from_user(userbuf, count, 0, &counter->count))
        return -EINVAL;

    return count;
}

static struct file_operations gpio_counter_fops = {
    .owner = THIS_MODULE,
    .read = gpio_counter_read,
    .write = gpio_counter_write,
};

static bool gpio_counter_get_state(const struct gpio_counter_platform_data *pdata)
{
    bool state = gpio_get_value(pdata->gpio);
    state ^= pdata->inverted;
    return state;
}

static void gpio_counter_process_state_change(struct gpio_counter *counter)
{
    if (counter->state && !counter->last_state)
        counter->count++;

    counter->last_state = counter->state;
}

static irqreturn_t gpio_counter_irq(int irq, void *dev_id)
{
    struct gpio_counter *counter = dev_id;

    counter->state = gpio_counter_get_state(counter->pdata);

    if (!counter->pdata->debounce_ms)
        gpio_counter_process_state_change(counter);

    else if (!delayed_work_pending(&counter->debounce_work))
        schedule_delayed_work(&counter->debounce_work,
            msecs_to_jiffies(counter->pdata->debounce_ms));

    return IRQ_HANDLED;
}

static void gpio_counter_debounce_work(struct work_struct *work)
{
    struct gpio_counter *counter = container_of(work, struct gpio_counter, debounce_work.work);

    bool debounced_state = gpio_counter_get_state(counter->pdata);
    if (debounced_state == counter->state)
        gpio_counter_process_state_change(counter);
}

static const struct of_device_id gpio_counter_of_match[] = {
    { .compatible = "gpio-counter", },
    { },
};
MODULE_DEVICE_TABLE(of, gpio_counter_of_match);

static struct gpio_counter_platform_data *gpio_counter_parse_dt(struct device *dev)
{
    const struct of_device_id *of_id =
                of_match_device(gpio_counter_of_match, dev);
    struct device_node *np = dev->of_node;
    struct gpio_counter_platform_data *pdata;
    enum of_gpio_flags flags;
    int err;

    if (!of_id || !np)
        return NULL;

    pdata = kzalloc(sizeof(struct gpio_counter_platform_data), GFP_KERNEL);
    if (!pdata)
        return ERR_PTR(-ENOMEM);

    pdata->gpio = of_get_gpio_flags(np, 0, &flags);
    pdata->inverted = flags & OF_GPIO_ACTIVE_LOW;

    err = of_property_read_u32(np, "debounce-delay-ms", &pdata->debounce_ms);
    if (err)
        pdata->debounce_ms = 0;

    return pdata;
}

static int gpio_counter_probe(struct platform_device *pdev)
{
    struct device *dev = &pdev->dev;
    const struct gpio_counter_platform_data *pdata = dev_get_platdata(dev);
    struct gpio_counter *counter;
    int err;

    if (!pdata) {
        pdata = gpio_counter_parse_dt(dev);
        if (IS_ERR(pdata))
            return PTR_ERR(pdata);

        if (!pdata) {
            dev_err(dev, "missing platform data\n");
            return -EINVAL;
        }
    }

    counter = kzalloc(sizeof(struct gpio_counter), GFP_KERNEL);
    if (!counter) {
        err = -ENOMEM;
        goto exit_free_mem;
    }

    counter->pdata = pdata;
    counter->count = 0;

    err = gpio_request_one(pdata->gpio, GPIOF_IN, dev_name(dev));
    if (err) {
        dev_err(dev, "unable to request GPIO %d\n", pdata->gpio);
        goto exit_free_mem;
    }

    counter->last_state = gpio_counter_get_state(pdata);
    counter->irq = gpio_to_irq(pdata->gpio);

    err = request_irq(counter->irq, &gpio_counter_irq,
                      IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
                      DRV_NAME, counter);
    if (err) {
        dev_err(dev, "unable to request IRQ %d\n", counter->irq);
        goto exit_free_gpio;
    }

    INIT_DELAYED_WORK(&counter->debounce_work, gpio_counter_debounce_work);

    counter->miscdev.minor  = MISC_DYNAMIC_MINOR;
    counter->miscdev.name   = dev_name(dev);
    counter->miscdev.fops   = &gpio_counter_fops;
    counter->miscdev.parent = dev;

    err = misc_register(&counter->miscdev);
    if (err) {
        dev_err(dev, "failed to register misc device\n");
        goto exit_free_irq;
    }

    dev_set_drvdata(dev, counter);
    dev_info(dev, "registered new misc device %s\n", counter->miscdev.name );
    return 0;

exit_free_irq:
    free_irq(counter->irq, counter);
exit_free_gpio:
    gpio_free(pdata->gpio);
exit_free_mem:
    kfree(counter);
    if (!dev_get_platdata(&pdev->dev))
        kfree(pdata);

    return err;
}

static int gpio_counter_remove(struct platform_device *pdev)
{
    struct gpio_counter *counter = platform_get_drvdata(pdev);
    const struct gpio_counter_platform_data *pdata = counter->pdata;

    device_init_wakeup(&pdev->dev, false);

    if (delayed_work_pending(&counter->debounce_work))
        cancel_delayed_work(&counter->debounce_work);

    misc_deregister(&counter->miscdev);
    free_irq(counter->irq, counter);
    gpio_free(pdata->gpio);
    kfree(counter);
    if (!dev_get_platdata(&pdev->dev))
        kfree(pdata);

    return 0;
}

static int gpio_counter_suspend(struct device *dev)
{
    struct gpio_counter *counter = dev_get_drvdata(dev);

    if (device_may_wakeup(dev))
        enable_irq_wake(counter->irq);

    return 0;
}

static int gpio_counter_resume(struct device *dev)
{
    struct gpio_counter *counter = dev_get_drvdata(dev);

    if (device_may_wakeup(dev))
        disable_irq_wake(counter->irq);

    return 0;
}

static SIMPLE_DEV_PM_OPS(gpio_counter_pm_ops,
         gpio_counter_suspend, gpio_counter_resume);

static struct platform_driver gpio_counter_driver = {
    .probe		= gpio_counter_probe,
    .remove		= gpio_counter_remove,
    .driver		= {
        .name	= DRV_NAME,
        .pm	= &gpio_counter_pm_ops,
        .of_match_table = of_match_ptr(gpio_counter_of_match),
    }
};
module_platform_driver(gpio_counter_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Generic GPIO counter driver");
MODULE_AUTHOR("Paweł Knioła <pawel.kn@gmail.com>");
