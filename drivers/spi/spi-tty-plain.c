/*
 * Copyright (c) dog hunter AG - Zug - CH
 * General Public License version 2 (GPLv2)
 * Author Aurelio Colosimo <aurelio@aureliocolosimo.it>
 * Originally copied from spi-tty-bathos-ds.c, by Federico Vaga
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/spi/spi.h>
#include <linux/gpio.h>
#include <linux/spinlock.h>
#include <linux/serial.h>
#include <linux/tty.h>
#include <linux/tty_driver.h>
#include <linux/tty_flip.h>
#include <linux/moduleparam.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>
#include <linux/platform_device.h>

/* ASCII char 0x5 is 'enquiry' and is here used to poll MCU */
#define SPI_TTY_ENQUIRY 0x5
#define SPI_TTY_ENQ_INT_MS 200
#define SPI_TTY_MSG_LEN 64
#define SPI_TTY_FREQ_HZ_RX 38400
#define SPI_TTY_FREQ_HZ_TX 115200
static unsigned int dev_count = 0;
static spinlock_t lock;

#define tty_to_spitty(_ptr) ((struct spi_tty*)dev_get_drvdata(_ptr->dev))

/*
 * It describe the driver status
 */
struct spi_tty {
	struct spi_device *spi;

	unsigned long flags;

	unsigned int tty_minor;
	struct device *tty_dev;
	struct tty_port port;

	char enq_buf[SPI_TTY_MSG_LEN + 1];
	struct workqueue_struct *wq;
	struct delayed_work work;
	struct mutex mtx;
};

#define SPI_SERIAL_TTY_MINORS 1
static struct tty_driver *spi_serial_tty_driver = NULL;
static struct tty_struct *ttys[SPI_SERIAL_TTY_MINORS];

/* * * * TTY Operations * * * */

static void spi_poll_work_handler(struct work_struct *w);
static unsigned long wqinterval;

/*
 * The kernel invokes this function when a program opens the TTY interface of
 * this driver
 */
static int spi_serial_tty_open(struct tty_struct *tty, struct file *filp)
{
	struct spi_tty *stty = tty_to_spitty(tty);
	int ret;

	ret = tty_port_open(&stty->port, tty, filp);

	return ret;
}

/*
 * The kernel invokes this function when a program closes the TTY interface of
 * this driver
 */
static void spi_serial_tty_close(struct tty_struct *tty, struct file *filp)
{
	struct spi_tty *stty = tty_to_spitty(tty);

	dev_vdbg(tty->dev, "%s:%d\n", __func__, __LINE__);

	mutex_lock(&stty->mtx);
	tty_ldisc_flush(tty);
	tty_port_close(&stty->port, tty, filp);
	mutex_unlock(&stty->mtx);

	wake_up_interruptible(&stty->port.open_wait);
	wake_up_interruptible(&stty->port.close_wait);
}

/*
 * This function return the number of bytes that this driver can accept. There
 * is not a real limit because se redirect all the traffic to the SPI
 * framework. So, the limit here is indicative.
 */
static int spi_serial_tty_write_room(struct tty_struct *tty)
{
	return SPI_TTY_MSG_LEN;
}

