/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2020 Synopsys, Inc. and/or its affiliates.
 * Synopsys DesignWare XPCS helpers
 */

#ifndef __LINUX_MDIO_XPCS_H
#define __LINUX_MDIO_XPCS_H

#include <linux/phylink.h>

#if IS_ENABLED(CONFIG_MDIO_XPCS)
struct phylink_pcs_ops *mdio_xpcs_get_ops(void);
#else
static inline struct phylink_pcs_ops *mdio_xpcs_get_ops(void)
{
	return NULL;
}
#endif

#endif /* __LINUX_MDIO_XPCS_H */
