# Reference only -- the build here is package.sh.
#
# theos `make` does not work on this machine: its vendored IOKit headers
# conflict with the macOS 27 SDK and the build dies with "could not build
# module 'Foundation'", with and without any change of ours. dockpid hit the
# same wall and sidesteps it the same way. This file is kept so anyone with a
# working theos has the canonical recipe.
#
# Keep it in step with package.sh. The link line is not optional decoration:
# without ellekit there is no MSHookFunction, and without SkyLight there is
# nothing to anchor dladdr on when resolving the server-side symbols.

TARGET := macosx:clang:latest:15.0
ARCH = arm64e

include $(THEOS)/makefiles/common.mk

BUNDLE_NAME = Vanish

Vanish_FILES = Vanish.m
Vanish_INSTALL_PATH = /Library/TweakInject/Tweaks/Bundles
Vanish_CFLAGS = -fobjc-arc

# MSHookFunction. Installed at /usr/local/lib/libellekit.dylib, which is also
# its install name, so -L is a link-time concern only.
Vanish_LIBRARIES = ellekit
Vanish_LDFLAGS = -L/Library/TweakInject

# SkyLight is private, so it needs the private-framework path rather than
# _FRAMEWORKS.
Vanish_FRAMEWORKS = Foundation CoreGraphics
Vanish_PRIVATE_FRAMEWORKS = SkyLight

include $(THEOS_MAKE_PATH)/bundle.mk
