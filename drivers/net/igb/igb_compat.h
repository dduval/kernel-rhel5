#ifndef __IGB_COMPAT_H__
#define __IGB_COMPAT_H__

#include <linux/if_vlan.h>
#include <linux/pci.h>

#define ETH_FCS_LEN               4

#define PCIE_LINK_STATE_L0S	0

static inline struct net_device *vlan_group_get_device(struct vlan_group *vg,
						       int vlan_id)
{
	return vg->vlan_devices[vlan_id];
}

static inline void vlan_group_set_device(struct vlan_group *vg, int vlan_id,
					 struct net_device *dev)
{
	vg->vlan_devices[vlan_id] = NULL;
}

#endif 
