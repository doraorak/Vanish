# Canonical Theos Makefile for Vanish.
#
# Standalone compilation and packaging without external Theos setup can be
# performed directly using ./package.sh.
#
# Required linkages:
# - ellekit for MSHookFunction
# - SkyLight (private framework) for compositor symbols and types

TARGET := macosx:clang:latest:15.0
ARCHS = arm64e

GO_EASY_ON_ME = 1

include $(THEOS)/makefiles/common.mk


BUNDLE_NAME = Vanish

Vanish_FILES = Vanish.c
Vanish_INSTALL_PATH = /Library/TweakInject/Tweaks/Bundles
Vanish_CFLAGS += -fblocks -std=c11 -fno-modules

# MSHookFunction. Installed at /usr/local/lib/libellekit.dylib, which is also
# its install name, so -L is a link-time concern only.
Vanish_LIBRARIES = ellekit
Vanish_LDFLAGS = -L/Library/TweakInject

# SkyLight is private, so it needs the private-framework path rather than
# _FRAMEWORKS.
Vanish_FRAMEWORKS = CoreFoundation CoreGraphics
Vanish_PRIVATE_FRAMEWORKS = SkyLight

include $(THEOS_MAKE_PATH)/bundle.mk