static int __spi_serial_tty_write(struct spi_tty *stty,
		const unsigned char *buf, int count, int discard_rx)
{
	int ret = 0;
	struct spi_message *m;
	struct spi_transfer *t;
	unsigned int len;
	int i;
	struct tty_struct *tty;
	unsigned char *rx_buf;

	if (!buf || !count) {
		return 0;
	}

	mutex_lock(&stty->mtx);

	if (discard_rx)
		len = min(SPI_TTY_MSG_LEN, count);
	else
		len = count;

	rx_buf = kzalloc(len, GFP_KERNEL | GFP_ATOMIC);
	if (!rx_buf) {
		mutex_unlock(&stty->mtx);
		return -ENOMEM;
	}

	m = kmalloc(sizeof(struct spi_message), GFP_KERNEL | GFP_ATOMIC);
	if (!m) {
		mutex_unlock(&stty->mtx);
		return -ENOMEM;
	}

	spi_message_init(m);

	t = kzalloc(sizeof(struct spi_transfer), GFP_KERNEL | GFP_ATOMIC);
	if (!t) {
		mutex_unlock(&stty->mtx);
		return -ENOMEM;
	}

	t->len = len;
	t->tx_buf = buf;
	t->rx_buf = rx_buf;
	t->speed_hz = discard_rx ? SPI_TTY_FREQ_HZ_TX : SPI_TTY_FREQ_HZ_RX;

	spi_message_add_tail(t, m);

	ret = spi_sync(stty->spi, m);

	if (discard_rx)
		goto end;

	if (ret)
		dev_dbg(stty->tty_dev, "%s %d bytes, spi_sync returns %d\n",
			__func__, len, ret);

	tty = tty_port_tty_get(&stty->port);

	if (!tty)
		goto end;

	for (i = 1; i < len; i++) {
		if (rx_buf[i] == '\0')
			continue;
		tty_insert_flip_char(tty, rx_buf[i], TTY_NORMAL);
	}
	tty_flip_buffer_push(tty);

end:

	kfree(t);
	kfree(m);
	kfree(rx_buf);

	mutex_unlock(&stty->mtx);
	return len;

}
/*
 * The kernel invokes this function when a program writes on the TTY interface
 * of this driver
 */
static int spi_serial_tty_write(struct tty_struct *tty,
				const unsigned char *buf, int count)
{
	struct spi_tty *stty = tty_to_spitty(tty);
	return __spi_serial_tty_write(stty, buf, count, 1);
}


static struct tty_struct *spi_serial_tty_lookup(struct tty_driver *driver,
		struct inode *inode, int idx)
{
	if (idx == 0);
		return ttys[0];
	return NULL;
}

static int spi_serial_tty_install(struct tty_driver *driver,
				  struct tty_struct *tty)
{
	if (ttys[0])
		return -EBUSY;

	if (tty_init_termios(tty))
		return -ENOMEM;

	tty_driver_kref_get(driver);
	tty->count++;

	ttys[0] = tty;
	driver->ttys = ttys;

	return 0;
}

static void spi_serial_tty_remove(struct tty_driver *self,
				  struct tty_struct *tty)
{
	ttys[0] = NULL;
}

static struct tty_operations spi_serial_ops = {
	.lookup		= spi_serial_tty_lookup,
	.install	= spi_serial_tty_install,
	.remove		= spi_serial_tty_remove,
	.open		= spi_serial_tty_open,
	.close		= spi_serial_tty_close,
	.write		= spi_serial_tty_write,
	.write_room	= spi_serial_tty_write_room,
};

static void spi_serial_port_dtr_rts(struct tty_port *port, int on){
	/* Nothing to do */
}

static const struct tty_port_operations spi_serial_port_ops = {
	.dtr_rts = spi_serial_port_dtr_rts, /* required, even if empty */
};

static void spi_poll_work_handler(struct work_struct *w)
{
	struct spi_tty *stty;

	stty = container_of(to_delayed_work(w), struct spi_tty, work);

	__spi_serial_tty_write(stty, stty->enq_buf, sizeof(stty->enq_buf), 0);
	if ((!delayed_work_pending(&stty->work)))
		queue_delayed_work(stty->wq, &stty->work, wqinterval);
}

/* * * * Driver Initialization * * * */
static int spi_tty_probe(struct spi_device *spi)

