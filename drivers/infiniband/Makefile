INFINIBANDINCLUDE := -Idrivers/infiniband/include \
	$(if $(KBUILD_SRC),-I$(srctree)/drivers/infiniband/include)

export CPPFLAGS := $(INFINIBANDINCLUDE) $(CPPFLAGS)

obj-$(CONFIG_INFINIBAND)		+= core/
obj-$(CONFIG_INFINIBAND_MTHCA)		+= hw/mthca/
obj-$(CONFIG_INFINIBAND_IPATH)		+= hw/ipath/
obj-$(CONFIG_INFINIBAND_EHCA)		+= hw/ehca/
obj-$(CONFIG_INFINIBAND_AMSO1100)	+= hw/amso1100/
obj-$(CONFIG_INFINIBAND_CXGB3)		+= hw/cxgb3/
obj-$(CONFIG_INFINIBAND_NES)		+= hw/nes/
obj-$(CONFIG_MLX4_INFINIBAND)		+= hw/mlx4/
obj-$(CONFIG_INFINIBAND_IPOIB)		+= ulp/ipoib/
obj-$(CONFIG_INFINIBAND_SRP)		+= ulp/srp/
obj-$(CONFIG_INFINIBAND_ISER)		+= ulp/iser/
obj-$(CONFIG_INFINIBAND_SDP)		+= ulp/sdp/
obj-$(CONFIG_INFINIBAND_QLGC_VNIC)	+= ulp/qlgc_vnic/
obj-$(CONFIG_INFINIBAND_MADEYE)		+= util/
