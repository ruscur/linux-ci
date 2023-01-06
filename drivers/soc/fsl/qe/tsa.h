/* SPDX-License-Identifier: GPL-2.0 */
/*
 * TSA management
 *
 * Copyright 2022 CS GROUP France
 *
 * Author: Herve Codina <herve.codina@bootlin.com>
 */
#ifndef __SOC_FSL_TSA_H__
#define __SOC_FSL_TSA_H__

#include <dt-bindings/soc/fsl-tsa.h>
#include <linux/types.h>

struct device_node;
struct device;
struct tsa;

struct tsa *tsa_get_byphandle(struct device_node *np, const char *phandle_name);
void tsa_put(struct tsa *tsa);
struct tsa *devm_tsa_get_byphandle(struct device *dev, struct device_node *np,
				   const char *phandle_name);

/* Connect and disconnect. cell_id is one of FSL_CPM_TSA_* available in
 * dt-bindings/soc/fsl-fsa.h
 */
int tsa_connect(struct tsa *tsa, unsigned int cell_id);
int tsa_disconnect(struct tsa *tsa, unsigned int cell_id);

/* Cell information */
struct tsa_cell_info {
	unsigned long rx_fs_rate;
	unsigned long rx_bit_rate;
	u8 nb_rx_ts;
	unsigned long tx_fs_rate;
	unsigned long tx_bit_rate;
	u8 nb_tx_ts;
};

/* Get information */
int tsa_get_info(struct tsa *tsa, unsigned int cell_id, struct tsa_cell_info *info);

#endif /* __SOC_FSL_TSA_H__ */