{
	struct spi_tty *stty;
	int err = 0;
	unsigned long flags;

	if (dev_count >= SPI_SERIAL_TTY_MINORS)
		return -ENOMEM;

	dev_info(&spi->dev, "%s\n", __func__);

	stty = kzalloc(sizeof(struct spi_tty), GFP_KERNEL);
	if (!stty)
		return -ENOMEM;
	spi_set_drvdata(spi, stty);
	stty->spi = spi;

	memset(stty->enq_buf, SPI_TTY_ENQUIRY, sizeof(stty->enq_buf));
	stty->enq_buf[sizeof(stty->enq_buf) - 1] = 0x00;

	mutex_init(&stty->mtx);

	/* Initialize port */
	tty_port_init(&stty->port);
	stty->port.ops = &spi_serial_port_ops;

	/* Register new port*/
	stty->tty_minor = dev_count;
	stty->tty_dev = tty_register_device(spi_serial_tty_driver,
					stty->tty_minor, &stty->spi->dev);
	if (IS_ERR(stty->tty_dev)) {
		dev_err(&spi->dev, "tty_register_device failed\n");
		err = PTR_ERR(stty->tty_dev);
		goto err_req_tty;
	}

	/* add private data to the device */
	dev_set_drvdata(stty->tty_dev, stty);

	spin_lock_irqsave(&lock, flags);
	dev_count++;
	spin_unlock_irqrestore(&lock, flags);

	/* start workqueue for enquiry poll */
	wqinterval = msecs_to_jiffies(SPI_TTY_ENQ_INT_MS);
	pr_info("spi_poll_wq called every %u jiffies\n",
		(unsigned)wqinterval);

	stty->wq = create_singlethread_workqueue("spi_poll_wq");

	if (stty->wq) {
		INIT_DELAYED_WORK(&stty->work, spi_poll_work_handler);
		queue_delayed_work(stty->wq, &stty->work, 0);
	}

	return 0;

err_req_tty:
	kfree(stty);
	return err;
}

static int spi_tty_remove(struct spi_device *spi)
{
	unsigned long flags;
	struct spi_tty *stty = spi_get_drvdata(spi);

	cancel_delayed_work_sync(&stty->work);
	flush_workqueue(stty->wq);
	destroy_workqueue(stty->wq);

	dev_info(&spi->dev, "%s\n", __func__);

	stty = spi_get_drvdata(spi);

	spin_lock_irqsave(&lock, flags);
	if (stty->tty_minor == dev_count - 1)
		dev_count--;
	spin_unlock_irqrestore(&lock, flags);

	spi_serial_tty_remove(spi_serial_tty_driver, NULL);
	/* Remove device */
	tty_unregister_device(spi_serial_tty_driver, stty->tty_minor);

	kfree(stty);
	return 0;
}

static const struct spi_device_id spi_tty_id[] = {
	{"atmega32u4"},
	{}
};

static struct spi_driver spi_tty_driver = {
	.driver = {
		.name	= KBUILD_MODNAME,
		.bus	= &spi_bus_type,
		.owner	= THIS_MODULE,
	},
	.id_table	= spi_tty_id,
	.probe		= spi_tty_probe,
	.remove		= spi_tty_remove,
};

static int spi_tty_init(void)
{
	int err;
	int i;

	spin_lock_init(&lock);

	for (i = 0; i < SPI_SERIAL_TTY_MINORS; i++)
		ttys[i] = NULL;

	/*
	 * Allocate driver structure and reserve space for a number of
	 * devices
	 */
	spi_serial_tty_driver = alloc_tty_driver(SPI_SERIAL_TTY_MINORS);

	if (!spi_serial_tty_driver)
		return -ENOMEM;

	/*
	 * Configure driver
	 */
	spi_serial_tty_driver->driver_name = "spiserialplain";
	spi_serial_tty_driver->name = "ttySPI";
	spi_serial_tty_driver->major = 0;
	spi_serial_tty_driver->minor_start = 0;
	spi_serial_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	spi_serial_tty_driver->subtype = SERIAL_TYPE_NORMAL;
	spi_serial_tty_driver->flags = TTY_DRIVER_DYNAMIC_DEV;

	tty_set_operations(spi_serial_tty_driver, &spi_serial_ops);
	err = tty_register_driver(spi_serial_tty_driver);
	if (err) {
		pr_err("%s - tty_register_driver failed\n", __func__);
		goto exit_reg_tty_driver;
	}

	return spi_register_driver(&spi_tty_driver);

exit_reg_tty_driver:
	put_tty_driver(spi_serial_tty_driver);
	return err;

}

static void spi_tty_exit(void)
{
	int err;
	err = tty_unregister_driver(spi_serial_tty_driver);
	put_tty_driver(spi_serial_tty_driver);
	driver_unregister(&spi_tty_driver.driver);
}

module_init(spi_tty_init);
module_exit(spi_tty_exit);
MODULE_AUTHOR("Aurelio Colosimo <aurelio@aureliocolosimo.it>");
MODULE_LICENSE("GPL");
