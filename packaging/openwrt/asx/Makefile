#
# OpenWrt package recipe for asx (bd-j4m.7)
#
# This recipe is intentionally dependency-light:
# - builds the static runtime library with embedded-router profile defaults
# - installs lib + headers for constrained targets
# - installs a no-op init script for startup sanity smoke
#
# SPDX-License-Identifier: MIT
#

include $(TOPDIR)/rules.mk

PKG_NAME:=asx
PKG_VERSION:=0.1.0
PKG_RELEASE:=1

PKG_SOURCE_PROTO:=git
PKG_SOURCE_URL:=https://github.com/Dicklesworthstone/asupersync_ansi_c.git
PKG_SOURCE_VERSION:=main
PKG_MIRROR_HASH:=skip

PKG_LICENSE:=MIT
PKG_LICENSE_FILES:=LICENSE
PKG_BUILD_PARALLEL:=1

include $(INCLUDE_DIR)/package.mk

define Package/asx
  SECTION:=libs
  CATEGORY:=Libraries
  TITLE:=asupersync ANSI C runtime (asx)
  URL:=https://github.com/Dicklesworthstone/asupersync_ansi_c
endef

define Package/asx/description
Portable dependency-free asupersync runtime in ANSI C with deterministic replay,
resource contracts, and embedded-router profile support.
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		build PROFILE=EMBEDDED_ROUTER CODEC=BIN DETERMINISTIC=1
endef

define Package/asx/install
	$(INSTALL_DIR) $(1)/usr/lib
	$(INSTALL_DATA) $(PKG_BUILD_DIR)/build/lib/libasx.a $(1)/usr/lib/libasx.a

	$(INSTALL_DIR) $(1)/usr/include
	$(CP) $(PKG_BUILD_DIR)/include/asx $(1)/usr/include/

	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/asx.init $(1)/etc/init.d/asx
endef

$(eval $(call BuildPackage,asx))
