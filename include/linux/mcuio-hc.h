/*
 * Copyright 2011 Dog Hunter SA
 * Author: Davide Ciminaghi <ciminaghi@gnudd.com>
 *
 * Header file for mcuio host controller
 *
 * GNU GPLv2 or later
 */
#ifndef __HOST_CONTROLLER_H__
#define __HOST_CONTROLLER_H__

#define MCUIO_HC_OUTBUF 0x8
#define MCUIO_HC_INBUF 0x108
/* How many 32bits words are in rx buffer */
#define MCUIO_RX_CNT   0x208
/* Irq register */
#define MCUIO_IRQ      0x20c
/* Irq status */
#define MCUIO_IRQ_STAT  0x210
#define RX_RDY         0x1
/* Clear register */
#define MCUIO_IRQ_CLR  0x214

#define MCUIO_HC_MAX_REGISTER 0x214

struct regmap;

typedef struct regmap *(*setup_regmap)(struct device *, void *data);

/*
 * Platform data for host controller
 *
 * @setup_regmap: pointer to function setting up a regmap for controller
 * @data: data to be passed on to setup_regmap.
 */
struct mcuio_hc_platform_data {
	setup_regmap setup_regmap;
	void *data;
};

#endif /* __HOST_CONTROLLER_H__ */