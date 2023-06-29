/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2019-2023 NXP
 */

#ifndef _FSL_ASRC_M2M_H
#define _FSL_ASRC_M2M_H

#include <linux/dma/imx-dma.h>
#include "fsl_asrc_common.h"

#define V4L_CAP OUT
#define V4L_OUT IN

#define ASRC_xPUT_DMA_CALLBACK(dir) \
	(((dir) == V4L_OUT) ? fsl_asrc_input_dma_callback \
	 : fsl_asrc_output_dma_callback)

#define DIR_STR(dir) (dir) == V4L_OUT ? "out" : "cap"

#if IS_ENABLED(CONFIG_SND_SOC_FSL_ASRC_M2M)
int fsl_asrc_m2m_probe(struct fsl_asrc *asrc);
int fsl_asrc_m2m_remove(struct platform_device *pdev);
int fsl_asrc_m2m_suspend(struct fsl_asrc *asrc);
int fsl_asrc_m2m_resume(struct fsl_asrc *asrc);
#else
static inline int fsl_asrc_m2m_probe(struct fsl_asrc *asrc)
{
	return 0;
}

static inline int fsl_asrc_m2m_remove(struct platform_device *pdev)
{
	return 0;
}

static inline int fsl_asrc_m2m_suspend(struct fsl_asrc *asrc)
{
	return 0;
}

static inline int fsl_asrc_m2m_resume(struct fsl_asrc *asrc)
{
	return 0;
}
#endif

#endif /* _FSL_EASRC_M2M_H */
