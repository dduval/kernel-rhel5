#ifndef __IXBVF_COMPAT_H__
#define __IXVVF_COMPAT_H__

#include <linux/if_vlan.h>

#define ETH_FCS_LEN               4

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
